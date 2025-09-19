#pragma once

#include <napi.h>

#include <mbgl/storage/file_source.hpp>
#include <mbgl/storage/resource.hpp>
#include <mbgl/util/async_request.hpp>

namespace node_mbgl {

class NodeRequest;

struct NodeAsyncRequest : public mbgl::AsyncRequest {
    NodeAsyncRequest();
    ~NodeAsyncRequest() override;
    NodeRequest* request = nullptr;
};

class NodeRequest : public Napi::ObjectWrap<NodeRequest> {
public:
    NodeRequest(mbgl::FileSource::Callback, NodeAsyncRequest*);
    ~NodeRequest() override;

    static Napi::FunctionReference constructor;
    static void Init(Napi::Env, Napi::Object);

    static void New(const Napi::CallbackInfo&);
    static Napi::Value HandleCallback(const Napi::CallbackInfo&);

    void unrefRequest();

    mbgl::FileSource::Callback callback;
    NodeAsyncRequest* asyncRequest;
};

} // namespace node_mbgl
