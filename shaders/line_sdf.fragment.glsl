layout (std140) uniform LineSDFTilePropsUBO {
    highp float u_tileratio;
    highp float u_crossfade_from;
    highp float u_crossfade_to;
    highp float u_lineatlas_width;
    highp float u_lineatlas_height;
    highp float u_mix;
    lowp float tileprops_pad1;
    lowp float tileprops_pad2;
};

layout (std140) uniform LineEvaluatedPropsUBO {
    highp vec4 u_color;
    lowp float u_blur;
    lowp float u_opacity;
    mediump float u_gapwidth;
    lowp float u_offset;
    mediump float u_width;
    lowp float u_floorwidth;
    highp vec4 u_dasharray_from;
    highp vec4 u_dasharray_to;
    lowp float props_pad1;
};

uniform sampler2D u_image;

in vec2 v_normal;
in vec2 v_width2;
in vec2 v_tex_a;
in vec2 v_tex_b;
in float v_gamma_scale;

#pragma mapbox: define highp vec4 color
#pragma mapbox: define lowp float blur
#pragma mapbox: define lowp float opacity
#pragma mapbox: define mediump float width
#pragma mapbox: define lowp float floorwidth
#pragma mapbox: define mediump vec4 dasharray_from
#pragma mapbox: define mediump vec4 dasharray_to

void main() {
    #pragma mapbox: initialize highp vec4 color
    #pragma mapbox: initialize lowp float blur
    #pragma mapbox: initialize lowp float opacity
    #pragma mapbox: initialize mediump float width
    #pragma mapbox: initialize lowp float floorwidth
    #pragma mapbox: initialize mediump vec4 dasharray_from
    #pragma mapbox: initialize mediump vec4 dasharray_to

    // Calculate the distance of the pixel from the line in pixels.
    float dist = length(v_normal) * v_width2.s;

    // Calculate the antialiasing fade factor. This is either when fading in
    // the line in case of an offset line (v_width2.t) or when fading out
    // (v_width2.s)
    float blur2 = (blur + 1.0 / DEVICE_PIXEL_RATIO) * v_gamma_scale;
    float alpha = clamp(min(dist - (v_width2.t - blur2), v_width2.s - dist) / blur2, 0.0, 1.0);

    float sdfdist_a = texture(u_image, v_tex_a).a;
    float sdfdist_b = texture(u_image, v_tex_b).a;
    float sdfdist = mix(sdfdist_a, sdfdist_b, u_mix);
    
    // Calculate SDF gamma - includes DEVICE_PIXEL_RATIO for proper antialiasing
    float sdfgamma = (u_lineatlas_width / 256.0 / DEVICE_PIXEL_RATIO) / min(dasharray_from.w, dasharray_to.w);
    alpha *= smoothstep(0.5 - sdfgamma / floorwidth, 0.5 + sdfgamma / floorwidth, sdfdist);

    fragColor = color * (alpha * opacity);

#ifdef OVERDRAW_INSPECTOR
    fragColor = vec4(1.0);
#endif
}
