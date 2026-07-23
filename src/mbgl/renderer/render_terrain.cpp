#include <mbgl/renderer/render_terrain.hpp>
#include <mbgl/renderer/update_parameters.hpp>
#include <mbgl/renderer/render_source.hpp>

// TEMP: LOD zoom-distribution diagnostic (Phase 0). Remove after.
#if defined(__ANDROID__)
#include <android/log.h>
#include <array>
#include <string>
#endif
#include <mbgl/renderer/render_tile.hpp>

#if defined(__ANDROID__)
#include <android/log.h>            // TEMP: LODTILES terrain tile-count diagnostic
#include <sys/system_properties.h> // TEMP: terrain_opaque / mesh_size perf toggles
#include <cstdlib>
#endif
#include <mbgl/renderer/render_pass.hpp>
#include <mbgl/renderer/render_tree.hpp>
#include <mbgl/renderer/render_static_data.hpp>
#include <mbgl/renderer/render_orchestrator.hpp>
#include <mbgl/renderer/render_target.hpp>
#include <mbgl/renderer/dem_elevation_provider.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/change_request.hpp>
#include <mbgl/renderer/layer_group.hpp>
#include <mbgl/renderer/layers/terrain_layer_tweaker.hpp>
#include <mbgl/renderer/buckets/hillshade_bucket.hpp>
#include <mbgl/geometry/dem_data.hpp>
#include <mbgl/util/tile_cover.hpp>
#include <mbgl/tile/raster_dem_tile.hpp>
#include <mbgl/tile/tile.hpp>
#if MLN_RENDER_BACKEND_OPENGL
#include <mbgl/gl/context.hpp>
#include <mbgl/gl/texture_2d_array.hpp>
#include <mbgl/gl/drawable_gl.hpp> // gl::DrawableGL::setArrayTexture for the instanced depth DEM
#endif
#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/renderable.hpp>
#include <mbgl/gfx/renderer_backend.hpp>
#include <mbgl/gfx/drawable.hpp>
#include <mbgl/gfx/drawable_impl.hpp>
#include <mbgl/gfx/drawable_builder.hpp>
#include <mbgl/gfx/shader_registry.hpp>
#include <mbgl/gfx/color_mode.hpp>
#include <mbgl/gfx/texture2d.hpp>
#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/shaders/terrain_layer_ubo.hpp>
#include <mbgl/shaders/shader_defines.hpp>
#include <mbgl/shaders/segment.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/util/geo.hpp>
#include <mbgl/math/angles.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/image.hpp>
#include <mbgl/util/mat4.hpp>
#include <mbgl/util/convert.hpp>         // util::cast for the instanced depth UBO matrix
#include <mbgl/util/hash.hpp>            // util::hash_combine for the depth-instance set signature
#include <mbgl/gfx/vertex_attribute.hpp> // VertexAttributeArray for the a_instance attribute

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <unordered_set>

namespace mbgl {

namespace {

// TEMP perf test: terrain mesh grid density per tile from `debug.mln.mesh_size` (16/32/64/128,
// default 128). The mesh is 128x128 by default -> ~1.4M triangles/frame at pitch (color + depth
// passes x ~20 tiles); lowering it cuts vertex/tiler load. Read once; the mesh is cached.
std::size_t terrainMeshGridSize() {
    static const std::size_t grid = [] {
        std::size_t g = 128;
#if defined(__ANDROID__)
        char v[PROP_VALUE_MAX] = {0};
        if (__system_property_get("debug.mln.mesh_size", v) > 0) {
            const int x = std::atoi(v);
            if (x == 16 || x == 32 || x == 64 || x == 128) g = static_cast<std::size_t>(x);
        }
        __android_log_print(ANDROID_LOG_ERROR, "MESHSIZE", "terrain grid = %zu", g);
#endif
        return g;
    }();
    return grid;
}

// TEMP perf test: the depth pass only needs the terrain silhouette for symbol occlusion, not
// the color surface's detail, so it can use a much coarser mesh - halving the per-frame terrain
// geometry (the depth pass is a full extra mesh render). `debug.mln.depth_mesh_size` (default 32).
std::size_t terrainDepthMeshGridSize() {
    static const std::size_t grid = [] {
        std::size_t g = 128; // default: full res - coarser breaks symbol occlusion
#if defined(__ANDROID__)
        char v[PROP_VALUE_MAX] = {0};
        if (__system_property_get("debug.mln.depth_mesh_size", v) > 0) {
            const int x = std::atoi(v);
            if (x == 8 || x == 16 || x == 32 || x == 64 || x == 128) g = static_cast<std::size_t>(x);
        }
        __android_log_print(ANDROID_LOG_ERROR, "MESHSIZE", "depth grid = %zu", g);
#endif
        return g;
    }();
    return grid;
}

// TEMP diagnostic: skip the terrain mesh skirts (edge curtains) via `debug.mln.skip_skirts 1`
// to isolate their overdraw contribution (leaves cracks between tiles; measurement only).
bool terrainSkipSkirts() {
    static const bool skip = [] {
#if defined(__ANDROID__)
        char v[PROP_VALUE_MAX] = {0};
        const bool s = __system_property_get("debug.mln.skip_skirts", v) > 0 && v[0] == '1';
        __android_log_print(ANDROID_LOG_ERROR, "SKIRTS", "skirts = %s", s ? "OFF" : "ON");
        return s;
#else
        return false;
#endif
    }();
    return skip;
}

// Scale and x/y offset mapping a child tile's local space into the (possibly
// ancestor) DEM tile that covers it: a child dz levels deeper occupies the
// 1/scale sub-square at (dx, dy) of the ancestor.
struct DEMSubTileOffset {
    float scale;
    float dx;
    float dy;
};

DEMSubTileOffset demSubTileOffset(const CanonicalTileID& child, const CanonicalTileID& ancestor) {
    const int dz = child.z - ancestor.z;
    return {static_cast<float>(1u << dz),
            static_cast<float>(child.x - (ancestor.x << dz)),
            static_cast<float>(child.y - (ancestor.y << dz))};
}

} // namespace

RenderTerrain::RenderTerrain(Immutable<style::Terrain::Impl> impl_)
    : impl(std::move(impl_)) {}

RenderTerrain::~RenderTerrain() = default;

std::set<UnwrappedTileID> RenderTerrain::expandToDeepestCover(const std::set<UnwrappedTileID>& tileIDs) {
    std::set<UnwrappedTileID> out;
    const std::function<void(const UnwrappedTileID&)> insert = [&](const UnwrappedTileID& id) {
        bool hasDeeper = false;
        for (const auto& other : tileIDs) {
            if (other.isChildOf(id)) {
                hasDeeper = true;
                break;
            }
        }
        if (!hasDeeper) {
            out.insert(id);
            return;
        }
        // A deeper tile overlaps this one: replace it by its children (the
        // children that are themselves in the set are handled at top level)
        for (const auto& childCanonical : id.canonical.children()) {
            const UnwrappedTileID child(id.wrap, childCanonical);
            if (!tileIDs.contains(child)) {
                insert(child);
            }
        }
    };
    for (const auto& id : tileIDs) {
        insert(id);
    }
    return out;
}

void RenderTerrain::update(RenderOrchestrator& orchestrator,
                           gfx::ShaderRegistry& shaders,
                           gfx::Context& context,
                           const TexturePool& texturePool,
                           const TransformState& state,
                           const std::shared_ptr<UpdateParameters>& /*updateParameters*/,
                           const RenderTree& /*renderTree*/,
                           UniqueChangeRequestVec& changes) {
    // Find the DEM source if we haven't already
    if (!demSource && !impl->sourceID.empty()) {
        demSource = orchestrator.getRenderSource(impl->sourceID);
        if (!demSource) {
            Log::Warning(Event::Render, "Terrain could not find DEM source: " + impl->sourceID);
        }
    }

    // Create layer group if we don't have one (including after rebuild)
    if (!layerGroup) {
        if (auto layerGroup_ = context.createLayerGroup(TERRAIN_LAYER_INDEX, /*initialCapacity=*/1, "terrain", false)) {
            layerGroup = std::move(layerGroup_);
            activateLayerGroup(true, changes);
        } else {
            Log::Error(Event::Render, "Failed to create terrain layer group");
            return;
        }
    }

    // Depth-pass twin of the terrain layer group; not activated in the
    // orchestrator, rendered only by renderDepth into the depth target
    if (!depthLayerGroup) {
        depthLayerGroup = context.createLayerGroup(TERRAIN_LAYER_INDEX, /*initialCapacity=*/1, "terrain-depth", false);
    }

    // Create tweaker if we don't have one
    if (!tweaker) {
        tweaker = std::make_unique<TerrainLayerTweaker>(this);
    }

    // If we don't have a DEM source, we can't create terrain drawables
    if (!demSource) {
        return;
    }

    // Get tiles from the DEM source
    auto renderTiles = demSource->getRawRenderTiles();
    if (renderTiles->empty()) {
        return;
    }

    // Cast to LayerGroup for addDrawable
    auto* lg = static_cast<LayerGroup*>(layerGroup.get());
    if (!lg) {
        return;
    }

    // Decode and cache the DEM textures of loaded DEM tiles
    ++demUpdateCounter;
    for (const auto& renderTile : *renderTiles) {
        const auto& tile = renderTile.getTile();
        if (tile.kind != Tile::Kind::RasterDEM) {
            continue;
        }
        auto* demTile = const_cast<RasterDEMTile*>(static_cast<const RasterDEMTile*>(&tile));
        auto* hillshadeBucket = demTile->getBucket();
        const auto* demData = hillshadeBucket ? &hillshadeBucket->getDEMData() : nullptr;
        if (demData && demData->getImagePtr() && !demData->getImagePtr()->size.isEmpty()) {
            // All tiles come from the same raster-dem source, so they share one
            // encoding and DEM dimension
            demUnpackVector = demData->getUnpackVector();
            demDim = demData->dim;
            if (auto existing = demTextures.find(renderTile.id); existing != demTextures.end()) {
                existing->second.lastUsed = demUpdateCounter;
            } else if (auto texture = createDEMTexture(context, *demData)) {
                // Keep the texture available for elevation sampling by non-draped layers
                demTextures[renderTile.id] = {texture, demData->dim, demUpdateCounter};
#if MLN_RENDER_BACKEND_OPENGL
                // Also pack this tile's DEM into the array for the (upcoming) instanced
                // depth pass. Additive: the per-tile texture above is still the fallback.
                packDEMArrayLayer(context, renderTile.id, *demData);
#endif
            }
        }
    }

    // The mesh tile set: parent fallback render tiles expanded to the ideal
    // cover so terrain meshes never overlap (a parent mesh would draw its
    // lower-resolution drape over the sharp meshes of its loaded children);
    // synthetic tiles sample ancestor DEM/drape textures instead.
    std::set<UnwrappedTileID> renderTileIDs;
    for (const auto& renderTile : *renderTiles) {
        renderTileIDs.insert(renderTile.id);
    }
    std::set<UnwrappedTileID> meshTiles = expandToDeepestCover(renderTileIDs);

#if defined(__ANDROID__)
    // TEMP Phase 0: zoom histograms of the DEM source cover (renderTileIDs) vs the mesh
    // set after expandToDeepestCover. If the source cover already has a spread of zooms
    // (LOD) but the mesh set is flattened to one deep zoom, expandToDeepestCover is the
    // anti-LOD; if the source cover is itself single-zoom, LOD must be added at cover time.
    {
        static uint32_t lodThrottle = 0;
        if (++lodThrottle % 30 == 1) {
            const auto histo = [](const std::set<UnwrappedTileID>& tiles) {
                std::array<int, 30> byZoom{};
                for (const auto& t : tiles) {
                    if (t.canonical.z < 30) byZoom[t.canonical.z]++;
                }
                std::string s;
                for (int z = 0; z < 30; ++z) {
                    if (byZoom[z]) s += "z" + std::to_string(z) + ":" + std::to_string(byZoom[z]) + " ";
                }
                return s;
            };
            __android_log_print(ANDROID_LOG_ERROR,
                                "DRAPE",
                                "LOD source[%zu]{%s} mesh[%zu]{%s}",
                                renderTileIDs.size(),
                                histo(renderTileIDs).c_str(),
                                meshTiles.size(),
                                histo(meshTiles).c_str());
        }
    }
#endif

    // Cap the mesh tile count: keep those nearest the map center, drop the farthest
    // (the horizon tiles a high tilt pulls in). Everything downstream - drape
    // targets, re-renders, depth draws - scales with this count.
    if (MAX_MESH_TILES > 0 && meshTiles.size() > MAX_MESH_TILES) {
        // Map center in normalized web-mercator [0,1] (standard projection)
        const LatLng center = state.getLatLng();
        const double cx = center.longitude() / 360.0 + 0.5;
        const double latRad = util::deg2rad(center.latitude());
        const double cy = 0.5 - std::log(std::tan(M_PI / 4.0 + latRad / 2.0)) / (2.0 * M_PI);

        const auto tileDist2 = [&](const UnwrappedTileID& id) {
            const double scale = static_cast<double>(1u << id.canonical.z);
            const double tx = (static_cast<double>(id.canonical.x) + 0.5) / scale + id.wrap;
            const double ty = (static_cast<double>(id.canonical.y) + 0.5) / scale;
            const double dx = tx - cx;
            const double dy = ty - cy;
            return dx * dx + dy * dy;
        };

        std::vector<UnwrappedTileID> sorted(meshTiles.begin(), meshTiles.end());
        std::partial_sort(sorted.begin(),
                          sorted.begin() + static_cast<std::ptrdiff_t>(MAX_MESH_TILES),
                          sorted.end(),
                          [&](const UnwrappedTileID& a, const UnwrappedTileID& b) {
                              return tileDist2(a) < tileDist2(b);
                          });
        meshTiles = std::set<UnwrappedTileID>(sorted.begin(),
                                              sorted.begin() + static_cast<std::ptrdiff_t>(MAX_MESH_TILES));
    }

    // Drop drawables and cached DEM textures for tiles that left the mesh tile
    // set, keeping everything else intact between frames
    std::unordered_set<OverscaledTileID> currentTiles;
    for (const auto& id : meshTiles) {
        currentTiles.emplace(id.canonical.z, id.wrap, id.canonical);
    }
    lg->removeDrawablesIf(
        [&](gfx::Drawable& drawable) { return drawable.getTileID() && !currentTiles.contains(*drawable.getTileID()); });
    auto* depthLg = static_cast<LayerGroup*>(depthLayerGroup.get());
    if (depthLg) {
        if (depthLg->removeDrawablesIf([&](gfx::Drawable& drawable) {
                return drawable.getTileID() && !currentTiles.contains(*drawable.getTileID());
            }) > 0) {
            depthDirty = true;
        }
    }
    for (auto it = tilesWithDrawables.begin(); it != tilesWithDrawables.end();) {
        if (!currentTiles.contains(it->first)) {
            drawableDemCoords.erase(it->first);
            it = tilesWithDrawables.erase(it);
        } else {
            ++it;
        }
    }
    // Retain cached DEM textures that are related to the current tile set so they
    // can serve as ancestor fallbacks while exact tiles load (as maplibre-gl-js
    // retains terrain tiles in its source cache); drop unrelated ones
    for (auto it = demTextures.begin(); it != demTextures.end();) {
        bool related = false;
        for (const auto& current : currentTiles) {
            const UnwrappedTileID unwrapped = current.toUnwrapped();
            if (unwrapped == it->first || unwrapped.isChildOf(it->first) || it->first.isChildOf(unwrapped)) {
                related = true;
                break;
            }
        }
        if (related) {
            it = std::next(it);
        } else {
#if MLN_RENDER_BACKEND_OPENGL
            freeDEMArrayLayer(it->first);
#endif
            it = demTextures.erase(it);
        }
    }
    // Cap the cache: ancestor/descendant relations accumulate while browsing
    // (zooming makes whole chains "related"), which previously grew past 2GB
    // of DEM textures and overflowed/OOMed. Evict least-recently-used entries
    // that were not used this frame until the cache is back under budget.
    if (demTextures.size() > maxDEMTextures) {
        std::vector<std::pair<uint64_t, UnwrappedTileID>> evictable;
        for (const auto& [id, entry] : demTextures) {
            if (entry.lastUsed != demUpdateCounter) {
                evictable.emplace_back(entry.lastUsed, id);
            }
        }
        std::sort(evictable.begin(), evictable.end());
        for (const auto& [lastUsed, id] : evictable) {
            if (demTextures.size() <= maxDEMTextures) {
                break;
            }
#if MLN_RENDER_BACKEND_OPENGL
            freeDEMArrayLayer(id);
#endif
            demTextures.erase(id);
        }
    }

    // Create terrain drawables for each mesh tile
    for (const auto& unwrapped : meshTiles) {
        const OverscaledTileID tileID(unwrapped.canonical.z, unwrapped.wrap, unwrapped.canonical);

        // Skip if the tile already has a drawable bound to its own DEM
        if (const auto existing = tilesWithDrawables.find(tileID);
            existing != tilesWithDrawables.end() && existing->second == 2) {
            continue;
        }

        // Resolve the DEM texture: the tile's own decoded DEM if available,
        // otherwise the closest cached ancestor as a fallback so the terrain
        // mesh stays up while the tile loads (as maplibre-gl-js does),
        // otherwise the flat placeholder
        std::shared_ptr<gfx::Texture2D> demTexture;
        // {scale, x offset, y offset, DEM dim}: maps tile-local coords (0..EXTENT)
        // into the bound DEM tile's normalized space, matching getTerrainData so
        // the terrain mesh and the elevated layers sample identically. The DEM
        // dimension rides in .w for the shader's get_elevation() call.
        std::array<float, 4> demCoords{{1.0f / util::EXTENT, 0.0f, 0.0f, static_cast<float>(demDim)}};
        uint8_t demTier = 0;
        const UnwrappedTileID* demTileUsed = nullptr; // DEM tile whose texture / array-layer this tile uses

        if (auto cached = demTextures.find(unwrapped); cached != demTextures.end()) {
            cached->second.lastUsed = demUpdateCounter;
            demTexture = cached->second.texture;
            demTier = 2;
            demTileUsed = &unwrapped;
        } else {
            // Fall back to the closest cached ancestor DEM
            const UnwrappedTileID* ancestorID = nullptr;
            DEMTextureEntry* ancestorEntry = nullptr;
            int bestZoom = -1;
            for (auto& [candidate, entry] : demTextures) {
                if (candidate != unwrapped && unwrapped.isChildOf(candidate) &&
                    static_cast<int>(candidate.canonical.z) > bestZoom) {
                    bestZoom = candidate.canonical.z;
                    ancestorID = &candidate;
                    ancestorEntry = &entry;
                    demTexture = entry.texture;
                }
            }
            if (!demTexture) {
                // No DEM at all yet: render the mesh flat with the placeholder
                // DEM so the draped map still shows (a briefly flat area is
                // less jarring than a hole in the terrain)
                demTexture = getPlaceholderDEMTexture(context);
                if (!demTexture) {
                    continue;
                }
            } else {
                ancestorEntry->lastUsed = demUpdateCounter;
                demTier = 1;
                demTileUsed = ancestorID;
                const auto off = demSubTileOffset(unwrapped.canonical, ancestorID->canonical);
                demCoords = {{1.0f / (util::EXTENT * off.scale),
                              off.dx / off.scale,
                              off.dy / off.scale,
                              static_cast<float>(demDim)}};
            }
        }

        // If a drawable already exists for this tile, keep it until a higher
        // DEM quality tier becomes available, then replace it
        if (const auto existing = tilesWithDrawables.find(tileID); existing != tilesWithDrawables.end()) {
            if (existing->second >= demTier) {
                continue;
            }
            lg->removeDrawablesIf(
                [&](gfx::Drawable& drawable) { return drawable.getTileID() && *drawable.getTileID() == tileID; });
            if (depthLg) {
                depthLg->removeDrawablesIf(
                    [&](gfx::Drawable& drawable) { return drawable.getTileID() && *drawable.getTileID() == tileID; });
                depthDirty = true;
            }
            tilesWithDrawables.erase(existing);
        }
        drawableDemCoords[tileID] = demCoords;
#if MLN_RENDER_BACKEND_OPENGL
        {
            float layer = -1.0f;
            if (demTileUsed) {
                if (const auto la = demArrayLayer.find(*demTileUsed); la != demArrayLayer.end()) {
                    layer = static_cast<float>(la->second);
                }
            }
            drawableDemLayer[tileID] = layer;
        }
#endif

        // Create terrain drawable for this tile
        const auto renderTarget = texturePool.getRenderTarget(unwrapped);
        if (!renderTarget) {
            continue;
        }
        auto drawable = createDrawableForTile(context, shaders, tileID, demTexture, renderTarget->getTexture());
        if (drawable) {
            lg->addDrawable(std::move(drawable));
            tilesWithDrawables[tileID] = demTier;
#if !MLN_RENDER_BACKEND_OPENGL
            // Non-GL backends: one depth drawable per tile (no instancing path there).
            if (depthLg) {
                if (auto depthDrawable = createDrawableForTile(
                        context, shaders, tileID, demTexture, nullptr, /*depthPass=*/true)) {
                    depthLg->addDrawable(std::move(depthDrawable));
                    depthDirty = true;
                }
            }
#endif
        }
    }

#if MLN_RENDER_BACKEND_OPENGL
    // GL: collect the per-instance (tile, dem_coords, dem_layer) list for the whole mesh tile
    // set and rebuild the single instanced depth drawable only when that set changes (its
    // transforms refresh every frame in updateInstancedDepthUBO). Tiles without a packed DEM
    // array layer (-1) are skipped - they briefly miss depth occlusion, the same tolerance the
    // old ancestor/placeholder fallback had.
    {
        std::vector<DepthInstance> instances;
        instances.reserve(meshTiles.size());
        std::size_t sig = 0;
        for (const auto& unwrapped : meshTiles) {
            const OverscaledTileID tileID(unwrapped.canonical.z, unwrapped.wrap, unwrapped.canonical);
            const auto lc = drawableDemLayer.find(tileID);
            if (lc == drawableDemLayer.end() || lc->second < 0.0f || instances.size() >= maxDepthInstances) {
                continue;
            }
            const auto cc = drawableDemCoords.find(tileID);
            const std::array<float, 4> coords =
                cc != drawableDemCoords.end()
                    ? cc->second
                    : std::array<float, 4>{{1.0f / util::EXTENT, 0.0f, 0.0f, static_cast<float>(demDim)}};
            instances.push_back({tileID, coords, lc->second});
            util::hash_combine(sig, std::hash<OverscaledTileID>{}(tileID));
            util::hash_combine(sig, static_cast<int>(lc->second));
        }
        depthInstances = std::move(instances);
        if (sig != depthInstanceSignature) {
            depthInstanceSignature = sig;
            rebuildInstancedDepthDrawable(context, shaders);
            depthDirty = true;
        }
    }
#endif
}

float RenderTerrain::getElevation(const UnwrappedTileID& tileID, float x, float y) const {
    if (!demSource) {
        return 0.0f;
    }

    // Find the DEM tile matching the requested tile, or its closest available ancestor
    const auto renderTiles = demSource->getRawRenderTiles();
    const RenderTile* demRenderTile = nullptr;
    int bestZoom = -1;
    for (const auto& renderTile : *renderTiles) {
        const UnwrappedTileID& candidate = renderTile.id;
        if ((candidate == tileID || tileID.isChildOf(candidate)) &&
            static_cast<int>(candidate.canonical.z) > bestZoom) {
            bestZoom = candidate.canonical.z;
            demRenderTile = &renderTile;
        }
    }
    if (!demRenderTile) {
        return 0.0f;
    }

    const auto& tile = demRenderTile->getTile();
    if (tile.kind != Tile::Kind::RasterDEM) {
        return 0.0f;
    }
    auto* demTile = const_cast<RasterDEMTile*>(static_cast<const RasterDEMTile*>(&tile));
    auto* bucket = demTile->getBucket();
    if (!bucket) {
        return 0.0f;
    }
    const auto& demData = bucket->getDEMData();
    if (!demData.getImagePtr() || demData.dim <= 0) {
        return 0.0f;
    }

    // Map the tile-local coordinate into the (possibly ancestor) DEM tile
    const UnwrappedTileID& demTileID = demRenderTile->id;
    const auto off = demSubTileOffset(tileID.canonical, demTileID.canonical);
    const float xInDem = (off.dx * util::EXTENT + x) / off.scale;
    const float yInDem = (off.dy * util::EXTENT + y) / off.scale;

    // Bilinear interpolation of the DEM texels, as in maplibre-gl-js Terrain.getDEMElevation
    const float dim = static_cast<float>(demData.dim);
    const float px = util::clamp(xInDem / util::EXTENT * dim, 0.0f, dim - 1.0f);
    const float py = util::clamp(yInDem / util::EXTENT * dim, 0.0f, dim - 1.0f);
    const auto x0 = static_cast<int32_t>(std::floor(px));
    const auto y0 = static_cast<int32_t>(std::floor(py));
    const float fx = px - static_cast<float>(x0);
    const float fy = py - static_cast<float>(y0);
    const float tl = static_cast<float>(demData.get(x0, y0));
    const float tr = static_cast<float>(demData.get(x0 + 1, y0));
    const float bl = static_cast<float>(demData.get(x0, y0 + 1));
    const float br = static_cast<float>(demData.get(x0 + 1, y0 + 1));
    const float top = tl + (tr - tl) * fx;
    const float bottom = bl + (br - bl) * fx;
    return top + (bottom - top) * fy;
}

float RenderTerrain::getElevationWithExaggeration(const UnwrappedTileID& tileID, float x, float y) const {
    return getElevation(tileID, x, y) * getExaggeration();
}

std::optional<RenderTerrain::TerrainData> RenderTerrain::getTerrainData(const UnwrappedTileID& tileID) const {
    // Find the DEM texture matching the requested tile, or its closest available ancestor
    const UnwrappedTileID* demTileID = nullptr;
    const DEMTextureEntry* entry = nullptr;
    int bestZoom = -1;
    for (const auto& [candidate, candidateEntry] : demTextures) {
        if ((candidate == tileID || tileID.isChildOf(candidate)) &&
            static_cast<int>(candidate.canonical.z) > bestZoom) {
            bestZoom = candidate.canonical.z;
            demTileID = &candidate;
            entry = &candidateEntry;
        }
    }
    if (!entry || !entry->texture) {
        return std::nullopt;
    }

    // Map tile-local coordinates (0..EXTENT) of the requested tile into
    // normalized coordinates (0..1) of the (possibly ancestor) DEM tile
    const auto off = demSubTileOffset(tileID.canonical, demTileID->canonical);

    return TerrainData{
        .demTexture = entry->texture,
        .demCoords = {{1.0f / (util::EXTENT * off.scale), off.dx / off.scale, off.dy / off.scale, 0.0f}},
        .demDim = static_cast<float>(entry->dim),
    };
}

const std::shared_ptr<gfx::Texture2D>& RenderTerrain::getPlaceholderDEMTexture(gfx::Context& context) {
    if (!placeholderDEMTexture) {
        auto image = std::make_shared<PremultipliedImage>(Size{1, 1});
        std::memset(image->data.get(), 0, image->bytes());
        placeholderDEMTexture = context.createTexture2D();
        placeholderDEMTexture->setImage(image);
        placeholderDEMTexture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Nearest,
                                                        .wrapU = gfx::TextureWrapType::Clamp,
                                                        .wrapV = gfx::TextureWrapType::Clamp});
    }
    return placeholderDEMTexture;
}

void RenderTerrain::renderDepth(RenderOrchestrator& orchestrator,
                                const RenderTree& renderTree,
                                PaintParameters& parameters) {
    if (!depthLayerGroup || depthLayerGroup->empty()) {
        return;
    }
    prepareDepthTarget(parameters);
    if (!depthRenderTarget) {
        return;
    }
#if MLN_RENDER_BACKEND_OPENGL
    // Instanced depth pass: refresh the per-instance UBO array (camera-dependent transforms)
    // and bind the packed DEM array to the unit the shader's u_dem_array sampler expects. The
    // instanced drawable carries no gfx textures, so nothing else touches this unit during the
    // depth render. (Bind integration is the main on-device shakeout item - Texture2DArray is
    // GL-only and outside the gfx texture abstraction.)
    updateInstancedDepthUBO(parameters);
    // The DEM array is bound by the instanced drawable itself (DrawableGL::setArrayTexture ->
    // bindTextures), so no manual bind here.
#endif
    depthRenderTarget->render(orchestrator, renderTree, parameters);
}

void RenderTerrain::prepareDepthTarget(PaintParameters& parameters) {
    // Called at the start of the render (before the upload phase) as well as from
    // renderDepth, so the depth target already exists when the symbol tweaker binds
    // getDepthTexture() for this frame. Creating it lazily in renderDepth alone left
    // the symbols bound to the far-plane placeholder for that frame - permanently so
    // in single-frame renders like the render tests, where terrain occlusion then
    // never engaged.
    const Size size = parameters.backend.getDefaultRenderable().getSize();
    if (size.isEmpty()) {
        // Early frames can run before the surface has a real size; a degenerate
        // render target here would hand the symbol tweaker a broken texture
        return;
    }
    if (!depthRenderTarget || !depthRenderTarget->getTexture() || depthRenderTarget->getTexture()->getSize() != size) {
        depthRenderTarget = parameters.context.createRenderTarget(
            size, gfx::TextureChannelDataType::UnsignedByte, /*stencil=*/false);
        if (!depthRenderTarget) {
            return;
        }
        // Far plane everywhere the terrain does not cover (unpack_depth(1,1,1,1) ~ 1.0)
        depthRenderTarget->setClearColor(Color::white());
    }
    // (Re)attach the depth layer group; it may not have existed yet when the
    // target was first created on an early frame
    if (depthLayerGroup) {
        depthRenderTarget->addLayerGroup(depthLayerGroup, /*replace=*/true);
    }
    // prepareDepthTarget only creates/attaches the target; the actual depth render happens in
    // renderDepth (every frame, matching upstream, to keep depth in lockstep with the terrain).
}

const std::shared_ptr<gfx::Texture2D>& RenderTerrain::getDepthTexture(gfx::Context& context) {
    if (depthRenderTarget && depthRenderTarget->getTexture()) {
        return depthRenderTarget->getTexture();
    }
    if (!placeholderDepthTexture) {
        // Far-plane packed depth: symbols compare as visible until the pass runs
        auto image = std::make_shared<PremultipliedImage>(Size{1, 1});
        std::memset(image->data.get(), 0xFF, image->bytes());
        placeholderDepthTexture = context.createTexture2D();
        placeholderDepthTexture->setImage(image);
        placeholderDepthTexture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Nearest,
                                                          .wrapU = gfx::TextureWrapType::Clamp,
                                                          .wrapV = gfx::TextureWrapType::Clamp});
    }
    return placeholderDepthTexture;
}

float RenderTerrain::getExaggeration() const {
    return impl->exaggeration;
}

const std::string& RenderTerrain::getSourceID() const {
    return impl->sourceID;
}

bool RenderTerrain::isEnabled() const {
    return !impl->sourceID.empty();
}

const RenderTerrain::TerrainMesh& RenderTerrain::getMesh(gfx::Context& context) {
    if (!mesh) {
        mesh = buildMesh(context, terrainMeshGridSize());
    }
    return *mesh;
}

const RenderTerrain::TerrainMesh& RenderTerrain::getDepthMesh(gfx::Context& context) {
    if (!depthMesh) {
        depthMesh = buildMesh(context, terrainDepthMeshGridSize());
    }
    return *depthMesh;
}

RenderTerrain::TerrainMesh RenderTerrain::buildMesh(gfx::Context& /*context*/, std::size_t gridSizeArg) {
    // A regular grid mesh (reused for every tile, displaced by the DEM in the
    // vertex shader) plus a skirt: each tile edge is duplicated into a curtain
    // that the shader drops by u_ele_delta, hiding the cracks between neighbouring
    // tiles at different zoom levels. Ported from maplibre-gl-js Terrain
    // getTerrainMesh()/_buildSkirts().
    const size_t gridSize = gridSizeArg; // TEMP: was MESH_SIZE (env-toggled per mesh)
    const bool skipSkirts = terrainSkipSkirts(); // TEMP: skirt-overdraw isolation
    const size_t vps = gridSize + 1; // vertices per side
    const float step = static_cast<float>(util::EXTENT) / static_cast<float>(gridSize);

    std::vector<int16_t> vertices;
    std::vector<uint16_t> indices;

    // Each vertex is 4 shorts: x, y, skirt flag (0 = surface, 1 = skirt),
    // unused. uv is derived from x,y in the shader, so the 3rd/4th shorts are free
    // to carry the skirt flag (the native analog of gl-js Pos3d.z).
    const auto addVert = [&](float x, float y, int16_t skirt) {
        vertices.push_back(static_cast<int16_t>(x));
        vertices.push_back(static_cast<int16_t>(y));
        vertices.push_back(skirt);
        vertices.push_back(0);
    };

    // Surface grid
    for (size_t y = 0; y < vps; ++y) {
        for (size_t x = 0; x < vps; ++x) {
            addVert(x * step, y * step, 0);
        }
    }
    for (size_t y = 0; y < gridSize; ++y) {
        for (size_t x = 0; x < gridSize; ++x) {
            const uint16_t topLeft = static_cast<uint16_t>(y * vps + x);
            const uint16_t topRight = static_cast<uint16_t>(topLeft + 1);
            const uint16_t bottomLeft = static_cast<uint16_t>((y + 1) * vps + x);
            const uint16_t bottomRight = static_cast<uint16_t>(bottomLeft + 1);
            indices.push_back(topLeft);
            indices.push_back(bottomLeft);
            indices.push_back(topRight);
            indices.push_back(topRight);
            indices.push_back(bottomLeft);
            indices.push_back(bottomRight);
        }
    }

    // Top/bottom skirt rows (reference the grid's top/bottom edge rows)
    const auto extent = static_cast<float>(util::EXTENT);
    const uint16_t offsetTop = static_cast<uint16_t>(vertices.size() / 4);
    const uint16_t offsetTopEdge = 0;
    const uint16_t offsetBottom = static_cast<uint16_t>(offsetTop + vps);
    const uint16_t offsetBottomEdge = static_cast<uint16_t>(vps * gridSize);
    for (size_t x = 0; x < vps; ++x) {
        addVert(x * step, 0.0f, 1);
    }
    for (size_t x = 0; x < vps; ++x) {
        addVert(x * step, extent, 1);
    }
    for (uint16_t x = 0; !skipSkirts && x < gridSize; ++x) {
        indices.insert(indices.end(),
                       {static_cast<uint16_t>(offsetBottomEdge + x),
                        static_cast<uint16_t>(offsetBottom + x),
                        static_cast<uint16_t>(offsetBottom + x + 1),
                        static_cast<uint16_t>(offsetBottomEdge + x),
                        static_cast<uint16_t>(offsetBottom + x + 1),
                        static_cast<uint16_t>(offsetBottomEdge + x + 1),
                        static_cast<uint16_t>(offsetTopEdge + x),
                        static_cast<uint16_t>(offsetTop + x + 1),
                        static_cast<uint16_t>(offsetTop + x),
                        static_cast<uint16_t>(offsetTopEdge + x),
                        static_cast<uint16_t>(offsetTopEdge + x + 1),
                        static_cast<uint16_t>(offsetTop + x + 1)});
    }

    // Left/right skirt frames (self-contained strips of paired surface/skirt verts)
    const uint16_t offsetLeft = static_cast<uint16_t>(vertices.size() / 4);
    const uint16_t offsetRight = static_cast<uint16_t>(offsetLeft + vps * 2);
    for (int edge = 0; edge <= 1; ++edge) {
        for (size_t y = 0; y < vps; ++y) {
            for (int16_t z = 0; z <= 1; ++z) {
                addVert(static_cast<float>(edge) * extent, y * step, z);
            }
        }
    }
    for (uint16_t y = 0; !skipSkirts && y < gridSize * 2; y += 2) {
        indices.insert(indices.end(),
                       {static_cast<uint16_t>(offsetLeft + y),
                        static_cast<uint16_t>(offsetLeft + y + 1),
                        static_cast<uint16_t>(offsetLeft + y + 3),
                        static_cast<uint16_t>(offsetLeft + y),
                        static_cast<uint16_t>(offsetLeft + y + 3),
                        static_cast<uint16_t>(offsetLeft + y + 2),
                        static_cast<uint16_t>(offsetRight + y),
                        static_cast<uint16_t>(offsetRight + y + 3),
                        static_cast<uint16_t>(offsetRight + y + 1),
                        static_cast<uint16_t>(offsetRight + y),
                        static_cast<uint16_t>(offsetRight + y + 2),
                        static_cast<uint16_t>(offsetRight + y + 3)});
    }

    return TerrainMesh{nullptr, // vertexBuffer - created when building the drawable
                       nullptr, // indexBuffer - created when building the drawable
                       vertices.size() / 4,
                       indices.size(),
                       std::move(vertices),
                       std::move(indices)};
}

std::shared_ptr<gfx::Texture2D> RenderTerrain::createDEMTexture(gfx::Context& context, const DEMData& demData) {
    // Get the DEM image data
    const auto& imagePtr = demData.getImagePtr();
    if (!imagePtr || imagePtr->size.isEmpty()) {
        Log::Warning(Event::Render, "DEM data has no image");
        return nullptr;
    }

    // Create a new texture
    auto texture = context.createTexture2D();
    if (!texture) {
        Log::Error(Event::Render, "Failed to create DEM texture");
        return nullptr;
    }

    // Set the image data
    texture->setImage(imagePtr);

    // Nearest filtering: the packed Terrain-RGB/Terrarium DEM cannot be hardware
    // interpolated (blending the encoded bytes does not blend the decoded
    // elevations), so shaders decode each texel and interpolate in meters via
    // get_elevation(). This matches maplibre-gl-js, which binds the DEM NEAREST.
    texture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Nearest,
                                      .wrapU = gfx::TextureWrapType::Clamp,
                                      .wrapV = gfx::TextureWrapType::Clamp});

    return texture;
}

std::unique_ptr<gfx::Drawable> RenderTerrain::createDrawableForTile(gfx::Context& context,
                                                                    gfx::ShaderRegistry& shaders,
                                                                    const OverscaledTileID& tileID,
                                                                    std::shared_ptr<gfx::Texture2D> demTexture,
                                                                    std::shared_ptr<gfx::Texture2D> mapTexture,
                                                                    bool depthPass) {
    // Ensure mesh is generated. The depth pass (symbol-occlusion silhouette) uses a coarser
    // mesh than the color surface - it needs the shape, not the detail - halving terrain geometry.
    const auto& terrainMesh = depthPass ? getDepthMesh(context) : getMesh(context);

    if (terrainMesh.vertices.empty() || terrainMesh.indices.empty()) {
        Log::Error(Event::Render, "Terrain mesh is empty, cannot create drawable");
        return nullptr;
    }

    // Get terrain shader
    auto terrainShader = context.getGenericShader(shaders, depthPass ? "TerrainDepthShader" : "TerrainShader");
    if (!terrainShader) {
        // The depth shader is not registered on all backends yet; symbols
        // then sample the far-plane placeholder and stay visible
        if (!depthPass) {
            Log::Error(Event::Render, "Terrain shader not found");
        }
        return nullptr;
    }

    // Create drawable builder
    auto builder = context.createDrawableBuilder(depthPass ? "terrain-depth-tile" : "terrain-tile");
    if (!builder) {
        Log::Error(Event::Render, "Failed to create drawable builder for terrain tile");
        return nullptr;
    }

    // Configure builder - terrain is 3D, depth-tested, unblended. This matches upstream
    // PR #4389 exactly: the surface goes in the Translucent pass. It must NOT be moved to
    // the Opaque pass - doing so writes depth into the main framebuffer before symbols are
    // drawn, and the labels then get depth-culled against the very surface they sit on and
    // vanish ("labels appear at warmup, then get covered"). Symbol occlusion by terrain is
    // handled separately in the shader via the packed depth texture (calculate_visibility).
    builder->setShader(terrainShader);
    builder->setRenderPass(RenderPass::Translucent);
    if (depthPass) {
        // The depth pass renders packed depth with real depth testing so the
        // nearest surface wins, into the terrain depth target (renderDepth)
        builder->setDepthType(gfx::DepthMaskType::ReadWrite);
        builder->setColorMode(gfx::ColorMode::unblended());
        builder->setEnableDepth(true);
        builder->setIs3D(true);
    } else {
        builder->setDepthType(gfx::DepthMaskType::ReadWrite);
        builder->setColorMode(gfx::ColorMode::unblended());
        builder->setEnableDepth(true);
        builder->setIs3D(true);
    }

    // Set vertex data - copy vertices to raw buffer
    std::vector<uint8_t> vertexData(terrainMesh.vertices.size() * sizeof(int16_t));
    std::memcpy(vertexData.data(), terrainMesh.vertices.data(), vertexData.size());
    builder->setRawVertices(std::move(vertexData), terrainMesh.vertexCount, gfx::AttributeDataType::Short4);

    // Set index data and segments
    // Create a single segment covering the entire terrain mesh
    SegmentVector segments;
    segments.emplace_back(0,                       // vertex offset
                          0,                       // index offset
                          terrainMesh.vertexCount, // vertex count
                          terrainMesh.indexCount); // index count

    std::vector<uint16_t> indexData = terrainMesh.indices;
    builder->setSegments(gfx::Triangles(), std::move(indexData), segments.data(), segments.size());

    // Set the DEM texture
    if (demTexture) {
        builder->setTexture(demTexture, 0); // Texture index 0 for DEM
    }

    // The depth pass samples only the DEM and writes packed depth, so it has no
    // map texture by design; only the draped pass binds the drape render target
    if (!depthPass) {
        if (mapTexture) {
            builder->setTexture(mapTexture, 1); // Texture index 1 for map
        } else {
            Log::Warning(Event::Render, "No drape texture for terrain tile " + util::toString(tileID));
        }
    }

    // Flush to create the drawable
    builder->flush(context);

    // Get the drawable
    auto drawables = builder->clearDrawables();
    if (drawables.empty()) {
        Log::Error(Event::Render, "Failed to create terrain drawable for tile");
        return nullptr;
    }

    // Set tile ID on the drawable
    auto& drawable = drawables[0];
    drawable->setTileID(tileID);

    return std::move(drawable);
}

void RenderTerrain::activateLayerGroup(bool activate, UniqueChangeRequestVec& changes) {
    if (layerGroup) {
        if (activate) {
            changes.emplace_back(std::make_unique<AddLayerGroupRequest>(layerGroup));
        } else {
            changes.emplace_back(std::make_unique<RemoveLayerGroupRequest>(layerGroup));
        }
    }
}

void RenderTerrain::deactivate(UniqueChangeRequestVec& changes) {
    // depthLayerGroup / depthRenderTarget are owned by this RenderTerrain and released with
    // it; only the mesh layerGroup is registered separately with the orchestrator, so that is
    // all we need to unregister here (see RenderOrchestrator::createRenderTree, which calls
    // this before dropping RenderTerrain to avoid an orphaned floating terrain surface).
    activateLayerGroup(false, changes);
}

#if MLN_RENDER_BACKEND_OPENGL
void RenderTerrain::packDEMArrayLayer(gfx::Context& context, const UnwrappedTileID& id, const DEMData& demData) {
    const auto& imagePtr = demData.getImagePtr();
    if (!imagePtr || imagePtr->size.isEmpty()) {
        return;
    }
    if (!demTextureArray) {
        demTextureArray = std::make_unique<gl::Texture2DArray>(static_cast<gl::Context&>(context));
    }
    // All DEM tiles from one source share a size, so this allocates once and no-ops after.
    demTextureArray->allocate(imagePtr->size, maxDEMArrayLayers);
    if (!demTextureArray->valid()) {
        return;
    }

    uint32_t layer = 0;
    if (const auto it = demArrayLayer.find(id); it != demArrayLayer.end()) {
        layer = it->second; // re-upload into the tile's existing slot
    } else if (!demArrayFreeLayers.empty()) {
        layer = demArrayFreeLayers.back();
        demArrayFreeLayers.pop_back();
        demArrayLayer[id] = layer;
    } else if (demArrayNextLayer < maxDEMArrayLayers) {
        layer = demArrayNextLayer++;
        demArrayLayer[id] = layer;
    } else {
        return; // array full - tile keeps its per-tile texture, just not instanced
    }
    demTextureArray->uploadLayer(layer, imagePtr->data.get());
}

void RenderTerrain::freeDEMArrayLayer(const UnwrappedTileID& id) {
    if (const auto it = demArrayLayer.find(id); it != demArrayLayer.end()) {
        demArrayFreeLayers.push_back(it->second);
        demArrayLayer.erase(it);
    }
}

// Build the single instanced depth drawable covering the current depthInstances: the shared
// depth mesh drawn N times, with a_instance = [0..N-1] selecting each tile's slot in the
// TerrainDepthInstanceUBO array (filled per frame in updateInstancedDepthUBO). No tile id is
// set, so the terrain tweaker skips it; no gfx textures, since the DEM array is bound manually
// in renderDepth. Called only when the tile set changes.
void RenderTerrain::rebuildInstancedDepthDrawable(gfx::Context& context, gfx::ShaderRegistry& shaders) {
    auto* depthLg = static_cast<LayerGroup*>(depthLayerGroup.get());
    if (!depthLg) {
        return;
    }
    depthLg->clearDrawables();
    const std::size_t n = depthInstances.size();
    if (n == 0 || !demTextureArray || !demTextureArray->valid()) {
        return;
    }

    const auto& depthMeshRef = getDepthMesh(context);
    if (depthMeshRef.vertices.empty() || depthMeshRef.indices.empty()) {
        return;
    }
    auto shader = context.getGenericShader(shaders, "TerrainDepthShader");
    if (!shader) {
        return;
    }
    auto builder = context.createDrawableBuilder("terrain-depth-instanced");
    if (!builder) {
        return;
    }
    builder->setShader(shader);
    builder->setRenderPass(RenderPass::Translucent);
    builder->setDepthType(gfx::DepthMaskType::ReadWrite);
    builder->setColorMode(gfx::ColorMode::unblended());
    builder->setEnableDepth(true);
    builder->setIs3D(true);

    std::vector<uint8_t> vtx(depthMeshRef.vertices.size() * sizeof(int16_t));
    std::memcpy(vtx.data(), depthMeshRef.vertices.data(), vtx.size());
    builder->setRawVertices(std::move(vtx), depthMeshRef.vertexCount, gfx::AttributeDataType::Short4);

    SegmentVector segs;
    segs.emplace_back(0, 0, depthMeshRef.vertexCount, depthMeshRef.indexCount);
    std::vector<uint16_t> idx = depthMeshRef.indices;
    builder->setSegments(gfx::Triangles(), std::move(idx), segs.data(), segs.size());

    // Per-instance index attribute (divisor 1). Its element count is the instance count the
    // GL backend draws (drawInstanced uses instanceAttrs->getMinCount()).
    auto instAttrs = context.createVertexAttributeArray();
    if (const auto& a = instAttrs->set(shaders::idTerrainInstanceVertexAttribute)) {
        for (std::size_t i = 0; i < n; ++i) {
            a->set(i, static_cast<float>(i));
        }
    }
    builder->setInstanceAttributes(std::move(instAttrs));

    builder->flush(context);
    auto drawables = builder->clearDrawables();
    if (!drawables.empty()) {
        // Bind the packed DEM array as u_dem_array (slot idTerrainDEMArrayTexture); DrawableGL
        // binds it in bindTextures() with the program active, using the shader sampler location.
        static_cast<gl::DrawableGL&>(*drawables[0])
            .setArrayTexture(demTextureArray.get(), shaders::idTerrainDEMArrayTexture);
        depthLg->addDrawable(std::move(drawables[0]));
    }
}

// Refresh the per-instance UBO array every frame (the transform depends on the camera) and
// bind it + the shared props UBO directly on the instanced drawable, so the terrain tweaker's
// layer-level TerrainDrawableUBO consolidation does not clobber it. Bound at idTerrainDrawableUBO
// as the array the shader indexes by a_instance.
void RenderTerrain::updateInstancedDepthUBO(PaintParameters& parameters) {
    auto* depthLg = static_cast<LayerGroup*>(depthLayerGroup.get());
    const std::size_t n = depthInstances.size();

    if (!depthLg || n == 0) {
        return;
    }
    auto& context = parameters.context;

    // The shader declares the block as a fixed array u_inst[TERRAIN_MAX_INSTANCES]
    // (== maxDepthInstances), so GLES requires the bound buffer/range to be at least
    // that full static size (sizeof(UBO) * maxDepthInstances). Allocate the whole block
    // and fill only the first n entries; the rest stay zero-initialized. Sizing the
    // buffer to n instead triggers "Bound buffer is too small" and the draw is dropped.
    std::vector<shaders::TerrainDepthInstanceUBO> arr(maxDepthInstances);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& inst = depthInstances[i];
        mat4 m = parameters.matrixForTile(inst.tileID.toUnwrapped());
#if !MLN_RENDER_BACKEND_OPENGL
        m[2] = 0.5 * (m[2] + m[3]);
        m[6] = 0.5 * (m[6] + m[7]);
        m[10] = 0.5 * (m[10] + m[11]);
        m[14] = 0.5 * (m[14] + m[15]);
#endif
        arr[i].matrix = util::cast<float>(m);
        arr[i].dem_coords = inst.demCoords;
        arr[i].dem_layer = inst.demLayer;
        arr[i].pad1 = arr[i].pad2 = arr[i].pad3 = 0.0f;
    }
    const std::size_t bytes = sizeof(shaders::TerrainDepthInstanceUBO) * maxDepthInstances;
    if (!depthInstanceUBO || depthInstanceUBO->getSize() < bytes) {
        depthInstanceUBO = context.createUniformBuffer(arr.data(), bytes, false, true);
    } else {
        depthInstanceUBO->update(arr.data(), bytes);
    }

    // Shared evaluated props (unpack / exaggeration / skirt offset), same as the tweaker.
    const auto zoom = std::max(static_cast<double>(parameters.state.getZoom()), 0.0);
    const float elevationOffset = static_cast<float>(util::M2PI * util::EARTH_RADIUS_M / std::pow(2.0, zoom) / 5.0);
    const shaders::TerrainEvaluatedPropsUBO propsUBO = {.unpack = getDEMUnpackVector(),
                                                        .exaggeration = getExaggeration(),
                                                        .elevation_offset = elevationOffset,
                                                        .pad1 = 0.0f,
                                                        .pad2 = 0.0f};

    depthLg->visitDrawables([&](gfx::Drawable& drawable) {
        auto& u = drawable.mutableUniformBuffers();
        u.set(shaders::idTerrainDrawableUBO, depthInstanceUBO);
        u.createOrUpdate(shaders::idTerrainEvaluatedPropsUBO, &propsUBO, context);
    });
}
#endif

} // namespace mbgl
