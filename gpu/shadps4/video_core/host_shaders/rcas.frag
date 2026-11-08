// SPDX-License-Identifier: MIT
// deception: AMD FidelityFX FSR 1 RCAS (robust contrast-adaptive sharpening), after FsrRcasF in
// ffx_fsr1.h (Copyright (c) 2021 Advanced Micro Devices, Inc., MIT), without its denoise option.
// Runs on the output-sized picture post_process.frag made, in its encoding (sRGB or PQ), as RCAS
// expects. Each pixel is pushed away from its four neighbours by at most what keeps it inside their
// range, so edges get sharper without halos or clipping.

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float sharpness; ///< 0 none to 1 the most: FSR's exp2(-attenuation in stops)
} rcas;

// FSR_RCAS_LIMIT: the most negative lobe, kept away from -0.25 where the filter would divide by 0.
const float limit = 0.25 - 1.0 / 16.0;

vec4 at(ivec2 p, ivec2 size) {
    return texelFetch(texSampler, clamp(p, ivec2(0), size - 1), 0);
}

void main() {
    const ivec2 size = textureSize(texSampler, 0);
    const ivec2 p = ivec2(gl_FragCoord.xy);
    //    b
    //  d e f
    //    h
    const vec3 b = at(p + ivec2(0, -1), size).rgb;
    const vec3 d = at(p + ivec2(-1, 0), size).rgb;
    const vec4 e = at(p, size);
    const vec3 f = at(p + ivec2(1, 0), size).rgb;
    const vec3 h = at(p + ivec2(0, 1), size).rgb;
    const vec3 mn4 = min(min(b, d), min(f, h));
    const vec3 mx4 = max(max(b, d), max(f, h));
    // The lobe that takes e to the edge of [0, 1] given the ring's range; the guards keep black and
    // white rings (0/0 in FSR) finite.
    const vec3 hit_min = min(mn4, e.rgb) / max(4.0 * mx4, vec3(1e-5));
    const vec3 hit_max = (1.0 - max(mx4, e.rgb)) / min(4.0 * mn4 - 4.0, vec3(-1e-5));
    const vec3 lobe3 = max(-hit_min, hit_max);
    const float lobe = max(-limit, min(max(lobe3.r, max(lobe3.g, lobe3.b)), 0.0)) * rcas.sharpness;
    color = vec4((lobe * (b + d + f + h) + e.rgb) / (4.0 * lobe + 1.0), e.a);
}
