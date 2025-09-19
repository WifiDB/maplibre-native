#include <napi.h>

#include <mbgl/util/run_loop.hpp>
#include <mbgl/gfx/backend.hpp>
#include <mbgl/gfx/backend_scope.hpp>

#include "node_map.hpp"
#include "node_logging.hpp"
#include "node_request.hpp"
#include "node_expression.hpp"

Napi::Value SetBackendType(const Napi::CallbackInfo& info) {
    if (info.Length() < 1 || info[0].IsUndefined()) {
        Napi::TypeError::New(info.Env(), "Requires a render backend name").ThrowAsJavaScriptException();
        return info.Env().Undefined();
    }

    const std::string backendName{info[0].As<Napi::String>().Utf8Value()};
    (void)backendName;
    return info.Env().Undefined();
}

Napi::Object RegisterModule(Napi::Env env, Napi::Object exports) {
    // This has the effect of:
    //    a) Ensuring that the static local variable is initialized before any
    //    thread contention. b) unreffing an async handle, which otherwise would
    //    keep the default loop running.
    static mbgl::util::RunLoop nodeRunLoop;
    nodeRunLoop.stop();

    exports.Set("setBackendType", Napi::Function::New(env, SetBackendType));

    node_mbgl::NodeMap::Init(env, exports);
    node_mbgl::NodeRequest::Init(env, exports);
    node_mbgl::NodeExpression::Init(env, exports);

    // Exports Resource constants.
    Napi::Object resource = Napi::Object::New(env);
    
    resource.Set("Unknown", Napi::Number::New(env, mbgl::Resource::Unknown));
    resource.Set("Style", Napi::Number::New(env, mbgl::Resource::Style));
    resource.Set("Source", Napi::Number::New(env, mbgl::Resource::Source));
    resource.Set("Tile", Napi::Number::New(env, mbgl::Resource::Tile));
    resource.Set("Glyphs", Napi::Number::New(env, mbgl::Resource::Glyphs));
    resource.Set("SpriteImage", Napi::Number::New(env, mbgl::Resource::SpriteImage));
    resource.Set("SpriteJSON", Napi::Number::New(env, mbgl::Resource::SpriteJSON));

    exports.Set("Resource", resource);

    // Make the exported object emit events
    mbgl::Log::setObserver(std::make_unique<node_mbgl::NodeLogObserver>(exports));

    return exports;
}

NODE_API_MODULE(mapbox_gl_native, RegisterModule)
