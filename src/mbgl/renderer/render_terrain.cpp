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
#include <mbgl/renderer/render_pass.hpp>
#include <mbgl/renderer/render_tree.hpp>
#include <mbgl/renderer/render_static_data.hpp>
#include <mbgl/renderer/render_orchestrator.hpp>
#include <mbgl/renderer/render_target.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/change_request.hpp>
#include <mbgl/renderer/layer_group.hpp>
#include <mbgl/renderer/layers/terrain_layer_tweaker.hpp>
#include <mbgl/renderer/buckets/hillshade_bucket.hpp>
#include <mbgl/geometry/dem_data.hpp>
#include <mbgl/tile/raster_dem_tile.hpp>
#include <mbgl/tile/tile.hpp>
#if MLN_RENDER_BACKEND_OPENGL
#include <mbgl/gl/context.hpp>
#include <mbgl/gl/texture_2d_array.hpp>
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

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <unordered_set>

namespace mbgl {

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

void RenderTerrain::update(const UpdateParameters& /*parameters*/) {
    // Find the DEM source if we haven't already
    if (!demSource && !impl->sourceID.empty()) {
        // In a full implementation, we would look up the source from parameters.sources
        // and cache the RenderSource pointer
        // For now, this is a placeholder
    }
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
        if (demSource) {
            Log::Info(Event::Render, "Terrain found DEM source: " + impl->sourceID);
        } else {
            Log::Warning(Event::Render, "Terrain could not find DEM source: " + impl->sourceID);
        }
    }

    // Create layer group if we don't have one (including after rebuild)
    if (!layerGroup) {
        if (auto layerGroup_ = context.createLayerGroup(TERRAIN_LAYER_INDEX, /*initialCapacity=*/1, "terrain", false)) {
            layerGroup = std::move(layerGroup_);
            activateLayerGroup(true, changes);
            Log::Info(Event::Render, "Created terrain layer group");
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
        Log::Info(Event::Render, "Created terrain layer tweaker");
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
            // All tiles come from the same raster-dem source, so they share one encoding
            demUnpackVector = demData->getUnpackVector();
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

    // TEMP diagnostic: periodic terrain state summary (remove before merging)
    if (demUpdateCounter % 120 == 1) {
        std::map<uint8_t, int> zoomHistogram;
        for (const auto& id : renderTileIDs) {
            zoomHistogram[id.canonical.z]++;
        }
        std::string zooms;
        for (const auto& [z, n] : zoomHistogram) {
            zooms += "z" + util::toString(static_cast<int>(z)) + ":" + util::toString(n) + " ";
        }
        Log::Info(Event::Render,
                  "Terrain: renderTiles=" + util::toString(renderTileIDs.size()) + " (" + zooms + ") meshTiles=" +
                      util::toString(meshTiles.size()) + " demTextures=" + util::toString(demTextures.size()) +
                      " drawables=" + util::toString(tilesWithDrawables.size()));
    }

    // Create terrain drawables for each mesh tile
    size_t skippedNoDEM = 0;
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
        std::array<float, 4> demCoords{{1, 0, 0, 0}};
        uint8_t demTier = 0;

        if (auto cached = demTextures.find(unwrapped); cached != demTextures.end()) {
            cached->second.lastUsed = demUpdateCounter;
            demTexture = cached->second.texture;
            demTier = 2;
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
                ++skippedNoDEM;
                demTexture = getPlaceholderDEMTexture(context);
                if (!demTexture) {
                    continue;
                }
            } else {
                ancestorEntry->lastUsed = demUpdateCounter;
                demTier = 1;
                const int dz = unwrapped.canonical.z - ancestorID->canonical.z;
                const float scale = static_cast<float>(1u << dz);
                const float dx = static_cast<float>(unwrapped.canonical.x - (ancestorID->canonical.x << dz));
                const float dy = static_cast<float>(unwrapped.canonical.y - (ancestorID->canonical.y << dz));
                demCoords = {{1.0f / scale, dx / scale, dy / scale, 0.0f}};
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

        // Create terrain drawable for this tile
        const auto renderTarget = texturePool.getRenderTarget(unwrapped);
        if (!renderTarget) {
            continue;
        }
        auto drawable = createDrawableForTile(context, shaders, tileID, demTexture, renderTarget->getTexture());
        if (drawable) {
            lg->addDrawable(std::move(drawable));
            tilesWithDrawables[tileID] = demTier;
            if (depthLg) {
                if (auto depthDrawable = createDrawableForTile(
                        context, shaders, tileID, demTexture, nullptr, /*depthPass=*/true)) {
                    depthLg->addDrawable(std::move(depthDrawable));
                    depthDirty = true;
                }
            }
        }
    }

    // TEMP diagnostic (remove before merging)
    if (skippedNoDEM > 0 && demUpdateCounter % 120 == 1) {
        Log::Warning(Event::Render,
                     "Terrain: " + util::toString(skippedNoDEM) + " mesh tiles have no DEM (own or ancestor) yet");
    }
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
    const int dz = tileID.canonical.z - demTileID.canonical.z;
    const float scale = static_cast<float>(1u << dz);
    const float dx = static_cast<float>(tileID.canonical.x - (demTileID.canonical.x << dz));
    const float dy = static_cast<float>(tileID.canonical.y - (demTileID.canonical.y << dz));
    const float xInDem = (dx * util::EXTENT + x) / scale;
    const float yInDem = (dy * util::EXTENT + y) / scale;

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
    const int dz = tileID.canonical.z - demTileID->canonical.z;
    const float scale = static_cast<float>(1u << dz);
    const float dx = static_cast<float>(tileID.canonical.x - (demTileID->canonical.x << dz));
    const float dy = static_cast<float>(tileID.canonical.y - (demTileID->canonical.y << dz));

    return TerrainData{
        .demTexture = entry->texture,
        .demCoords = {{1.0f / (util::EXTENT * scale), dx / scale, dy / scale, 0.0f}},
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
    const Size size = parameters.backend.getDefaultRenderable().getSize();
    if (!depthRenderTarget || depthRenderTarget->getTexture()->getSize() != size) {
        depthRenderTarget = parameters.context.createRenderTarget(size, gfx::TextureChannelDataType::UnsignedByte);
        if (!depthRenderTarget) {
            return;
        }
        // Far plane everywhere the terrain does not cover (unpack_depth(1,1,1,1) ~ 1.0)
        depthRenderTarget->setClearColor(Color::white());
        depthRenderTarget->addLayerGroup(depthLayerGroup, /*replace=*/true);
    }

    // Render the packed depth every frame, matching upstream. It was previously gated on
    // camera-projection changes to save a mesh pass on static scenes, but that gate did not
    // invalidate on DEM/terrain-shape changes: while DEM tiles stream in during warmup, the
    // terrain surface (and the symbol anchors displaced onto it) shift, but the frozen depth
    // texture did not, so symbol occlusion (calculate_visibility) went out of sync and labels
    // that had appeared were wrongly culled ("appear at warmup, then vanish"). Drawing it
    // every frame keeps the depth in lockstep with the terrain, as upstream does.
    depthRenderTarget->render(orchestrator, renderTree, parameters);
}

const std::shared_ptr<gfx::Texture2D>& RenderTerrain::getDepthTexture(gfx::Context& context) {
    if (depthRenderTarget) {
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
        generateMesh(context);
    }
    return *mesh;
}

void RenderTerrain::generateMesh(gfx::Context& /*context*/) {
    // Generate a regular grid mesh for terrain
    // This mesh will be reused for all tiles and displaced by DEM data in shaders

    const size_t gridSize = MESH_SIZE;
    const size_t verticesPerSide = gridSize + 1;
    const size_t totalVertices = verticesPerSide * verticesPerSide;

    // Vertex data: Each vertex has pos (x,y) and texture_pos (u,v)
    // Store as int16_t (short) for Metal short2 attribute format
    // Format: [pos.x, pos.y, tex.u, tex.v, pos.x, pos.y, tex.u, tex.v, ...]
    std::vector<int16_t> vertices;
    vertices.reserve(totalVertices * 4); // 4 shorts per vertex (x, y, u, v)

    const float posStep = static_cast<float>(util::EXTENT) / static_cast<float>(gridSize);
    const float texStep = static_cast<float>(util::EXTENT) / static_cast<float>(gridSize);

    for (size_t y = 0; y < verticesPerSide; ++y) {
        for (size_t x = 0; x < verticesPerSide; ++x) {
            // Position coordinates (in tile space 0-8192)
            vertices.push_back(static_cast<int16_t>(x * posStep));
            vertices.push_back(static_cast<int16_t>(y * posStep));
            // Texture coordinates (same as position for now - will be used to sample DEM)
            vertices.push_back(static_cast<int16_t>(x * texStep));
            vertices.push_back(static_cast<int16_t>(y * texStep));
        }
    }

    // Index data: generate triangles for the grid
    std::vector<uint16_t> indices;
    indices.reserve(gridSize * gridSize * 6); // 2 triangles per grid cell, 3 indices per triangle

    for (size_t y = 0; y < gridSize; ++y) {
        for (size_t x = 0; x < gridSize; ++x) {
            // Calculate vertex indices for this grid cell
            uint16_t topLeft = static_cast<uint16_t>(y * verticesPerSide + x);
            uint16_t topRight = topLeft + 1;
            uint16_t bottomLeft = static_cast<uint16_t>((y + 1) * verticesPerSide + x);
            uint16_t bottomRight = bottomLeft + 1;

            // First triangle (top-left, bottom-left, top-right)
            indices.push_back(topLeft);
            indices.push_back(bottomLeft);
            indices.push_back(topRight);

            // Second triangle (top-right, bottom-left, bottom-right)
            indices.push_back(topRight);
            indices.push_back(bottomLeft);
            indices.push_back(bottomRight);
        }
    }

    // Store mesh data with raw vertices and indices
    mesh = TerrainMesh{nullptr,             // vertexBuffer - will be created when creating drawable
                       nullptr,             // indexBuffer - will be created when creating drawable
                       vertices.size() / 4, // 4 shorts per vertex (x, y, u, v)
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

    // Configure sampler - use linear filtering for smooth elevation interpolation
    texture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                      .wrapU = gfx::TextureWrapType::Clamp,
                                      .wrapV = gfx::TextureWrapType::Clamp});

    return texture;
}

std::shared_ptr<gfx::Texture2D> RenderTerrain::createTestMapTexture(gfx::Context& context) {
    // Create a simple test texture with a checkerboard pattern
    // This will be replaced with actual render-to-texture output later
    const uint32_t size = 512;       // 512x512 texture
    const uint32_t checkerSize = 64; // Size of each checker square

    // Create RGBA pixel data
    auto imageData = std::make_unique<uint8_t[]>(size * size * 4);

    for (uint32_t y = 0; y < size; y++) {
        for (uint32_t x = 0; x < size; x++) {
            uint32_t index = (y * size + x) * 4;

            // Create checkerboard pattern
            bool isWhite = ((x / checkerSize) + (y / checkerSize)) % 2 == 0;

            if (isWhite) {
                // White with full alpha
                imageData[index + 0] = 255; // R
                imageData[index + 1] = 255; // G
                imageData[index + 2] = 255; // B
                imageData[index + 3] = 255; // A
            } else {
                // Light blue with full alpha
                imageData[index + 0] = 100; // R
                imageData[index + 1] = 150; // G
                imageData[index + 2] = 255; // B
                imageData[index + 3] = 255; // A
            }
        }
    }

    // Create PremultipliedImage from raw data
    auto image = std::make_shared<PremultipliedImage>(Size{size, size}, std::move(imageData));

    // Create texture
    auto texture = context.createTexture2D();
    if (!texture) {
        Log::Error(Event::Render, "Failed to create test map texture");
        return nullptr;
    }

    // Set the image
    texture->setImage(image);

    // Configure sampler
    texture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                      .wrapU = gfx::TextureWrapType::Repeat,
                                      .wrapV = gfx::TextureWrapType::Repeat});

    return texture;
}

std::unique_ptr<gfx::Drawable> RenderTerrain::createDrawableForTile(gfx::Context& context,
                                                                    gfx::ShaderRegistry& shaders,
                                                                    const OverscaledTileID& tileID,
                                                                    std::shared_ptr<gfx::Texture2D> demTexture,
                                                                    std::shared_ptr<gfx::Texture2D> mapTexture,
                                                                    bool depthPass) {
    // Ensure mesh is generated
    const auto& terrainMesh = getMesh(context);

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
    // map texture by design; only the draped pass needs one (falling back to the
    // test pattern if the render target has no texture yet)
    if (!depthPass) {
        if (!mapTexture) {
            mapTexture = createTestMapTexture(context);
        }
        if (mapTexture) {
            builder->setTexture(mapTexture, 1); // Texture index 1 for map
        } else {
            Log::Warning(Event::Render, "Failed to create test map texture for tile " + util::toString(tileID));
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

RenderSource* RenderTerrain::findDEMSource(const UpdateParameters& /*parameters*/) {
    // TODO: Implement source lookup
    // This would iterate through parameters.sources to find the raster-dem source
    // matching impl->sourceID
    return nullptr;
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
#endif

} // namespace mbgl
