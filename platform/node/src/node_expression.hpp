#pragma once

#include <mbgl/style/conversion_impl.hpp>
#include <mbgl/style/expression/expression.hpp>
#include <exception>
#include <memory>

#include <napi.h>

namespace node_mbgl {

Napi::Value toJS(Napi::Env env, const mbgl::style::expression::Value&);

class NodeExpression : public Napi::ObjectWrap<NodeExpression> {
public:
    static void Init(Napi::Env env, Napi::Object exports);
    static Napi::Object NewInstance(Napi::Env env, std::unique_ptr<mbgl::style::expression::Expression> expression);

    NodeExpression(const Napi::CallbackInfo& info);

private:
    NodeExpression(Napi::Env env, std::unique_ptr<mbgl::style::expression::Expression> expression_);

    // Napi::ObjectWrap automatically handles construction via DefineClass
    // The previous Nan::FunctionCallbackInfo<v8::Value> signatures are replaced
    // with Napi::CallbackInfo
    Napi::Value Evaluate(const Napi::CallbackInfo& info);
    Napi::Value GetType(const Napi::CallbackInfo& info);
    Napi::Value IsFeatureConstant(const Napi::CallbackInfo& info);
    Napi::Value IsZoomConstant(const Napi::CallbackInfo& info);
    Napi::Value Serialize(const Napi::CallbackInfo& info);

    // Static methods still need to be declared, but take the Napi::CallbackInfo
    static void Parse(const Napi::CallbackInfo& info);

    static Napi::FunctionReference constructor;

    std::unique_ptr<mbgl::style::expression::Expression> expression;
};

} // namespace node_mbgl
