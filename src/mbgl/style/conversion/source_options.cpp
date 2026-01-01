#include <mbgl/style/conversion/source_options.hpp>
#include <mbgl/style/conversion_impl.hpp>
#include <mbgl/style/expression/dsl.hpp>

namespace mbgl {
namespace style {
namespace conversion {

std::optional<SourceOptions> Converter<SourceOptions>::operator()(const Convertible& value, Error& error) const {
    const auto encodingValue = objectMember(value, "encoding");
    if (encodingValue) {
        const auto encoding = toString(*encodingValue);
        if (encoding && *encoding == "terrarium") {
            return {{.rasterEncoding = Tileset::RasterEncoding::Terrarium}};
        } else if (encoding && *encoding == "mapbox") {
            return {{.rasterEncoding = Tileset::RasterEncoding::Mapbox}};
        } else if (encoding && *encoding == "custom") {
            // parse optional custom factors
            SourceOptions opts;
            opts.rasterEncoding = Tileset::RasterEncoding::Custom;
            if (const auto rf = objectMember(value, "redFactor")) {
                if (auto v = toNumber(*rf)) opts.redFactor = static_cast<float>(*v);
            }
            if (const auto gf = objectMember(value, "greenFactor")) {
                if (auto v = toNumber(*gf)) opts.greenFactor = static_cast<float>(*v);
            }
            if (const auto bf = objectMember(value, "blueFactor")) {
                if (auto v = toNumber(*bf)) opts.blueFactor = static_cast<float>(*v);
            }
            if (const auto bs = objectMember(value, "baseShift")) {
                if (auto v = toNumber(*bs)) opts.baseShift = static_cast<float>(*v);
            }
            return {{std::move(opts)}};
        } else if (encoding && *encoding == "mvt") {
            return {{.vectorEncoding = Tileset::VectorEncoding::Mapbox}};
        } else if (encoding && *encoding == "mlt") {
            return {{.vectorEncoding = Tileset::VectorEncoding::MLT}};
        } else {
            error.message =
                "invalid encoding - valid types are 'mapbox' and 'terrarium' for raster sources, 'mvt' and 'mlt' for "
                "vector sources";
            return std::nullopt;
        }
    }
    return {};
}

} // namespace conversion
} // namespace style
} // namespace mbgl
