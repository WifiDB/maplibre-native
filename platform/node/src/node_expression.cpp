#include "node_conversion.hpp"
#include "node_expression.hpp"
#include "node_feature.hpp"

#include <mbgl/style/expression/parsing_context.hpp>
#include <mbgl/style/expression/is_constant.hpp>
#include <mbgl/style/conversion/function.hpp>
#include <mbgl/style/conversion/geojson.hpp>
#include <mbgl/util/geojson.hpp>

using namespace mbgl::style;
using namespace mbgl::style::expression;

namespace node_mbgl {

Napi::FunctionReference NodeExpression::constructor;

void NodeExpression::Init(Napi::Env env, Napi::Object exports) {
    Napi::HandleScope scope(env);

    Napi::Function func = DefineClass(env, "Expression", {
        InstanceMethod("evaluate", &NodeExpression::Evaluate),
        InstanceMethod("getType", &NodeExpression::GetType),
        InstanceMethod("isFeatureConstant", &NodeExpression::IsFeatureConstant),
        InstanceMethod("isZoomConstant", &NodeExpression::IsZoomConstant),
        InstanceMethod("serialize", &NodeExpression::Serialize),
        StaticMethod("parse", &NodeExpression::Parse)
    });

    constructor = Napi::Persistent(func);
    constructor.SuppressDestruct();

    exports.Set("Expression", func);
}

NodeExpression::NodeExpression(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<NodeExpression>(info) {}

NodeExpression::NodeExpression(Napi::Env env, std::unique_ptr<Expression> expr)
    : Napi::ObjectWrap<NodeExpression>(env, constructor.New({})),
      expression(std::move(expr)) {}

static type::Type parseType(const Napi::Object& type) {
    static std::unordered_map<std::string, type::Type> types = {
        {"string", type::String},
        {"number", type::Number},
        {"boolean", type::Boolean},
        {"object", type::Object},
        {"color", type::Color},
        {"padding", type::Padding},
        {"value", type::Value},
        {"formatted", type::Formatted},
        {"number-format", type::String},
        {"resolvedImage", type::Image},
        {"variableAnchorOffsetCollection", type::VariableAnchorOffsetCollection}};

    Napi::String kindStr = type.Get("kind").As<Napi::String>();
    std::string kind = kindStr.Utf8Value();

    if (kind == "array") {
        type::Type itemType = parseType(type.Get("itemType").As<Napi::Object>());
        std::optional<std::size_t> N;

        if (type.Has("N")) {
            N = static_cast<std::size_t>(type.Get("N").As<Napi::Number>().Int64Value());
        }
        return type::Array(itemType, N);
    }
    
    auto it = types.find(kind);
    if (it != types.end()) {
        return it->second;
    }
    
    // Default or unknown type
    return type::Value;
}

void NodeExpression::Parse(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::HandleScope scope(env);
    
    if (info.Length() < 1 || info[0].IsUndefined()) {
        Napi::TypeError::New(env, "Requires a JSON style expression argument.").ThrowAsJavaScriptException();
        return;
    }

    std::optional<type::Type> expected;
    if (info.Length() > 1 && info[1].IsObject()) {
        expected = parseType(info[1].As<Napi::Object>());
    }

    Napi::Value expr = info[0];

    try {
        mbgl::style::conversion::Convertible convertible(expr);

        if (expr.IsObject() && !expr.IsArray() && expected) {
            mbgl::style::conversion::Error error;
            auto func = convertFunctionToExpression(*expected, convertible, error, false);
            if (func) {
                Napi::Object obj = constructor.New({});
                (new NodeExpression(env, std::move(*func)))->Wrap(obj);
                info.GetReturnValue().Set(obj);
                return;
            }
            Napi::Array errorsArray = Napi::Array::New(env, 1);
            Napi::Object errObj = Napi::Object::New(env);
            errObj.Set("key", Napi::String::New(env, ""));
            errObj.Set("error", Napi::String::New(env, error.message));
            errorsArray.Set(0U, errObj);
            info.GetReturnValue().Set(errorsArray);
            return;
        }

        ParsingContext ctx = expected ? ParsingContext(*expected) : ParsingContext();
        ParseResult parsed = ctx.parseLayerPropertyExpression(mbgl::style::conversion::Convertible(expr));
        if (parsed) {
            assert(ctx.getErrors().empty());
            Napi::Object obj = constructor.New({});
            (new NodeExpression(env, std::move(*parsed)))->Wrap(obj);
            info.GetReturnValue().Set(obj);
            return;
        }

        Napi::Array errorsArray = Napi::Array::New(env, ctx.getErrors().size());
        for (std::size_t i = 0; i < ctx.getErrors().size(); ++i) {
            const auto& error = ctx.getErrors()[i];
            Napi::Object errObj = Napi::Object::New(env);
            errObj.Set("key", Napi::String::New(env, error.key));
            errObj.Set("error", Napi::String::New(env, error.message));
            errorsArray.Set(static_cast<uint32_t>(i), errObj);
        }
        info.GetReturnValue().Set(errorsArray);
    } catch (std::exception& ex) {
        Napi::TypeError::New(env, ex.what()).ThrowAsJavaScriptException();
    }
}

void NodeExpression::Evaluate(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::HandleScope scope(env);
    
    if (info.Length() < 2 || !info[0].IsObject()) {
        Napi::TypeError::New(env, "Requires globals and feature arguments.").ThrowAsJavaScriptException();
        return;
    }

    std::optional<float> zoom;
    Napi::Object globals = info[0].As<Napi::Object>();
    if (globals.Has("zoom") && globals.Get("zoom").IsNumber()) {
        zoom = static_cast<float>(globals.Get("zoom").As<Napi::Number>().DoubleValue());
    }

    std::optional<double> heatmapDensity;
    if (globals.Has("heatmapDensity") && globals.Get("heatmapDensity").IsNumber()) {
        heatmapDensity = globals.Get("heatmapDensity").As<Napi::Number>().DoubleValue();
    }
    
    conversion::Error conversionError;
    std::optional<mbgl::GeoJSON> geoJSON = conversion::convert<mbgl::GeoJSON>(info[1], conversionError);
    if (!geoJSON) {
        Napi::TypeError::New(env, conversionError.message.c_str()).ThrowAsJavaScriptException();
        return;
    }

    try {
        mapbox::geojson::feature feature = geoJSON->get<mapbox::geojson::feature>();
        auto result = expression->evaluate(zoom, feature, heatmapDensity);
        if (result) {
            info.GetReturnValue().Set(toJS(env, *result));
        } else {
            Napi::Object res = Napi::Object::New(env);
            res.Set("error", Napi::String::New(env, result.error().message));
            info.GetReturnValue().Set(res);
        }
    } catch (std::exception& ex) {
        Napi::TypeError::New(env, ex.what()).ThrowAsJavaScriptException();
    }
}

void NodeExpression::GetType(const Napi::CallbackInfo& info) {
    const type::Type type = this->expression->getType();
    const std::string name = type.match([&](const auto& t) { return t.getName(); });
    info.GetReturnValue().Set(Napi::String::New(info.Env(), name));
}

void NodeExpression::IsFeatureConstant(const Napi::CallbackInfo& info) {
    info.GetReturnValue().Set(Napi::Boolean::New(info.Env(), isFeatureConstant(*this->expression)));
}

void NodeExpression::IsZoomConstant(const Napi::CallbackInfo& info) {
    info.GetReturnValue().Set(Napi::Boolean::New(info.Env(), isZoomConstant(*this->expression)));
}

void NodeExpression::Serialize(const Napi::CallbackInfo& info) {
    const mbgl::Value serialized = this->expression->serialize();
    info.GetReturnValue().Set(toJS(info.Env(), serialized));
}

Napi::Value toJS(Napi::Env env, const Value& value) {
    class ToValue : public mapbox::util::Value::ConstVisitor {
    public:
        ToValue(Napi::Env env_) : env(env_) {}

        Napi::Value operator()(mbgl::NullValue) const {
            return env.Null();
        }

        Napi::Value operator()(bool t) const {
            return Napi::Boolean::New(env, t);
        }

        Napi::Value operator()(double t) const {
            return Napi::Number::New(env, t);
        }

        Napi::Value operator()(const std::string& t) const {
            return Napi::String::New(env, t);
        }

        Napi::Value operator()(const std::vector<Value>& array) const {
            Napi::Array result = Napi::Array::New(env, array.size());
            for (std::uint32_t i = 0; i < array.size(); i++) {
                result.Set(i, toJS(env, array[i]));
            }
            return result;
        }

        Napi::Value operator()(const Collator&) const {
            assert(false);
            return env.Null();
        }

        Napi::Value operator()(const Formatted& formatted) const {
            Napi::Object serialized = Napi::Object::New(env);
            Napi::Array sections = Napi::Array::New(env, formatted.sections.size());
            for (std::uint32_t i = 0; i < formatted.sections.size(); ++i) {
                const auto& section = formatted.sections[i];
                Napi::Object serializedSection = Napi::Object::New(env);
                serializedSection.Set("text", Napi::String::New(env, section.text));
                if (section.fontScale) {
                    serializedSection.Set("scale", Napi::Number::New(env, *section.fontScale));
                } else {
                    serializedSection.Set("scale", env.Null());
                }
                if (section.fontStack) {
                    serializedSection.Set("fontStack", Napi::String::New(env, mbgl::fontStackToString(*section.fontStack)));
                } else {
                    serializedSection.Set("fontStack", env.Null());
                }
                if (section.textColor) {
                    serializedSection.Set("textColor", toJS(env, section.textColor->toObject()));
                } else {
                    serializedSection.Set("textColor", env.Null());
                }
                sections.Set(i, serializedSection);
            }
            serialized.Set("sections", sections);
            return serialized;
        }

        Napi::Value operator()(const mbgl::Color& color) const {
            return operator()(std::vector<Value>{static_cast<double>(color.r),
                                                  static_cast<double>(color.g),
                                                  static_cast<double>(color.b),
                                                  static_cast<double>(color.a)});
        }
        
        Napi::Value operator()(const mbgl::Padding& padding) const {
            return operator()(std::vector<Value>{static_cast<double>(padding.top),
                                                  static_cast<double>(padding.right),
                                                  static_cast<double>(padding.bottom),
                                                  static_cast<double>(padding.left)});
        }

        Napi::Value operator()(const std::unordered_map<std::string, Value>& map) const {
            Napi::Object result = Napi::Object::New(env);
            for (const auto& entry : map) {
                result.Set(Napi::String::New(env, entry.first), toJS(env, entry.second));
            }
            return result;
        }
        
        Napi::Value operator()(const Image& image) const {
            return toJS(env, image.toValue());
        }
        
        Napi::Value operator()(const mbgl::VariableAnchorOffsetCollection& variableAnchorOffsets) const {
            std::vector<Value> components;
            components.reserve(variableAnchorOffsets.size() * 2);
            for (const auto& variableAnchorOffset : variableAnchorOffsets) {
                components.emplace_back(std::string(mbgl::Enum<mbgl::style::SymbolAnchorType>::toString(variableAnchorOffset.anchorType)));
                components.emplace_back(std::vector<Value>{static_cast<double>(variableAnchorOffset.offset[0]),
                                                          static_cast<double>(variableAnchorOffset.offset[1])});
            }
            return operator()(components);
        }

    private:
        Napi::Env env;
    };

    return Value::visit(value, ToValue(env));
}
