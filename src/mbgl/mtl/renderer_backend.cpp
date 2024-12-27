#include "mbgl/mtl/renderer_backend.hpp"

#include <mbgl/gfx/backend_scope.hpp>
#include <mbgl/gfx/shader_registry.hpp>
#include <mbgl/mtl/context.hpp>
#include <mbgl/shaders/background_layer_ubo.hpp>

#include <mbgl/shaders/mtl/shader_group.hpp>
#include <mbgl/shaders/mtl/shader_program.hpp>
#include <mbgl/shaders/shader_manifest.hpp>
#include <mbgl/util/logging.hpp>

// ... shader_manifest.hpp
#include <mbgl/shaders/mtl/background.hpp>
#include <mbgl/shaders/mtl/background_pattern.hpp>
#include <mbgl/shaders/mtl/circle.hpp>
#include <mbgl/shaders/mtl/clipping_mask.hpp>
#include <mbgl/shaders/mtl/collision_box.hpp>
#include <mbgl/shaders/mtl/collision_circle.hpp>
#include <mbgl/shaders/mtl/custom_symbol_icon.hpp>
#include <mbgl/shaders/mtl/debug.hpp>
#include <mbgl/shaders/mtl/fill.hpp>
#include <mbgl/shaders/mtl/fill_extrusion.hpp>
#include <mbgl/shaders/mtl/fill_extrusion_pattern.hpp>
#include <mbgl/shaders/mtl/heatmap.hpp>
#include <mbgl/shaders/mtl/heatmap_texture.hpp>
#include <mbgl/shaders/mtl/hillshade.hpp>
#include <mbgl/shaders/mtl/hillshade_prepare.hpp>
#include <mbgl/shaders/mtl/line.hpp>
#include <mbgl/shaders/mtl/fill.hpp>
#include <mbgl/shaders/mtl/raster.hpp>
#include <mbgl/shaders/mtl/symbol_icon.hpp>
#include <mbgl/shaders/mtl/symbol_sdf.hpp>
#include <mbgl/shaders/mtl/symbol_text_and_icon.hpp>
#include <mbgl/shaders/mtl/widevector.hpp>

#include <cassert>
#include <string>

namespace mbgl {
namespace mtl {

     // Helper function to release a Metal object
    template <typename T>
    void safeRelease(T object, const char* name) {
        if (object) {
            [object release];
            MBGL_DEBUG("RendererBackend: Released: %s", name);
        }
    }
    
    // Helper function to track the creation of a metal object.
    template <typename T>
    T safeCreate(T object, const char* name){
        if(!object){
            MBGL_ERROR("RendererBackend: Failed to create: %s", name);
        } else {
          MBGL_DEBUG("RendererBackend: Created: %s", name);
        }
        return object;
    }

RendererBackend::RendererBackend(const gfx::ContextMode contextMode_)
    : gfx::RendererBackend(contextMode_) {
    MBGL_DEBUG("RendererBackend::RendererBackend()");
    device = safeCreate(MTL::CreateSystemDefaultDevice(), "MTLDevice");
     if(!device){
        MBGL_ERROR("RendererBackend::RendererBackend - MTLDevice creation failed");
          return;
    }
    commandQueue = safeCreate(device ? NS::TransferPtr(device->newCommandQueue()) : nullptr, "MTLCommandQueue");
     if(!commandQueue) {
        MBGL_ERROR("RendererBackend::RendererBackend - MTLCommandQueue creation failed");
        safeRelease(device, "MTLDevice");
        device = nullptr;
       return;
    }
    

#if TARGET_OS_SIMULATOR
    baseVertexInstanceDrawingSupported = true;
#else
    baseVertexInstanceDrawingSupported = device->supportsFamily(MTL::GPUFamilyApple3);
#endif
      MBGL_DEBUG("RendererBackend::RendererBackend() - baseVertexInstanceDrawingSupported: %d", baseVertexInstanceDrawingSupported);
}

RendererBackend::~RendererBackend() {
    MBGL_DEBUG("RendererBackend::~RendererBackend()");
    if(commandQueue){
        safeRelease(commandQueue, "MTLCommandQueue");
    }
    if(device){
        safeRelease(device, "MTLDevice");
    }
}

std::unique_ptr<gfx::Context> RendererBackend::createContext() {
    MBGL_DEBUG("RendererBackend::createContext()");
    return std::make_unique<mtl::Context>(*this);
}

PremultipliedImage RendererBackend::readFramebuffer(const Size& size) {
    MBGL_DEBUG("RendererBackend::readFramebuffer()");
    return PremultipliedImage(size);
}

void RendererBackend::assumeFramebufferBinding(const mtl::FramebufferID) {
    MBGL_DEBUG("RendererBackend::assumeFramebufferBinding()");
}

void RendererBackend::assumeViewport(int32_t, int32_t, const Size&) {
    MBGL_DEBUG("RendererBackend::assumeViewport()");
}

void RendererBackend::assumeScissorTest(bool) {
    MBGL_DEBUG("RendererBackend::assumeScissorTest()");
}

bool RendererBackend::implicitFramebufferBound() {
     MBGL_DEBUG("RendererBackend::implicitFramebufferBound()");
    return false;
}

void RendererBackend::setFramebufferBinding(const mtl::FramebufferID) {
    MBGL_DEBUG("RendererBackend::setFramebufferBinding()");
}

void RendererBackend::setViewport(int32_t, int32_t, const Size&) {
     MBGL_DEBUG("RendererBackend::setViewport()");
}

void RendererBackend::setScissorTest(bool) {
     MBGL_DEBUG("RendererBackend::setScissorTest()");
}

/// @brief Register a list of types with a shader registry instance
/// @tparam ...ShaderID Pack of BuiltIn:: shader IDs
/// @param registry A shader registry instance
/// @param programParameters ProgramParameters used to initialize each instance
template <shaders::BuiltIn... ShaderID>
void registerTypes(gfx::ShaderRegistry& registry, const ProgramParameters& programParameters) {
    /// The following fold expression will create a shader for every type
    /// in the parameter pack and register it with the shader registry.

    /// Registration calls are wrapped in a lambda that throws on registration
    /// failure, we shouldn't expect registration to faill unless the shader
    /// registry instance provided already has conflicting programs present.
    (
        [&]() {
            using namespace std::string_literals;
            using ShaderClass = shaders::ShaderSource<ShaderID, gfx::Backend::Type::Metal>;
            auto group = std::make_shared<ShaderGroup<ShaderID>>(programParameters);
            if (!registry.registerShaderGroup(std::move(group), ShaderClass::name)) {
                MBGL_ERROR("RendererBackend::registerTypes - Failed to register shader group: %s", ShaderClass::name.c_str());
                return false;
            }
          MBGL_DEBUG("RendererBackend::registerTypes - Registered shader group: %s", ShaderClass::name.c_str());
          return true;
        }(),
        ...);
}

void RendererBackend::initShaders(gfx::ShaderRegistry& shaders, const ProgramParameters& programParameters) {
    MBGL_DEBUG("RendererBackend::initShaders()");
    if(!registerTypes<shaders::BuiltIn::BackgroundShader,
                  shaders::BuiltIn::BackgroundPatternShader,
                  shaders::BuiltIn::CircleShader,
                  shaders::BuiltIn::ClippingMaskProgram,
                  shaders::BuiltIn::CollisionBoxShader,
                  shaders::BuiltIn::CollisionCircleShader,
                  shaders::BuiltIn::CustomSymbolIconShader,
                  shaders::BuiltIn::DebugShader,
                  shaders::BuiltIn::FillShader,
                  shaders::BuiltIn::FillOutlineShader,
                  shaders::BuiltIn::FillPatternShader,
                  shaders::BuiltIn::FillOutlinePatternShader,
                  shaders::BuiltIn::FillOutlineTriangulatedShader,
                  shaders::BuiltIn::FillExtrusionShader,
                  shaders::BuiltIn::FillExtrusionPatternShader,
                  shaders::BuiltIn::HeatmapShader,
                  shaders::BuiltIn::HeatmapTextureShader,
                  shaders::BuiltIn::HillshadeShader,
                  shaders::BuiltIn::HillshadePrepareShader,
                  shaders::BuiltIn::LineShader,
                  shaders::BuiltIn::LineGradientShader,
                  shaders::BuiltIn::LineSDFShader,
                  shaders::BuiltIn::LinePatternShader,
                  shaders::BuiltIn::RasterShader,
                  shaders::BuiltIn::SymbolIconShader,
                  shaders::BuiltIn::SymbolSDFIconShader,
                  shaders::BuiltIn::SymbolTextAndIconShader,
                  shaders::BuiltIn::WideVectorShader>(shaders, programParameters)){
          MBGL_ERROR("RendererBackend::initShaders - Failed to register one or more shader groups");
          return;
        }
}

} // namespace mtl
} // namespace mbgl
