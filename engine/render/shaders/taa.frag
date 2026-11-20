#version 450
// SPDX-License-Identifier: Apache-2.0
// Temporal anti-aliasing resolve (Karis 2014, Salvi 2016 variance clipping). The current
// frame is rendered with a sub-pixel jitter; the history is reprojected from scene depth
// (closest depth of a 3x3 neighbourhood, so silhouettes follow the foreground), sampled with a
// 5-tap Catmull-Rom filter and clipped to the current neighbourhood's YCoCg colour box, then
// blended in luminance-weighted space to keep bright pixels from flickering. Surfaces that moved
// by themselves (rigid or skinned) carry object motion in u_motion (ADR-0054), added to the camera
// reprojection at the same closest-depth texel.
layout(location = 0) in vec2 v_uv;
layout(set = 2, binding = 0) uniform sampler2D u_current;
layout(set = 2, binding = 1) uniform sampler2D u_history;
layout(set = 2, binding = 2) uniform sampler2D u_depth;
layout(set = 2, binding = 3) uniform sampler2D u_motion; // RG: object motion in uv (when params.w)
layout(set = 2, binding = 4) uniform sampler2D u_reactive; // R8 reactive mask, render resolution (quant.y)
layout(set = 2, binding = 5) uniform sampler2D u_reactive_prev; // last frame's mask (quant.w)
layout(set = 2, binding = 6) uniform sampler2D u_depth_prev; // ADR-0147: linear depth history (depth_clip.x)
layout(set = 3, binding = 0) uniform Taa {
    mat4 reproject;  // current NDC -> previous clip (both unjittered)
    vec4 params;     // x: current-frame weight, y: history valid, z: clip gamma, w: object motion present
    vec4 screen;     // xy: 1 / render size, zw: 1 / history size
    vec4 quant;       // x: compact HDR frame + 1 (0 off), y: reactive mask, z: its max current weight, w: last frame's mask
    vec4 depth_clip;  // ADR-0147: x: disocclusion strength (0 off), yz: linear depth 1 / (y d + z), w: tolerance per unit depth
} u;

// Stochastic rounding to R11G11B10_FLOAT (compact HDR, ADR-0129). The result is exactly
// representable, so the hardware conversion (round-to-nearest or toward zero, which Vulkan leaves
// to the implementation) cannot bias it, and repeated stores (TAA history) do not drift darker.
// Interleaved gradient noise (Jimenez 2014) per channel, offset per frame.
vec3 hdr_quantize(vec3 c, float frame) {
    vec2 p = gl_FragCoord.xy + 5.588238 * frame;
    const vec2 k = vec2(0.06711056, 0.00583715);
    vec3 n = vec3(fract(52.9829189 * fract(dot(p, k))), fract(52.9829189 * fract(dot(p + vec2(17.0, 59.0), k))),
                  fract(52.9829189 * fract(dot(p + vec2(83.0, 31.0), k))));
    c = clamp(c, vec3(0.0), vec3(64512.0));
#ifdef VICTUS_ESSL
    // GLSL ES 3.00 has no frexp/ldexp: the exponent comes from the float bits, and the scales are
    // exact powers of two built from bits, so the products are exact as with ldexp.
    ivec3 e = ((floatBitsToInt(max(c, vec3(1e-30))) >> 23) & 0xff) - 126;
    ivec3 sh = max(e - 1, ivec3(-14)) - ivec3(6, 6, 5);
    return floor(c * intBitsToFloat((127 - sh) << 23) + n) * intBitsToFloat((sh + 127) << 23);
#else
    ivec3 e;
    frexp(max(c, vec3(1e-30)), e); // c = m * 2^e with m in [0.5, 1)
    // Scale by exact powers of two with ldexp, not exp2 and a division: Metal compiles with fast
    // math by default, where exp2 and division are approximate, so the result was not exactly
    // representable and truncating stores biased it dark again.
    ivec3 sh = max(e - 1, ivec3(-14)) - ivec3(6, 6, 5);
    return ldexp(floor(ldexp(c, -sh) + n), sh);
#endif
}
layout(location = 0) out vec4 out_color;
layout(location = 1) out float out_depth; // ADR-0147: linear depth history

// ADR-0147: linear view depth from 0..1 window depth (u.depth_clip.yz), clamped to the R16F range.
float linear_depth(float d) { return min(1.0 / max(u.depth_clip.y * d + u.depth_clip.z, 1e-6), 65000.0); }

// Depth-based disocclusion (ADR-0147, FSR2's depth clip in gather form). `reprojected` is the linear
// depth this surface had in the previous view. The farthest of the 3x3 depth-history texels around
// prev_uv must be nearer than that by more than the tolerance: only then was the surface hidden
// behind something last frame, and its history belongs to the occluder. Taking the farthest of a
// 3x3 footprint keeps silhouettes (whose dilated depth edge moves up to a pixel with the jitter) and
// slanted surfaces (whose taps straddle the exact depth) from false positives.
float disocclusion(vec2 prev_uv, float reprojected) {
    vec2 size = 1.0 / u.screen.zw;
    vec2 i0 = floor(prev_uv * size);
    float farthest = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 t = clamp(i0 + vec2(x, y), vec2(0.0), size - 1.0);
            farthest = max(farthest, textureLod(u_depth_prev, (t + 0.5) * u.screen.zw, 0.0).r);
        }
    }
    float tolerance = u.depth_clip.w * reprojected;
    float separation = reprojected - farthest;
    if (separation <= tolerance) return 0.0;
    float k = tolerance / separation;
    return 1.0 - k * k;
}

vec3 to_ycocg(vec3 c) {
    return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
vec3 from_ycocg(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 safe(vec3 c) { return any(isnan(c)) || any(isinf(c)) ? vec3(0.0) : max(c, vec3(0.0)); }

vec3 history_catmull_rom(vec2 uv) {
    vec2 size = 1.0 / u.screen.zw;
    vec2 pos = uv * size;
    vec2 c1 = floor(pos - 0.5) + 0.5;
    vec2 f = pos - c1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 t0 = (c1 - 1.0) * u.screen.zw, t3 = (c1 + 2.0) * u.screen.zw;
    vec2 t12 = (c1 + w2 / w12) * u.screen.zw;
    vec3 r = textureLod(u_history, vec2(t12.x, t0.y), 0.0).rgb * (w12.x * w0.y)
           + textureLod(u_history, vec2(t0.x, t12.y), 0.0).rgb * (w0.x * w12.y)
           + textureLod(u_history, t12, 0.0).rgb * (w12.x * w12.y)
           + textureLod(u_history, vec2(t3.x, t12.y), 0.0).rgb * (w3.x * w12.y)
           + textureLod(u_history, vec2(t12.x, t3.y), 0.0).rgb * (w12.x * w3.y);
    float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return safe(r / wsum);
}

void main() {
    vec2 px = u.screen.xy;
    vec3 current = safe(textureLod(u_current, v_uv, 0.0).rgb);
    // Neighbourhood moments (YCoCg of luminance-weighted colour) and closest depth.
    vec3 m1 = vec3(0.0), m2 = vec3(0.0);
    float closest = 1.0;
    vec2 closest_uv = v_uv;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 uv = v_uv + vec2(x, y) * px;
            vec3 c = x == 0 && y == 0 ? current : safe(textureLod(u_current, uv, 0.0).rgb);
            c = to_ycocg(c / (1.0 + luma(c)));
            m1 += c;
            m2 += c * c;
            float d = textureLod(u_depth, uv, 0.0).r;
            if (d < closest) {
                closest = d;
                closest_uv = uv;
            }
        }
    }
    // The pixel's own depth, not the dilated closest: a dilated history widens thin features (fence
    // posts) to fill the disocclusion footprint, and their sub-pixel flicker under the jitter then
    // reads as uncovered every frame.
    out_depth = linear_depth(textureLod(u_depth, v_uv, 0.0).r);
    if (u.params.y < 0.5) { out_color = vec4(current, 1.0); return; }

    vec4 prev = u.reproject * vec4(v_uv.x * 2.0 - 1.0, 1.0 - v_uv.y * 2.0, closest, 1.0);
    vec2 prev_ndc = prev.xy / max(prev.w, 1e-6);
    vec2 prev_uv = vec2(prev_ndc.x * 0.5 + 0.5, 0.5 - prev_ndc.y * 0.5);
    if (u.params.w > 0.5) prev_uv -= textureLod(u_motion, closest_uv, 0.0).rg;
    if (prev.w <= 0.0 || any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0)))) {
        out_color = vec4(current, 1.0);
        return;
    }
    vec3 history = history_catmull_rom(prev_uv);
    // ADR-0147: was this surface hidden behind something nearer last frame? Surfaces with object
    // motion are left to the colour clip: the camera-only reprojected depth is wrong for them (a
    // receding object would read as uncovered).
    float dis = 0.0;
    if (u.depth_clip.x > 0.0) {
        bool moving = false;
        if (u.params.w > 0.5) {
            vec2 mv = textureLod(u_motion, closest_uv, 0.0).rg;
            moving = dot(mv, mv) > 0.0;
        }
        // The far plane (sky) reprojects to itself; through the inverted matrices its linear depth
        // loses most of its precision (1 / (a d + b) with a d + b near zero) and would read as uncovered.
        float reprojected = linear_depth(closest >= 1.0 ? 1.0 : prev.z / prev.w);
        if (!moving) dis = u.depth_clip.x * disocclusion(prev_uv, reprojected);
    }

    vec3 mean = m1 / 9.0;
    vec3 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0))) * u.params.z;
    vec3 lo = mean - sigma, hi = mean + sigma;
    vec3 hw = to_ycocg(history / (1.0 + luma(history)));
    // Clip towards the box centre (not clamp per channel): keeps hue, avoids box-corner colours.
    vec3 centre = 0.5 * (lo + hi), extent = 0.5 * (hi - lo) + 1e-5;
    vec3 v = hw - centre;
    vec3 a = abs(v / extent);
    float m = max(a.x, max(a.y, a.z));
    if (m > 1.0) hw = centre + v / m;

    vec3 cw = to_ycocg(current / (1.0 + luma(current)));
    // Less history where the pixel moved a lot in screen space (reduces smearing on fast motion).
    vec2 motion = (v_uv - prev_uv) / px;
    float alpha = clamp(u.params.x + length(motion) * 0.01, u.params.x, 0.5);
    // Reactive mask (ADR-0143): particles, water and rain have no motion vectors; trust this frame there.
    // Last frame's mask at the reprojected position: where a particle was, the history holds it, so the
    // pixel it just left must also take this frame (FSR2 decays its accumulation there instead).
    float reactive = 0.0;
    if (u.quant.y > 0.5) reactive = textureLod(u_reactive, v_uv, 0.0).r;
    if (u.quant.w > 0.5) reactive = max(reactive, textureLod(u_reactive_prev, prev_uv, 0.0).r);
    alpha = max(alpha, reactive * u.quant.z);
    // Uncovered surfaces take this frame: their history belongs to the occluder (ADR-0147; a smaller
    // clip box or a partial weight measured worse in the stripes test, also when upscaling).
    alpha = mix(alpha, 1.0, dis);
    vec3 r = from_ycocg(mix(hw, cw, alpha));
    r = r / max(1.0 - luma(r), 1e-4);
    out_color = vec4(safe(r), 1.0);
    if (u.quant.x > 0.0) out_color.rgb = hdr_quantize(out_color.rgb, u.quant.x);
}
