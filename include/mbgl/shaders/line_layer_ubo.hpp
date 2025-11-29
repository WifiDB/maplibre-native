#pragma once

#include <mbgl/shaders/layer_ubo.hpp>
#include <mbgl/style/property_expression.hpp>
#include <mbgl/util/bitmask_operations.hpp>
#include <mbgl/gfx/gpu_expression.hpp>

namespace mbgl {
namespace shaders {

//
// Line

struct alignas(16) LineDrawableUBO {
    /*  0 */ std::array<float, 4 * 4> matrix;
    /* 64 */ float ratio;

    // Interpolations
    /* 68 */ float color_t;
    /* 72 */ float blur_t;
    /* 76 */ float opacity_t;
    /* 80 */ float gapwidth_t;
    /* 84 */ float offset_t;
    /* 88 */ float width_t;
    /* 92 */ float pad1;
    /* 96 */
};
static_assert(sizeof(LineDrawableUBO) == 6 * 16);

//
// Line gradient

struct alignas(16) LineGradientDrawableUBO {
    /*  0 */ std::array<float, 4 * 4> matrix;
    /* 64 */ float ratio;

    // Interpolations
    /* 68 */ float blur_t;
    /* 72 */ float opacity_t;
    /* 76 */ float gapwidth_t;
    /* 80 */ float offset_t;
    /* 84 */ float width_t;
    /* 88 */ float pad1;
    /* 92 */ float pad2;
    /* 96 */
};
static_assert(sizeof(LineGradientDrawableUBO) == 6 * 16);

//
// Line pattern

struct alignas(16) LinePatternDrawableUBO {
    /*  0 */ std::array<float, 4 * 4> matrix;
    /* 64 */ float ratio;

    // Interpolations
    /* 68 */ float blur_t;
    /* 72 */ float opacity_t;
    /* 76 */ float gapwidth_t;
    /* 80 */ float offset_t;
    /* 84 */ float width_t;
    /* 88 */ float pattern_from_t;
    /* 92 */ float pattern_to_t;
    /* 96 */
};
static_assert(sizeof(LinePatternDrawableUBO) == 6 * 16);

struct alignas(16) LinePatternTilePropsUBO {
    /*  0 */ std::array<float, 4> pattern_from;
    /* 16 */ std::array<float, 4> pattern_to;
    /* 32 */ std::array<float, 4> scale;
    /* 48 */ std::array<float, 2> texsize;
    /* 56 */ float fade;
    /* 60 */ float pad1;
    /* 64 */
};
static_assert(sizeof(LinePatternTilePropsUBO) == 4 * 16);

//
// Line SDF

struct alignas(16) LineSDFDrawableUBO {
    /*   0 */ std::array<float, 4 * 4> matrix;
    /*  64 */ float ratio;
    
    // Interpolations
    /*  68 */ float color_t;
    /*  72 */ float blur_t;
    /*  76 */ float opacity_t;
    /*  80 */ float gapwidth_t;
    /*  84 */ float offset_t;
    /*  88 */ float width_t;
    /*  92 */ float floorwidth_t;
    /*  96 */ float dasharray_from_t;
    /* 100 */ float dasharray_to_t;
    /* 104 */ float pad_sdf_drawable_1;
    /* 108 */ float pad_sdf_drawable_2;
    /* 112 */
};
static_assert(sizeof(LineSDFDrawableUBO) == 7 * 16);

// Update LineSDFTilePropsUBO:
struct alignas(16) LineSDFTilePropsUBO {
    /*  0 */ float tileratio;
    /*  4 */ float crossfade_from;
    /*  8 */ float crossfade_to;
    /* 12 */ float lineatlas_width;
    /* 16 */ float lineatlas_height;
    /* 20 */ float mix;
    /* 24 */ float pad_sdf_tileprops_1;
    /* 28 */ float pad_sdf_tileprops_2; 
    /* 32 */
};
static_assert(sizeof(LineSDFTilePropsUBO) == 2 * 16);

/// Expression properties that do not depend on the tile
enum class LineExpressionMask : uint32_t {
    None = 0,
    Color = 1 << 0,
    Opacity = 1 << 1,
    Blur = 1 << 2,
    Width = 1 << 3,
    GapWidth = 1 << 4,
    FloorWidth = 1 << 5,
    Offset = 1 << 6,
    DasharrayFrom = 1 << 7,
    DasharrayTo = 1 << 8,
};

struct alignas(16) LineExpressionUBO {
    gfx::GPUExpression color;
    gfx::GPUExpression blur;
    gfx::GPUExpression opacity;
    gfx::GPUExpression gapwidth;
    gfx::GPUExpression offset;
    gfx::GPUExpression width;
    gfx::GPUExpression floorWidth;
    gfx::GPUExpression dasharrayFrom;
    gfx::GPUExpression dasharrayTo;
};
static_assert(sizeof(LineExpressionUBO) % 16 == 0);

struct alignas(16) LineEvaluatedPropsUBO {
    /*  0 */ Color color;
    /* 16 */ float blur;
    /* 20 */ float opacity;
    /* 24 */ float gapwidth;
    /* 28 */ float offset;
    /* 32 */ float width;
    /* 36 */ float floorwidth;
    /* 40 */ std::array<float, 4> dasharray_from;
    /* 56 */ std::array<float, 4> dasharray_to;
    /* 72 */ LineExpressionMask expressionMask;
    /* 76 */ float pad_evaluated_props_1;
    /* 80 */
};
static_assert(sizeof(LineEvaluatedPropsUBO) == 5 * 16);

#if MLN_UBO_CONSOLIDATION

union LineDrawableUnionUBO {
    LineDrawableUBO lineDrawableUBO;
    LineGradientDrawableUBO lineGradientDrawableUBO;
    LinePatternDrawableUBO linePatternDrawableUBO;
    LineSDFDrawableUBO lineSDFDrawableUBO;
};

union LineTilePropsUnionUBO {
    LinePatternTilePropsUBO linePatternTilePropsUBO;
    LineSDFTilePropsUBO lineSDFTilePropsUBO;
};

#endif

} // namespace shaders
} // namespace mbgl
