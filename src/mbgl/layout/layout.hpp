#pragma once
#include <mbgl/style/image_impl.hpp>
#include <mbgl/text/glyph.hpp>
#include <mbgl/tile/geometry_tile_data.hpp>
#include <mbgl/text/glyph_manager.hpp>
#include <mbgl/util/containers.hpp>
#include <memory>

namespace mbgl {

class Bucket;
class BucketParameters;
class RenderLayer;
class FeatureIndex;
class LayerRenderData;
class GlyphManager;

// ADDED: Forward declarations for DashPositions
class DashEntry;
using DashPositions = mbgl::unordered_map<std::string, DashEntry>;

class Layout {
public:
    virtual ~Layout() = default;
    
    // MODIFIED: Added DashPositions parameter as second parameter
    virtual void createBucket(const ImagePositions&,
                              const DashPositions&,
                              std::unique_ptr<FeatureIndex>&,
                              mbgl::unordered_map<std::string, LayerRenderData>&,
                              bool,
                              bool,
                              const CanonicalTileID&) = 0;
    
    virtual void prepareSymbols(const GlyphMap&, const GlyphPositions&, const ImageMap&, const ImagePositions&) {}
    virtual void finalizeSymbols(HBShapeResults&) {}
    virtual bool needFinalizeSymbols() { return false; }
    virtual bool hasSymbolInstances() const { return true; }
    virtual bool hasDependencies() const = 0;
};

class LayoutParameters {
public:
    const BucketParameters& bucketParameters;
    std::shared_ptr<FontFaces> fontFaces;
    GlyphDependencies& glyphDependencies;
    ImageDependencies& imageDependencies;
    std::set<std::string>& availableImages;
};

} // namespace mbgl
