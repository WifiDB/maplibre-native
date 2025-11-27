#pragma once

#include <mbgl/shaders/line_layer_ubo.hpp>
#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/shaders/mtl/shader_program.hpp>

namespace mbgl {
namespace shaders {

constexpr auto lineShadePrelude = R"(

enum {
    idLineDrawableUBO = idDrawableReservedVertexOnlyUBO,
    idLineTilePropsUBO = idDrawableReservedFragmentOnlyUBO,
    idLineEvaluatedPropsUBO = drawableReservedUBOCount,
    idLineExpressionUBO,
    lineUBOCount
};

//
// Line

struct alignas(16) LineDrawableUBO {
    /*  0 */ float4x4 matrix;
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
static_assert(sizeof(LineDrawableUBO) == 6 * 16, "wrong size");

//
// Line gradient

struct alignas(16) LineGradientDrawableUBO {
    /*  0 */ float4x4 matrix;
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
static_assert(sizeof(LineGradientDrawableUBO) == 6 * 16, "wrong size");

//
// Line pattern

struct alignas(16) LinePatternDrawableUBO {
    /*  0 */ float4x4 matrix;
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
static_assert(sizeof(LinePatternDrawableUBO) == 6 * 16, "wrong size");

struct alignas(16) LinePatternTilePropsUBO {
    /*  0 */ float4 pattern_from;
    /* 16 */ float4 pattern_to;
    /* 32 */ float4 scale;
    /* 48 */ float2 texsize;
    /* 56 */ float fade;
    /* 60 */ float pad2;
    /* 64 */
};
static_assert(sizeof(LinePatternTilePropsUBO) == 4 * 16, "wrong size");

//
// Line SDF

struct alignas(16) LineSDFDrawableUBO {
    /*   0 */ float4x4 matrix;
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
static_assert(sizeof(LineSDFDrawableUBO) == 7 * 16, "wrong size");

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
static_assert(sizeof(LineSDFTilePropsUBO) == 2 * 16, "wrong size");

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
bool operator&(LineExpressionMask a, LineExpressionMask b) { return (uint32_t)a & (uint32_t)b; }

struct alignas(16) LineExpressionUBO {
    GPUExpression color;
    GPUExpression blur;
    GPUExpression opacity;
    GPUExpression gapwidth;
    GPUExpression offset;
    GPUExpression width;
    GPUExpression floorwidth;
    GPUExpression dasharrayFrom;
    GPUExpression dasharrayTo;
};
static_assert(sizeof(LineExpressionUBO) % 16 == 0, "wrong alignment");

/// Evaluated properties that do not depend on the tile
struct alignas(16) LineEvaluatedPropsUBO {
    /*  0 */ float4 color;
    /* 16 */ float blur;
    /* 20 */ float opacity;
    /* 24 */ float gapwidth;
    /* 28 */ float offset;
    /* 32 */ float width;
    /* 36 */ float floorwidth;
    /* 40 */ LineExpressionMask expressionMask;
    /* 44 */ float pad_evaluated_props_1;
    /* 48 */ float4 dasharray_from;
    /* 64 */ float4 dasharray_to;
    /* 80 */
};
static_assert(sizeof(LineEvaluatedPropsUBO) == 5 * 16, "wrong size");

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

)";

template <>
struct ShaderSource<BuiltIn::LineShader, gfx::Backend::Type::Metal> {
    static constexpr auto name = "LineShader";
    static constexpr auto vertexMainFunction = "vertexMain";
    static constexpr auto fragmentMainFunction = "fragmentMain";

    static const std::array<AttributeInfo, 8> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 0> textures;

    static constexpr auto prelude = lineShadePrelude;
    static constexpr auto source = R"(

struct VertexStage {
    short2 pos_normal [[attribute(lineUBOCount + 0)]];
    uchar4 data [[attribute(lineUBOCount + 1)]];

#if !defined(HAS_UNIFORM_u_color)
    float4 color [[attribute(lineUBOCount + 2)]];
#endif
#if !defined(HAS_UNIFORM_u_blur)
    float2 blur [[attribute(lineUBOCount + 3)]];
#endif
#if !defined(HAS_UNIFORM_u_opacity)
    float2 opacity [[attribute(lineUBOCount + 4)]];
#endif
#if !defined(HAS_UNIFORM_u_gapwidth)
    float2 gapwidth [[attribute(lineUBOCount + 5)]];
#endif
#if !defined(HAS_UNIFORM_u_offset)
    float2 offset [[attribute(lineUBOCount + 6)]];
#endif
#if !defined(HAS_UNIFORM_u_width)
    float2 width [[attribute(lineUBOCount + 7)]];
#endif
};

struct FragmentStage {
    float4 position [[position, invariant]];
    float2 width2;
    float2 normal;
    half gamma_scale;

#if !defined(HAS_UNIFORM_u_color)
    float4 color;
#endif
#if !defined(HAS_UNIFORM_u_blur)
    float blur;
#endif
#if !defined(HAS_UNIFORM_u_opacity)
    float opacity;
#endif
};

FragmentStage vertex vertexMain(thread const VertexStage vertx [[stage_in]],
                                device const GlobalPaintParamsUBO& paintParams [[buffer(idGlobalPaintParamsUBO)]],
                                device const uint32_t& uboIndex [[buffer(idGlobalUBOIndex)]],
                                device const LineDrawableUnionUBO* drawableVector [[buffer(idLineDrawableUBO)]],
                                device const LineEvaluatedPropsUBO& props [[buffer(idLineEvaluatedPropsUBO)]],
                                device const LineExpressionUBO& expr [[buffer(idLineExpressionUBO)]]) {

    device const LineDrawableUBO& drawable = drawableVector[uboIndex].lineDrawableUBO;

#if defined(HAS_UNIFORM_u_gapwidth)
    const auto exprGapWidth = (props.expressionMask & LineExpressionMask::GapWidth);
    const auto gapwidth = (exprGapWidth ? expr.gapwidth.eval(paintParams.map_zoom) : props.gapwidth) / 2.0;
#else
    const auto gapwidth = unpack_mix_float(vertx.gapwidth, drawable.gapwidth_t) / 2.0;
#endif

#if defined(HAS_UNIFORM_u_offset)
    const auto exprOffset = (props.expressionMask & LineExpressionMask::Offset);
    const auto offset   = (exprOffset ? expr.offset.eval(paintParams.map_zoom) : props.offset) * -1.0;
#else
    const auto offset   = unpack_mix_float(vertx.offset, drawable.offset_t) * -1.0;
#endif

#if defined(HAS_UNIFORM_u_width)
    const auto exprWidth = (props.expressionMask & LineExpressionMask::Width);
    const auto width    = exprWidth ? expr.width.eval(paintParams.map_zoom) : props.width;
#else
    const auto width    = unpack_mix_float(vertx.width, drawable.width_t);
#endif

    // the distance over which the line edge fades out.
    // Retina devices need a smaller distance to avoid aliasing.
    const float ANTIALIASING = 1.0 / DEVICE_PIXEL_RATIO / 2.0;

    const float2 a_extrude = float2(vertx.data.xy) - 128.0;
    const float a_direction = glMod(float(vertx.data.z), 4.0) - 1.0;
    const float2 pos = floor(float2(vertx.pos_normal) * 0.5);

    // x is 1 if it's a round cap, 0 otherwise
    // y is 1 if the normal points up, and -1 if it points down
    // We store these in the least significant bit of a_pos_normal
    const float2 normal = float2(vertx.pos_normal) - 2.0 * pos;
    const float2 v_normal = float2(normal.x, normal.y * 2.0 - 1.0);

    const float halfwidth = width / 2.0;
    const float inset = gapwidth + (gapwidth > 0.0 ? ANTIALIASING : 0.0);
    const float outset = gapwidth + halfwidth * (gapwidth > 0.0 ? 2.0 : 1.0) + (halfwidth == 0.0 ? 0.0 : ANTIALIASING);

    // Scale the extrusion vector down to a normal and then up by the line width of this vertex.
    const float2 dist = outset * a_extrude * LINE_NORMAL_SCALE;

    // Calculate the offset when drawing a line that is to the side of the actual line.
    // We do this by creating a vector that points towards the extrude, but rotate
    // it when we're drawing round end points (a_direction = -1 or 1) since their
    // extrude vector points in another direction.
    const float u = 0.5 * a_direction;
    const float t = 1.0 - abs(u);
    const float2 offset2 = offset * a_extrude * LINE_NORMAL_SCALE * v_normal.y * float2x2(t, -u, u, t);

    const float4 projected_extrude = drawable.matrix * float4(dist / drawable.ratio, 0.0, 0.0);
    const float4 position = drawable.matrix * float4(pos + offset2 / drawable.ratio, 0.0, 1.0) + projected_extrude;

    // calculate how much the perspective view squishes or stretches the extrude
    const float extrude_length_without_perspective = length(dist);
    const float extrude_length_with_perspective = length(projected_extrude.xy / position.w * paintParams.units_to_pixels);

    return {
        .position     = position,
        .width2       = float2(outset, inset),
        .normal       = v_normal,
        .gamma_scale  = half(extrude_length_without_perspective / extrude_length_with_perspective),

#if !defined(HAS_UNIFORM_u_color)
        .color        = unpack_mix_color(vertx.color, drawable.color_t),
#endif
#if !defined(HAS_UNIFORM_u_blur)
        .blur         = unpack_mix_float(vertx.blur, drawable.blur_t),
#endif
#if !defined(HAS_UNIFORM_u_opacity)
        .opacity      = unpack_mix_float(vertx.opacity, drawable.opacity_t),
#endif
    };
}

half4 fragment fragmentMain(FragmentStage in [[stage_in]],
                            device const GlobalPaintParamsUBO& paintParams [[buffer(idGlobalPaintParamsUBO)]],
                            device const LineEvaluatedPropsUBO& props [[buffer(idLineEvaluatedPropsUBO)]],
                            device const LineExpressionUBO& expr [[buffer(idLineExpressionUBO)]]) {

#if defined(OVERDRAW_INSPECTOR)
    return half4(1.0);
#endif

#if defined(HAS_UNIFORM_u_color)
    const auto exprColor = (props.expressionMask & LineExpressionMask::Color);
    const auto color     = exprColor ? expr.color.evalColor(paintParams.map_zoom) : props.color;
#else
    const float4 color = in.color;
#endif

#if defined(HAS_UNIFORM_u_blur)
    const auto exprBlur = (props.expressionMask & LineExpressionMask::Blur);
    const float blur = exprBlur ? expr.blur.eval(paintParams.map_zoom) : props.blur;
#else
    const float blur = in.blur;
#endif

#if defined(HAS_UNIFORM_u_opacity)
    const auto exprOpacity = (props.expressionMask & LineExpressionMask::Opacity);
    const float opacity = exprOpacity ? expr.opacity.eval(paintParams.map_zoom) : props.opacity;
#else
    const float opacity = in.opacity;
#endif

    // Calculate the distance of the pixel from the line in pixels.
    const float dist = length(in.normal) * in.width2.x;

    // Calculate the antialiasing fade factor. This is either when fading in the
    // line in case of an offset line (`v_width2.y`) or when fading out (`v_width2.x`)
    const float blur2 = (blur + 1.0 / DEVICE_PIXEL_RATIO) * in.gamma_scale;
    const float alpha = clamp(min(dist - (in.width2.y - blur2), in.width2.x - dist) / blur2, 0.0, 1.0);

    return half4(color * (alpha * opacity));
}
)";
};

template <>
struct ShaderSource<BuiltIn::LineGradientShader, gfx::Backend::Type::Metal> {
    static constexpr auto name = "LineGradientShader";
    static constexpr auto vertexMainFunction = "vertexMain";
    static constexpr auto fragmentMainFunction = "fragmentMain";

    static const std::array<AttributeInfo, 9> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 1> textures;

    static constexpr auto prelude = lineShadePrelude;
    static constexpr auto source = R"(

struct VertexStage {
    short2 pos_normal [[attribute(lineUBOCount + 0)]];
    uchar4 data [[attribute(lineUBOCount + 1)]];
    float uv_x [[attribute(lineUBOCount + 2)]];
    float split_index [[attribute(lineUBOCount + 3)]];

#if !defined(HAS_UNIFORM_u_blur)
    float2 blur [[attribute(lineUBOCount + 4)]];
#endif
#if !defined(HAS_UNIFORM_u_opacity)
    float2 opacity [[attribute(lineUBOCount + 5)]];
#endif
#if !defined(HAS_UNIFORM_u_gapwidth)
    float2 gapwidth [[attribute(lineUBOCount + 6)]];
#endif
#if !defined(HAS_UNIFORM_u_offset)
    float2 offset [[attribute(lineUBOCount + 7)]];
#endif
#if !defined(HAS_UNIFORM_u_width)
    float2 width [[attribute(lineUBOCount + 8)]];
#endif
};

struct FragmentStage {
    float4 position [[position, invariant]];
    float2 width2;
    float2 normal;
    half2 uv;
    half gamma_scale;

#if !defined(HAS_UNIFORM_u_blur)
    float blur;
#endif
#if !defined(HAS_UNIFORM_u_opacity)
    float opacity;
#endif
};

FragmentStage vertex vertexMain(thread const VertexStage vertx [[stage_in]],
                                device const GlobalPaintParamsUBO& paintParams [[buffer(idGlobalPaintParamsUBO)]],
                                device const uint32_t& uboIndex [[buffer(idGlobalUBOIndex)]],
                                device const LineDrawableUnionUBO* drawableVector [[buffer(idLineDrawableUBO)]],
                                device const LineEvaluatedPropsUBO& props [[buffer(idLineEvaluatedPropsUBO)]],
                                device const LineExpressionUBO& expr [[buffer(idLineExpressionUBO)]],
                                constant float& image_height [[buffer(idLineEvaluatedPropsUBO + 1)]]) {

    device const LineGradientDrawableUBO& drawable = drawableVector[uboIndex].lineGradientDrawableUBO;

#if defined(HAS_UNIFORM_u_gapwidth)
    const auto exprGapWidth = (props.expressionMask & LineExpressionMask::GapWidth);
    const auto gapwidth = (exprGapWidth ? expr.gapwidth.eval(paintParams.map_zoom) : props.gapwidth) / 2.0;
#else
    const auto gapwidth = unpack_mix_float(vertx.gapwidth, drawable.gapwidth_t) / 2.0;
#endif

#if defined(HAS_UNIFORM_u_offset)
    const auto exprOffset = (props.expressionMask & LineExpressionMask::Offset);
    const auto offset   = (exprOffset ? expr.offset.eval(paintParams.map_zoom) : props.offset) * -1.0;
#else
    const auto offset   = unpack_mix_float(vertx.offset, drawable.offset_t) * -1.0;
#endif

#if defined(HAS_UNIFORM_u_width)
    const auto exprWidth = (props.expressionMask & LineExpressionMask::Width);
    const auto width    = exprWidth ? expr.width.eval(paintParams.map_zoom) : props.width;
#else
    const auto width    = unpack_mix_float(vertx.width, drawable.width_t);
#endif

    // the distance over which the line edge fades out.
    // Retina devices need a smaller distance to avoid aliasing.
    const float ANTIALIASING = 1.0 / DEVICE_PIXEL_RATIO / 2.0;

    const float2 a_extrude = float2(vertx.data.xy) - 128.0;
    const float a_direction = glMod(float(vertx.data.z), 4.0) - 1.0;
    const float2 pos = floor(float2(vertx.pos_normal) * 0.5);

    // x is 1 if it's a round cap, 0 otherwise
    // y is 1 if the normal points up, and -1 if it points down
    // We store these in the least significant bit of a_pos_normal
    const float2 normal = float2(vertx.pos_normal) - 2.0 * pos;
    const float2 v_normal = float2(normal.x, normal.y * 2.0 - 1.0);

    const float halfwidth = width / 2.0;
    const float inset = gapwidth + (gapwidth > 0.0 ? ANTIALIASING : 0.0);
    const float outset = gapwidth + halfwidth * (gapwidth > 0.0 ? 2.0 : 1.0) + (halfwidth == 0.0 ? 0.0 : ANTIALIASING);

    // Scale the extrusion vector down to a normal and then up by the line width of this vertex.
    const float2 dist = outset * a_extrude * LINE_NORMAL_SCALE;

    // Calculate the offset when drawing a line that is to the side of the actual line.
    // We do this by creating a vector that points towards the extrude, but rotate
    // it when we're drawing round end points (a_direction = -1 or 1) since their
    // extrude vector points in another direction.
    const float u = 0.5 * a_direction;
    const float t = 1.0 - abs(u);
    const float2 offset2 = offset * a_extrude * LINE_NORMAL_SCALE * v_normal.y * float2x2(t, -u, u, t);

    const float4 projected_extrude = drawable.matrix * float4(dist / drawable.ratio, 0.0, 0.0);
    const float4 position = drawable.matrix * float4(pos + offset2 / drawable.ratio, 0.0, 1.0) + projected_extrude;

    // calculate how much the perspective view squishes or stretches the extrude
    const float extrude_length_without_perspective = length(dist);
    const float extrude_length_with_perspective = length(projected_extrude.xy / position.w * paintParams.units_to_pixels);

    const float texel_height = 1.0 / image_height;
    const float half_texel_height = 0.5 * texel_height;
    const float2 uv = float2(vertx.uv_x, vertx.split_index * texel_height - half_texel_height);

    return {
        .position     = position,
        .width2       = float2(outset, inset),
        .normal       = v_normal,
        .uv           = half2(uv),
        .gamma_scale  = half(extrude_length_without_perspective / extrude_length_with_perspective),

#if !defined(HAS_UNIFORM_u_blur)
        .blur         = unpack_mix_float(vertx.blur, drawable.blur_t),
#endif
#if !defined(HAS_UNIFORM_u_opacity)
        .opacity      = unpack_mix_float(vertx.opacity, drawable.opacity_t),
#endif
    };
}

half4 fragment fragmentMain(FragmentStage in [[stage_in]],
                            device const GlobalPaintParamsUBO& paintParams [[buffer(idGlobalPaintParamsUBO)]],
                            device const LineEvaluatedPropsUBO& props [[buffer(idLineEvaluatedPropsUBO)]],
                            device const LineExpressionUBO& expr [[buffer(idLineExpressionUBO)]],
                            texture2d<float, access::sample> image0 [[texture(0)]],
                            sampler image0_sampler [[sampler(0)]]) {

#if defined(OVERDRAW_INSPECTOR)
    return half4(1.0);
#endif

#if defined(HAS_UNIFORM_u_blur)
    const auto exprBlur = (props.expressionMask & LineExpressionMask::Blur);
    const float blur = exprBlur ? expr.blur.eval(paintParams.map_zoom) : props.blur;
#else
    const float blur = in.blur;
#endif

#if defined(HAS_UNIFORM_u_opacity)
    const auto exprOpacity = (props.expressionMask & LineExpressionMask::Opacity);
    const float opacity = exprOpacity ? expr.opacity.eval(paintParams.map_zoom) : props.opacity;
#else
    const float opacity = in.opacity;
#endif

    // Calculate the distance of the pixel from the line in pixels.
    const float dist = length(in.normal) * in.width2.x;

    // Calculate the antialiasing fade factor. This is either when fading in the
    // line in case of an offset line (`v_width2.y`) or when fading out (`v_width2.x`)
    const float blur2 = (blur + 1.0 / DEVICE_PIXEL_RATIO) * in.gamma_scale;
    const float alpha = clamp(min(dist - (in.width2.y - blur2), in.width2.x - dist) / blur2, 0.0, 1.0);

    const float4 color = image0.sample(image0_sampler, float2(in.uv));

    return half4(color * (alpha * opacity));
}
)";
};

template <>
struct ShaderSource<BuiltIn::LinePatternShader, gfx::Backend::Type::Metal> {
    static constexpr auto name = "LinePatternShader";
    static constexpr auto vertexMainFunction = "vertexMain";
    static constexpr auto fragmentMainFunction = "fragmentMain";

    static const std::array<AttributeInfo, 10> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 1> textures;

    static constexpr auto prelude = lineShadePrelude;
    static constexpr auto source = R"(

struct VertexStage {
    short2 pos_normal [[attribute(lineUBOCount + 0)]];
    uchar4 data [[attribute(lineUBOCount + 1)]];

#if !defined(HAS_UNIFORM_u_blur)
    float2 blur [[attribute(lineUBOCount + 2)]];
#endif
#if !defined(HAS_UNIFORM_u_opacity)
    float2 opacity [[attribute(lineUBOCount + 3)]];
#endif
#if !defined(HAS_UNIFORM_u_gapwidth)
    float2 gapwidth [[attribute(lineUBOCount + 4)]];
#endif
#if !defined(HAS_UNIFORM_u_offset)
    float2 offset [[attribute(lineUBOCount + 5)]];
#endif
#if !defined(HAS_UNIFORM_u_width)
    float2 width [[attribute(lineUBOCount + 6)]];
#endif
#if !defined(HAS_UNIFORM_u_pattern_from)
    float4 pattern_from [[attribute(lineUBOCount + 7)]];
#endif
#if !defined(HAS_UNIFORM_u_pattern_to)
    float4 pattern_to [[attribute(lineUBOCount + 8)]];
#endif
    float2 tile_units [[attribute(lineUBOCount + 9)]];
};

struct FragmentStage {
    float4 position [[position, invariant]];
    float2 width2;
    float2 normal;
    half gamma_scale;
    float2 pos_a;
    float2 pos_b;

#if !defined(HAS_UNIFORM_u_blur)
    float blur;
#endif
#if !defined(HAS_UNIFORM_u_opacity)
    float opacity;
#endif
};

FragmentStage vertex vertexMain(thread const VertexStage vertx [[stage_in]],
                                device const GlobalPaintParamsUBO& paintParams [[buffer(idGlobalPaintParamsUBO)]],
                                device const uint32_t& uboIndex [[buffer(idGlobalUBOIndex)]],
                                device const LineDrawableUnionUBO* drawableVector [[buffer(idLineDrawableUBO)]],
                                device const LineTilePropsUnionUBO* tilePropsVector [[buffer(idLineTilePropsUBO)]],
                                device const LineEvaluatedPropsUBO& props [[buffer(idLineEvaluatedPropsUBO)]],
                                device const LineExpressionUBO& expr [[buffer(idLineExpressionUBO)]]) {

    device const LinePatternDrawableUBO& drawable = drawableVector[uboIndex].linePatternDrawableUBO;
    device const LinePatternTilePropsUBO& tileProps = tilePropsVector[uboIndex].linePatternTilePropsUBO;

#if defined(HAS_UNIFORM_u_gapwidth)
    const auto exprGapWidth = (props.expressionMask & LineExpressionMask::GapWidth);
    const auto gapwidth = (exprGapWidth ? expr.gapwidth.eval(paintParams.map_zoom) : props.gapwidth) / 2.0;
#else
    const auto gapwidth = unpack_mix_float(vertx.gapwidth, drawable.gapwidth_t) / 2.0;
#endif

#if defined(HAS_UNIFORM_u_offset)
    const auto exprOffset = (props.expressionMask & LineExpressionMask::Offset);
    const auto offset   = (exprOffset ? expr.offset.eval(paintParams.map_zoom) : props.offset) * -1.0;
#else
    const auto offset   = unpack_mix_float(vertx.offset, drawable.offset_t) * -1.0;
#endif

#if defined(HAS_UNIFORM_u_width)
    const auto exprWidth = (props.expressionMask & LineExpressionMask::Width);
    const auto width    = exprWidth ? expr.width.eval(paintParams.map_zoom) : props.width;
#else
    const auto width    = unpack_mix_float(vertx.width, drawable.width_t);
#endif

#if defined(HAS_UNIFORM_u_pattern_from)
    const float4 pattern_from = props.pattern_from;
#else
    const float4 pattern_from = unpack_mix_vec4(vertx.pattern_from, drawable.pattern_from_t);
#endif

#if defined(HAS_UNIFORM_u_pattern_to)
    const float4 pattern_to = props.pattern_to;
#else
    const float4 pattern_to = unpack_mix_vec4(vertx.pattern_to, drawable.pattern_to_t);
#endif

    // the distance over which the line edge fades out.
    // Retina devices need a smaller distance to avoid aliasing.
    const float ANTIALIASING = 1.0 / DEVICE_PIXEL_RATIO / 2.0;
    const float LINE_DISTANCE_SCALE = 2.0;

    const float2 a_extrude = float2(vertx.data.xy) - 128.0;
    const float a_direction = glMod(float(vertx.data.z), 4.0) - 1.0;
    float linesofar = (floor(float(vertx.data.z) / 4.0) + float(vertx.data.w) * 64.0) * LINE_DISTANCE_SCALE;
    const float2 pos = floor(float2(vertx.pos_normal) * 0.5);

    // x is 1 if it's a round cap, 0 otherwise
    // y is 1 if the normal points up, and -1 if it points down
    // We store these in the least significant bit of a_pos_normal
    const float2 normal = float2(vertx.pos_normal) - 2.0 * pos;
    const float2 v_normal = float2(normal.x, normal.y * 2.0 - 1.0);

    const float halfwidth = width / 2.0;
    const float inset = gapwidth + (gapwidth > 0.0 ? ANTIALIASING : 0.0);
    const float outset = gapwidth + halfwidth * (gapwidth > 0.0 ? 2.0 : 1.0) + (halfwidth == 0.0 ? 0.0 : ANTIALIASING);

    // Scale the extrusion vector down to a normal and then up by the line width of this vertex.
    const float2 dist = outset * a_extrude * LINE_NORMAL_SCALE;

    // Calculate the offset when drawing a line that is to the side of the actual line.
    // We do this by creating a vector that points towards the extrude, but rotate
    // it when we're drawing round end points (a_direction = -1 or 1) since their
    // extrude vector points in another direction.
    const float u = 0.5 * a_direction;
    const float t = 1.0 - abs(u);
    const float2 offset2 = offset * a_extrude * LINE_NORMAL_SCALE * v_normal.y * float2x2(t, -u, u, t);

    const float4 projected_extrude = drawable.matrix * float4(dist / drawable.ratio, 0.0, 0.0);
    const float4 position = drawable.matrix * float4(pos + offset2 / drawable.ratio, 0.0, 1.0) + projected_extrude;

    // calculate how much the perspective view squishes or stretches the extrude
    const float extrude_length_without_perspective = length(dist);
    const float extrude_length_with_perspective = length(projected_extrude.xy / position.w * paintParams.units_to_pixels);

    const float2 pattern_tl_a = pattern_from.xy;
    const float2 pattern_br_a = pattern_from.zw;
    const float2 pattern_tl_b = pattern_to.xy;
    const float2 pattern_br_b = pattern_to.zw;

    float pixelOffsetA = ((linesofar / vertx.tile_units.y) + v_normal.y * width / 2.0) / tileProps.scale.x;
    float pixelOffsetB = ((linesofar / vertx.tile_units.y) + v_normal.y * width / 2.0) / tileProps.scale.y;

    const float2 pos_a = mix(pattern_tl_a / tileProps.texsize, pattern_br_a / tileProps.texsize, fract(float2(pixelOffsetA, 0.5)));
    const float2 pos_b = mix(pattern_tl_b / tileProps.texsize, pattern_br_b / tileProps.texsize, fract(float2(pixelOffsetB, 0.5)));

    return {
        .position     = position,
        .width2       = float2(outset, inset),
        .normal       = v_normal,
        .gamma_scale  = half(extrude_length_without_perspective / extrude_length_with_perspective),
        .pos_a        = pos_a,
        .pos_b        = pos_b,

#if !defined(HAS_UNIFORM_u_blur)
        .blur         = unpack_mix_float(vertx.blur, drawable.blur_t),
#endif
#if !defined(HAS_UNIFORM_u_opacity)
        .opacity      = unpack_mix_float(vertx.opacity, drawable.opacity_t),
#endif
    };
}

half4 fragment fragmentMain(FragmentStage in [[stage_in]],
                            device const GlobalPaintParamsUBO& paintParams [[buffer(idGlobalPaintParamsUBO)]],
                            device const uint32_t& uboIndex [[buffer(idGlobalUBOIndex)]],
                            device const LineTilePropsUnionUBO* tilePropsVector [[buffer(idLineTilePropsUBO)]],
                            device const LineEvaluatedPropsUBO& props [[buffer(idLineEvaluatedPropsUBO)]],
                            device const LineExpressionUBO& expr [[buffer(idLineExpressionUBO)]],
                            texture2d<float, access::sample> image0 [[texture(0)]],
                            sampler image0_sampler [[sampler(0)]]) {

#if defined(OVERDRAW_INSPECTOR)
    return half4(1.0);
#endif

    device const LinePatternTilePropsUBO& tileProps = tilePropsVector[uboIndex].linePatternTilePropsUBO;

#if defined(HAS_UNIFORM_u_blur)
    const auto exprBlur = (props.expressionMask & LineExpressionMask::Blur);
    const float blur = exprBlur ? expr.blur.eval(paintParams.map_zoom) : props.blur;
#else
    const float blur = in.blur;
#endif

#if defined(HAS_UNIFORM_u_opacity)
    const auto exprOpacity = (props.expressionMask & LineExpressionMask::Opacity);
    const float opacity = exprOpacity ? expr.opacity.eval(paintParams.map_zoom) : props.opacity;
#else
    const float opacity = in.opacity;
#endif

    // Calculate the distance of the pixel from the line in pixels.
    const float dist = length(in.normal) * in.width2.x;

    // Calculate the antialiasing fade factor. This is either when fading in the
    // line in case of an offset line (`v_width2.y`) or when fading out (`v_width2.x`)
    const float blur2 = (blur + 1.0 / DEVICE_PIXEL_RATIO) * in.gamma_scale;
    const float alpha = clamp(min(dist - (in.width2.y - blur2), in.width2.x - dist) / blur2, 0.0, 1.0);

    const float4 color = mix(image0.sample(image0_sampler, in.pos_a), image0.sample(image0_sampler, in.pos_b), tileProps.fade);

    return half4(color * alpha * opacity);
}
)";
};

template <>
struct ShaderSource<BuiltIn::LineSDFShader, gfx::Backend::Type::Metal> {
    static constexpr auto name = "LineSDFShader";
    static constexpr auto vertexMainFunction = "vertexMain";
    static constexpr auto fragmentMainFunction = "fragmentMain";

    static const std::array<AttributeInfo, 11> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 1> textures;

    static constexpr auto prelude = lineShadePrelude;
    static constexpr auto source = R"(

struct VertexStage {
    short2 pos_normal [[attribute(lineUBOCount + 0)]];
    uchar4 data [[attribute(lineUBOCount + 1)]];

#if !defined(HAS_UNIFORM_u_color)
    float4 color [[attribute(lineUBOCount + 2)]];
#endif
#if !defined(HAS_UNIFORM_u_blur)
    float2 blur [[attribute(lineUBOCount + 3)]];
#endif
#if !defined(HAS_UNIFORM_u_opacity)
    float2 opacity [[attribute(lineUBOCount + 4)]];
#endif
#if !defined(HAS_UNIFORM_u_gapwidth)
    float2 gapwidth [[attribute(lineUBOCount + 5)]];
#endif
#if !defined(HAS_UNIFORM_u_offset)
    float2 offset [[attribute(lineUBOCount + 6)]];
#endif
#if !defined(HAS_UNIFORM_u_width)
    float2 width [[attribute(lineUBOCount + 7)]];
#endif
#if !defined(HAS_UNIFORM_u_floorwidth)
    float2 floorwidth [[attribute(lineUBOCount + 8)]];
#endif
#if !defined(HAS_UNIFORM_u_dasharray_from)
    float4 dasharray_from [[attribute(lineUBOCount + 9)]];
#endif
#if !defined(HAS_UNIFORM_u_dasharray_to)
    float4 dasharray_to [[attribute(lineUBOCount + 10)]];
#endif
};

struct FragmentStage {
    float4 position [[position, invariant]];
    float2 width2;
    float2 normal;
    float2 tex_a;
    float2 tex_b;
    half gamma_scale;

#if !defined(HAS_UNIFORM_u_color)
    float4 color;
#endif
#if !defined(HAS_UNIFORM_u_blur)
    float blur;
#endif
#if !defined(HAS_UNIFORM_u_opacity)
    float opacity;
#endif
#if !defined(HAS_UNIFORM_u_floorwidth)
    float floorwidth;
#endif
#if !defined(HAS_UNIFORM_u_dasharray_from)
    float4 dasharray_from;
#endif
#if !defined(HAS_UNIFORM_u_dasharray_to)
    float4 dasharray_to;
#endif
};

FragmentStage vertex vertexMain(thread const VertexStage vertx [[stage_in]],
                                device const GlobalPaintParamsUBO& paintParams [[buffer(idGlobalPaintParamsUBO)]],
                                device const uint32_t& uboIndex [[buffer(idGlobalUBOIndex)]],
                                device const LineDrawableUnionUBO* drawableVector [[buffer(idLineDrawableUBO)]],
                                device const LineTilePropsUnionUBO* tilePropsVector [[buffer(idLineTilePropsUBO)]],
                                device const LineEvaluatedPropsUBO& props [[buffer(idLineEvaluatedPropsUBO)]],
                                device const LineExpressionUBO& expr [[buffer(idLineExpressionUBO)]]) {

    device const LineSDFDrawableUBO& drawable = drawableVector[uboIndex].lineSDFDrawableUBO;
    device const LineSDFTilePropsUBO& tileProps = tilePropsVector[uboIndex].lineSDFTilePropsUBO;

#if defined(HAS_UNIFORM_u_gapwidth)
    const auto exprGapWidth = (props.expressionMask & LineExpressionMask::GapWidth);
    const auto gapwidth = (exprGapWidth ? expr.gapwidth.eval(paintParams.map_zoom) : props.gapwidth) / 2.0;
#else
    const auto gapwidth = unpack_mix_float(vertx.gapwidth, drawable.gapwidth_t) / 2.0;
#endif

#if defined(HAS_UNIFORM_u_offset)
    const auto exprOffset = (props.expressionMask & LineExpressionMask::Offset);
    const auto offset   = (exprOffset ? expr.offset.eval(paintParams.map_zoom) : props.offset) * -1.0;
#else
    const auto offset   = unpack_mix_float(vertx.offset, drawable.offset_t) * -1.0;
#endif

#if defined(HAS_UNIFORM_u_width)
    const auto exprWidth = (props.expressionMask & LineExpressionMask::Width);
    const auto width    = exprWidth ? expr.width.eval(paintParams.map_zoom) : props.width;
#else
    const auto width    = unpack_mix_float(vertx.width, drawable.width_t);
#endif

#if defined(HAS_UNIFORM_u_floorwidth)
    const auto exprFloorWidth = (props.expressionMask & LineExpressionMask::FloorWidth);
    const auto floorwidth = exprFloorWidth ? expr.floorwidth.eval(paintParams.map_zoom) : props.floorwidth;
#else
    const auto floorwidth = unpack_mix_float(vertx.floorwidth, drawable.floorwidth_t);
#endif

#if defined(HAS_UNIFORM_u_dasharray_from)
    const auto exprDasharrayFrom = (props.expressionMask & LineExpressionMask::DasharrayFrom);
    const auto dasharray_from = exprDasharrayFrom ? expr.dasharrayFrom.evalVec4(paintParams.map_zoom) : props.dasharray_from;
#else
    const auto dasharray_from = unpack_mix_vec4(vertx.dasharray_from, drawable.dasharray_from_t);
#endif

#if defined(HAS_UNIFORM_u_dasharray_to)
    const auto exprDasharrayTo = (props.expressionMask & LineExpressionMask::DasharrayTo);
    const auto dasharray_to = exprDasharrayTo ? expr.dasharrayTo.evalVec4(paintParams.map_zoom) : props.dasharray_to;
#else
    const auto dasharray_to = unpack_mix_vec4(vertx.dasharray_to, drawable.dasharray_to_t);
#endif

    // the distance over which the line edge fades out.
    // Retina devices need a smaller distance to avoid aliasing.
    const float ANTIALIASING = 1.0 / DEVICE_PIXEL_RATIO / 2.0;
    const float LINE_DISTANCE_SCALE = 2.0;

    const float2 a_extrude = float2(vertx.data.xy) - 128.0;
    const float a_direction = glMod(float(vertx.data.z), 4.0) - 1.0;
    float linesofar = (floor(float(vertx.data.z) / 4.0) + float(vertx.data.w) * 64.0) * LINE_DISTANCE_SCALE;
    const float2 pos = floor(float2(vertx.pos_normal) * 0.5);

    // x is 1 if it's a round cap, 0 otherwise
    // y is 1 if the normal points up, and -1 if it points down
    // We store these in the least significant bit of a_pos_normal
    const float2 normal = float2(vertx.pos_normal) - 2.0 * pos;
    const float2 v_normal = float2(normal.x, normal.y * 2.0 - 1.0);

    const float halfwidth = width / 2.0;
    const float inset = gapwidth + (gapwidth > 0.0 ? ANTIALIASING : 0.0);
    const float outset = gapwidth + halfwidth * (gapwidth > 0.0 ? 2.0 : 1.0) + (halfwidth == 0.0 ? 0.0 : ANTIALIASING);

    // Scale the extrusion vector down to a normal and then up by the line width of this vertex.
    const float2 dist = outset * a_extrude * LINE_NORMAL_SCALE;

    // Calculate the offset when drawing a line that is to the side of the actual line.
    // We do this by creating a vector that points towards the extrude, but rotate
    // it when we're drawing round end points (a_direction = -1 or 1) since their
    // extrude vector points in another direction.
    const float u = 0.5 * a_direction;
    const float t = 1.0 - abs(u);
    const float2 offset2 = offset * a_extrude * LINE_NORMAL_SCALE * v_normal.y * float2x2(t, -u, u, t);

    const float4 projected_extrude = drawable.matrix * float4(dist / drawable.ratio, 0.0, 0.0);
    const float4 position = drawable.matrix * float4(pos + offset2 / drawable.ratio, 0.0, 1.0) + projected_extrude;

    // calculate how much the perspective view squishes or stretches the extrude
    const float extrude_length_without_perspective = length(dist);
    const float extrude_length_with_perspective = length(projected_extrude.xy / position.w * paintParams.units_to_pixels);

    // Calculate texture coordinates for dash pattern
    const float u_patternscale_a_x = tileProps.tileratio / dasharray_from.w / tileProps.crossfade_from;
    const float u_patternscale_a_y = -dasharray_from.z / 2.0 / tileProps.lineatlas_height;
    const float u_patternscale_b_x = tileProps.tileratio / dasharray_to.w / tileProps.crossfade_to;
    const float u_patternscale_b_y = -dasharray_to.z / 2.0 / tileProps.lineatlas_height;
    
    const float2 v_tex_a = float2(linesofar * u_patternscale_a_x / floorwidth, 
                                   v_normal.y * u_patternscale_a_y + (dasharray_from.y + 0.5) / tileProps.lineatlas_height);
    const float2 v_tex_b = float2(linesofar * u_patternscale_b_x / floorwidth, 
                                   v_normal.y * u_patternscale_b_y + (dasharray_to.y + 0.5) / tileProps.lineatlas_height);

    return {
        .position     = position,
        .width2       = float2(outset, inset),
        .normal       = v_normal,
        .gamma_scale  = half(extrude_length_without_perspective / extrude_length_with_perspective),
        .tex_a        = v_tex_a,
        .tex_b        = v_tex_b,

#if !defined(HAS_UNIFORM_u_color)
        .color        = unpack_mix_color(vertx.color, drawable.color_t),
#endif
#if !defined(HAS_UNIFORM_u_blur)
        .blur         = unpack_mix_float(vertx.blur, drawable.blur_t),
#endif
#if !defined(HAS_UNIFORM_u_opacity)
        .opacity      = unpack_mix_float(vertx.opacity, drawable.opacity_t),
#endif
#if !defined(HAS_UNIFORM_u_floorwidth)
        .floorwidth   = floorwidth,
#endif
#if !defined(HAS_UNIFORM_u_dasharray_from)
        .dasharray_from = dasharray_from,
#endif
#if !defined(HAS_UNIFORM_u_dasharray_to)
        .dasharray_to = dasharray_to,
#endif
    };
}

half4 fragment fragmentMain(FragmentStage in [[stage_in]],
                            device const GlobalPaintParamsUBO& paintParams [[buffer(idGlobalPaintParamsUBO)]],
                            device const uint32_t& uboIndex [[buffer(idGlobalUBOIndex)]],
                            device const LineTilePropsUnionUBO* tilePropsVector [[buffer(idLineTilePropsUBO)]],
                            device const LineEvaluatedPropsUBO& props [[buffer(idLineEvaluatedPropsUBO)]],
                            device const LineExpressionUBO& expr [[buffer(idLineExpressionUBO)]],
                            texture2d<float, access::sample> image0 [[texture(0)]],
                            sampler image0_sampler [[sampler(0)]]) {

#if defined(OVERDRAW_INSPECTOR)
    return half4(1.0);
#endif

    device const LineSDFTilePropsUBO& tileProps = tilePropsVector[uboIndex].lineSDFTilePropsUBO;

#if defined(HAS_UNIFORM_u_color)
    const auto exprColor = (props.expressionMask & LineExpressionMask::Color);
    const auto color     = exprColor ? expr.color.evalColor(paintParams.map_zoom) : props.color;
#else
    const float4 color = in.color;
#endif

#if defined(HAS_UNIFORM_u_blur)
    const auto exprBlur = (props.expressionMask & LineExpressionMask::Blur);
    const float blur = exprBlur ? expr.blur.eval(paintParams.map_zoom) : props.blur;
#else
    const float blur = in.blur;
#endif

#if defined(HAS_UNIFORM_u_opacity)
    const auto exprOpacity = (props.expressionMask & LineExpressionMask::Opacity);
    const float opacity = exprOpacity ? expr.opacity.eval(paintParams.map_zoom) : props.opacity;
#else
    const float opacity = in.opacity;
#endif

#if defined(HAS_UNIFORM_u_floorwidth)
    const auto exprFloorWidth = (props.expressionMask & LineExpressionMask::FloorWidth);
    const auto floorwidth = exprFloorWidth ? expr.floorwidth.eval(paintParams.map_zoom) : props.floorwidth;
#else
    const auto floorwidth = in.floorwidth;
#endif

#if defined(HAS_UNIFORM_u_dasharray_from)
    const auto exprDasharrayFrom = (props.expressionMask & LineExpressionMask::DasharrayFrom);
    const auto dasharray_from = exprDasharrayFrom ? expr.dasharrayFrom.evalVec4(paintParams.map_zoom) : props.dasharray_from;
#else
    const auto dasharray_from = in.dasharray_from;
#endif

#if defined(HAS_UNIFORM_u_dasharray_to)
    const auto exprDasharrayTo = (props.expressionMask & LineExpressionMask::DasharrayTo);
    const auto dasharray_to = exprDasharrayTo ? expr.dasharrayTo.evalVec4(paintParams.map_zoom) : props.dasharray_to;
#else
    const auto dasharray_to = in.dasharray_to;
#endif

    // Calculate the distance of the pixel from the line in pixels.
    const float dist = length(in.normal) * in.width2.x;

    // Calculate the antialiasing fade factor. This is either when fading in the
    // line in case of an offset line (`v_width2.y`) or when fading out (`v_width2.x`)
    const float blur2 = (blur + 1.0 / DEVICE_PIXEL_RATIO) * in.gamma_scale;

    const float sdfdist_a = image0.sample(image0_sampler, in.tex_a).a;
    const float sdfdist_b = image0.sample(image0_sampler, in.tex_b).a;
    const float sdfdist = mix(sdfdist_a, sdfdist_b, tileProps.mix);
    
    const float sdfgamma = (tileProps.lineatlas_width / 256.0 / DEVICE_PIXEL_RATIO) / min(dasharray_from.w, dasharray_to.w);
    const float alpha = clamp(min(dist - (in.width2.y - blur2), in.width2.x - dist) / blur2, 0.0, 1.0) *
                        smoothstep(0.5 - sdfgamma / floorwidth, 0.5 + sdfgamma / floorwidth, sdfdist);

    return half4(color * (alpha * opacity));
}
)";
};

} // namespace shaders
} // namespace mbgl
