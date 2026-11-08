// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
    bool srgb_input;
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// Exact inverse of gamma() at unit gamma, for buffers that are sRGB encoded but must be sampled
// through a UNORM view because Vulkan has no sRGB variant of the 10-bit format.
vec3 degamma(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

vec4 fetch(vec2 at) {
    vec4 c = texture(texSampler, at);
    if (pp.srgb_input && !pp.hdr) c.rgb = degamma(c.rgb);
    return c;
}

// deception: a frame larger than the output (render_resolution=4k on a smaller screen) is
// filtered down over each output pixel's whole footprint: a grid of up to 4x4 bilinear taps spread
// across it, averaged in linear light. One bilinear tap reads only 2x2 texels, so at 4K -> 1440p
// (1.5 texels per pixel) it skipped texels and edges shimmered. At or below 1:1 it is one tap.
void main() {
    vec2 footprint = fwidth(uv) * vec2(textureSize(texSampler, 0));
    ivec2 taps = ivec2(clamp(ceil(footprint - 0.01), vec2(1.0), vec2(4.0)));
    vec2 step_uv = fwidth(uv) / vec2(taps);
    vec4 color_linear = vec4(0.0);
    for (int y = 0; y < taps.y; ++y) {
        for (int x = 0; x < taps.x; ++x) {
            color_linear += fetch(uv + (vec2(x, y) + 0.5 - vec2(taps) * 0.5) * step_uv);
        }
    }
    color_linear /= float(taps.x * taps.y);
    if (pp.hdr) {
        color = color_linear;
    } else {
        color = vec4(gamma(color_linear.rgb), color_linear.a);
    }
}
