#include <mbgl/renderer/renderer_impl.hpp>

#include <mbgl/geometry/line_atlas.hpp>
#include <mbgl/gfx/backend_scope.hpp>
#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/cull_face_mode.hpp>
#include <mbgl/gfx/render_pass.hpp>
#include <mbgl/gfx/renderer_backend.hpp>
#include <mbgl/gfx/renderable.hpp>
#include <mbgl/gfx/upload_pass.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/pattern_atlas.hpp>
#include <mbgl/renderer/renderer_observer.hpp>
#include <mbgl/renderer/render_static_data.hpp>
#include <mbgl/renderer/render_tree.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/renderer/texture_pool.hpp>
#include <mbgl/renderer/update_parameters.hpp>
#include <mbgl/shaders/program_parameters.hpp>
#include <mbgl/util/convert.hpp>
#include <mbgl/util/string.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/instrumentation.hpp>

#include <mbgl/gfx/drawable.hpp>
#include <mbgl/gfx/drawable_tweaker.hpp>
#include <mbgl/renderer/layer_group.hpp>
#include <mbgl/renderer/layer_tweaker.hpp>
#include <mbgl/renderer/render_target.hpp>
#include <mbgl/renderer/render_terrain.hpp>
#include <mbgl/renderer/layers/terrain_layer_tweaker.hpp>
#include <mbgl/util/hash.hpp>

#include <cstdint>
#include <functional>
#include <map>

#if MLN_RENDER_BACKEND_METAL
#include <mbgl/mtl/renderer_backend.hpp>
#include <Metal/MTLCaptureManager.hpp>
#include <Metal/MTLCaptureScope.hpp>
/// Enable programmatic Metal frame captures for specific frame numbers.
/// Requires iOS 13
constexpr auto EnableMetalCapture = 0;
constexpr auto CaptureFrameStart = 0; // frames are 0-based
constexpr auto CaptureFrameCount = 1;
#elif MLN_RENDER_BACKEND_OPENGL
#include <mbgl/gl/defines.hpp>
#include <mbgl/gl/drawable_gl.hpp>
#endif // !MLN_RENDER_BACKEND_METAL

namespace mbgl {

using namespace style;

namespace {

RendererObserver& nullObserver() {
    static RendererObserver observer;
    return observer;
}

} // namespace

Renderer::Impl::Impl(gfx::RendererBackend& backend_,
                     float pixelRatio_,
                     const std::optional<std::string>& localFontFamily_)
    : orchestrator(!backend_.contextIsShared(), backend_.getThreadPool(), localFontFamily_),
      backend(backend_),
      observer(&nullObserver()),
      pixelRatio(pixelRatio_) {}

Renderer::Impl::~Impl() {
    assert(gfx::BackendScope::exists());
};

void Renderer::Impl::onPreCompileShader(shaders::BuiltIn shaderID,
                                        gfx::Backend::Type type,
                                        const std::string& additionalDefines) {
    observer->onPreCompileShader(shaderID, type, additionalDefines);
}

void Renderer::Impl::onPostCompileShader(shaders::BuiltIn shaderID,
                                         gfx::Backend::Type type,
                                         const std::string& additionalDefines) {
    observer->onPostCompileShader(shaderID, type, additionalDefines);
}

void Renderer::Impl::onShaderCompileFailed(shaders::BuiltIn shaderID,
                                           gfx::Backend::Type type,
                                           const std::string& additionalDefines) {
    observer->onShaderCompileFailed(shaderID, type, additionalDefines);
}

void Renderer::Impl::onRenderError(std::exception_ptr error) {
    observer->onRenderError(error);
}

void Renderer::Impl::setObserver(RendererObserver* observer_) {
    observer = observer_ ? observer_ : &nullObserver();
}

void Renderer::Impl::render(const RenderTree& renderTree, const std::shared_ptr<UpdateParameters>& updateParameters) {
    MLN_TRACE_FUNC();
    auto& context = backend.getContext();
    context.setObserver(this);

    assert(updateParameters);

#if MLN_RENDER_BACKEND_METAL
#if MLN_CREATE_AUTORELEASEPOOL
    NS::SharedPtr pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
#endif

    if constexpr (EnableMetalCapture) {
        const auto& mtlBackend = static_cast<mtl::RendererBackend&>(backend);

        const auto& mtlDevice = mtlBackend.getDevice();

        if (!commandCaptureScope) {
            if (const auto& cmdQueue = mtlBackend.getCommandQueue()) {
                if (const auto captureManager = NS::RetainPtr(MTL::CaptureManager::sharedCaptureManager())) {
                    // NOLINTNEXTLINE(bugprone-assignment-in-if-condition)
                    if ((commandCaptureScope = NS::TransferPtr(captureManager->newCaptureScope(cmdQueue.get())))) {
                        const auto label = "Renderer::Impl frame=" + util::toString(frameCount);
                        commandCaptureScope->setLabel(NS::String::string(label.c_str(), NS::UTF8StringEncoding));
                        captureManager->setDefaultCaptureScope(commandCaptureScope.get());
                    }
                }
            }
        }

        // "When you capture a frame programmatically, you can capture Metal commands that span multiple
        //  frames by using a custom capture scope. For example, by calling begin() at the start of frame
        //  1 and end() after frame 3, the trace will contain command data from all the buffers that were
        //  committed in the three frames."
        // https://developer.apple.com/documentation/metal/debugging_tools/capturing_gpu_command_data_programmatically
        if constexpr (0 < CaptureFrameStart && 0 < CaptureFrameCount) {
            if (commandCaptureScope) {
                const auto captureManager = NS::RetainPtr(MTL::CaptureManager::sharedCaptureManager());
                if (frameCount == CaptureFrameStart) {
                    constexpr auto captureDest = MTL::CaptureDestination::CaptureDestinationDeveloperTools;
                    if (captureManager && !captureManager->isCapturing() &&
                        captureManager->supportsDestination(captureDest)) {
                        if (auto captureDesc = NS::TransferPtr(MTL::CaptureDescriptor::alloc()->init())) {
                            captureDesc->setCaptureObject(mtlDevice.get());
                            captureDesc->setDestination(captureDest);
                            NS::Error* errorPtr = nullptr;
                            if (captureManager->startCapture(captureDesc.get(), &errorPtr)) {
                                Log::Warning(Event::Render, "Capture Started");
                            } else {
                                std::string errStr = "<none>";
                                if (auto error = NS::TransferPtr(errorPtr)) {
                                    if (auto str = error->localizedDescription()) {
                                        if (auto cstr = str->utf8String()) {
                                            errStr = cstr;
                                        }
                                    }
                                }
                                Log::Warning(Event::Render, "Capture Failed: " + errStr);
                            }
                        }
                    }
                }
            }
        }
        if (commandCaptureScope) {
            commandCaptureScope->beginScope();

            const auto captureManager = NS::RetainPtr(MTL::CaptureManager::sharedCaptureManager());
            if (captureManager->isCapturing()) {
                Log::Info(Event::Render, "Capturing frame " + util::toString(frameCount));
            }
        }
    }
#endif // MLN_RENDER_BACKEND_METAL

    // Blocks execution until the renderable is available.
    backend.getDefaultRenderable().wait();
    context.beginFrame();

    if (!staticData) {
        staticData = std::make_unique<RenderStaticData>(std::make_unique<gfx::ShaderRegistry>());

        // Initialize shaders for drawables
        const auto programParameters = ProgramParameters{pixelRatio, false};
        backend.initShaders(*staticData->shaders, programParameters);

        // Notify post-shader registration
        observer->onRegisterShaders(*staticData->shaders);
    }

    const auto& renderTreeParameters = renderTree.getParameters();
    staticData->has3D = renderTreeParameters.has3D;
    staticData->backendSize = backend.getDefaultRenderable().getSize();

    if (renderState == RenderState::Never) {
        observer->onWillStartRenderingMap();
    }

    observer->onWillStartRenderingFrame();

    const TransformState& state = renderTreeParameters.transformParams.state;
    const Size& size = staticData->backendSize;
    const EdgeInsets& frustumOffset = state.getFrustumOffset();
    const gfx::ScissorRect scissorRect = {
        .x = static_cast<int32_t>(frustumOffset.left() * pixelRatio),
#if MLN_RENDER_BACKEND_OPENGL
        .y = static_cast<int32_t>(frustumOffset.bottom() * pixelRatio),
#else
        .y = static_cast<int32_t>(frustumOffset.top() * pixelRatio),
#endif
        .width = size.width - static_cast<uint32_t>((frustumOffset.left() + frustumOffset.right()) * pixelRatio),
        .height = size.height - static_cast<uint32_t>((frustumOffset.top() + frustumOffset.bottom()) * pixelRatio),
    };

    PaintParameters parameters{
        context,
        pixelRatio,
        backend,
        renderTreeParameters.light,
        renderTreeParameters.mapMode,
        renderTreeParameters.debugOptions,
        renderTreeParameters.timePoint,
        renderTreeParameters.transformParams,
        *staticData,
        renderTree.getLineAtlas(),
        renderTree.getPatternAtlas(),
        texturePool,
        frameCount,
        updateParameters->tileLodMinRadius,
        updateParameters->tileLodScale,
        updateParameters->tileLodPitchThreshold,
        updateParameters->tileLodMode,
        scissorRect,
    };

    parameters.symbolFadeChange = renderTreeParameters.symbolFadeChange;
    parameters.opaquePassCutoff = renderTreeParameters.opaquePassCutOff;
    parameters.terrain = orchestrator.getRenderTerrain();
    const auto& sourceRenderItems = renderTree.getSourceRenderItems();

    const auto& layerRenderItems = renderTree.getLayerRenderItemMap();

    if (auto* terrain = orchestrator.getRenderTerrain()) {
        RenderSource* demSource = orchestrator.getRenderSource(terrain->getSourceID());
        auto renderTiles = demSource->getRawRenderTiles();

        // Match RenderTerrain's mesh tile set: parent fallback tiles expanded
        // to the ideal cover so each mesh tile has its own drape target
        std::set<UnwrappedTileID> renderTileIDs;
        for (const auto& renderTile : *renderTiles) {
            renderTileIDs.insert(renderTile.id);
        }
        const std::set<UnwrappedTileID> demTileIDs = RenderTerrain::expandToDeepestCover(renderTileIDs);
        for (const auto& id : demTileIDs) {
            texturePool.createRenderTarget(context, id, renderTreeParameters.backgroundColor);
        }
        texturePool.removeStaleRenderTargets(demTileIDs);
    } else {
        // The pool persists across frames, so release the drape targets when
        // terrain is disabled instead of holding their textures indefinitely
        texturePool.removeStaleRenderTargets({});
    }

    // - UPLOAD PASS -------------------------------------------------------------------------------
    // Uploads all required buffers and images before we do any actual rendering.
    {
        const auto uploadPass = parameters.encoder->createUploadPass("upload",
                                                                     parameters.backend.getDefaultRenderable());
#if !defined(NDEBUG)
        const auto debugGroup = uploadPass->createDebugGroup("upload");
#endif

        // Update all clipping IDs + upload buckets.
        for (const RenderItem& item : sourceRenderItems) {
            item.upload(*uploadPass);
        }
        for (const RenderItem& item : layerRenderItems) {
            item.upload(*uploadPass);
        }
        staticData->upload(*uploadPass);
        renderTree.getLineAtlas().upload(*uploadPass);
        renderTree.getPatternAtlas().upload(*uploadPass);
    }

    // - LAYER GROUP UPDATE ------------------------------------------------------------------------
    // Updates all layer groups and process changes
    if (staticData && staticData->shaders) {
        orchestrator.updateLayers(*staticData->shaders,
                                  context,
                                  renderTreeParameters.transformParams.state,
                                  updateParameters,
                                  renderTree,
                                  texturePool);
    }

    orchestrator.processChanges();
    orchestrator.addRenderTargets(texturePool);
    // Draped layer groups are not routed into individual render targets here;
    // each drape RenderTarget renders every overlapping draped drawable itself
    // (RenderTarget::renderDrapedLayerGroups), so a tile at a different zoom than
    // the terrain cover is drawn into every target it covers, like gl-js.

    // Per-drape-target content signatures for this frame (see
    // PaintParameters::perTargetDrapeSignature). Populated in the tweaker pass
    // below when terrain is active; must outlive the drape targets pass that reads
    // it, hence the function scope. parameters points into it for the frame.
    std::map<UnwrappedTileID, std::size_t> perTargetDrapeSignature;

    // Upload layer groups
    {
        const auto uploadPass = parameters.encoder->createUploadPass("layerGroup-upload",
                                                                     parameters.backend.getDefaultRenderable());
#if !defined(NDEBUG)
        const auto debugGroup = uploadPass->createDebugGroup("layerGroup-upload");
#endif

        // Update the debug layer groups
        orchestrator.updateDebugLayerGroups(renderTree, parameters);

        // Compute the frame-global draped-content signature once (see
        // PaintParameters::drapedContentSignature). It folds every input a terrain
        // drape's baked content depends on - the ids of all draped drawables, the
        // draped group count, the zoom, and the property epoch - and is reused both
        // to gate the draped tweakers here and by the per-target drape short-circuit
        // later this frame. Computed once, it is O(draped drawables), not per target.
        const bool terrainActive = orchestrator.getRenderTerrain() != nullptr;
        bool drapedContentChanged = true;
        if (terrainActive) {
            const double zoom = parameters.state.getZoom();
            // Fold only the integer tile-zoom into drape signatures, not the continuous
            // zoom. A drape tile's rasterized content is stable within a zoom level; a
            // continuous zoom would change every target's signature on every frame of a
            // pinch gesture, re-rendering all drape textures each frame (the dominant
            // render-thread CPU cost). This caches drapes across pan/pitch/zoom-in-level,
            // re-rendering only when crossing an integer zoom (where tiles change anyway).
            const int32_t zoomLevel = static_cast<int32_t>(zoom);
            const uint64_t propertiesEpoch = LayerTweaker::getPropertiesEpoch();

            // Single pass over all draped drawables building three things:
            //  - the global signature (order-dependent hash) for the tweaker gate;
            //  - tileAccum[X]: order-independent sum of per-drawable hashes for the
            //    drawables exactly at tile X;
            //  - subtreeAccum[X]: the same, but for X and every tile below it, built
            //    by adding each drawable's hash to all of its ancestors (and itself).
            // From these, a target's own content signature is a couple of O(depth)
            // lookups instead of another full O(drawables) scan.
            std::size_t signature = 0;
            std::size_t drapedGroupCount = 0;
            std::map<UnwrappedTileID, std::size_t> tileAccum;
            std::map<UnwrappedTileID, std::size_t> subtreeAccum;
            const std::hash<std::int64_t> hashId;
            orchestrator.visitLayerGroups([&](LayerGroupBase& layerGroup) {
                if (layerGroup.getType() != LayerGroupBase::Type::TileLayerGroup ||
                    !layerGroup.shouldRenderToTerrain()) {
                    return;
                }
                drapedGroupCount++;
                static_cast<TileLayerGroup&>(layerGroup).visitDrawables([&](const gfx::Drawable& drawable) {
                    if (!drawable.getEnabled() || !drawable.getTileID()) {
                        return;
                    }
                    util::hash_combine(signature, drawable.getID().id());
                    const std::size_t h = hashId(drawable.getID().id());
                    const UnwrappedTileID tile = drawable.getTileID()->toUnwrapped();
                    tileAccum[tile] += h;
                    // Add to this tile and every ancestor up to z0 (order-independent
                    // sum, so traversal order does not matter)
                    for (int zz = tile.canonical.z; zz >= 0; --zz) {
                        subtreeAccum[UnwrappedTileID(tile.wrap, tile.canonical.scaledTo(static_cast<uint8_t>(zz)))] +=
                            h;
                    }
                });
            });
            util::hash_combine(signature, drapedGroupCount);
            util::hash_combine(signature, zoomLevel);
            util::hash_combine(signature, propertiesEpoch);
            parameters.drapedContentSignature = signature;
            drapedContentChanged = (lastDrapedContentSignature != signature);
            lastDrapedContentSignature = signature;

            // Build each drape target's own signature: the drawables in its subtree
            // (itself + descendants) plus those at its strict ancestors, folded with
            // the global zoom / property epoch / group count.
            const auto lookup = [](const std::map<UnwrappedTileID, std::size_t>& m, const UnwrappedTileID& k) {
                const auto it = m.find(k);
                return it == m.end() ? std::size_t{0} : it->second;
            };
            orchestrator.visitRenderTargets([&](RenderTarget& renderTarget) {
                const auto& tid = renderTarget.getDrapeTileID();
                if (!tid) {
                    return;
                }
                std::size_t sig = lookup(subtreeAccum, *tid); // self + descendants
                for (int zz = static_cast<int>(tid->canonical.z) - 1; zz >= 0; --zz) {
                    sig += lookup(tileAccum,
                                  UnwrappedTileID(tid->wrap, tid->canonical.scaledTo(static_cast<uint8_t>(zz))));
                }
                util::hash_combine(sig, drapedGroupCount);
                util::hash_combine(sig, zoomLevel);
                util::hash_combine(sig, propertiesEpoch);
                perTargetDrapeSignature[*tid] = sig;
            });
            parameters.perTargetDrapeSignature = &perTargetDrapeSignature;
        }

        // Tweakers are run in the upload pass so they can set up uniforms.
        parameters.currentLayer = 0;
        orchestrator.visitLayerGroups([&](LayerGroupBase& layerGroup) {
            // Skip a draped layer group's tweaker when the drape content is
            // unchanged: its cached drape texture is not re-rendered this frame,
            // and drapes are camera-independent (rendered with a tile-local matrix),
            // so the recomputed per-drawable camera UBOs would go unused. A real
            // change moves the signature and re-runs the tweaker the same frame the
            // drape re-renders, keeping the two consistent.
            const bool skipDrapedTweaker = terrainActive && !drapedContentChanged &&
                                           layerGroup.shouldRenderToTerrain();
            if (!skipDrapedTweaker) {
                layerGroup.runTweakers(renderTree, parameters);
            }
            parameters.currentLayer++;
        });

        // Run terrain tweaker if terrain is enabled
        if (auto* terrain = orchestrator.getRenderTerrain()) {
            if (auto* terrainTweaker = terrain->getTweaker()) {
                const double start = util::MonotonicTimer::now().count();
                if (const auto& layerGroup = terrain->getLayerGroup()) {
                    terrainTweaker->execute(*layerGroup, parameters);
                }
                if (const auto& depthLayerGroup = terrain->getDepthLayerGroup()) {
                    terrainTweaker->execute(*depthLayerGroup, parameters);
                }
                context.renderingStats().terrainTweakerTime = util::MonotonicTimer::now().count() - start;
            }
        }

        parameters.currentLayer = 0;
        orchestrator.visitDebugLayerGroups([&](LayerGroupBase& layerGroup) {
            layerGroup.runTweakers(renderTree, parameters);
            parameters.currentLayer++;
        });

        // Give the layers a chance to upload
        orchestrator.visitLayerGroups([&](LayerGroupBase& layerGroup) { layerGroup.upload(*uploadPass); });

        // Give the render targets a chance to upload
        orchestrator.visitRenderTargets([&](RenderTarget& renderTarget) { renderTarget.upload(*uploadPass); });

        // Upload the Debug layer group
        orchestrator.visitDebugLayerGroups([&](LayerGroupBase& layerGroup) { layerGroup.upload(*uploadPass); });
    }

    const Size atlasSize = parameters.patternAtlas.getPixelSize();
    const auto& worldSize = parameters.staticData.backendSize;
    const shaders::GlobalPaintParamsUBO globalPaintParamsUBO = {
        .pattern_atlas_texsize = {static_cast<float>(atlasSize.width), static_cast<float>(atlasSize.height)},
        .units_to_pixels = {1.0f / parameters.pixelsToGLUnits[0], 1.0f / parameters.pixelsToGLUnits[1]},
        .world_size = {static_cast<float>(worldSize.width), static_cast<float>(worldSize.height)},
        .camera_to_center_distance = parameters.state.getCameraToCenterDistance(),
        .symbol_fade_change = parameters.symbolFadeChange,
        .aspect_ratio = parameters.state.getSize().aspectRatio(),
        .pixel_ratio = parameters.pixelRatio,
        .map_zoom = static_cast<float>(parameters.state.getZoom()),
        .pad1 = 0,
        // Target tile while drawing into a terrain drape target (w != 0);
        // the per-target buffers set it in RenderTarget::updateDrapeGlobalUBO
        .drape_tile = {0.0f, 0.0f, 0.0f, 0.0f},
    };
    auto& globalUniforms = context.mutableGlobalUniformBuffers();
    globalUniforms.createOrUpdate(shaders::idGlobalPaintParamsUBO, &globalPaintParamsUBO, context);

    // Refresh each terrain drape target's copy of the global paint params
    // (same values plus the target tile in drape_tile)
    if (orchestrator.getRenderTerrain()) {
        texturePool.visitRenderTargets([&](std::shared_ptr<RenderTarget>& renderTarget) {
            renderTarget->updateDrapeGlobalUBO(globalPaintParamsUBO, context);
        });
    }

    // - 3D PASS
    // -------------------------------------------------------------------------------------
    // Renders any 3D layers bottom-to-top to unique FBOs with texture
    // attachments, but share the same depth rbo between them.
    const auto common3DPass = [&] {
        if (parameters.staticData.has3D) {
            parameters.staticData.backendSize = parameters.backend.getDefaultRenderable().getSize();

            const auto debugGroup(parameters.encoder->createDebugGroup("common-3d"));
            parameters.pass = RenderPass::Pass3D;
#if MLN_RENDER_BACKEND_OPENGL
            parameters.updateStencilBufferAvailability();
#endif

            // TODO is this needed?
            // if (!parameters.staticData.depthRenderbuffer ||
            //    parameters.staticData.depthRenderbuffer->getSize() != parameters.staticData.backendSize) {
            //    parameters.staticData.depthRenderbuffer =
            //        parameters.context.createRenderbuffer<gfx::RenderbufferPixelType::Depth>(
            //            parameters.staticData.backendSize);
            //}
            // parameters.staticData.depthRenderbuffer->setShouldClear(true);
        }
    };

    const auto drawable3DPass = [&] {
        const auto debugGroup(parameters.encoder->createDebugGroup("drawables-3d"));
        assert(parameters.pass == RenderPass::Pass3D);

        // draw layer groups, 3D pass
        parameters.currentLayer = static_cast<uint32_t>(orchestrator.numLayerGroups()) - 1;
        orchestrator.visitLayerGroups([&](LayerGroupBase& layerGroup) {
            layerGroup.render(orchestrator, parameters);
            if (parameters.currentLayer > 0) {
                parameters.currentLayer--;
            }
        });
    };

    const auto drawableTargetsPass = [&] {
        // Render targets are held in insertion order, but the terrain drape targets
        // consume the others: draping the hillshade layer samples the texture its
        // prepare pass renders. A drape target added in an earlier frame therefore
        // sits ahead of a prepare target added later and would sample it before it
        // was drawn this frame - reading black, which the hillshade decodes as the
        // maximum slope (the prepare pass encodes flat as 0.5, not 0), shading the
        // whole tile solid. Draw the producers first, then the drapes that sample
        // them.

        // Per-frame reset for the drape-churn measurement: RenderTarget::render
        // increments these for each drape it re-renders (a cache miss) and for
        // each one that runs the full coverage scan (a fast-path miss).
        context.renderingStats().numDrapeTargetsRendered = 0;
        context.renderingStats().numDrapeCoverageScans = 0;
        // parameters.drapedContentSignature was computed once earlier this frame
        // (before the tweaker pass) and is reused by each target's short-circuit.

        orchestrator.visitRenderTargets([&](RenderTarget& renderTarget) {
            if (!renderTarget.getDrapeTileID()) {
                renderTarget.render(orchestrator, renderTree, parameters);
            }
        });
        orchestrator.visitRenderTargets([&](RenderTarget& renderTarget) {
            if (renderTarget.getDrapeTileID()) {
                renderTarget.render(orchestrator, renderTree, parameters);
            }
        });
    };

    const auto commonClearPass = [&] {
        // - CLEAR
        // -------------------------------------------------------------------------------------
        // Renders the backdrop of the OpenGL view. This also paints in areas where
        // we don't have any tiles whatsoever.
        {
            std::optional<Color> color;
            if (parameters.debugOptions & MapDebugOptions::Overdraw) {
                color = Color::black();
            } else if (!backend.contextIsShared()) {
                color = renderTreeParameters.backgroundColor;
            }
            parameters.renderPass = parameters.encoder->createRenderPass(
                "main buffer",
                {.renderable = parameters.backend.getDefaultRenderable(),
                 .clearColor = color,
                 .clearDepth = 1.0f,
                 .clearStencil = 0});
#if MLN_RENDER_BACKEND_OPENGL
            parameters.updateStencilBufferAvailability();
#endif
        }
    };

    // Actually render the layers
    // Drawables
    const auto drawableOpaquePass = [&] {
        const auto debugGroup(parameters.renderPass->createDebugGroup("drawables-opaque"));
        parameters.pass = RenderPass::Opaque;
        parameters.depthRangeSize = 1 - (orchestrator.numLayerGroups() + 2) * PaintParameters::numSublayers *
                                            PaintParameters::depthEpsilon;

        // draw layer groups, opaque pass
        parameters.currentLayer = 0;
        orchestrator.visitLayerGroupsReversed([&](LayerGroupBase& layerGroup) {
            if (!(parameters.terrain && layerGroup.getType() == LayerGroupBase::Type::TileLayerGroup &&
                  layerGroup.shouldRenderToTerrain())) {
                layerGroup.render(orchestrator, parameters);
            }
            parameters.currentLayer++;
        });
    };

    const auto drawableTranslucentPass = [&] {
        const auto debugGroup(parameters.renderPass->createDebugGroup("drawables-translucent"));
        parameters.pass = RenderPass::Translucent;
        parameters.depthRangeSize = 1 - (orchestrator.numLayerGroups() + 2) * PaintParameters::numSublayers *
                                            PaintParameters::depthEpsilon;

        // draw layer groups, translucent pass; draped groups render only into the
        // terrain render targets (RenderTarget::renderDrapedLayerGroups)
        parameters.currentLayer = static_cast<uint32_t>(orchestrator.numLayerGroups()) - 1;
        orchestrator.visitLayerGroups([&](LayerGroupBase& layerGroup) {
            if (!(parameters.terrain && layerGroup.getType() == LayerGroupBase::Type::TileLayerGroup &&
                  layerGroup.shouldRenderToTerrain())) {
                layerGroup.render(orchestrator, parameters);
            }
            if (parameters.currentLayer > 0) {
                parameters.currentLayer--;
            }
        });

        // Finally, render any legacy layers which have not been converted to drawables.
        // Note that they may be out of order, this is just a temporary fix for `RenderLocationIndicatorLayer` (#2216)
        parameters.depthRangeSize = 1 - (layerRenderItems.size() + 2) * PaintParameters::numSublayers *
                                            PaintParameters::depthEpsilon;
        int32_t i = static_cast<int32_t>(layerRenderItems.size()) - 1;
        for (auto it = layerRenderItems.begin(); it != layerRenderItems.end() && i >= 0; ++it, --i) {
            parameters.currentLayer = i;
            const RenderItem& item = *it;
            if (item.hasRenderPass(parameters.pass)) {
                item.render(parameters);
            }
        }
    };

    const auto drawableDebugOverlays = [&] {
        // Renders debug overlays.
        {
            const auto debugGroup(parameters.renderPass->createDebugGroup("debug"));
            parameters.currentLayer = 0;
            orchestrator.visitDebugLayerGroups([&](LayerGroupBase& layerGroup) {
                layerGroup.render(orchestrator, parameters);
                parameters.currentLayer++;
            });
        }
    };

    if (parameters.staticData.has3D) {
        common3DPass();
        drawable3DPass();
    }
    drawableTargetsPass();
    // Terrain depth pass for symbol occlusion (sampled by calculate_visibility)
    if (auto* terrain = orchestrator.getRenderTerrain()) {
        const double start = util::MonotonicTimer::now().count();
        terrain->renderDepth(orchestrator, renderTree, parameters);
        context.renderingStats().terrainDepthTime = util::MonotonicTimer::now().count() - start;
    }
    commonClearPass();
    context.bindGlobalUniformBuffers(*parameters.renderPass);
    drawableOpaquePass();
    drawableTranslucentPass();
    drawableDebugOverlays();

    // Give the layers a chance to do cleanup
    orchestrator.visitLayerGroups([&](LayerGroupBase& layerGroup) { layerGroup.postRender(orchestrator, parameters); });
    context.unbindGlobalUniformBuffers(*parameters.renderPass);

    // Ends the RenderPass
    parameters.renderPass.reset();

    const auto startRendering = util::MonotonicTimer::now().count();
    // present submits render commands
    parameters.encoder->present(parameters.backend.getDefaultRenderable());
    context.renderingStats().renderingTime = util::MonotonicTimer::now().count() - startRendering;

    parameters.encoder.reset();
    context.endFrame();

#if MLN_RENDER_BACKEND_METAL
    if constexpr (EnableMetalCapture) {
        if (commandCaptureScope) {
            commandCaptureScope->endScope();

            const auto captureManager = NS::RetainPtr(MTL::CaptureManager::sharedCaptureManager());
            if (frameCount == CaptureFrameStart + CaptureFrameCount - 1 && captureManager->isCapturing()) {
                captureManager->stopCapture();
            }
        }
    }
#endif // MLN_RENDER_BACKEND_METAL

    context.renderingStats().encodingTime = renderTree.getElapsedTime() - context.renderingStats().renderingTime;

    observer->onDidFinishRenderingFrame(
        renderTreeParameters.loaded ? RendererObserver::RenderMode::Full : RendererObserver::RenderMode::Partial,
        renderTreeParameters.needsRepaint,
        renderTreeParameters.placementChanged,
        context.threadSafeCopyRenderingStats());

    if (!renderTreeParameters.loaded) {
        renderState = RenderState::Partial;
    } else if (renderState != RenderState::Fully) {
        renderState = RenderState::Fully;
        observer->onDidFinishRenderingMap();
    }

    frameCount += 1;
    MLN_END_FRAME();
}

void Renderer::Impl::reduceMemoryUse() {
    assert(gfx::BackendScope::exists());
    // The drape targets are the largest reclaimable GPU allocation (one
    // tile-sized texture per terrain tile); they are rebuilt on the next frame
    texturePool.removeStaleRenderTargets({});
    backend.getContext().reduceMemoryUsage();
}

} // namespace mbgl
