#version 450
// SPDX-License-Identifier: Apache-2.0
// Final HDR resolve: bloom composite, manual or automatic exposure, tonemapping (ACES fit,
// Khronos PBR Neutral or luminance Reinhard), contrast/saturation grading, vignette and sRGB
// encoding. With FXAA enabled the output is always sRGB-encoded with luma in alpha.
// Optional camera-lens imperfections (ADR-0146): barrel distortion, lateral chromatic aberration
// and luminance-weighted sensor grain. All three are off (exactly the old path) at zero.
layout(location = 0) in vec2 v_uv;
layout(set = 2, binding = 0) uniform sampler2D u_hdr;
layout(set = 2, binding = 1) uniform sampler2D u_bloom;
layout(set = 2, binding = 2) uniform sampler2D u_exposure;
layout(set = 3, binding = 0) uniform Post {
    vec4 params;  // x: exposure, y: vignette, z: encode sRGB, w: tonemapper (0 none, 1 ACES, 2 neutral, 3 Reinhard)
    vec4 params2; // x: bloom intensity, y: auto exposure, z: exposure compensation (linear), w: saturation
    vec4 params3; // x: contrast, y: luma in alpha (FXAA input), z: linear HDR output (probe captures)
    vec4 params4; // x: lens distortion (barrel > 0), y: chromatic aberration, z: film grain, w: frame seed
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

// Integer hash (PCG-ish) -> [0, 1); stable across GPUs, no texture needed.
float hash13(uvec3 p) {
    p = p * uvec3(1664525u, 1013904223u, 2654435769u);
    p.x += p.y * p.z; p.y += p.z * p.x; p.z += p.x * p.y;
    p ^= p >> 16u;
    p.x += p.y * p.z;
    return float(p.x & 0x00ffffffu) / 16777216.0;
}

void main() {
    vec2 uv = v_uv;
    vec2 cd = uv - 0.5;
    float r2 = dot(cd, cd);
    if (post.params4.x != 0.0) {
        // Brown-Conrady k1 barrel, normalised so the frame corners stay on screen (zoom to fit).
        float k = post.params4.x * 0.25;
        uv = 0.5 + cd * (1.0 + k * r2) / (1.0 + k * 0.5);
    }
    vec3 c;
    if (post.params4.y > 0.0) {
        // Lateral CA: red and blue magnified slightly differently, growing toward the edges.
        vec2 off = (uv - 0.5) * (post.params4.y * 0.006);
        c = vec3(texture(u_hdr, uv + off).r, texture(u_hdr, uv).g, texture(u_hdr, uv - off).b);
    } else {
        c = texture(u_hdr, uv).rgb;
    }
    if (post.params2.x > 0.0) c += texture(u_bloom, uv).rgb * post.params2.x;
    float exposure = post.params.x;
    if (post.params2.y > 0.5) {
        // Middle grey (0.18) at the adapted average luminance.
        float avg = exp2(textureLod(u_exposure, vec2(0.5), 0.0).r);
        exposure = 0.18 / max(avg, 1e-5);
    }
    c *= exposure * post.params2.z;
    if (post.params3.z > 0.5) { // scene capture: keep linear radiance (float target)
        out_color = vec4(max(c, vec3(0.0)), 1.0);
        return;
    }
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
    if (post.params4.z > 0.0) {
        // Sensor noise: zero-mean triangular noise per pixel per frame, added in perceptual
        // (square-root) space so the shadows are not swamped; strongest in the darks.
        uvec3 q = uvec3(uvec2(gl_FragCoord.xy), uint(post.params4.w));
        float n = (hash13(q) + hash13(q + uvec3(0u, 0u, 7919u))) - 1.0;
        vec3 p = sqrt(c);
        float w = post.params4.z * 0.05 * mix(1.0, 0.35, smoothstep(0.0, 0.7, luma(p)));
        p = clamp(p + vec3(n * w), 0.0, 1.0);
        c = p * p;
    }
    if (post.params3.y > 0.5) {
        vec3 s = to_srgb(c);
        out_color = vec4(s, dot(s, vec3(0.299, 0.587, 0.114)));
        return;
    }
    if (post.params.z > 0.5) c = to_srgb(c);
    out_color = vec4(c, 1.0);
}
