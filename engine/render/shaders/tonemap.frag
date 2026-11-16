#version 450
// SPDX-License-Identifier: Apache-2.0
// Final HDR resolve: bloom composite, manual or automatic exposure, tonemapping (ACES fit,
// Khronos PBR Neutral or luminance Reinhard), contrast/saturation grading, vignette and sRGB
// encoding. With FXAA enabled the output is always sRGB-encoded with luma in alpha.
layout(location = 0) in vec2 v_uv;
layout(set = 2, binding = 0) uniform sampler2D u_hdr;
layout(set = 2, binding = 1) uniform sampler2D u_bloom;
layout(set = 2, binding = 2) uniform sampler2D u_exposure;
layout(set = 3, binding = 0) uniform Post {
    vec4 params;  // x: exposure, y: vignette, z: encode sRGB, w: tonemapper (0 none, 1 ACES, 2 neutral, 3 Reinhard)
    vec4 params2; // x: bloom intensity, y: auto exposure, z: exposure compensation (linear), w: saturation
    vec4 params3; // x: contrast, y: luma in alpha (FXAA input)
} post;
layout(location = 0) out vec4 out_color;

vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
// Khronos PBR Neutral tone mapper (Khronos 3D Commerce, 2024).
vec3 pbr_neutral(vec3 c) {
    const float start = 0.8 - 0.04;
    const float desat = 0.15;
    float x = min(c.r, min(c.g, c.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    c -= offset;
    float peak = max(c.r, max(c.g, c.b));
    if (peak < start) return c;
    float d = 1.0 - start;
    float new_peak = 1.0 - d * d / (peak + d - start);
    c *= new_peak / peak;
    float g = 1.0 - 1.0 / (desat * (peak - new_peak) + 1.0);
    return mix(c, vec3(new_peak), g);
}
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 reinhard(vec3 c) {
    float l = luma(c);
    return c / (1.0 + l);
}
vec3 to_srgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }

void main() {
    vec3 c = texture(u_hdr, v_uv).rgb;
    if (post.params2.x > 0.0) c += texture(u_bloom, v_uv).rgb * post.params2.x;
    float exposure = post.params.x;
    if (post.params2.y > 0.5) {
        // Middle grey (0.18) at the adapted average luminance.
        float avg = exp2(textureLod(u_exposure, vec2(0.5), 0.0).r);
        exposure = 0.18 / max(avg, 1e-5);
    }
    c *= exposure * post.params2.z;
    int op = int(post.params.w + 0.5);
    if (op == 1) c = aces(c);
    else if (op == 2) c = pbr_neutral(c);
    else if (op == 3) c = reinhard(c);
    c = clamp(c, 0.0, 1.0);
    // Grading in display-linear space around middle grey.
    float l = luma(c);
    c = max(mix(vec3(l), c, post.params2.w), 0.0);
    c = clamp((c - 0.18) * post.params3.x + 0.18, 0.0, 1.0);
    vec2 d = v_uv - 0.5;
    c *= mix(1.0, 1.0 - dot(d, d) * 1.6, post.params.y);
    if (post.params3.y > 0.5) {
        vec3 s = to_srgb(c);
        out_color = vec4(s, dot(s, vec3(0.299, 0.587, 0.114)));
        return;
    }
    if (post.params.z > 0.5) c = to_srgb(c);
    out_color = vec4(c, 1.0);
}
