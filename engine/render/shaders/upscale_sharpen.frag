#version 450
// SPDX-License-Identifier: Apache-2.0
// Spatial upscale and sharpen (ADR-0066): the final pass when the 3D view is rendered below output
// resolution without temporal upscaling (phones, Low/Medium tiers). The input is the tonemapped
// image at render resolution in an 8-bit sRGB texture, so every fetch and filter is linear light. It is resampled with a
// Catmull-Rom bicubic filter (9 bilinear taps; sharper than bilinear, no ringing past the lobes) and
// then sharpened with robust contrast-adaptive sharpening after AMD FidelityFX RCAS: the sharpening
// lobe is limited by the local min/max so it never overshoots into halos, and flat areas stay flat.
// With render scale 1 it runs as a plain RCAS pass (params.w > 0) at output resolution.
layout(location = 0) in vec2 v_uv;
layout(set = 2, binding = 0) uniform sampler2D u_ldr;
layout(set = 3, binding = 0) uniform Upscale {
    vec4 params; // xy: 1 / input size, z: 0 linear out, 1 encode to sRGB, 2 encode + luma in alpha (FXAA next), w: sharpness 0..1
    vec4 size;   // xy: input size in texels, z: 1 when the input is already at output size (no resampling)
} u;
layout(location = 0) out vec4 out_color;

vec3 to_srgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }
vec3 tap(vec2 uv) { return textureLod(u_ldr, uv, 0.0).rgb; }

// Catmull-Rom resampling with 9 bilinear fetches (weights folded into the middle taps).
vec3 catmull_rom(vec2 uv) {
    vec2 pos = uv * u.size.xy;
    vec2 c1 = floor(pos - 0.5) + 0.5;
    vec2 f = pos - c1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 o12 = w2 / w12;
    vec2 t0 = (c1 - 1.0) * u.params.xy;
    vec2 t3 = (c1 + 2.0) * u.params.xy;
    vec2 t12 = (c1 + o12) * u.params.xy;
    vec3 r = tap(vec2(t0.x, t0.y)) * w0.x * w0.y + tap(vec2(t12.x, t0.y)) * w12.x * w0.y + tap(vec2(t3.x, t0.y)) * w3.x * w0.y
           + tap(vec2(t0.x, t12.y)) * w0.x * w12.y + tap(vec2(t12.x, t12.y)) * w12.x * w12.y + tap(vec2(t3.x, t12.y)) * w3.x * w12.y
           + tap(vec2(t0.x, t3.y)) * w0.x * w3.y + tap(vec2(t12.x, t3.y)) * w12.x * w3.y + tap(vec2(t3.x, t3.y)) * w3.x * w3.y;
    return max(r, vec3(0.0));
}

void main() {
    vec2 px = u.params.xy;
    bool resample = u.size.z < 0.5;
    vec3 c = resample ? catmull_rom(v_uv) : tap(v_uv);
    // Cross neighbourhood one input texel away (bilinear at the output position).
    vec3 n = tap(v_uv + vec2(0.0, -px.y)), s = tap(v_uv + vec2(0.0, px.y));
    vec3 e = tap(v_uv + vec2(px.x, 0.0)), w = tap(v_uv + vec2(-px.x, 0.0));
    vec3 mn = min(min(n, s), min(e, w)), mx = max(max(n, s), max(e, w));
    // Catmull-Rom's negative lobes overshoot at hard edges; keep it inside the 2x2 input texels around the
    // sample (no ringing, the edge keeps its full contrast).
    if (resample) {
        vec2 c0 = (floor(v_uv * u.size.xy - 0.5) + 0.5) * px;
        vec3 a = tap(c0), b = tap(c0 + vec2(px.x, 0.0)), d = tap(c0 + vec2(0.0, px.y)), g = tap(c0 + px);
        c = clamp(c, min(min(a, b), min(d, g)), max(max(a, b), max(d, g)));
    }
    mn = min(mn, c);
    mx = max(mx, c);
    float sharp = u.params.w;
    if (sharp > 0.0) {
        // RCAS: the most negative lobe that keeps every channel inside [0, 1] after the filter.
        vec3 hit_min = mn / max(4.0 * mx, vec3(1e-4));
        vec3 hit_max = (1.0 - mx) / min(4.0 * mn - 4.0, vec3(-1e-4));
        vec3 lobe3 = max(-hit_min, hit_max);
        float lobe = max(-0.1875, min(max(lobe3.r, max(lobe3.g, lobe3.b)), 0.0)) * sharp;
        c = (lobe * (n + s + e + w) + c) / (4.0 * lobe + 1.0);
        c = clamp(c, mn, mx);
    }
    if (u.params.z > 1.5) {
        vec3 e = to_srgb(c);
        out_color = vec4(e, dot(e, vec3(0.299, 0.587, 0.114))); // luma in alpha for FXAA
        return;
    }
    out_color = vec4(u.params.z > 0.5 ? to_srgb(c) : c, 1.0);
}
