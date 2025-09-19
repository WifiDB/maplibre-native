#pragma once

#include <napi.h>

#include <mbgl/util/feature.hpp>
#include <mbgl/style/conversion/geojson.hpp>
#include <mbgl/style/conversion_impl.hpp>

namespace mbgl {
namespace style {
namespace conversion {

template <>
class ConversionTraits<Napi::Value> {
public:
    static bool isUndefined(const Napi::Value& value) {
        return value.IsUndefined() || value.IsNull();
    }

    static bool isArray(const Napi::Value& value) {
        return value.IsArray();
    }

    static std::size_t arrayLength(const Napi::Value& value) {
        return value.As<Napi::Array>().Length();
    }

    static Napi::Value arrayMember(const Napi::Value& value, std::size_t i) {
        return value.As<Napi::Array>().Get(static_cast<uint32_t>(i));
    }

    static bool isObject(const Napi::Value& value) {
        return value.IsObject() && !value.IsArray();
    }

    static std::optional<Napi::Value> objectMember(const Napi::Value& value, const char* name) {
        Napi::Object obj = value.As<Napi::Object>();
        if (!obj.Has(name)) {
            return {};
        }
        return { obj.Get(name) };
    }

    template <class Fn>
    static std::optional<Error> eachMember(const Napi::Value& value, Fn&& fn) {
        Napi::Object obj = value.As<Napi::Object>();
        Napi::Array names = obj.GetPropertyNames();
        for (uint32_t i = 0; i < names.Length(); ++i) {
            Napi::Value k_val = names.Get(i);
            Napi::Value v_val = obj.Get(k_val);
            std::optional<Error> result = fn(k_val.As<Napi::String>().Utf8Value(), std::move(v_val));
            if (result) {
                return result;
            }
        }
        return {};
    }

    static std::optional<bool> toBool(const Napi::Value& value) {
        if (!value.IsBoolean()) {
            return {};
        }
        return value.As<Napi::Boolean>().Value();
    }

    static std::optional<float> toNumber(const Napi::Value& value) {
        if (!value.IsNumber()) {
            return {};
        }
        return static_cast<float>(value.As<Napi::Number>().DoubleValue());
    }

    static std::optional<double> toDouble(const Napi::Value& value) {
        if (!value.IsNumber()) {
            return {};
        }
        return value.As<Napi::Number>().DoubleValue();
    }

    static std::optional<std::string> toString(const Napi::Value& value) {
        if (!value.IsString()) {
            return {};
        }
        return value.As<Napi::String>().Utf8Value();
    }

    static std::optional<Value> toValue(const Napi::Value& value) {
        if (value.IsBoolean()) {
            return { value.As<Napi::Boolean>().Value() };
        } else if (value.IsString()) {
            return { value.As<Napi::String>().Utf8Value() };
        } else if (value.IsNumber()) {
            Napi::Number num = value.As<Napi::Number>();
            double d = num.DoubleValue();
            if (d == static_cast<std::uint32_t>(d)) {
                return { static_cast<std::uint32_t>(d) };
            } else if (d == static_cast<std::int32_t>(d)) {
                return { static_cast<std::int32_t>(d) };
            } else {
                return { d };
            }
        } else {
            return {};
        }
    }
    
    static std::optional<GeoJSON> toGeoJSON(const Napi::Value& value, Error& error) {
        try {
            Napi::String stringified = Napi::JSON::Stringify(value.Env(), value.As<Napi::Object>());
            std::string string = stringified.Utf8Value();
            return parseGeoJSON(string, error);
        } catch (const Napi::Error& ex) {
            error = {ex.what()};
            return {};
        } catch (const std::exception& ex) {
            error = {ex.what()};
            return {};
        }
    }
};

template <class T, class... Args>
std::optional<T> convert(const Napi::Value& value, Error& error, Args&&... args) {
    return convert<T>(Convertible(value), error, std::forward<Args>(args)...);
}

} // namespace conversion
} // namespace style
} // namespace mbgl
