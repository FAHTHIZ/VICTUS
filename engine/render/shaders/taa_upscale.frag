#version 450
// SPDX-License-Identifier: Apache-2.0
// Temporal upscaling resolve (ADR-0055). Runs instead of taa.frag when the 3D scene renders below
// the output resolution: every output pixel is reconstructed from the jittered render-resolution
// samples around it, weighted by their true (unjittered) distance to the output pixel centre, and
// accumulated into an output-resolution history. Over a jitter cycle each output pixel receives
// samples from many sub-pixel positions, so the resolved image converges towards the detail of a
// native-resolution render instead of a bilinear blow-up. Reprojection (camera + object motion),
// Catmull-Rom history sampling, YCoCg variance clipping and luminance weighting follow taa.frag.
layout(location = 0) in vec2 v_uv; // output uv (unjittered)
layout(set = 2, binding = 0) uniform sampler2D u_current; // render resolution, jittered
layout(set = 2, binding = 1) uniform sampler2D u_history; // output resolution
layout(set = 2, binding = 2) uniform sampler2D u_depth;   // render resolution
layout(set = 2, binding = 3) uniform sampler2D u_motion;  // render resolution, RG: object motion in uv
layout(set = 2, binding = 4) uniform sampler2D u_reactive; // R8 reactive mask, render resolution (quant.y)
layout(set = 2, binding = 5) uniform sampler2D u_reactive_prev; // last frame's mask (quant.w)
layout(set = 3, binding = 0) uniform TaaUpscale {
    mat4 reproject; // current NDC -> previous clip (both unjittered)
    vec4 params;    // x: current-frame weight, y: history valid, z: clip gamma, w: object motion present
    vec4 screen;    // xy: 1 / render size, zw: 1 / history size
    vec4 jitter;    // xy: jitter in render pixels (x right, y down), zw: render size
    vec4 output_size; // xy: 1 / output size, zw: output size
    vec4 quant;       // x: compact HDR frame + 1 (0 off), y: reactive mask, z: its max current weight, w: last frame's mask
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
    vec2 rsize = u.jitter.zw;
    vec2 px = u.screen.xy;
    // This output pixel's centre in render pixels, and the render texel it falls in once the
    // jitter is undone (the sample of texel t lies at t + 0.5 - jitter in unjittered pixels).
    vec2 pos = v_uv * rsize;
    vec2 ratio = u.output_size.zw / rsize; // output pixels per render pixel
    vec2 base = floor(pos + u.jitter.xy);
    vec3 sum = vec3(0.0);
    float wsum = 0.0, wmax = 0.0;
    vec3 m1 = vec3(0.0), m2 = vec3(0.0);
    float closest = 1.0;
    vec2 closest_uv = (base + 0.5) * px;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 texel = clamp(base + vec2(x, y), vec2(0.0), rsize - 1.0);
            vec2 uv = (texel + 0.5) * px;
            vec3 c = safe(textureLod(u_current, uv, 0.0).rgb);
            // Gaussian reconstruction kernel measured in output pixels (radius ~1.8 output pixels).
            // exp(-1.5 d^2) is a little wider than a Blackman-Harris fit (2.29): it averages more
            // jittered samples per frame, which measured lower error and less shimmer (ADR-0142).
            vec2 d = (pos - (texel + 0.5 - u.jitter.xy)) * ratio;
            float w = exp(-1.5 * dot(d, d));
            vec3 cw = c / (1.0 + luma(c));
            sum += cw * w;
            wsum += w;
            wmax = max(wmax, w);
            vec3 cy = to_ycocg(cw);
            m1 += cy;
            m2 += cy * cy;
            float dd = textureLod(u_depth, uv, 0.0).r;
            if (dd < closest) {
                closest = dd;
                closest_uv = uv;
            }
        }
    }
    // Reconstructed current colour (luminance-weighted space keeps fireflies from spreading).
    vec3 cur_w = sum / max(wsum, 1e-5);
    vec3 current = cur_w / max(1.0 - luma(cur_w), 1e-4);
    if (u.params.y < 0.5) { out_color = vec4(safe(current), 1.0); return; }

    vec4 prev = u.reproject * vec4(v_uv.x * 2.0 - 1.0, 1.0 - v_uv.y * 2.0, closest, 1.0);
    vec2 prev_ndc = prev.xy / max(prev.w, 1e-6);
    vec2 prev_uv = vec2(prev_ndc.x * 0.5 + 0.5, 0.5 - prev_ndc.y * 0.5);
    if (u.params.w > 0.5) prev_uv -= textureLod(u_motion, closest_uv, 0.0).rg;
    if (prev.w <= 0.0 || any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0)))) {
        out_color = vec4(safe(current), 1.0);
        return;
    }
    vec3 history = history_catmull_rom(prev_uv);

    vec3 mean = m1 / 9.0;
    vec3 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0))) * u.params.z;
    vec3 lo = mean - sigma, hi = mean + sigma;
    vec3 hw = to_ycocg(history / (1.0 + luma(history)));
    vec3 centre = 0.5 * (lo + hi), extent = 0.5 * (hi - lo) + 1e-5;
    vec3 v = hw - centre;
    vec3 a = abs(v / extent);
    float m = max(a.x, max(a.y, a.z));
    if (m > 1.0) hw = centre + v / m;

    // A sample landing near this output pixel is trusted at the full current-frame weight; output
    // pixels between this frame's samples lean on the history until the jitter reaches them (at no
    // less than 30 % of the weight, so the history keeps refreshing between jitter hits).
    vec2 motion = (v_uv - prev_uv) * u.output_size.zw;
    float alpha = u.params.x * mix(0.3, 1.0, wmax);
    alpha = clamp(alpha + length(motion) * 0.01, 0.01, 0.5);
    // Reactive mask (ADR-0143, render resolution, filtered): particles, water and rain have no motion
    // vectors and change every frame, so the history is mostly wrong there; trust this frame instead.
    // Last frame's mask at the reprojected position: where a particle was, the history holds it, so the
    // pixel it just left must also take this frame (FSR2 decays its accumulation there instead).
    float reactive = 0.0;
    if (u.quant.y > 0.5) reactive = textureLod(u_reactive, v_uv, 0.0).r;
    if (u.quant.w > 0.5) reactive = max(reactive, textureLod(u_reactive_prev, prev_uv, 0.0).r);
    alpha = max(alpha, reactive * u.quant.z);
    vec3 r = from_ycocg(mix(hw, to_ycocg(cur_w), alpha));
    r = r / max(1.0 - luma(r), 1e-4);
    out_color = vec4(safe(r), 1.0);
    if (u.quant.x > 0.0) out_color.rgb = hdr_quantize(out_color.rgb, u.quant.x);
}
