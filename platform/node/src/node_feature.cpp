#include "node_feature.hpp"

namespace node_mbgl {

using namespace mapbox::geometry;

using Value = mbgl::Value;
using Feature = mbgl::Feature;
using FeatureIdentifier = mbgl::FeatureIdentifier;
using Geometry = mbgl::Feature::geometry_type;
using GeometryCollection = mapbox::geometry::geometry_collection<double>;
using Properties = mbgl::PropertyMap;

template <class T>
struct ToType {
public:
    ToType(Napi::Env env_) : env(env_) {}

    Napi::String operator()(const empty&) { return type("Empty"); }
    Napi::String operator()(const point<T>&) { return type("Point"); }
    Napi::String operator()(const line_string<T>&) { return type("LineString"); }
    Napi::String operator()(const polygon<T>&) { return type("Polygon"); }
    Napi::String operator()(const multi_point<T>&) { return type("MultiPoint"); }
    Napi::String operator()(const multi_line_string<T>&) { return type("MultiLineString"); }
    Napi::String operator()(const multi_polygon<T>&) { return type("MultiPolygon"); }
    Napi::String operator()(const geometry_collection<T>&) { return type("GeometryCollection"); }

private:
    Napi::String type(const char* type_) {
        return Napi::String::New(env, type_);
    }
    Napi::Env env;
};

template <class T>
struct ToCoordinatesOrGeometries {
public:
    ToCoordinatesOrGeometries(Napi::Env env_) : env(env_) {}

    // Handles line_string, polygon, multi_point, multi_line_string,
    // multi_polygon, and geometry_collection.
    template <class E>
    Napi::Value operator()(const std::vector<E>& vector) {
        Napi::Array result = Napi::Array::New(env, vector.size());
        for (std::size_t i = 0; i < vector.size(); ++i) {
            result.Set(static_cast<uint32_t>(i), operator()(vector[i]));
        }
        return result;
    }

    Napi::Value operator()(const point<T>& point) {
        Napi::Array result = Napi::Array::New(env, 2);
        result.Set(0U, Napi::Number::New(env, point.x));
        result.Set(1U, Napi::Number::New(env, point.y));
        return result;
    }

    Napi::Value operator()(const geometry<T>& geometry) { return toJS(env, geometry); }

private:
    Napi::Env env;
};

struct ToValue {
    ToValue(Napi::Env env_) : env(env_) {}

    Napi::Value operator()(mbgl::NullValue) { return env.Null(); }
    Napi::Value operator()(bool t) { return Napi::Boolean::New(env, t); }
    Napi::Value operator()(int64_t t) { return operator()(static_cast<double>(t)); }
    Napi::Value operator()(uint64_t t) { return operator()(static_cast<double>(t)); }
    Napi::Value operator()(double t) { return Napi::Number::New(env, t); }
    Napi::Value operator()(const std::string& t) { return Napi::String::New(env, t); }

    Napi::Value operator()(const std::vector<mbgl::Value>& array) {
        Napi::Array result = Napi::Array::New(env, array.size());
        for (std::size_t i = 0; i < array.size(); i++) {
            result.Set(static_cast<uint32_t>(i), toJS(env, array[i]));
        }
        return result;
    }

    Napi::Value operator()(const std::unordered_map<std::string, mbgl::Value>& map) { return toJS(env, map); }

private:
    Napi::Env env;
};

Napi::Object toJS(Napi::Env env, const Geometry& geometry) {
    Napi::Object result = Napi::Object::New(env);

    result.Set("type", Geometry::visit(geometry, ToType<double>(env)));

    result.Set(geometry.is<GeometryCollection>() ? "geometries" : "coordinates",
               Geometry::visit(geometry, ToCoordinatesOrGeometries<double>(env)));

    return result;
}

Napi::Value toJS(Napi::Env env, const Value& value) {
    return Value::visit(value, ToValue(env));
}

Napi::Object toJS(Napi::Env env, const Properties& properties) {
    Napi::Object result = Napi::Object::New(env);
    for (const auto& property : properties) {
        result.Set(Napi::String::New(env, property.first), toJS(env, property.second));
    }
    return result;
}

Napi::Object toJS(Napi::Env env, const Feature& feature) {
    Napi::Object result = Napi::Object::New(env);

    result.Set("type", Napi::String::New(env, "Feature"));
    result.Set("geometry", toJS(env, feature.geometry));
    result.Set("properties", toJS(env, feature.properties));

    if (!feature.id.is<mbgl::NullValue>()) {
        result.Set("id", FeatureIdentifier::visit(feature.id, ToValue(env)));
    }

    result.Set("source", toJS(env, feature.source));
    if (!feature.sourceLayer.empty()) {
        result.Set("sourceLayer", toJS(env, feature.sourceLayer));
    }
    result.Set("state", toJS(env, feature.state));

    return result;
}

} // namespace node_mbgl
