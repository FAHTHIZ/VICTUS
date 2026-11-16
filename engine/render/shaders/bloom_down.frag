#version 450
// SPDX-License-Identifier: Apache-2.0
// Bloom downsample: 13-tap filter (Jimenez, "Next Generation Post Processing in Call of
// Duty: Advanced Warfare", 2014). The first level applies a soft-knee threshold and a Karis
// (1 / (1 + luma)) average per 2x2 group, which removes fireflies from tiny bright pixels.
layout(location = 0) in vec2 v_uv;
layout(set = 2, binding = 0) uniform sampler2D u_src;
layout(set = 3, binding = 0) uniform Down {
    vec4 params;    // xy: 1 / source size, z: first level (prefilter), w: unused
    vec4 threshold; // x: threshold, y: knee, z: clamp
} u;
layout(location = 0) out vec4 out_color;

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 karis(vec3 a, vec3 b, vec3 c, vec3 d) {
    float wa = 1.0 / (1.0 + luma(a)), wb = 1.0 / (1.0 + luma(b)), wc = 1.0 / (1.0 + luma(c)), wd = 1.0 / (1.0 + luma(d));
    return (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);
}
vec3 fetch(vec2 uv) {
    vec3 c = textureLod(u_src, uv, 0.0).rgb;
    // Guard against NaN/Inf and extreme values leaking through the whole chain.
    return clamp(c, vec3(0.0), vec3(u.threshold.z));
}

void main() {
    vec2 t = u.params.xy;
    vec3 a = fetch(v_uv + t * vec2(-2, -2)), b = fetch(v_uv + t * vec2(0, -2)), c = fetch(v_uv + t * vec2(2, -2));
    vec3 d = fetch(v_uv + t * vec2(-2, 0)), e = fetch(v_uv), f = fetch(v_uv + t * vec2(2, 0));
    vec3 g = fetch(v_uv + t * vec2(-2, 2)), h = fetch(v_uv + t * vec2(0, 2)), i = fetch(v_uv + t * vec2(2, 2));
    vec3 j = fetch(v_uv + t * vec2(-1, -1)), k = fetch(v_uv + t * vec2(1, -1));
    vec3 l = fetch(v_uv + t * vec2(-1, 1)), m = fetch(v_uv + t * vec2(1, 1));
    vec3 color;
    if (u.params.z > 0.5) {
        color = karis(j, k, l, m) * 0.5 + karis(a, b, d, e) * 0.125 + karis(b, c, e, f) * 0.125 +
                karis(d, e, g, h) * 0.125 + karis(e, f, h, i) * 0.125;
        // Soft-knee threshold (quadratic ramp between threshold - knee and threshold + knee).
        float br = max(color.r, max(color.g, color.b));
        float knee = max(u.threshold.y, 1e-4);
        float rq = clamp(br - u.threshold.x + knee, 0.0, 2.0 * knee);
        rq = rq * rq / (4.0 * knee);
        color *= max(rq, br - u.threshold.x) / max(br, 1e-4);
    } else {
        color = (j + k + l + m) * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + e * 0.125;
    }
    out_color = vec4(max(color, vec3(0.0)), 1.0);
}
