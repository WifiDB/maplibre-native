#include "node_request.hpp"
#include "node_map.hpp"
#include <mbgl/storage/response.hpp>
#include <mbgl/util/chrono.hpp>

#include <napi.h>
#include <cmath>

namespace node_mbgl {

NodeRequest::NodeRequest(mbgl::FileSource::Callback callback_, NodeAsyncRequest* asyncRequest_)
    : callback(std::move(callback_)),
      asyncRequest(asyncRequest_) {
    asyncRequest->request = this;
}

NodeRequest::~NodeRequest() {
    // When this object gets garbage collected, make sure that the
    // AsyncRequest can no longer attempt to remove the callback function
    // this object was holding (it can't be fired anymore).
    if (asyncRequest) {
        asyncRequest->request = nullptr;
    }
}

Napi::FunctionReference NodeRequest::constructor;

void NodeRequest::Init(Napi::Env env, Napi::Object exports) {
    Napi::HandleScope scope(env);

    Napi::Function func = DefineClass(env, "Request", {
        InstanceMethod("respond", &NodeRequest::HandleCallback),
    });
    
    constructor = Napi::Persistent(func);
    constructor.SuppressDestruct();
}

void NodeRequest::New(const Napi::CallbackInfo& info) {
    auto target = info[0].As<Napi::External<NodeMap>>().Data();
    auto callback = info[1].As<Napi::External<mbgl::FileSource::Callback>>().Data();
    auto asyncRequest = info[2].As<Napi::External<NodeAsyncRequest>>().Data();

    auto request = new NodeRequest(*callback, asyncRequest);

    request->Wrap(info.This());
    
    info.This().As<Napi::Object>().Set("url", info[3]);
    info.This().As<Napi::Object>().Set("kind", info[4]);

    info.This().As<Napi::Object>().Get("on").As<Napi::Function>().Call(
        info.This(), {Napi::String::New(info.Env(), "request"), info[5]}
    );
}

Napi::Value NodeRequest::HandleCallback(const Napi::CallbackInfo& info) {
    auto request = Napi::ObjectWrap<NodeRequest>::Unwrap(info.This());

    // Move out of the object so callback() can only be fired once.
    auto callback = std::move(request->callback);
    request->callback = {};
    if (!callback) {
        request->unrefRequest();
        return info.Env().Undefined();
    }

    mbgl::Response response;

    if (info.Length() < 1) {
        response.noContent = true;
    } else if (info[0].IsObject()) {
        auto err = info[0].As<Napi::Object>();
        auto msg = Napi::String::New(info.Env(), "message");

        if (err.Has(msg)) {
            response.error = std::make_unique<mbgl::Response::Error>(
                mbgl::Response::Error::Reason::Other, err.Get(msg).As<Napi::String>().Utf8Value());
        }
    } else if (info[0].IsString()) {
        response.error = std::make_unique<mbgl::Response::Error>(mbgl::Response::Error::Reason::Other,
                                                                 info[0].As<Napi::String>().Utf8Value());
    } else if (info.Length() < 2 || !info[1].IsObject()) {
        request->unrefRequest();
        Napi::TypeError::New(info.Env(), "Second argument must be a response object").ThrowAsJavaScriptException();
        return info.Env().Undefined();
    } else {
        auto res = info[1].As<Napi::Object>();

        if (res.Has("modified")) {
            const double modified = res.Get("modified").As<Napi::Number>().DoubleValue();
            if (!std::isnan(modified)) {
                response.modified = mbgl::Timestamp{mbgl::Seconds(static_cast<mbgl::Seconds::rep>(modified / 1000))};
            }
        }

        if (res.Has("expires")) {
            const double expires = res.Get("expires").As<Napi::Number>().DoubleValue();
            if (!std::isnan(expires)) {
                response.expires = mbgl::Timestamp{mbgl::Seconds(static_cast<mbgl::Seconds::rep>(expires / 1000))};
            }
        }

        if (res.Has("etag")) {
            const std::string etag = res.Get("etag").As<Napi::String>().Utf8Value();
            response.etag = etag;
        }

        if (res.Has("data")) {
            auto data = res.Get("data");
            if (data.IsBuffer()) {
                response.data = std::make_shared<std::string>(data.As<Napi::Buffer<char>>().Data(), data.As<Napi::Buffer<char>>().Length());
            } else {
                request->unrefRequest();
                Napi::TypeError::New(info.Env(), "Response data must be a Buffer").ThrowAsJavaScriptException();
                return info.Env().Undefined();
            }
        }
    }

    // Send the response object to the NodeFileSource object
    callback(response);
    request->unrefRequest();
    return info.Env().Undefined();
}

void NodeRequest::unrefRequest() {
    Unref();
}

NodeAsyncRequest::NodeAsyncRequest() = default;

NodeAsyncRequest::~NodeAsyncRequest() {
    if (request) {
        // Remove the callback function because the AsyncRequest was
        // canceled and we are no longer interested in the result.
        if (request->callback) {
            request->callback = {};
        }
        request->asyncRequest = nullptr;
    }
}

} // namespace node_mbgl
