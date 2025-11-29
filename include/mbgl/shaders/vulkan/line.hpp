#pragma once

#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/shaders/vulkan/shader_program.hpp>

namespace mbgl {
namespace shaders {

constexpr auto lineShadePrelude = R"(

#define idLineDrawableUBO           idDrawableReservedVertexOnlyUBO
#define idLineTilePropsUBO          idDrawableReservedFragmentOnlyUBO
#define idLineEvaluatedPropsUBO     layerUBOStartId

)";

template <>
struct ShaderSource<BuiltIn::LineShader, gfx::Backend::Type::Vulkan> {
    static constexpr const char* name = "LineShader";

    static const std::array<AttributeInfo, 8> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 0> textures;

    static constexpr auto prelude = lineShadePrelude;
    static constexpr auto vertex = R"(

layout(location = 0) in ivec2 in_pos_normal;
layout(location = 1) in uvec4 in_data;

#if !defined(HAS_UNIFORM_u_color)
layout(location = 2) in vec4 in_color;
#endif

#if !defined(HAS_UNIFORM_u_blur)
layout(location = 3) in vec2 in_blur;
#endif

#if !defined(HAS_UNIFORM_u_opacity)
layout(location = 4) in vec2 in_opacity;
#endif

#if !defined(HAS_UNIFORM_u_gapwidth)
layout(location = 5) in vec2 in_gapwidth;
#endif

#if !defined(HAS_UNIFORM_u_offset)
layout(location = 6) in vec2 in_offset;
#endif

#if !defined(HAS_UNIFORM_u_width)
layout(location = 7) in vec2 in_width;
#endif

layout(push_constant) uniform Constants {
    int ubo_index;
} constant;

struct LineDrawableUBO {
    mat4 matrix;
    mediump float ratio;
    // Interpolations
    float color_t;
    float blur_t;
    float opacity_t;
    float gapwidth_t;
    float offset_t;
    float width_t;
    float pad1;
    vec4 pad2;
    vec4 pad3;
};

layout(std140, set = LAYER_SET_INDEX, binding = idLineDrawableUBO) readonly buffer LineDrawableUBOVector {
    LineDrawableUBO drawable_ubo[];
} drawableVector;

layout(set = LAYER_SET_INDEX, binding = idLineEvaluatedPropsUBO) uniform LineEvaluatedPropsUBO {
    vec4 color;
    float blur;
    float opacity;
    float gapwidth;
    float offset;
    float width;
    float floorwidth;
    uint expressionMask;
    float pad1;
} props;

layout(location = 0) out lowp vec2 frag_normal;
layout(location = 1) out lowp vec2 frag_width2;
layout(location = 2) out lowp float frag_gamma_scale;

#if !defined(HAS_UNIFORM_u_color)
layout(location = 3) out lowp vec4 frag_color;
#endif

#if !defined(HAS_UNIFORM_u_blur)
layout(location = 4) out lowp float frag_blur;
#endif

#if !defined(HAS_UNIFORM_u_opacity)
layout(location = 5) out lowp float frag_opacity;
#endif

void main() {
    const LineDrawableUBO drawable = drawableVector.drawable_ubo[constant.ubo_index];

#ifndef HAS_UNIFORM_u_color
    frag_color = unpack_mix_color(in_color, drawable.color_t);
#endif

#ifndef HAS_UNIFORM_u_blur
    frag_blur = unpack_mix_float(in_blur, drawable.blur_t);
#endif

#ifndef HAS_UNIFORM_u_opacity
    frag_opacity = unpack_mix_float(in_opacity, drawable.opacity_t);
#endif

#ifndef HAS_UNIFORM_u_gapwidth
    const mediump float gapwidth = unpack_mix_float(in_gapwidth, drawable.gapwidth_t) / 2.0;
#else
    const mediump float gapwidth = props.gapwidth / 2.0;
#endif

#ifndef HAS_UNIFORM_u_offset
    const lowp float offset = unpack_mix_float(in_offset, drawable.offset_t) * -1.0;
#else
    const lowp float offset = props.offset * -1.0;
#endif

#ifndef HAS_UNIFORM_u_width
    mediump float width = unpack_mix_float(in_width, drawable.width_t);
#else
    mediump float width = props.width;
#endif

    // the distance over which the line edge fades out.
    // Retina devices need a smaller distance to avoid aliasing.
    const float ANTIALIASING = 1.0 / DEVICE_PIXEL_RATIO / 2.0;

    vec2 a_extrude = in_data.xy - 128.0;
    float a_direction = mod(in_data.z, 4.0) - 1.0;
    vec2 pos = floor(in_pos_normal * 0.5);

    // x is 1 if it's a round cap, 0 otherwise
    // y is 1 if the normal points up, and -1 if it points down
    // We store these in the least significant bit of in_pos_normal
    mediump vec2 normal = in_pos_normal - 2.0 * pos;
    frag_normal = vec2(normal.x, normal.y * 2.0 - 1.0);

    // these transformations used to be applied in the JS and native code bases.
    // moved them into the shader for clarity and simplicity.
    float halfwidth = width / 2.0;

    float inset = gapwidth + (gapwidth > 0.0 ? ANTIALIASING : 0.0);
    float outset = gapwidth + halfwidth * (gapwidth > 0.0 ? 2.0 : 1.0) + (halfwidth == 0.0 ? 0.0 : ANTIALIASING);

    // Scale the extrusion vector down to a normal and then up by the line width
    // of this vertex.
    mediump vec2 dist = outset * a_extrude * LINE_NORMAL_SCALE;

    // Calculate the offset when drawing a line that is to the side of the actual line.
    // We do this by creating a vector that points towards the extrude, but rotate
    // it when we're drawing round end points (a_direction = -1 or 1) since their
    // extrude vector points in another direction.
    mediump float u = 0.5 * a_direction;
    mediump float t = 1.0 - abs(u);
    mediump vec2 offset2 = offset * a_extrude * LINE_NORMAL_SCALE * frag_normal.y * mat2(t, -u, u, t);

    vec4 projected_extrude = drawable.matrix * vec4(dist / drawable.ratio, 0.0, 0.0);
    gl_Position = drawable.matrix * vec4(pos + offset2 / drawable.ratio, 0.0, 1.0) + projected_extrude;
    applySurfaceTransform();

    // calculate how much the perspective view squishes or stretches the extrude
    float extrude_length_without_perspective = length(dist);
    float extrude_length_with_perspective = length(projected_extrude.xy / gl_Position.w * paintParams.units_to_pixels);
    frag_gamma_scale = extrude_length_without_perspective / extrude_length_with_perspective;

    frag_width2 = vec2(outset, inset);
}
)";

    static constexpr auto fragment = R"(

layout(location = 0) in lowp vec2 frag_normal;
layout(location = 1) in lowp vec2 frag_width2;
layout(location = 2) in lowp float frag_gamma_scale;

#if !defined(HAS_UNIFORM_u_color)
layout(location = 3) in lowp vec4 frag_color;
#endif

#if !defined(HAS_UNIFORM_u_blur)
layout(location = 4) in lowp float frag_blur;
#endif

#if !defined(HAS_UNIFORM_u_opacity)
layout(location = 5) in lowp float frag_opacity;
#endif

layout(location = 0) out vec4 out_color;

layout(set = LAYER_SET_INDEX, binding = idLineEvaluatedPropsUBO) uniform LineEvaluatedPropsUBO {
    vec4 color;
    float blur;
    float opacity;
    float gapwidth;
    float offset;
    float width;
    float floorwidth;
    uint expressionMask;
    float pad1;
} props;

void main() {

#ifdef OVERDRAW_INSPECTOR
    out_color = vec4(1.0);
    return;
#endif

#ifdef HAS_UNIFORM_u_color
    highp vec4 color = props.color;
#else
    highp vec4 color = frag_color;
#endif

#ifdef HAS_UNIFORM_u_blur
    lowp float blur = props.blur;
#else
    lowp float blur = frag_blur;
#endif

#ifdef HAS_UNIFORM_u_opacity
    lowp float opacity = props.opacity;
#else
    lowp float opacity = frag_opacity;
#endif

    // Calculate the distance of the pixel from the line in pixels.
    float dist = length(frag_normal) * frag_width2.s;

    // Calculate the antialiasing fade factor. This is either when fading in
    // the line in case of an offset line (frag_width2.t) or when fading out
    // (frag_width2.s)
    float blur2 = (blur + 1.0 / DEVICE_PIXEL_RATIO) * frag_gamma_scale;
    float alpha = clamp(min(dist - (frag_width2.t - blur2), frag_width2.s - dist) / blur2, 0.0, 1.0);

    out_color = color * (alpha * opacity);
}
)";
};

template <>
struct ShaderSource<BuiltIn::LineGradientShader, gfx::Backend::Type::Vulkan> {
    static constexpr const char* name = "LineGradientShader";

    static const std::array<AttributeInfo, 7> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 1> textures;

    static constexpr auto prelude = lineShadePrelude;
    static constexpr auto vertex = R"(

layout(location = 0) in ivec2 in_pos_normal;
layout(location = 1) in uvec4 in_data;
layout(location = 2) in vec2 in_uv;

#if !defined(HAS_UNIFORM_u_blur)
layout(location = 3) in vec2 in_blur;
#endif

#if !defined(HAS_UNIFORM_u_opacity)
layout(location = 4) in vec2 in_opacity;
#endif

#if !defined(HAS_UNIFORM_u_gapwidth)
layout(location = 5) in vec2 in_gapwidth;
#endif

#if !defined(HAS_UNIFORM_u_offset)
layout(location = 6) in vec2 in_offset;
#endif

#if !defined(HAS_UNIFORM_u_width)
layout(location = 7) in vec2 in_width;
#endif

layout(push_constant) uniform Constants {
    int ubo_index;
} constant;

struct LineGradientDrawableUBO {
    mat4 matrix;
    float ratio;
    // Interpolations
    float blur_t;
    float opacity_t;
    float gapwidth_t;
    float offset_t;
    float width_t;
    float pad1;
    float pad2;
    vec4 pad3;
    vec4 pad4;
};

layout(std140, set = LAYER_SET_INDEX, binding = idLineDrawableUBO) readonly buffer LineGradientDrawableUBOVector {
    LineGradientDrawableUBO drawable_ubo[];
} drawableVector;

layout(set = LAYER_SET_INDEX, binding = idLineEvaluatedPropsUBO) uniform LineEvaluatedPropsUBO {
    vec4 color;
    float blur;
    float opacity;
    float gapwidth;
    float offset;
    float width;
    float floorwidth;
    uint expressionMask;
    float pad1;
} props;

layout(location = 0) out lowp vec2 frag_normal;
layout(location = 1) out lowp vec2 frag_width2;
layout(location = 2) out lowp float frag_gamma_scale;
layout(location = 3) out vec2 frag_uv;

#if !defined(HAS_UNIFORM_u_blur)
layout(location = 4) out lowp float frag_blur;
#endif

#if !defined(HAS_UNIFORM_u_opacity)
layout(location = 5) out lowp float frag_opacity;
#endif

void main() {
    const LineGradientDrawableUBO drawable = drawableVector.drawable_ubo[constant.ubo_index];

#ifndef HAS_UNIFORM_u_blur
    frag_blur = unpack_mix_float(in_blur, drawable.blur_t);
#endif

#ifndef HAS_UNIFORM_u_opacity
    frag_opacity = unpack_mix_float(in_opacity, drawable.opacity_t);
#endif

#ifndef HAS_UNIFORM_u_gapwidth
    const mediump float gapwidth = unpack_mix_float(in_gapwidth, drawable.gapwidth_t) / 2.0;
#else
    const mediump float gapwidth = props.gapwidth / 2.0;
#endif

#ifndef HAS_UNIFORM_u_offset
    const lowp float offset = unpack_mix_float(in_offset, drawable.offset_t) * -1.0;
#else
    const lowp float offset = props.offset * -1.0;
#endif

#ifndef HAS_UNIFORM_u_width
    mediump float width = unpack_mix_float(in_width, drawable.width_t);
#else
    mediump float width = props.width;
#endif

    // the distance over which the line edge fades out.
    // Retina devices need a smaller distance to avoid aliasing.
    const float ANTIALIASING = 1.0 / DEVICE_PIXEL_RATIO / 2.0;

    vec2 a_extrude = in_data.xy - 128.0;
    float a_direction = mod(in_data.z, 4.0) - 1.0;
    vec2 pos = floor(in_pos_normal * 0.5);

    // x is 1 if it's a round cap, 0 otherwise
    // y is 1 if the normal points up, and -1 if it points down
    // We store these in the least significant bit of in_pos_normal
    mediump vec2 normal = in_pos_normal - 2.0 * pos;
    frag_normal = vec2(normal.x, normal.y * 2.0 - 1.0);

    // these transformations used to be applied in the JS and native code bases.
    // moved them into the shader for clarity and simplicity.
    float halfwidth = width / 2.0;

    float inset = gapwidth + (gapwidth > 0.0 ? ANTIALIASING : 0.0);
    float outset = gapwidth + halfwidth * (gapwidth > 0.0 ? 2.0 : 1.0) + (halfwidth == 0.0 ? 0.0 : ANTIALIASING);

    // Scale the extrusion vector down to a normal and then up by the line width
    // of this vertex.
    mediump vec2 dist = outset * a_extrude * LINE_NORMAL_SCALE;

    // Calculate the offset when drawing a line that is to the side of the actual line.
    // We do this by creating a vector that points towards the extrude, but rotate
    // it when we're drawing round end points (a_direction = -1 or 1) since their
    // extrude vector points in another direction.
    mediump float u = 0.5 * a_direction;
    mediump float t = 1.0 - abs(u);
    mediump vec2 offset2 = offset * a_extrude * LINE_NORMAL_SCALE * frag_normal.y * mat2(t, -u, u, t);

    vec4 projected_extrude = drawable.matrix * vec4(dist / drawable.ratio, 0.0, 0.0);
    gl_Position = drawable.matrix * vec4(pos + offset2 / drawable.ratio, 0.0, 1.0) + projected_extrude;
    applySurfaceTransform();

    // calculate how much the perspective view squishes or stretches the extrude
    float extrude_length_without_perspective = length(dist);
    float extrude_length_with_perspective = length(projected_extrude.xy / gl_Position.w * paintParams.units_to_pixels);
    frag_gamma_scale = extrude_length_without_perspective / extrude_length_with_perspective;

    frag_width2 = vec2(outset, inset);
    frag_uv = in_uv;
}
)";

    static constexpr auto fragment = R"(

layout(location = 0) in lowp vec2 frag_normal;
layout(location = 1) in lowp vec2 frag_width2;
layout(location = 2) in lowp float frag_gamma_scale;
layout(location = 3) in vec2 frag_uv;

#if !defined(HAS_UNIFORM_u_blur)
layout(location = 4) in lowp float frag_blur;
#endif

#if !defined(HAS_UNIFORM_u_opacity)
layout(location = 5) in lowp float frag_opacity;
#endif

layout(location = 0) out vec4 out_color;

layout(set = LAYER_SET_INDEX, binding = idLineEvaluatedPropsUBO) uniform LineEvaluatedPropsUBO {
    vec4 color;
    float blur;
    float opacity;
    float gapwidth;
    float offset;
    float width;
    float floorwidth;
    uint expressionMask;
    float pad1;
} props;

layout(set = DRAWABLE_IMAGE_SET_INDEX, binding = 0) uniform sampler2D image0_sampler;

void main() {

#ifdef OVERDRAW_INSPECTOR
    out_color = vec4(1.0);
    return;
#endif

#ifdef HAS_UNIFORM_u_blur
    lowp float blur = props.blur;
#else
    lowp float blur = frag_blur;
#endif

#ifdef HAS_UNIFORM_u_opacity
    lowp float opacity = props.opacity;
#else
    lowp float opacity = frag_opacity;
#endif

    // Calculate the distance of the pixel from the line in pixels.
    float dist = length(frag_normal) * frag_width2.s;

    // Calculate the antialiasing fade factor. This is either when fading in
    // the line in case of an offset line (frag_width2.t) or when fading out
    // (frag_width2.s)
    float blur2 = (blur + 1.0 / DEVICE_PIXEL_RATIO) * frag_gamma_scale;
    float alpha = clamp(min(dist - (frag_width2.t - blur2), frag_width2.s - dist) / blur2, 0.0, 1.0);

    vec4 color = texture(image0_sampler, frag_uv);

    out_color = color * (alpha * opacity);
}
)";
};

template <>
struct ShaderSource<BuiltIn::LinePatternShader, gfx::Backend::Type::Vulkan> {
    static constexpr const char* name = "LinePatternShader";

    static const std::array<AttributeInfo, 10> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 1> textures;

    static constexpr auto prelude = lineShadePrelude;
    static constexpr auto vertex = R"(

layout(location = 0) in ivec2 in_pos_normal;
layout(location = 1) in uvec4 in_data;

#if !defined(HAS_UNIFORM_u_blur)
layout(location = 2) in vec2 in_blur;
#endif

#if !defined(HAS_UNIFORM_u_opacity)
layout(location = 3) in vec2 in_opacity;
#endif

#if !defined(HAS_UNIFORM_u_gapwidth)
layout(location = 4) in vec2 in_gapwidth;
#endif

#if !defined(HAS_UNIFORM_u_offset)
layout(location = 5) in vec2 in_offset;
#endif

#if !defined(HAS_UNIFORM_u_width)
layout(location = 6) in vec2 in_width;
#endif

#if !defined(HAS_UNIFORM_u_pattern_from)
layout(location = 7) in vec4 in_pattern_from;
#endif

#if !defined(HAS_UNIFORM_u_pattern_to)
layout(location = 8) in vec4 in_pattern_to;
#endif

layout(location = 9) in vec2 in_tile_units;

layout(push_constant) uniform Constants {
    int ubo_index;
} constant;

struct LinePatternDrawableUBO {
    mat4 matrix;
    float ratio;
    // Interpolations
    float blur_t;
    float opacity_t;
    float gapwidth_t;
    float offset_t;
    float width_t;
    float pattern_from_t;
    float pattern_to_t;
    vec4 pad1;
    vec4 pad2;
};

layout(std140, set = LAYER_SET_INDEX, binding = idLineDrawableUBO) readonly buffer LinePatternDrawableUBOVector {
    LinePatternDrawableUBO drawable_ubo[];
} drawableVector;

struct LinePatternTilePropsUBO {
    vec4 pattern_from;
    vec4 pattern_to;
    vec4 scale;
    vec2 texsize;
    float fade;
    float pad2;
};

layout(std140, set = LAYER_SET_INDEX, binding = idLineTilePropsUBO) readonly buffer LinePatternTilePropsUBOVector {
    LinePatternTilePropsUBO tile_props_ubo[];
} tilePropsVector;

layout(set = LAYER_SET_INDEX, binding = idLineEvaluatedPropsUBO) uniform LineEvaluatedPropsUBO {
    vec4 color;
    float blur;
    float opacity;
    float gapwidth;
    float offset;
    float width;
    float floorwidth;
    uint expressionMask;
    float pad1;
} props;

layout(location = 0) out lowp vec2 frag_normal;
layout(location = 1) out lowp vec2 frag_width2;
layout(location = 2) out lowp float frag_gamma_scale;
layout(location = 3) out vec2 frag_pos_a;
layout(location = 4) out vec2 frag_pos_b;

#if !defined(HAS_UNIFORM_u_blur)
layout(location = 5) out lowp float frag_blur;
#endif

#if !defined(HAS_UNIFORM_u_opacity)
layout(location = 6) out lowp float frag_opacity;
#endif

void main() {
    const LinePatternDrawableUBO drawable = drawableVector.drawable_ubo[constant.ubo_index];
    const LinePatternTilePropsUBO tileProps = tilePropsVector.tile_props_ubo[constant.ubo_index];

#ifndef HAS_UNIFORM_u_blur
    frag_blur = unpack_mix_float(in_blur, drawable.blur_t);
#endif

#ifndef HAS_UNIFORM_u_opacity
    frag_opacity = unpack_mix_float(in_opacity, drawable.opacity_t);
#endif

#ifndef HAS_UNIFORM_u_pattern_from
    const vec4 pattern_from = unpack_mix_vec4(in_pattern_from, drawable.pattern_from_t);
#else
    const vec4 pattern_from = tileProps.pattern_from;
#endif

#ifndef HAS_UNIFORM_u_pattern_to
    const vec4 pattern_to = unpack_mix_vec4(in_pattern_to, drawable.pattern_to_t);
#else
    const vec4 pattern_to = tileProps.pattern_to;
#endif

#ifndef HAS_UNIFORM_u_gapwidth
    const mediump float gapwidth = unpack_mix_float(in_gapwidth, drawable.gapwidth_t) / 2.0;
#else
    const mediump float gapwidth = props.gapwidth / 2.0;
#endif

#ifndef HAS_UNIFORM_u_offset
    const lowp float offset = unpack_mix_float(in_offset, drawable.offset_t) * -1.0;
#else
    const lowp float offset = props.offset * -1.0;
#endif

#ifndef HAS_UNIFORM_u_width
    mediump float width = unpack_mix_float(in_width, drawable.width_t);
#else
    mediump float width = props.width;
#endif

    // the distance over which the line edge fades out.
    // Retina devices need a smaller distance to avoid aliasing.
    const float ANTIALIASING = 1.0 / DEVICE_PIXEL_RATIO / 2.0;
    const float LINE_DISTANCE_SCALE = 2.0;

    vec2 a_extrude = in_data.xy - 128.0;
    float a_direction = mod(in_data.z, 4.0) - 1.0;
    float linesofar = (floor(in_data.z / 4.0) + in_data.w * 64.0) * LINE_DISTANCE_SCALE;
    vec2 pos = floor(in_pos_normal * 0.5);

    // x is 1 if it's a round cap, 0 otherwise
    // y is 1 if the normal points up, and -1 if it points down
    // We store these in the least significant bit of in_pos_normal
    mediump vec2 normal = in_pos_normal - 2.0 * pos;
    frag_normal = vec2(normal.x, normal.y * 2.0 - 1.0);

    // these transformations used to be applied in the JS and native code bases.
    // moved them into the shader for clarity and simplicity.
    float halfwidth = width / 2.0;

    float inset = gapwidth + (gapwidth > 0.0 ? ANTIALIASING : 0.0);
    float outset = gapwidth + halfwidth * (gapwidth > 0.0 ? 2.0 : 1.0) + (halfwidth == 0.0 ? 0.0 : ANTIALIASING);

    // Scale the extrusion vector down to a normal and then up by the line width
    // of this vertex.
    mediump vec2 dist = outset * a_extrude * LINE_NORMAL_SCALE;

    // Calculate the offset when drawing a line that is to the side of the actual line.
    // We do this by creating a vector that points towards the extrude, but rotate
    // it when we're drawing round end points (a_direction = -1 or 1) since their
    // extrude vector points in another direction.
    mediump float u = 0.5 * a_direction;
    mediump float t = 1.0 - abs(u);
    mediump vec2 offset2 = offset * a_extrude * LINE_NORMAL_SCALE * frag_normal.y * mat2(t, -u, u, t);

    vec4 projected_extrude = drawable.matrix * vec4(dist / drawable.ratio, 0.0, 0.0);
    gl_Position = drawable.matrix * vec4(pos + offset2 / drawable.ratio, 0.0, 1.0) + projected_extrude;
    applySurfaceTransform();

    // calculate how much the perspective view squishes or stretches the extrude
    float extrude_length_without_perspective = length(dist);
    float extrude_length_with_perspective = length(projected_extrude.xy / gl_Position.w * paintParams.units_to_pixels);
    frag_gamma_scale = extrude_length_without_perspective / extrude_length_with_perspective;

    frag_width2 = vec2(outset, inset);

    vec2 pattern_tl_a = pattern_from.xy;
    vec2 pattern_br_a = pattern_from.zw;
    vec2 pattern_tl_b = pattern_to.xy;
    vec2 pattern_br_b = pattern_to.zw;

    float pixelOffsetA = ((linesofar / in_tile_units.y) + frag_normal.y * width / 2.0) / tileProps.scale.x;
    float pixelOffsetB = ((linesofar / in_tile_units.y) + frag_normal.y * width / 2.0) / tileProps.scale.y;

    frag_pos_a = mix(pattern_tl_a / tileProps.texsize, pattern_br_a / tileProps.texsize, fract(vec2(pixelOffsetA, 0.5)));
    frag_pos_b = mix(pattern_tl_b / tileProps.texsize, pattern_br_b / tileProps.texsize, fract(vec2(pixelOffsetB, 0.5)));
}
)";

    static constexpr auto fragment = R"(

layout(location = 0) in lowp vec2 frag_normal;
layout(location = 1) in lowp vec2 frag_width2;
layout(location = 2) in lowp float frag_gamma_scale;
layout(location = 3) in vec2 frag_pos_a;
layout(location = 4) in vec2 frag_pos_b;

#if !defined(HAS_UNIFORM_u_blur)
layout(location = 5) in lowp float frag_blur;
#endif

#if !defined(HAS_UNIFORM_u_opacity)
layout(location = 6) in lowp float frag_opacity;
#endif

layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Constants {
    int ubo_index;
} constant;

struct LinePatternTilePropsUBO {
    vec4 pattern_from;
    vec4 pattern_to;
    vec4 scale;
    vec2 texsize;
    float fade;
    float pad2;
};

layout(std140, set = LAYER_SET_INDEX, binding = idLineTilePropsUBO) readonly buffer LinePatternTilePropsUBOVector {
    LinePatternTilePropsUBO tile_props_ubo[];
} tilePropsVector;

layout(set = LAYER_SET_INDEX, binding = idLineEvaluatedPropsUBO) uniform LineEvaluatedPropsUBO {
    vec4 color;
    float blur;
    float opacity;
    float gapwidth;
    float offset;
    float width;
    float floorwidth;
    uint expressionMask;
    float pad1;
} props;

layout(set = DRAWABLE_IMAGE_SET_INDEX, binding = 0) uniform sampler2D image0_sampler;

void main() {

#ifdef OVERDRAW_INSPECTOR
    out_color = vec4(1.0);
    return;
#endif

    const LinePatternTilePropsUBO tileProps = tilePropsVector.tile_props_ubo[constant.ubo_index];

#ifdef HAS_UNIFORM_u_blur
    lowp float blur = props.blur;
#else
    lowp float blur = frag_blur;
#endif

#ifdef HAS_UNIFORM_u_opacity
    lowp float opacity = props.opacity;
#else
    lowp float opacity = frag_opacity;
#endif

    // Calculate the distance of the pixel from the line in pixels.
    float dist = length(frag_normal) * frag_width2.s;

    // Calculate the antialiasing fade factor. This is either when fading in
    // the line in case of an offset line (frag_width2.t) or when fading out
    // (frag_width2.s)
    float blur2 = (blur + 1.0 / DEVICE_PIXEL_RATIO) * frag_gamma_scale;
    float alpha = clamp(min(dist - (frag_width2.t - blur2), frag_width2.s - dist) / blur2, 0.0, 1.0);

    vec4 color = mix(texture(image0_sampler, frag_pos_a), texture(image0_sampler, frag_pos_b), tileProps.fade);

    out_color = color * alpha * opacity;
}
)";
};

template <>
struct ShaderSource<BuiltIn::LineSDFShader, gfx::Backend::Type::Vulkan> {
    static constexpr const char* name = "LineSDFShader";

    static const std::array<AttributeInfo, 11> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 1> textures;

    static constexpr auto prelude = lineShadePrelude;
    static constexpr auto vertex = R"(

layout(location = 0) in ivec2 in_pos_normal;
layout(location = 1) in uvec4 in_data;

#if !defined(HAS_UNIFORM_u_color)
layout(location = 2) in vec4 in_color;
#endif

#if !defined(HAS_UNIFORM_u_blur)
layout(location = 3) in vec2 in_blur;
#endif

#if !defined(HAS_UNIFORM_u_opacity)
layout(location = 4) in vec2 in_opacity;
#endif

#if !defined(HAS_UNIFORM_u_gapwidth)
layout(location = 5) in vec2 in_gapwidth;
#endif

#if !defined(HAS_UNIFORM_u_offset)
layout(location = 6) in vec2 in_offset;
#endif

#if !defined(HAS_UNIFORM_u_width)
layout(location = 7) in vec2 in_width;
#endif

#if !defined(HAS_UNIFORM_u_floorwidth)
layout(location = 8) in vec2 in_floorwidth;
#endif

#if !defined(HAS_UNIFORM_u_dasharray_from)
layout(location = 9) in vec4 in_dasharray_from;
#endif

#if !defined(HAS_UNIFORM_u_dasharray_to)
layout(location = 10) in vec4 in_dasharray_to;
#endif

layout(push_constant) uniform Constants {
    int ubo_index;
} constant;

struct LineSDFDrawableUBO {
    mat4 matrix;
    float ratio;
    // Interpolations
    float color_t;
    float blur_t;
    float opacity_t;
    float gapwidth_t;
    float offset_t;
    float width_t;
    float floorwidth_t;
    float dasharray_from_t;
    float dasharray_to_t;
    float pad_sdf_drawable_1;
    float pad_sdf_drawable_2;
};

layout(std140, set = LAYER_SET_INDEX, binding = idLineDrawableUBO) readonly buffer LineSDFDrawableUBOVector {
    LineSDFDrawableUBO drawable_ubo[];
} drawableVector;

layout(set = LAYER_SET_INDEX, binding = idLineEvaluatedPropsUBO) uniform LineEvaluatedPropsUBO {
    vec4 color;
    float blur;
    float opacity;
    float gapwidth;
    float offset;
    float width;
    float floorwidth;
    uint expressionMask;
    float pad_evaluated_props_1;
    vec4 dasharray_from;
    vec4 dasharray_to;
} props;

struct LineSDFTilePropsUBO {
    float tileratio;
    float crossfade_from;
    float crossfade_to;
    float lineatlas_width;
    float lineatlas_height;
    float mix;
    float pad_sdf_tileprops_1;
    float pad_sdf_tileprops_2;
};

layout(std140, set = LAYER_SET_INDEX, binding = idLineTilePropsUBO) readonly buffer LineSDFTilePropsUBOVector {
    LineSDFTilePropsUBO tile_props_ubo[];
} tilePropsVector;

layout(location = 0) out lowp vec2 frag_normal;
layout(location = 1) out lowp vec2 frag_width2;
layout(location = 2) out lowp float frag_gamma_scale;
layout(location = 3) out vec2 frag_tex_a;
layout(location = 4) out vec2 frag_tex_b;

#if !defined(HAS_UNIFORM_u_color)
layout(location = 5) out lowp vec4 frag_color;
#endif

#if !defined(HAS_UNIFORM_u_blur)
layout(location = 6) out lowp float frag_blur;
#endif

#if !defined(HAS_UNIFORM_u_opacity)
layout(location = 7) out lowp float frag_opacity;
#endif

#if !defined(HAS_UNIFORM_u_floorwidth)
layout(location = 8) out mediump float frag_floorwidth;
#endif

#if !defined(HAS_UNIFORM_u_dasharray_from)
layout(location = 9) out vec4 frag_dasharray_from;
#endif

#if !defined(HAS_UNIFORM_u_dasharray_to)
layout(location = 10) out vec4 frag_dasharray_to;
#endif

void main() {
    const LineSDFDrawableUBO drawable = drawableVector.drawable_ubo[constant.ubo_index];
    const LineSDFTilePropsUBO tileProps = tilePropsVector.tile_props_ubo[constant.ubo_index];

#ifndef HAS_UNIFORM_u_color
    frag_color = unpack_mix_color(in_color, drawable.color_t);
#endif

#ifndef HAS_UNIFORM_u_blur
    frag_blur = unpack_mix_float(in_blur, drawable.blur_t);
#endif

#ifndef HAS_UNIFORM_u_opacity
    frag_opacity = unpack_mix_float(in_opacity, drawable.opacity_t);
#endif

#ifndef HAS_UNIFORM_u_floorwidth
    const float floorwidth = unpack_mix_float(in_floorwidth, drawable.floorwidth_t);
    frag_floorwidth = floorwidth;
#else
    const float floorwidth = props.floorwidth;
#endif

#ifndef HAS_UNIFORM_u_dasharray_from
    const vec4 dasharray_from = unpack_mix_vec4(in_dasharray_from, drawable.dasharray_from_t);
    frag_dasharray_from = dasharray_from;
#else
    const vec4 dasharray_from = props.dasharray_from;
#endif

#ifndef HAS_UNIFORM_u_dasharray_to
    const vec4 dasharray_to = unpack_mix_vec4(in_dasharray_to, drawable.dasharray_to_t);
    frag_dasharray_to = dasharray_to;
#else
    const vec4 dasharray_to = props.dasharray_to;
#endif

#ifndef HAS_UNIFORM_u_offset
    const mediump float offset = unpack_mix_float(in_offset, drawable.offset_t) * -1.0;
#else
    const mediump float offset = props.offset * -1.0;
#endif

#ifndef HAS_UNIFORM_u_width
    const mediump float width = unpack_mix_float(in_width, drawable.width_t);
#else
    const mediump float width = props.width;
#endif

#ifndef HAS_UNIFORM_u_gapwidth
    const mediump float gapwidth = unpack_mix_float(in_gapwidth, drawable.gapwidth_t) / 2.0;
#else
    const mediump float gapwidth = props.gapwidth / 2.0;
#endif

    // the distance over which the line edge fades out.
    // Retina devices need a smaller distance to avoid aliasing.
    const float ANTIALIASING = 1.0 / DEVICE_PIXEL_RATIO / 2.0;
    const float LINE_DISTANCE_SCALE = 2.0;

    vec2 a_extrude = in_data.xy - 128.0;
    float a_direction = mod(in_data.z, 4.0) - 1.0;
    float linesofar = (floor(in_data.z / 4.0) + in_data.w * 64.0) * LINE_DISTANCE_SCALE;
    vec2 pos = floor(in_pos_normal * 0.5);

    // x is 1 if it's a round cap, 0 otherwise
    // y is 1 if the normal points up, and -1 if it points down
    // We store these in the least significant bit of in_pos_normal
    mediump vec2 normal = in_pos_normal - 2.0 * pos;
    frag_normal = vec2(normal.x, normal.y * 2.0 - 1.0);

    // these transformations used to be applied in the JS and native code bases.
    // moved them into the shader for clarity and simplicity.
    float halfwidth = width / 2.0;

    float inset = gapwidth + (gapwidth > 0.0 ? ANTIALIASING : 0.0);
    float outset = gapwidth + halfwidth * (gapwidth > 0.0 ? 2.0 : 1.0) + (halfwidth == 0.0 ? 0.0 : ANTIALIASING);

    // Scale the extrusion vector down to a normal and then up by the line width
    // of this vertex.
    mediump vec2 dist = outset * a_extrude * LINE_NORMAL_SCALE;

    // Calculate the offset when drawing a line that is to the side of the actual line.
    // We do this by creating a vector that points towards the extrude, but rotate
    // it when we're drawing round end points (a_direction = -1 or 1) since their
    // extrude vector points in another direction.
    mediump float u = 0.5 * a_direction;
    mediump float t = 1.0 - abs(u);
    mediump vec2 offset2 = offset * a_extrude * LINE_NORMAL_SCALE * frag_normal.y * mat2(t, -u, u, t);

    vec4 projected_extrude = drawable.matrix * vec4(dist / drawable.ratio, 0.0, 0.0);
    gl_Position = drawable.matrix * vec4(pos + offset2 / drawable.ratio, 0.0, 1.0) + projected_extrude;
    applySurfaceTransform();

    // calculate how much the perspective view squishes or stretches the extrude
    float extrude_length_without_perspective = length(dist);
    float extrude_length_with_perspective = length(projected_extrude.xy / gl_Position.w * paintParams.units_to_pixels);
    frag_gamma_scale = extrude_length_without_perspective / extrude_length_with_perspective;

    frag_width2 = vec2(outset, inset);

    // Calculate texture coordinates for dash pattern
    float u_patternscale_a_x = tileProps.tileratio / dasharray_from.w / tileProps.crossfade_from;
    float u_patternscale_a_y = -dasharray_from.z / 2.0 / tileProps.lineatlas_height;
    float u_patternscale_b_x = tileProps.tileratio / dasharray_to.w / tileProps.crossfade_to;
    float u_patternscale_b_y = -dasharray_to.z / 2.0 / tileProps.lineatlas_height;
    
    frag_tex_a = vec2(linesofar * u_patternscale_a_x / floorwidth, 
                      frag_normal.y * u_patternscale_a_y + (dasharray_from.y + 0.5) / tileProps.lineatlas_height);
    frag_tex_b = vec2(linesofar * u_patternscale_b_x / floorwidth, 
                      frag_normal.y * u_patternscale_b_y + (dasharray_to.y + 0.5) / tileProps.lineatlas_height);
}
)";

    static constexpr auto fragment = R"(

layout(location = 0) in lowp vec2 frag_normal;
layout(location = 1) in lowp vec2 frag_width2;
layout(location = 2) in lowp float frag_gamma_scale;
layout(location = 3) in vec2 frag_tex_a;
layout(location = 4) in vec2 frag_tex_b;

#if !defined(HAS_UNIFORM_u_color)
layout(location = 5) in lowp vec4 frag_color;
#endif

#if !defined(HAS_UNIFORM_u_blur)
layout(location = 6) in lowp float frag_blur;
#endif

#if !defined(HAS_UNIFORM_u_opacity)
layout(location = 7) in lowp float frag_opacity;
#endif

#if !defined(HAS_UNIFORM_u_floorwidth)
layout(location = 8) in mediump float frag_floorwidth;
#endif

#if !defined(HAS_UNIFORM_u_dasharray_from)
layout(location = 9) in vec4 frag_dasharray_from;
#endif

#if !defined(HAS_UNIFORM_u_dasharray_to)
layout(location = 10) in vec4 frag_dasharray_to;
#endif

layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Constants {
    int ubo_index;
} constant;

struct LineSDFTilePropsUBO {
    float tileratio;
    float crossfade_from;
    float crossfade_to;
    float lineatlas_width;
    float lineatlas_height;
    float mix;
    float pad_sdf_tileprops_1;
    float pad_sdf_tileprops_2;
};

layout(std140, set = LAYER_SET_INDEX, binding = idLineTilePropsUBO) readonly buffer LineSDFTilePropsUBOVector {
    LineSDFTilePropsUBO tile_props_ubo[];
} tilePropsVector;

layout(set = LAYER_SET_INDEX, binding = idLineEvaluatedPropsUBO) uniform LineEvaluatedPropsUBO {
    vec4 color;
    float blur;
    float opacity;
    float gapwidth;
    float offset;
    float width;
    float floorwidth;
    uint expressionMask;
    float pad_evaluated_props_1;
    vec4 dasharray_from;
    vec4 dasharray_to;
} props;

layout(set = DRAWABLE_IMAGE_SET_INDEX, binding = 0) uniform sampler2D image0_sampler;

void main() {

#ifdef OVERDRAW_INSPECTOR
    out_color = vec4(1.0);
    return;
#endif

    const LineSDFTilePropsUBO tileProps = tilePropsVector.tile_props_ubo[constant.ubo_index];

#ifdef HAS_UNIFORM_u_color
    const lowp vec4 color = props.color;
#else
    const lowp vec4 color = frag_color;
#endif

#ifdef HAS_UNIFORM_u_blur
    const lowp float blur = props.blur;
#else
    const lowp float blur = frag_blur;
#endif

#ifdef HAS_UNIFORM_u_opacity
    const lowp float opacity = props.opacity;
#else
    const lowp float opacity = frag_opacity;
#endif

#ifdef HAS_UNIFORM_u_floorwidth
    const lowp float floorwidth = props.floorwidth;
#else
    const lowp float floorwidth = frag_floorwidth;
#endif

#ifdef HAS_UNIFORM_u_dasharray_from
    const vec4 dasharray_from = props.dasharray_from;
#else
    const vec4 dasharray_from = frag_dasharray_from;
#endif

#ifdef HAS_UNIFORM_u_dasharray_to
    const vec4 dasharray_to = props.dasharray_to;
#else
    const vec4 dasharray_to = frag_dasharray_to;
#endif

    // Calculate the distance of the pixel from the line in pixels.
    const float dist = length(frag_normal) * frag_width2.x;

    // Calculate the antialiasing fade factor. This is either when fading in the
    // line in case of an offset line (`v_width2.y`) or when fading out (`v_width2.x`)
    const float blur2 = (blur + 1.0 / DEVICE_PIXEL_RATIO) * frag_gamma_scale;

    const float sdfdist_a = texture(image0_sampler, frag_tex_a).a;
    const float sdfdist_b = texture(image0_sampler, frag_tex_b).a;
    const float sdfdist = mix(sdfdist_a, sdfdist_b, tileProps.mix);
    
    const float sdfgamma = (tileProps.lineatlas_width / 256.0 / DEVICE_PIXEL_RATIO) / min(dasharray_from.w, dasharray_to.w);
    const float alpha = clamp(min(dist - (frag_width2.y - blur2), frag_width2.x - dist) / blur2, 0.0, 1.0) *
                        smoothstep(0.5 - sdfgamma / floorwidth, 0.5 + sdfgamma / floorwidth, sdfdist);

    out_color = color * (alpha * opacity);
}
)";
};

} // namespace shaders
} // namespace mbgl
