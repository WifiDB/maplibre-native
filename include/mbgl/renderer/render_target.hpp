#pragma once

#include <mbgl/gfx/types.hpp>
#include <mbgl/tile/tile_id.hpp>
#include <mbgl/util/size.hpp>
#include <mbgl/util/color.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>

namespace mbgl {

namespace gfx {
class Context;
class Texture2D;
class OffscreenTexture;
class UniformBuffer;
class UploadPass;
using Texture2DPtr = std::shared_ptr<Texture2D>;
using UniformBufferPtr = std::shared_ptr<UniformBuffer>;
} // namespace gfx

namespace shaders {
struct GlobalPaintParamsUBO;
} // namespace shaders

class LayerGroupBase;
class PaintParameters;
class RenderOrchestrator;
class RenderTree;

using LayerGroupBasePtr = std::shared_ptr<LayerGroupBase>;

/// Render target class
class RenderTarget {
public:
    RenderTarget(gfx::Context& context, const Size size, const gfx::TextureChannelDataType type);
    ~RenderTarget();

    /// Set the color the render target is cleared to before its layer groups are drawn
    void setClearColor(const Color& color) { backgroundColor = color; }

    /// Mark this render target as a terrain drape target for the given tile.
    /// Drape targets render the orchestrator's draped tile layer groups
    /// (filtered to overlapping tiles) instead of their own layer groups.
    void setDrapeTileID(const UnwrappedTileID& id);
    const std::optional<UnwrappedTileID>& getDrapeTileID() const { return drapeTileID; }

    /// Refresh this drape target's copy of the global paint parameters,
    /// carrying the target tile in `drape_tile` (no-op for non-drape targets)
    void updateDrapeGlobalUBO(const shaders::GlobalPaintParamsUBO& params, gfx::Context& context);

    /// Get the render target texture
    const gfx::Texture2DPtr& getTexture();

    /// @brief Add a layer group to the render target
    /// @param replace Flag to replace if exists
    /// @return whether added
    bool addLayerGroup(LayerGroupBasePtr, bool replace);

    /// @brief Remove a layer group
    /// @param layerIndex index of the layer to remove
    /// @return whether removed
    bool removeLayerGroup(const int32_t layerIndex);

    /// Get the layer group count
    size_t numLayerGroups() const noexcept;

    /// @brief  Get a specific layer group by index
    /// @param layerIndex index
    /// @return the layer group if existent, otherwise a shared null pointer
    const LayerGroupBasePtr& getLayerGroup(const int32_t layerIndex) const;

    /// Execute the given function for each contained layer group
    template <typename Func /* void(LayerGroupBase&) */>
    void visitLayerGroups(Func f) {
        for (auto& pair : layerGroupsByLayerIndex) {
            if (pair.second) {
                f(*pair.second);
            }
        }
    }

    /// Execute the given function for each contained layer group in reversed order
    template <typename Func /* void(LayerGroupBase&) */>
    void visitLayerGroupsReversed(Func f) {
        for (auto& rit : std::ranges::reverse_view(layerGroupsByLayerIndex)) {
            if (rit.second) {
                f(*rit.second);
            }
        }
    }

    /// Upload the layer groups
    void upload(gfx::UploadPass& uploadPass);

    /// Render the layer groups
    void render(RenderOrchestrator&, const RenderTree&, PaintParameters&);

protected:
    void renderDrapedLayerGroups(RenderOrchestrator&, PaintParameters&);

    /// What the draped layers would currently render into this target: how well
    /// they cover it, and a signature of the exact content
    struct DrapeCoverage {
        int32_t totalGroups = -1;      // draped layer groups in the style
        int32_t groupsWithContent = 0; // groups with at least one usable tile
        int64_t zoomDeficit = 0;       // sum of zoom levels lost to ancestor fallbacks

        /// Hash of the drawables overlapping this target, by unique drawable id, so
        /// a tile loading, unloading, or being rebuilt from new bucket data all
        /// change it (a tile id alone would not catch a rebuild).
        std::size_t contentHash = 0;
        /// Integer tile-zoom (draped UBOs carry zoom-derived values like line ratio);
        /// stored quantized so a pinch within one zoom level does not invalidate the
        /// cache. See computeDrapeCoverage.
        double zoom = -1;
        /// Evaluated-property generation. Retained for reference but intentionally NOT
        /// part of sameContentAs: the drape cache must not re-render on paint changes
        /// (matches maplibre-gl-js). See computeDrapeCoverage.
        uint64_t propertiesEpoch = 0;

        /// Whether this would draw exactly what `other` already did
        bool sameContentAs(const DrapeCoverage& other) const {
            return totalGroups == other.totalGroups && contentHash == other.contentHash && zoom == other.zoom;
        }
        /// Whether this would draw less than `other`: fewer layers with content, or
        /// the same layers via coarser ancestor fallbacks
        bool worseThan(const DrapeCoverage& other) const {
            return groupsWithContent < other.groupsWithContent ||
                   (groupsWithContent == other.groupsWithContent && zoomDeficit > other.zoomDeficit);
        }
    };
    DrapeCoverage computeDrapeCoverage(RenderOrchestrator&, const PaintParameters&) const;

    gfx::Context& context;
    std::unique_ptr<gfx::OffscreenTexture> offscreenTexture;
    using LayerGroupMap = std::map<int32_t, LayerGroupBasePtr>;
    LayerGroupMap layerGroupsByLayerIndex;
    Color backgroundColor;
    std::optional<UnwrappedTileID> drapeTileID;
    // (z, x including wrap, y, 1) of the drape tile, as consumed by
    // apply_drape_transform; w = 1 marks an active drape target
    std::array<float, 4> drapeTileValues{{0, 0, 0, 0}};
    gfx::UniformBufferPtr drapeGlobalUniformBuffer;
    // Coverage baked into the target texture by the last actual render. The
    // target keeps its previously rendered content whenever the currently
    // available coverage is strictly worse, so a drape never regresses to
    // fewer layers / coarser fallbacks than it already shows (anti-flicker);
    // see RenderTarget::render.
    DrapeCoverage bakedCoverage;
    // Opt-in "render once" for immutable targets (hillshade prepare, whose DEM input is
    // baked into the prepare drawable once). When set, the target renders on its first
    // frame and is skipped after (the offscreen texture persists). NOT set for the terrain
    // depth target, which must re-render whenever the camera moves. Enabled via
    // setRenderOnce() by RenderHillshadeLayer when it creates a prepare target.
    bool renderOnce = false;
    // Whether a render-once target has already produced its texture.
    bool renderedOnce = false;

public:
    void setRenderOnce(bool value) { renderOnce = value; }

protected:
    // This target's own content signature (PaintParameters::perTargetDrapeSignature)
    // as of the last frame it evaluated its coverage: a signature of just the
    // drawables overlapping this target, plus zoom and the property epoch. While it
    // is unchanged, nothing this target draws has changed, so its baked texture is
    // still correct and the O(draped drawables) coverage scan is skipped. Being
    // per-target (not global), an unrelated tile loading elsewhere no longer forces
    // this target to re-scan. Unset until the target has been evaluated once.
    std::optional<std::size_t> bakedSignature;
};

} // namespace mbgl
