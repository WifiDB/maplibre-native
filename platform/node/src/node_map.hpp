#pragma once

#include <mbgl/map/map.hpp>
#include <mbgl/storage/file_source.hpp>
#include <mbgl/storage/resource_options.hpp>
#include <mbgl/util/async_request.hpp>
#include <mbgl/util/client_options.hpp>
#include <mbgl/util/image.hpp>

#include <napi.h>
#include <exception>

namespace mbgl {
class HeadlessFrontend;
} // namespace mbgl

namespace node_mbgl {

struct NodeMapObserver : public mbgl::MapObserver {
    void onDidFailLoadingMap(mbgl::MapLoadError, const std::string&) final;
};

class RenderRequest;

class NodeMap : public Napi::ObjectWrap<NodeMap> {
public:
    struct RenderOptions;
    class RenderWorker;

    NodeMap(const Napi::CallbackInfo&);
    ~NodeMap() override = default;

    static Napi::FunctionReference constructor;

    static void Init(Napi::Env, Napi::Object);

    // Instance Methods
    static Napi::Value Load(const Napi::CallbackInfo&);
    static Napi::Value Loaded(const Napi::CallbackInfo&);
    static Napi::Value Render(const Napi::CallbackInfo&);
    static Napi::Value Release(const Napi::CallbackInfo&);
    static Napi::Value Cancel(const Napi::CallbackInfo&);
    static Napi::Value AddSource(const Napi::CallbackInfo&);
    static Napi::Value RemoveSource(const Napi::CallbackInfo&);
    static Napi::Value AddLayer(const Napi::CallbackInfo&);
    static Napi::Value RemoveLayer(const Napi::CallbackInfo&);
    static Napi::Value AddImage(const Napi::CallbackInfo&);
    static Napi::Value RemoveImage(const Napi::CallbackInfo&);
    static Napi::Value SetLayerZoomRange(const Napi::CallbackInfo&);
    static Napi::Value SetLayerProperty(const Napi::CallbackInfo&);
    static Napi::Value SetFilter(const Napi::CallbackInfo&);
    static Napi::Value SetSize(const Napi::CallbackInfo&);
    static Napi::Value SetCenter(const Napi::CallbackInfo&);
    static Napi::Value SetZoom(const Napi::CallbackInfo&);
    static Napi::Value SetBearing(const Napi::CallbackInfo&);
    static Napi::Value SetPitch(const Napi::CallbackInfo&);
    static Napi::Value SetLight(const Napi::CallbackInfo&);
    static Napi::Value SetAxonometric(const Napi::CallbackInfo&);
    static Napi::Value SetXSkew(const Napi::CallbackInfo&);
    static Napi::Value SetYSkew(const Napi::CallbackInfo&);
    static Napi::Value DumpDebugLogs(const Napi::CallbackInfo&);
    static Napi::Value QueryRenderedFeatures(const Napi::CallbackInfo&);

    static Napi::Value SetFeatureState(const Napi::CallbackInfo&);
    static Napi::Value GetFeatureState(const Napi::CallbackInfo&);
    static Napi::Value RemoveFeatureState(const Napi::CallbackInfo&);

    void startRender(const RenderOptions& options);
    void release();
    void cancel();

    static RenderOptions ParseOptions(Napi::Object);

    const float pixelRatio;
    mbgl::MapMode mode;
    bool crossSourceCollisions;
    NodeMapObserver mapObserver;
    std::unique_ptr<mbgl::HeadlessFrontend> frontend;
    std::unique_ptr<mbgl::Map> map;

    std::exception_ptr error;
    mbgl::PremultipliedImage image;
    Napi::AsyncWorker* req = nullptr;

    bool loaded = false;
};

struct NodeFileSource : public mbgl::FileSource {
    NodeFileSource(NodeMap* nodeMap_)
        : nodeMap(nodeMap_) {}
    ~NodeFileSource() override = default;
    std::unique_ptr<mbgl::AsyncRequest> request(const mbgl::Resource&, mbgl::FileSource::Callback) final;
    bool canRequest(const mbgl::Resource&) const override;
    void setResourceOptions(mbgl::ResourceOptions) override;
    mbgl::ResourceOptions getResourceOptions() override;
    void setClientOptions(mbgl::ClientOptions) override;
    mbgl::ClientOptions getClientOptions() override;
    NodeMap* nodeMap;
    mbgl::ResourceOptions _resourceOptions;
    mbgl::ClientOptions _clientOptions;
};

} // namespace node_mbgl
