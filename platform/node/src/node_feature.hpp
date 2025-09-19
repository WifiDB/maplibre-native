#pragma once

#include <mbgl/util/feature.hpp>
#include <napi.h>

namespace node_mbgl {

Napi::Value toJS(Napi::Env env, const mbgl::Value&);
Napi::Object toJS(Napi::Env env, const mbgl::Feature&);
Napi::Object toJS(Napi::Env env, const mbgl::Feature::geometry_type&);
Napi::Object toJS(Napi::Env env, const mbgl::PropertyMap&);

} // namespace node_mbgl
