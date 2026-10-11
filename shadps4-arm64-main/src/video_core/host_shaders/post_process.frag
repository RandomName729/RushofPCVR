// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
    // Above 0, the picture is sharpened by so much (up to 1): for one that is shown larger
    // than it was drawn, which blurs it.
    float sharpen;
    // The target encodes what it is given for display by itself (an sRGB image): it has to be
    // given linear light, or the picture would be encoded twice.
    bool linear_out;
    // Above 0, everything outside the two lens circles of a one-picture title (Rush of Blood
    // draws a circle for each eye side by side, on coloured corners) goes black; the number
    // scales the circles (1: as drawn, above: they leave more of the corners).
    float mask;
    // Above 0, edges are smoothed (an edge-following blur, as FXAA does): up to 1, the stronger.
    float fxaa;
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// What goes to the display for the picture at a place.
vec3 shown(vec2 at) {
    vec3 rgb = textureLod(texSampler, at, 0.0).rgb;
    return pp.hdr ? rgb : gamma(rgb);
}

float luma(vec3 rgb) {
    return dot(rgb, vec3(0.299, 0.587, 0.114));
}

// Edge smoothing after FXAA: the picture is blurred along an edge, in the direction it runs,
// and only where there is a contrast to smooth.
vec3 smoothed(vec2 at, vec3 centre) {
    vec2 texel = 1.0 / vec2(textureSize(texSampler, 0));
    vec3 nw = shown(at + vec2(-1.0, -1.0) * texel);
    vec3 ne = shown(at + vec2(1.0, -1.0) * texel);
    vec3 sw = shown(at + vec2(-1.0, 1.0) * texel);
    vec3 se = shown(at + vec2(1.0, 1.0) * texel);
    float luma_nw = luma(nw), luma_ne = luma(ne), luma_sw = luma(sw), luma_se = luma(se);
    float luma_m = luma(centre);
    float luma_min = min(luma_m, min(min(luma_nw, luma_ne), min(luma_sw, luma_se)));
    float luma_max = max(luma_m, max(max(luma_nw, luma_ne), max(luma_sw, luma_se)));
    // Flat areas are left alone.
    if (luma_max - luma_min < max(0.0312, luma_max * 0.125)) {
        return centre;
    }
    vec2 dir = vec2(-((luma_nw + luma_ne) - (luma_sw + luma_se)),
                    (luma_nw + luma_sw) - (luma_ne + luma_se));
    float reduce = max((luma_nw + luma_ne + luma_sw + luma_se) * 0.25 * (1.0 / 8.0), 1.0 / 128.0);
    float rcp_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    float span = mix(2.0, 8.0, clamp(pp.fxaa, 0.0, 1.0));
    dir = clamp(dir * rcp_min, vec2(-span), vec2(span)) * texel;
    vec3 narrow = 0.5 * (shown(at + dir * (1.0 / 3.0 - 0.5)) + shown(at + dir * (2.0 / 3.0 - 0.5)));
    vec3 wide = narrow * 0.5 + 0.25 * (shown(at + dir * -0.5) + shown(at + dir * 0.5));
    float luma_wide = luma(wide);
    return (luma_wide < luma_min || luma_wide > luma_max) ? narrow : wide;
}

void main() {
    vec4 color_linear = texture(texSampler, uv);
    vec3 here = pp.hdr ? color_linear.rgb : gamma(color_linear.rgb);
    if (pp.fxaa > 0.0) {
        here = smoothed(uv, here);
    }
    if (pp.sharpen > 0.0) {
        // Contrast adaptive sharpening: a pixel is pushed away from the four next to it, the
        // more the less they differ already, and never beyond black or white.
        vec2 texel = 1.0 / vec2(textureSize(texSampler, 0));
        vec3 above = shown(uv - vec2(0.0, texel.y));
        vec3 below = shown(uv + vec2(0.0, texel.y));
        vec3 left = shown(uv - vec2(texel.x, 0.0));
        vec3 right = shown(uv + vec2(texel.x, 0.0));
        vec3 darkest = min(min(min(above, below), min(left, right)), here);
        vec3 brightest = max(max(max(above, below), max(left, right)), here);
        vec3 room = sqrt(clamp(min(darkest, 1.0 - brightest) / max(brightest, vec3(1e-5)),
                               0.0, 1.0));
        vec3 weight = room * (-1.0 / mix(8.0, 5.0, clamp(pp.sharpen, 0.0, 1.0)));
        here = clamp(((above + below + left + right) * weight + here) / (1.0 + 4.0 * weight),
                     0.0, 1.0);
    }
    if (pp.mask > 0.0) {
        // Circles as measured on the title's picture: centres and radii in picture units.
        const vec2 centre_left = vec2(0.2335, 0.475);
        const vec2 centre_right = vec2(0.7533, 0.475);
        vec2 radius = vec2(0.2816, 0.523) * pp.mask;
        float inside = min(length((uv - centre_left) / radius),
                           length((uv - centre_right) / radius));
        here *= 1.0 - smoothstep(0.98, 1.0, inside);
    }
    if (pp.linear_out && !pp.hdr) {
        // Back to linear light, by the curve the target encodes with.
        here = mix(pow((here + 0.055) / 1.055, vec3(2.4)), here / 12.92,
                   lessThanEqual(here, vec3(0.04045)));
    }
    color = vec4(here, color_linear.a);
}
