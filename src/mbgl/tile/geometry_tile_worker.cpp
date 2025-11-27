#include <mbgl/tile/geometry_tile_worker.hpp>
#include <mbgl/tile/geometry_tile_data.hpp>
#include <mbgl/tile/geometry_tile.hpp>
#include <mbgl/layermanager/layer_manager.hpp>
#include <mbgl/layout/layout.hpp>
#include <mbgl/layout/symbol_layout.hpp>
#include <mbgl/layout/pattern_layout.hpp>
#include <mbgl/renderer/bucket_parameters.hpp>
#include <mbgl/renderer/group_by_layout.hpp>
#include <mbgl/style/filter.hpp>
#include <mbgl/style/layers/symbol_layer_impl.hpp>
#include <mbgl/renderer/layers/render_fill_layer.hpp>
#include <mbgl/renderer/layers/render_fill_extrusion_layer.hpp>
#include <mbgl/renderer/layers/render_line_layer.hpp>
#include <mbgl/renderer/layers/render_symbol_layer.hpp>
#include <mbgl/renderer/buckets/symbol_bucket.hpp>
#include <mbgl/util/instrumentation.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/util/string.hpp>
#include <mbgl/util/exception.hpp>
#include <mbgl/util/stopwatch.hpp>
#include <mbgl/util/thread_pool.hpp>

// ADDED: Includes for data-driven dasharray support
#include <mbgl/geometry/line_atlas.hpp>
#include <mbgl/layout/line_dash_util.hpp>
#include <mbgl/style/layers/line_layer_properties.hpp>

#include <unordered_set>
#include <utility>

namespace mbgl {

using namespace style;

GeometryTileWorker::GeometryTileWorker(ActorRef<GeometryTileWorker> self_,
                                       ActorRef<GeometryTile> parent_,
                                       const TaggedScheduler& scheduler_,
                                       OverscaledTileID id_,
                                       std::string sourceID_,
                                       const std::atomic<bool>& obsolete_,
                                       const MapMode mode_,
                                       const float pixelRatio_,
                                       const bool showCollisionBoxes_,
                                       gfx::DynamicTextureAtlasPtr dynamicTextureAtlas_,
                                       std::shared_ptr<LineAtlas> lineAtlas_,  // ADDED
                                       std::shared_ptr<FontFaces> fontFaces_)
    : self(std::move(self_)),
      parent(std::move(parent_)),
      scheduler(scheduler_),
      id(id_),
      sourceID(std::move(sourceID_)),
      obsolete(obsolete_),
      mode(mode_),
      pixelRatio(pixelRatio_),
      showCollisionBoxes(showCollisionBoxes_),
      dynamicTextureAtlas(dynamicTextureAtlas_),
      lineAtlas(std::move(lineAtlas_)),  // ADDED
      fontFaces(fontFaces_) {}

GeometryTileWorker::~GeometryTileWorker() {
    MLN_TRACE_FUNC();

    scheduler.runOnRenderThread([renderData_{std::move(renderData)}]() {});
}

/*
   GeometryTileWorker is a state machine. This is its transition diagram.
   States are indicated by [state], lines are transitions triggered by
   messages, (parentheses) are actions taken on transition.

                         [Idle] <-------------------------.
                            |                             |
        set{Data,Layers}, symbolDependenciesChanged,      |
                    setShowCollisionBoxes                 |
                            |                             |
   (do parse and/or symbol layout; self-send "coalesced") |
                            v                             |
                      [Coalescing] --- coalesced ---------.
                         |   |
             .-----------.   .---------------------.
             |                                     |
   .--- set{Data,Layers}                setShowCollisionBoxes,
   |         |                         symbolDependenciesChanged --.
   |         |                                     |               |
   |         v                                     v               |
   .-- [NeedsParse] <-- set{Data,Layers} -- [NeedsSymbolLayout] ---.
             |                                     |
         coalesced                             coalesced
             |                                     |
             v                                     v
   (do parse or symbol layout; self-send "coalesced"; goto [coalescing])

   The idea is that in the [idle] state, parsing happens immediately in response
   to a "set" message, and symbol layout happens once all symbol dependencies
   are met. During this processing, multiple "set" messages might get queued in
   the mailbox. At the end of processing, we self-send "coalesced", read all the
   queued messages until we get to "coalesced", and then re-parse if there were
   one or more "set"s or return to the [idle] state if not.

   One important goal of the design is to prevent starvation. Under heavy load
   new requests for tiles should not prevent in progress request from
   completing. It is nevertheless possible to restart an in-progress request:

    - [Idle] setData -> parse()
        sends getGlyphs, hasPendingDependencies() is true
        enters [Coalescing], sends coalesced
    - [Coalescing] coalesced -> [Idle]
    - [Idle] setData -> new parse(), interrupts old parse()
        sends getGlyphs, hasPendingDependencies() is true
        enters [Coalescing], sends coalesced
    - [Coalescing] onGlyphsAvailable -> [NeedsSymbolLayout]
           hasPendingDependencies() may or may not be true
    - [NeedsSymbolLayout] coalesced -> finalizeLayout()
           Generates result depending on whether dependencies are met
           -> [Idle]

   In this situation, we are counting on the idea that even with rapid changes
   to the tile's data, the set of glyphs/images it requires will not keep
   growing without limit.

   Although parsing (which populates all non-symbol buckets and requests
   dependencies for symbol buckets) is internally separate from symbol layout,
   we only return results to the foreground when we have completed both steps.
   Because we _move_ the result buckets to the foreground, it is necessary to
   re-generate all buckets from scratch for `setShowCollisionBoxes`, even though
   it only affects symbol layers.

   The GL JS equivalent (in worker_tile.js and vector_tile_worker_source.js)
   is somewhat simpler because it relies on getGlyphs/getImages calls that
   transfer an entire set of glyphs/images on every tile load, while the native
   logic maintains a local state that can be incrementally updated. Because each
   tile load call becomes self-contained, the equivalent of the coalescing logic
   is handled by 'reloadTile' queueing a single extra 'reloadTile' callback to
   run after the next completed parse.
*/

void GeometryTileWorker::setData(std::unique_ptr<const GeometryTileData> data_,
                                 std::set<std::string> availableImages_,
                                 uint64_t correlationID_) {
    MLN_TRACE_FUNC();

    try {
        data = std::move(data_);
        correlationID = correlationID_;
        availableImages = std::move(availableImages_);

        switch (state) {
            case Idle:
                parse();
                coalesce();
                break;

            case Coalescing:
            case NeedsParse:
            case NeedsSymbolLayout:
                state = NeedsParse;
                break;
        }
    } catch (...) {
        parent.invoke(&GeometryTile::onError, std::current_exception(), correlationID);
    }
}

void GeometryTileWorker::setLayers(std::vector<Immutable<LayerProperties>> layers_,
                                   std::set<std::string> availableImages_,
                                   uint64_t correlationID_) {
    MLN_TRACE_FUNC();

    try {
        layers = std::move(layers_);
        correlationID = correlationID_;
        availableImages = std::move(availableImages_);

        switch (state) {
            case Idle:
                parse();
                coalesce();
                break;

            case Coalescing:
            case NeedsSymbolLayout:
                state = NeedsParse;
                break;

            case NeedsParse:
                break;
        }
    } catch (...) {
        parent.invoke(&GeometryTile::onError, std::current_exception(), correlationID);
    }
}

void GeometryTileWorker::reset(uint64_t correlationID_) {
    layers = std::nullopt;
    data = std::nullopt;
    correlationID = correlationID_;

    switch (state) {
        case Idle:
        case NeedsParse:
            break;
        case Coalescing:
        case NeedsSymbolLayout:
            state = NeedsParse;
            break;
    }
}

void GeometryTileWorker::setShowCollisionBoxes(bool showCollisionBoxes_, uint64_t correlationID_) {
    MLN_TRACE_FUNC();

    try {
        showCollisionBoxes = showCollisionBoxes_;
        correlationID = correlationID_;

        switch (state) {
            case Idle:
                parse();
                coalesce();
                break;

            case Coalescing:
                state = NeedsParse;
                break;

            case NeedsSymbolLayout:
                state = NeedsParse;
                break;

            case NeedsParse:
                break;
        }
    } catch (...) {
        parent.invoke(&GeometryTile::onError, std::current_exception(), correlationID);
    }
}

void GeometryTileWorker::onGlyphsAvailable(GlyphMap glyphs, HBShapeResults requests) {
    MLN_TRACE_FUNC();

    for (auto& pair : glyphs) {
        const auto& fontStack = pair.first;
        const auto& glyphRange = pair.second;

        auto it = glyphMap.find(fontStack);
        if (it == glyphMap.end()) {
            glyphMap.emplace(fontStack, std::move(glyphRange));
        } else {
            for (auto& glyph : glyphRange) {
                it->second.emplace(glyph.first, std::move(glyph.second));
            }
        }

        pendingGlyphDependencies.glyphs.erase(fontStack);
    }

    for (auto& pair : requests) {
        const auto& fontStack = pair.first;
        const auto& shapingRequests = pair.second;

        for (auto& shapingPair : shapingRequests) {
            const auto& type = shapingPair.first;
            const auto& shapeRequests = shapingPair.second;
            auto it = pendingGlyphDependencies.shapes[fontStack][type].begin();
            while (it != pendingGlyphDependencies.shapes[fontStack][type].end()) {
                if (shapeRequests.count(*it)) {
                    it = pendingGlyphDependencies.shapes[fontStack][type].erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    symbolDependenciesChanged();
}

void GeometryTileWorker::onImagesAvailable(ImageMap newIconMap,
                                           ImageMap newPatternMap,
                                           ImageVersionMap versionMap_,
                                           uint64_t imageCorrelationID_) {
    MLN_TRACE_FUNC();

    if (imageCorrelationID_ != imageCorrelationID) {
        return; // Ignore outdated image response.
    }

    for (auto& newIcon : newIconMap) {
        iconMap.emplace(newIcon.first, std::move(newIcon.second));
        pendingImageDependencies.erase(newIcon.first);
    }

    for (auto& newPattern : newPatternMap) {
        patternMap.emplace(newPattern.first, std::move(newPattern.second));
        pendingImageDependencies.erase(newPattern.first);
    }

    versionMap = std::move(versionMap_);
    symbolDependenciesChanged();
}

void GeometryTileWorker::coalesce() {
    state = Coalescing;
    self.invoke(&GeometryTileWorker::coalesced);
}

void GeometryTileWorker::coalesced() {
    MLN_TRACE_FUNC();

    try {
        switch (state) {
            case Idle:
                assert(false);
                break;

            case Coalescing:
                state = Idle;
                break;

            case NeedsParse:
                parse();
                coalesce();
                break;

            case NeedsSymbolLayout:
                // We don't abort symbol layout for new data, but we do require that parsing has not yet
                // begun for the new parse. If it has, we're behind and we need to abort the layout, and
                // start parsing again.
                if (hasPendingParseResult()) {
                    finalizeLayout();
                } else {
                    parse();
                    coalesce();
                }
                break;
        }
    } catch (...) {
        parent.invoke(&GeometryTile::onError, std::current_exception(), correlationID);
    }
}

void GeometryTileWorker::symbolDependenciesChanged() {
    MLN_TRACE_FUNC();

    if (!hasPendingDependencies()) {
        state = NeedsSymbolLayout;
    }

    self.invoke(&GeometryTileWorker::coalesced);
}

void GeometryTileWorker::requestNewGlyphs(const GlyphDependencies& glyphDependencies) {
    MLN_TRACE_FUNC();

    for (auto& glyphDependency : glyphDependencies.glyphs) {
        const auto& fontStack = glyphDependency.first;
        const auto& glyphRange = glyphDependency.second;

        auto it = glyphMap.find(fontStack);
        if (it == glyphMap.end() || it->second.empty()) {
            pendingGlyphDependencies.glyphs[fontStack].insert(glyphRange.begin(), glyphRange.end());
        } else {
            for (auto& glyph : glyphRange) {
                if (it->second.find(glyph) == it->second.end()) {
                    pendingGlyphDependencies.glyphs[fontStack].insert(glyph);
                }
            }
        }
    }

    for (auto& shapeDependency : glyphDependencies.shapes) {
        const auto& fontStack = shapeDependency.first;
        const auto& types = shapeDependency.second;

        for (auto& typeDependencies : types) {
            auto& type = typeDependencies.first;
            auto& strs = typeDependencies.second;
            for (auto& str : strs) {
                pendingGlyphDependencies.shapes[fontStack][type].insert(str);
            }
        }
    }
    if (!pendingGlyphDependencies.glyphs.empty()) {
        parent.invoke(&GeometryTile::getGlyphs, pendingGlyphDependencies);
    }
}

void GeometryTileWorker::requestNewImages(const ImageDependencies& imageDependencies) {
    MLN_TRACE_FUNC();

    pendingImageDependencies = imageDependencies;

    if (!pendingImageDependencies.empty()) {
        parent.invoke(&GeometryTile::getImages, std::make_pair(pendingImageDependencies, ++imageCorrelationID));
    }
}

void GeometryTileWorker::parse() {
    MLN_TRACE_FUNC();

    if (!data || !layers) {
        return;
    }

    MBGL_TIMING_START(watch)

    std::unordered_map<std::string, std::unique_ptr<SymbolLayout>> symbolLayoutMap;

    renderData.clear();
    layouts.clear();

    featureIndex = std::make_unique<FeatureIndex>(*data ? (*data)->clone() : nullptr);

    // Avoid small reallocations for populated cells.
    // If we had a total feature count, this could be based on that and the cell count.
    constexpr auto estimatedElementsPerCell = 8;
    featureIndex->reserve(estimatedElementsPerCell);

    GlyphDependencies glyphDependencies;
    ImageDependencies imageDependencies;

    // Create render layers and group by layout
    mbgl::unordered_map<std::string, std::vector<Immutable<style::LayerProperties>>> groupMap;
    groupMap.reserve(layers->size());

    for (auto layer : *layers) {
        groupMap[layoutKey(*layer->baseImpl)].push_back(std::move(layer));
    }

    for (auto& pair : groupMap) {
        const auto& group = pair.second;
        if (obsolete) {
            return;
        }

        if (!*data) {
            continue; // Tile has no data.
        }

        const style::Layer::Impl& leaderImpl = *(group.at(0)->baseImpl);
        BucketParameters parameters{id, mode, pixelRatio, leaderImpl.getTypeInfo()};

        auto geometryLayer = (*data)->getLayer(leaderImpl.sourceLayer);
        if (!geometryLayer) {
            continue;
        }

        std::vector<std::string> layerIDs;
        layerIDs.reserve(group.size());
        for (const auto& layer : group) {
            layerIDs.push_back(layer->baseImpl->id);
        }

        featureIndex->setBucketLayerIDs(leaderImpl.id, layerIDs);

        // Symbol layers and layers that support pattern properties have an
        // extra step at layout time to figure out what images/glyphs are needed
        // to render the layer. They use the intermediate Layout data structure
        // to accomplish this, and either immediately create a bucket if no
        // images/glyphs are used, or the Layout is stored until the
        // images/glyphs are available to add the features to the buckets.
        if (leaderImpl.getTypeInfo()->layout == LayerTypeInfo::Layout::Required) {
            std::unique_ptr<Layout> layout = LayerManager::get()->createLayout(
                {parameters, fontFaces, glyphDependencies, imageDependencies, availableImages},
                std::move(geometryLayer),
                group);
            if (layout->hasDependencies()) {
                layouts.push_back(std::move(layout));
            } else {
                layout->createBucket({}, featureIndex, renderData, firstLoad, showCollisionBoxes, id.canonical);
            }
        } else {
            const Filter& filter = leaderImpl.filter;
            const std::string& sourceLayerID = leaderImpl.sourceLayer;
            std::shared_ptr<Bucket> bucket = LayerManager::get()->createBucket(parameters, group);

            for (std::size_t i = 0; !obsolete && i < geometryLayer->featureCount(); i++) {
                std::unique_ptr<GeometryTileFeature> feature = geometryLayer->getFeature(i);

                if (!filter(expression::EvaluationContext(static_cast<float>(this->id.overscaledZ), feature.get())
                                .withCanonicalTileID(&id.canonical)))
                    continue;

                const GeometryCollection& geometries = feature->getGeometries();
                
                // ADDED: Build pattern dependencies for data-driven properties (including dasharrays)
                PatternLayerMap patternDependencies;
                DashPositions dashPositions;
                
                // Check if any layer in the group has data-driven dasharray
                bool hasDataDrivenDasharray = false;
                for (const auto& layerProps : group) {
                    if (layerProps->baseImpl->getTypeInfo() == LineLayer::Impl::staticTypeInfo()) {
                        const auto& lineProps = static_cast<const LineLayerProperties&>(*layerProps);
                        const auto& dasharrayProp = lineProps.evaluated.get<LineDasharray>();
                        if (!dasharrayProp.isConstant()) {
                            hasDataDrivenDasharray = true;
                            break;
                        }
                    }
                }

                // Evaluate dasharray expressions if needed and LineAtlas is available
                if (hasDataDrivenDasharray && lineAtlas) {
                    const float zoom = static_cast<float>(id.overscaledZ);
                    
                    for (const auto& layerProps : group) {
                        // Only process line layers
                        if (layerProps->baseImpl->getTypeInfo() != LineLayer::Impl::staticTypeInfo()) {
                            continue;
                        }
                        
                        const auto& lineProps = static_cast<const LineLayerProperties&>(*layerProps);
                        const auto& dasharrayProp = lineProps.evaluated.get<LineDasharray>();
                        
                        // Skip constant dasharrays (handled by tweaker uniforms)
                        if (dasharrayProp.isConstant()) {
                            continue;
                        }
                        
                        // Get line-cap to determine pattern cap
                        const auto lineCap = lineProps.evaluated.get<LineCap>();
                        const LinePatternCap cap = (lineCap == LineCapType::Round) 
                            ? LinePatternCap::Round 
                            : LinePatternCap::Square;
                        
                        // Evaluate dasharray at three zoom levels for smooth interpolation
                        auto minDash = dasharrayProp.evaluate(*feature, zoom - 1.0f, id.canonical, {});
                        auto midDash = dasharrayProp.evaluate(*feature, zoom, id.canonical, {});
                        auto maxDash = dasharrayProp.evaluate(*feature, zoom + 1.0f, id.canonical, {});
                        
                        // Format as keys for LineAtlas lookup
                        std::string minKey = formatDashKey(minDash, cap);
                        std::string midKey = formatDashKey(midDash, cap);
                        std::string maxKey = formatDashKey(maxDash, cap);
                        
                        // Store in pattern dependencies map
                        patternDependencies[layerProps->baseImpl->id] = PatternDependency{
                            std::move(minKey),
                            std::move(midKey),
                            std::move(maxKey)
                        };
                    }
                    
                    // Build dash positions from pattern dependencies using LineAtlas
                    if (!patternDependencies.empty()) {
                        dashPositions = prepareDashPositions(patternDependencies, *lineAtlas);
                    }
                }

                // Call addFeature with pattern dependencies and dash positions
                bucket->addFeature(*feature, geometries, {}, patternDependencies, dashPositions, i, id.canonical);
                featureIndex->insert(geometries, i, sourceLayerID, leaderImpl.id);
            }

            if (!bucket->hasData()) {
                continue;
            }

            for (const auto& layer : group) {
                renderData.emplace(layer->baseImpl->id, LayerRenderData{bucket, layer});
            }
        }
    }

    requestNewGlyphs(glyphDependencies);
    requestNewImages(imageDependencies);

    MBGL_TIMING_FINISH(watch,
                       " Action: " << "Parsing,"
                                   << " SourceID: " << sourceID.c_str()
                                   << " Canonical: " << static_cast<int>(id.canonical.z) << "/" << id.canonical.x << "/"
                                   << id.canonical.y << " Time");
    finalizeLayout();
}

bool GeometryTileWorker::hasPendingDependencies() const {
    for (auto& glyphDependency : pendingGlyphDependencies.glyphs) {
        if (!glyphDependency.second.empty()) {
            return true;
        }
    }
    return !pendingImageDependencies.empty();
}

bool GeometryTileWorker::hasPendingParseResult() const {
    return bool(featureIndex);
}

void GeometryTileWorker::finalizeLayout() {
    MLN_TRACE_FUNC();

    if (!data || !layers || !hasPendingParseResult() || hasPendingDependencies()) {
        return;
    }

    MBGL_TIMING_START(watch);
    gfx::ImageAtlas imageAtlas;
    gfx::GlyphAtlas glyphAtlas;
    if (dynamicTextureAtlas) {
        imageAtlas = dynamicTextureAtlas->uploadIconsAndPatterns(iconMap, patternMap, versionMap);
    }
    if (!layouts.empty()) {
        if (dynamicTextureAtlas) {
            glyphAtlas = dynamicTextureAtlas->uploadGlyphs(glyphMap);
        }

        for (auto& layout : layouts) {
            if (obsolete) {
                dynamicTextureAtlas->removeTextures(glyphAtlas.textureHandles, glyphAtlas.dynamicTexture);
                dynamicTextureAtlas->removeTextures(imageAtlas.textureHandles, imageAtlas.dynamicTexture);
                return;
            }

            layout->prepareSymbols(glyphMap, glyphAtlas.glyphPositions, iconMap, imageAtlas.iconPositions);

            if (!layout->hasSymbolInstances()) {
                continue;
            }

            // layout adds the bucket to buckets
            layout->createBucket(
                imageAtlas.patternPositions, featureIndex, renderData, firstLoad, showCollisionBoxes, id.canonical);
        }
    }

    layouts.clear();

    firstLoad = false;

    MBGL_TIMING_FINISH(watch,
                       " Action: " << "SymbolLayout,"
                                   << " SourceID: " << sourceID.c_str()
                                   << " Canonical: " << static_cast<int>(id.canonical.z) << "/" << id.canonical.x << "/"
                                   << id.canonical.y << " Time");

    parent.invoke(&GeometryTile::onLayout,
                  std::make_shared<GeometryTile::LayoutResult>(std::move(renderData),
                                                               std::move(featureIndex),
                                                               std::move(glyphAtlas),
                                                               std::move(imageAtlas),
                                                               dynamicTextureAtlas),
                  correlationID);
}

} // namespace mbgl
