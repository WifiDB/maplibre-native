#include "node_map.hpp"
#include "node_request.hpp"
#include "node_feature.hpp"
#include "node_conversion.hpp"

#include <mbgl/renderer/renderer.hpp>
#include <mbgl/gfx/headless_frontend.hpp>
#include <mbgl/style/conversion/source.hpp>
#include <mbgl/style/conversion/layer.hpp>
#include <mbgl/style/conversion/filter.hpp>
#include <mbgl/style/conversion/light.hpp>

#include <mbgl/style/layers/background_layer.hpp>
#include <mbgl/style/layers/circle_layer.hpp>
#include <mbgl/style/layers/fill_layer.hpp>
#include <mbgl/style/layers/fill_extrusion_layer.hpp>
#include <mbgl/style/layers/heatmap_layer.hpp>
#include <mbgl/style/layers/hillshade_layer.hpp>
#include <mbgl/style/layers/line_layer.hpp>
#include <mbgl/style/layers/raster_layer.hpp>
#include <mbgl/style/layers/symbol_layer.hpp>

#include <mbgl/map/map.hpp>
#include <mbgl/map/map_observer.hpp>
#include <mbgl/storage/file_source_manager.hpp>
#include <mbgl/storage/resource_options.hpp>
#include <mbgl/style/image.hpp>
#include <mbgl/style/light.hpp>
#include <mbgl/style/style.hpp>
#include <mbgl/util/async_request.hpp>
#include <mbgl/util/exception.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/premultiply.hpp>

namespace node_mbgl {

struct NodeMap::RenderOptions {
    double zoom = 0;
    double bearing = 0;
    mbgl::style::Light light;
    double pitch = 0;
    double latitude = 0;
    double longitude = 0;
    mbgl::Size size = {512, 512};
    bool axonometric = false;
    double xSkew = 0;
    double ySkew = 1;
    std::vector<std::string> classes;
    mbgl::MapDebugOptions debugOptions = mbgl::MapDebugOptions::NoDebug;
};

Napi::FunctionReference NodeMap::constructor;

static const char* releasedMessage() {
    return "Map resources have already been released";
}

void NodeMapObserver::onDidFailLoadingMap(mbgl::MapLoadError error, const std::string& description) {
    Napi::Env env = Napi::GetCurrent(env);
    switch (error) {
        case mbgl::MapLoadError::StyleParseError:
            Napi::TypeError::New(env, description.c_str()).ThrowAsJavaScriptException();
            break;
        default:
            Napi::Error::New(env, description.c_str()).ThrowAsJavaScriptException();
    }
}

void NodeMap::Init(Napi::Env env, Napi::Object exports) {
    Napi::HandleScope scope(env);

    Napi::Function func = DefineClass(env, "Map", {
        InstanceMethod("load", &NodeMap::Load),
        InstanceMethod("loaded", &NodeMap::Loaded),
        InstanceMethod("render", &NodeMap::Render),
        InstanceMethod("release", &NodeMap::Release),
        InstanceMethod("cancel", &NodeMap::Cancel),
        InstanceMethod("addSource", &NodeMap::AddSource),
        InstanceMethod("removeSource", &NodeMap::RemoveSource),
        InstanceMethod("addLayer", &NodeMap::AddLayer),
        InstanceMethod("removeLayer", &NodeMap::RemoveLayer),
        InstanceMethod("addImage", &NodeMap::AddImage),
        InstanceMethod("removeImage", &NodeMap::RemoveImage),
        InstanceMethod("setLayerZoomRange", &NodeMap::SetLayerZoomRange),
        InstanceMethod("setLayoutProperty", &NodeMap::SetLayerProperty),
        InstanceMethod("setPaintProperty", &NodeMap::SetLayerProperty),
        InstanceMethod("setFilter", &NodeMap::SetFilter),
        InstanceMethod("setSize", &NodeMap::SetSize),
        InstanceMethod("setCenter", &NodeMap::SetCenter),
        InstanceMethod("setZoom", &NodeMap::SetZoom),
        InstanceMethod("setBearing", &NodeMap::SetBearing),
        InstanceMethod("setPitch", &NodeMap::SetPitch),
        InstanceMethod("setLight", &NodeMap::SetLight),
        InstanceMethod("setAxonometric", &NodeMap::SetAxonometric),
        InstanceMethod("setXSkew", &NodeMap::SetXSkew),
        InstanceMethod("setYSkew", &NodeMap::SetYSkew),
        InstanceMethod("setFeatureState", &NodeMap::SetFeatureState),
        InstanceMethod("getFeatureState", &NodeMap::GetFeatureState),
        InstanceMethod("removeFeatureState", &NodeMap::RemoveFeatureState),
        InstanceMethod("dumpDebugLogs", &NodeMap::DumpDebugLogs),
        InstanceMethod("queryRenderedFeatures", &NodeMap::QueryRenderedFeatures)
    });

    constructor = Napi::Persistent(func);
    constructor.SuppressDestruct();

    exports.Set("Map", func);
}

/**
 * A request object, given to the `request` handler of a map, is an
 * encapsulation of a URL and type of a resource that the map asks you to load.
 *
 * The `kind` property is one of
 *
 * "Unknown": 0,
 * "Style": 1,
 * "Source": 2,
 * "Tile": 3,
 * "Glyphs": 4,
 * "SpriteImage": 5,
 * "SpriteJSON": 6
 *
 * @typedef
 * @name Request
 * @property {string} url
 * @property {number} kind
 */

/**
 * Mapbox GL object: this object loads stylesheets and renders them into
 * images.
 *
 * @class
 * @name Map
 * @param {Object} options
 * @param {Function} options.request a method used to request resources
 * over the internet
 * @param {Function} [options.cancel]
 * @param {number} options.ratio pixel ratio
 * @example
 * var map = new mbgl.Map({ request: function() {} });
 * map.load(require('./test/fixtures/style.json'));
 * map.render({}, function(err, image) {
 * if (err) throw err;
 * fs.writeFileSync('image.png', image);
 * });
 */
NodeMap::NodeMap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<NodeMap>(info),
      async(Napi::AsyncWork::New(info.Env(), "mbgl:RenderRequest")),
      pixelRatio(1.0) {
    if (info.Length() > 0 && !info[0].IsObject()) {
        Napi::TypeError::New(info.Env(), "Requires an options object as first argument").ThrowAsJavaScriptException();
        return;
    }

    Napi::Object options = info.Length() > 0 ? info[0].As<Napi::Object>() : Napi::Object::New(info.Env());

    // Check that if 'request' is set it must be a function, if 'cancel' is set
    // it must be a function and if 'ratio' is set it must be a number.
    if (options.Has("request") && !options.Get("request").IsFunction()) {
        Napi::Error::New(info.Env(), "Options object 'request' property must be a function").ThrowAsJavaScriptException();
        return;
    }

    if (options.Has("cancel") && !options.Get("cancel").IsFunction()) {
        Napi::Error::New(info.Env(), "Options object 'cancel' property must be a function").ThrowAsJavaScriptException();
        return;
    }

    if (options.Has("ratio") && !options.Get("ratio").IsNumber()) {
        Napi::Error::New(info.Env(), "Options object 'ratio' property must be a number").ThrowAsJavaScriptException();
        return;
    }

    // Set the options object on the instance for later access
    info.This().Set("options", options);

    if (options.Has("request") && options.Get("request").IsFunction()) {
        mbgl::FileSourceManager::get()->registerFileSourceFactory(
            mbgl::FileSourceType::ResourceLoader,
            [](const mbgl::ResourceOptions& resourceOptions, const mbgl::ClientOptions&) {
                return std::make_unique<node_mbgl::NodeFileSource>(
                    reinterpret_cast<node_mbgl::NodeMap*>(resourceOptions.platformContext()));
            });
    }

    try {
        if (options.Has("ratio")) {
            pixelRatio = options.Get("ratio").As<Napi::Number>().DoubleValue();
        }

        frontend = std::make_unique<mbgl::HeadlessFrontend>(mbgl::Size{512, 512}, pixelRatio);
        map = std::make_unique<mbgl::Map>(*frontend,
                                          mapObserver,
                                          mbgl::MapOptions()
                                              .withSize(frontend->getSize())
                                              .withPixelRatio(pixelRatio)
                                              .withMapMode(mode)
                                              .withCrossSourceCollisions(crossSourceCollisions),
                                          mbgl::ResourceOptions().withPlatformContext(reinterpret_cast<void*>(this)),
                                          mbgl::ClientOptions());
    } catch (std::exception& ex) {
        Napi::Error::New(info.Env(), ex.what()).ThrowAsJavaScriptException();
    }
}

/**
 * Load a stylesheet
 *
 * @function
 * @name load
 * @param {string|Object} stylesheet either an object or a JSON representation
 * @param {Object} options
 * @param {boolean} options.defaultStyleCamera if true, sets the default style
 * camera
 * @returns {undefined} loads stylesheet into map
 * @throws {Error} if stylesheet is missing or invalid
 * @example
 * // providing an object
 * map.load(require('./test/fixtures/style.json'));
 *
 * // providing a string
 * map.load(fs.readFileSync('./test/fixtures/style.json', 'utf8'));
 */
void NodeMap::Load(const Napi::CallbackInfo& info) {
    auto nodeMap = NodeMap::Unwrap(info.This());
    if (!nodeMap->map) {
        Napi::Error::New(info.Env(), releasedMessage()).ThrowAsJavaScriptException();
        return;
    }

    // Reset the flag as this could be the second time
    // we are calling this (being the previous successful).
    nodeMap->loaded = false;

    if (info.Length() < 1) {
        Napi::TypeError::New(info.Env(), "Requires a map style as first argument").ThrowAsJavaScriptException();
        return;
    }

    std::string style;

    if (info[0].IsObject()) {
        style = info[0].As<Napi::Object>().ToString().Utf8Value();
    } else if (info[0].IsString()) {
        style = info[0].As<Napi::String>().Utf8Value();
    } else {
        Napi::TypeError::New(info.Env(), "First argument must be a string or object").ThrowAsJavaScriptException();
        return;
    }

    try {
        nodeMap->map->getStyle().loadJSON(style);
    } catch (const mbgl::util::StyleParseException& ex) {
        Napi::TypeError::New(info.Env(), ex.what()).ThrowAsJavaScriptException();
        return;
    } catch (const std::exception& ex) {
        Napi::Error::New(info.Env(), ex.what()).ThrowAsJavaScriptException();
        return;
    }

    if (info.Length() == 2) {
        if (!info[1].IsObject()) {
            Napi::TypeError::New(info.Env(), "Second argument must be an options object").ThrowAsJavaScriptException();
            return;
        }
        auto options = info[1].As<Napi::Object>();
        if (options.Has("defaultStyleCamera") && options.Get("defaultStyleCamera").IsBoolean()) {
            if (options.Get("cameraMutated").As<Napi::Boolean>().Value()) {
                nodeMap->map->jumpTo(nodeMap->map->getStyle().getDefaultCamera());
            }
        }
    }

    nodeMap->loaded = true;
    info.GetReturnValue().SetUndefined();
}

void NodeMap::Loaded(const Napi::CallbackInfo& info) {
    auto nodeMap = NodeMap::Unwrap(info.This());
    if (!nodeMap->map) {
        Napi::Error::New(info.Env(), releasedMessage()).ThrowAsJavaScriptException();
        return;
    }

    bool loaded = false;

    try {
        loaded = nodeMap->map->isFullyLoaded();
    } catch (const std::exception& ex) {
        Napi::Error::New(info.Env(), ex.what()).ThrowAsJavaScriptException();
        return;
    }

    info.GetReturnValue().Set(Napi::Boolean::New(info.Env(), loaded));
}

NodeMap::RenderOptions NodeMap::ParseOptions(Napi::Object obj) {
    RenderOptions options;

    if (obj.Has("zoom")) {
        options.zoom = obj.Get("zoom").As<Napi::Number>().DoubleValue();
    }

    if (obj.Has("bearing")) {
        options.bearing = obj.Get("bearing").As<Napi::Number>().DoubleValue();
    }

    if (obj.Has("pitch")) {
        options.pitch = obj.Get("pitch").As<Napi::Number>().DoubleValue();
    }

    if (obj.Has("light")) {
        auto lightObj = obj.Get("light");
        mbgl::style::conversion::Error conversionError;
        if (auto light = mbgl::style::conversion::convert<mbgl::style::Light>(lightObj, conversionError)) {
            options.light = *light;
        } else {
            throw std::move(conversionError);
        }
    }

    if (obj.Has("axonometric")) {
        options.axonometric = obj.Get("axonometric").As<Napi::Boolean>().Value();
    }

    if (obj.Has("skew")) {
        auto skewObj = obj.Get("skew");
        if (skewObj.IsArray()) {
            auto skew = skewObj.As<Napi::Array>();
            if (skew.Length() > 0) {
                options.xSkew = skew.Get(0u).As<Napi::Number>().DoubleValue();
            }
            if (skew.Length() > 1) {
                options.ySkew = skew.Get(1u).As<Napi::Number>().DoubleValue();
            }
        }
    }

    if (obj.Has("center")) {
        auto centerObj = obj.Get("center");
        if (centerObj.IsArray()) {
            auto center = centerObj.As<Napi::Array>();
            if (center.Length() > 0) {
                options.longitude = center.Get(0u).As<Napi::Number>().DoubleValue();
            }
            if (center.Length() > 1) {
                options.latitude = center.Get(1u).As<Napi::Number>().DoubleValue();
            }
        }
    }

    if (obj.Has("width")) {
        options.size.width = static_cast<uint32_t>(obj.Get("width").As<Napi::Number>().Uint32Value());
    }

    if (obj.Has("height")) {
        options.size.height = static_cast<uint32_t>(obj.Get("height").As<Napi::Number>().Uint32Value());
    }

    if (obj.Has("classes")) {
        auto classes = obj.Get("classes").As<Napi::Array>();
        const uint32_t length = classes.Length();
        options.classes.reserve(length);
        for (uint32_t i = 0; i < length; i++) {
            options.classes.emplace_back(classes.Get(i).As<Napi::String>().Utf8Value());
        }
    }

    if (obj.Has("debug")) {
        auto debug = obj.Get("debug").As<Napi::Object>();
        if (debug.Has("tileBorders") && debug.Get("tileBorders").As<Napi::Boolean>().Value()) {
            options.debugOptions = options.debugOptions | mbgl::MapDebugOptions::TileBorders;
        }
        if (debug.Has("parseStatus") && debug.Get("parseStatus").As<Napi::Boolean>().Value()) {
            options.debugOptions = options.debugOptions | mbgl::MapDebugOptions::ParseStatus;
        }
        if (debug.Has("timestamps") && debug.Get("timestamps").As<Napi::Boolean>().Value()) {
            options.debugOptions = options.debugOptions | mbgl::MapDebugOptions::Timestamps;
        }
        if (debug.Has("collision") && debug.Get("collision").As<Napi::Boolean>().Value()) {
            options.debugOptions = options.debugOptions | mbgl::MapDebugOptions::Collision;
        }
        if (debug.Has("overdraw") && debug.Get("overdraw").As<Napi::Boolean>().Value()) {
            options.debugOptions = mbgl::MapDebugOptions::Overdraw;
        }
    }

    return options;
}

class RenderWorker : public Napi::AsyncWorker {
public:
    RenderWorker(Napi::Function& callback, NodeMap* map, NodeMap::RenderOptions options)
        : Napi::AsyncWorker(callback), map(map), options(options) {}

    void Execute() override {
        try {
            if (map) {
                map->startRender(options);
            } else {
                SetError(releasedMessage());
            }
        } catch (const mbgl::util::StyleParseException& ex) {
            SetError(ex.what());
        } catch (const std::exception& ex) {
            SetError(ex.what());
        }
    }

    void OnOK() override {
        Napi::HandleScope scope(Env());
        if (map && map->image.data) {
            Napi::Buffer<uint8_t> pixels = Napi::Buffer<uint8_t>::New(
                Env(),
                map->image.data.get(),
                map->image.bytes(),
                [](Napi::Env, uint8_t* data) { delete[] data; },
                map->image.data.release());
            
            Callback().Call({Env().Null(), pixels});
        } else {
            Callback().Call({Napi::Error::New(Env(), "Didn't get an image").Value()});
        }
    }

    void OnError(const Napi::Error& e) override {
        Napi::HandleScope scope(Env());
        Callback().Call({e.Value()});
    }

private:
    NodeMap* map;
    NodeMap::RenderOptions options;
};

/**
 * Render an image from the currently-loaded style
 *
 * @name render
 * @param {Object} options
 * @param {number} [options.zoom=0]
 * @param {number} [options.width=512]
 * @param {number} [options.height=512]
 * @param {Array<number>} [options.center=[0,0]] longitude, latitude center
 * of the map
 * @param {number} [options.bearing=0] rotation
 * @param {Array<string>} [options.classes=[]] style classes
 * @param {Function} callback
 * @returns {undefined} calls callback
 * @throws {Error} if stylesheet is not loaded or if map is already rendering
 */
void NodeMap::Render(const Napi::CallbackInfo& info) {
    auto nodeMap = NodeMap::Unwrap(info.This());
    if (!nodeMap->map) {
        Napi::Error::New(info.Env(), releasedMessage()).ThrowAsJavaScriptException();
        return;
    }

    if (info.Length() <= 0 || (!info[0].IsObject() && !info[0].IsFunction())) {
        Napi::TypeError::New(info.Env(), "First argument must be an options object or a callback function").ThrowAsJavaScriptException();
        return;
    }

    if ((info.Length() <= 1 && !info[0].IsFunction()) || (info.Length() > 1 && !info[1].IsFunction())) {
        Napi::TypeError::New(info.Env(), "Second argument must be a callback function").ThrowAsJavaScriptException();
        return;
    }

    if (!nodeMap->loaded) {
        Napi::TypeError::New(info.Env(), "Style is not loaded").ThrowAsJavaScriptException();
        return;
    }

    if (nodeMap->req) {
        Napi::Error::New(info.Env(), "Map is currently processing a RenderRequest").ThrowAsJavaScriptException();
        return;
    }

    try {
        Napi::Function callback;
        NodeMap::RenderOptions options;
        if (info[0].IsFunction()) {
            callback = info[0].As<Napi::Function>();
        } else {
            options = ParseOptions(info[0].As<Napi::Object>());
            callback = info[1].As<Napi::Function>();
        }
        
        nodeMap->req = new RenderWorker(callback, nodeMap, options);
        nodeMap->req->Queue();
    } catch (const mbgl::style::conversion::Error& err) {
        Napi::TypeError::New(info.Env(), err.message.c_str()).ThrowAsJavaScriptException();
    } catch (const mbgl::util::StyleParseException& ex) {
        Napi::TypeError::New(info.Env(), ex.what()).ThrowAsJavaScriptException();
    } catch (const mbgl::util::Exception& ex) {
        Napi::Error::New(info.Env(), ex.what()).ThrowAsJavaScriptException();
    }

    info.GetReturnValue().SetUndefined();
}

void NodeMap::startRender(const NodeMap::RenderOptions& options) {
    frontend->setSize(options.size);
    map->setSize(options.size);

    mbgl::CameraOptions camera;
    camera.center = mbgl::LatLng{options.latitude, options.longitude};
    camera.zoom = options.zoom;
    camera.bearing = options.bearing;
    camera.pitch = options.pitch;

    auto projectionOptions =
        mbgl::ProjectionMode().withAxonometric(options.axonometric).withXSkew(options.xSkew).withYSkew(options.ySkew);

    map->setProjectionMode(projectionOptions);

    map->renderStill(camera, options.debugOptions, [this](const std::exception_ptr& eptr) {
        // Napi::AsyncWork will handle this
    });

    // We're retaining the object through the Napi::AsyncWork object.
}


/**
 * Clean up any resources used by a map instance.options
 * @name release
 * @returns {undefined}
 */
void NodeMap::Release(const Napi::CallbackInfo& info) {
    auto nodeMap = NodeMap::Unwrap(info.This());
    if (!nodeMap->map) {
        Napi::Error::New(info.Env(), releasedMessage()).ThrowAsJavaScriptException();
        return;
    }

    try {
        nodeMap->release();
    } catch (const std::exception& ex) {
        Napi::Error::New(info.Env(), ex.what()).ThrowAsJavaScriptException();
        return;
    }

    info.GetReturnValue().SetUndefined();
}

void NodeMap::release() {
    if (!map) throw mbgl::util::Exception(releasedMessage());

    map.reset();
    frontend.reset();
}

/**
 * Cancel an ongoing render request. The callback will be called with
 * the error set to "Canceled". Will throw if no rendering is in progress.
 * @name cancel
 * @returns {undefined}
 */
void NodeMap::Cancel(const Napi::CallbackInfo& info) {
    auto nodeMap = NodeMap::Unwrap(info.This());

    if (!nodeMap->map) {
        Napi::Error::New(info.Env(), releasedMessage()).ThrowAsJavaScriptException();
        return;
    }
    if (!nodeMap->req) {
        Napi::Error::New(info.Env(), "No render in progress").ThrowAsJavaScriptException();
        return;
    }

    try {
        nodeMap->cancel();
    } catch (const std::exception& ex) {
        Napi::Error::New(info.Env(), ex.what()).ThrowAsJavaScriptException();
        return;
    }

    info.GetReturnValue().SetUndefined();
}

void NodeMap::cancel() {
    // There is no direct Napi::AsyncWork::Cancel method, so we handle cancellation logic manually.
    if (req) {
        req->Cancel();
        req.reset();
        
        // The original code re-creates the entire map object which is very expensive.
        // It's a workaround for a missing core feature.
        // For this conversion, we will assume this is still the intended behavior
        // and replicate the logic.
        
        auto style = map->getStyle().getJSON();
        map.reset();
        frontend.reset();
        
        // Re-create the map
        frontend = std::make_unique<mbgl::HeadlessFrontend>(mbgl::Size{512, 512}, pixelRatio);
        map = std::make_unique<mbgl::Map>(*frontend,
                                           mapObserver,
                                           mbgl::MapOptions()
                                               .withSize(frontend->getSize())
                                               .withPixelRatio(pixelRatio)
                                               .withMapMode(mode)
                                               .withCrossSourceCollisions(crossSourceCollisions),
                                           mbgl::ResourceOptions().withPlatformContext(reinterpret_cast<void*>(this)),
                                           mbgl::ClientOptions());
        map->getStyle().loadJSON(style);
    }
}

void NodeMap::AddSource(const Napi::CallbackInfo& info) {
    using namespace mbgl::style;
    using namespace mbgl::style::conversion;

    auto nodeMap = NodeMap::Unwrap(info.This());
    if (!nodeMap->map) {
        Napi::Error::New(info.Env(), releasedMessage()).ThrowAsJavaScriptException();
        return;
    }

    if (info.Length() != 2) {
        Napi::TypeError::New(info.Env(), "Two argument required").ThrowAsJavaScriptException();
        return;
    }

    if (!info[0].IsString()) {
        Napi::TypeError::New(info.Env(), "First argument must be a string").ThrowAsJavaScriptException();
        return;
    }

    if (!info[1].IsObject()) {
        Napi::TypeError::New(info.Env(), "Second argument must be an object").ThrowAsJavaScriptException();
        return;
    }

    Error error;
    std::optional<std::unique_ptr<Source>> source = convert<std::unique_ptr<Source>>(
        info[1], error, info[0].As<Napi::String>().Utf8Value());
    if (!source) {
        Napi::TypeError::New(info.Env(), error.message.c_str()).ThrowAsJavaScriptException();
        return;
    }

    nodeMap->map->getStyle().addSource(std::move(*source));
}

void NodeMap::RemoveSource(const Napi::CallbackInfo& info) {
    auto nodeMap = NodeMap::Unwrap(info.This());
    if (!nodeMap->map) {
        Napi::Error::New(info.Env(), releasedMessage()).ThrowAsJavaScriptException();
        return;
    }

    if (info.Length() != 1) {
        Napi::TypeError::New(info.Env(), "One argument required").ThrowAsJavaScriptException();
        return;
    }

    if (!info[0].IsString()) {
        Napi::TypeError::New(info.Env(), "First argument must be a string").ThrowAsJavaScriptException();
        return;
    }

    nodeMap->map->getStyle().removeSource(info[0].As<Napi::String>().Utf8Value());
}

void NodeMap::AddLayer(const Napi::CallbackInfo& info) {
    using namespace mbgl::style;
    using namespace mbgl::style::conversion;

    auto nodeMap = NodeMap::Unwrap(info.This());
    if (!nodeMap->map) {
        Napi::Error::New(info.Env(), releasedMessage()).ThrowAsJavaScriptException();
        return;
    }

    if (info.Length() != 1) {
        Napi::TypeError::New(info.Env(), "One argument required").ThrowAsJavaScriptException();
        return;
    }

    Error error;
    std::optional<std::unique_ptr<Layer>> layer = convert<std::unique_ptr<Layer>>(info[0], error);
    if (!layer) {
        Napi::TypeError::New(info.Env(), error.message.c_str()).ThrowAsJavaScriptException();
        return;
    }

    nodeMap->map->getStyle().addLayer(std::move(*layer));
}

void NodeMap::RemoveLayer(const Napi::CallbackInfo& info) {
    auto nodeMap = NodeMap::Unwrap(info.This());
    if (!nodeMap->map) {
        Napi::Error::New(info.Env(), releasedMessage()).ThrowAsJavaScriptException();
        return;
    }

    if (info.Length() != 1) {
        Napi::TypeError::New(info.Env(), "One argument required").ThrowAsJavaScriptException();
        return;
    }

    if (!info[0].IsString()) {
        Napi::TypeError::New(info.Env(), "First argument must be a string").ThrowAsJavaScriptException();
        return;
    }

    nodeMap->map->getStyle().removeLayer(info[0].As<Napi::String>().Utf8Value());
}

void NodeMap::AddImage(const Napi::CallbackInfo& info) {
    using namespace mbgl::style;
    using namespace mbgl::style::conversion;

    auto nodeMap = NodeMap::Unwrap(info.This());
    if (!nodeMap->map) {
        Napi::Error::New(info.Env(), releasedMessage()).ThrowAsJavaScriptException();
        return;
    }

    if (info.Length() != 3) {
        Napi::TypeError::New(info.Env(), "Three arguments required").ThrowAsJavaScriptException();
        return;
    }

    if (!info[0].IsString()) {
        Napi::TypeError::New(info.Env(), "First argument must be a string").ThrowAsJavaScriptException();
        return;
    }

    if (!info[1].IsObject()) {
        Napi::TypeError::New(info.Env(), "Second argument must be an object").ThrowAsJavaScriptException();
        return;
    }

    if (!info[2].IsObject()) {
        Napi::TypeError::New(info.Env(), "Third argument must be an object").ThrowAsJavaScriptException();
        return;
    }

    auto optionObject = info[2].As<Napi::Object>();

    if (!optionObject.Has("height") || !optionObject.Get("height").IsNumber()) {
        Napi::TypeError::New(info.Env(), "height parameter required and must be a number").ThrowAsJavaScriptException();
        return;
    }

    if (!optionObject.Has("width") || !optionObject.Get("width").IsNumber()) {
        Napi::TypeError::New(info.Env(), "width parameter required and must be a number").ThrowAsJavaScriptException();
        return;
    }

    if (!optionObject.Has("pixelRatio") || !optionObject.Get("pixelRatio").IsNumber()) {
        Napi::TypeError::New(info.Env(), "pixelRatio parameter required and must be a number").ThrowAsJavaScriptException();
        return;
    }

    uint32_t imageHeight = optionObject.Get("height").As<Napi::Number>().Uint32Value();
    uint32_t imageWidth = optionObject.Get("width").As<Napi::Number>().Uint32Value();

    if (imageWidth > 1024 || imageHeight > 1024) {
        Napi::TypeError::New(info.Env(), "Max height and width is 1024").ThrowAsJavaScriptException();
        return;
    }

    bool sdf = false;
    if (optionObject.Has("sdf") && optionObject.Get("sdf").IsBoolean()) {
        sdf = optionObject.Get("sdf").As<Napi::Boolean>().Value();
    }
    
    // The rest of the function was not included in the original prompt,
    // so the conversion stops here.
}
