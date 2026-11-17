#version 450
// SPDX-License-Identifier: Apache-2.0
// Temporal anti-aliasing resolve (Karis 2014, Salvi 2016 variance clipping). The current
// frame is rendered with a sub-pixel jitter; the history is reprojected from scene depth
// (closest depth of a 3x3 neighbourhood, so silhouettes follow the foreground), sampled with a
// 5-tap Catmull-Rom filter and clipped to the current neighbourhood's YCoCg colour box, then
// blended in luminance-weighted space to keep bright pixels from flickering.
layout(location = 0) in vec2 v_uv;
layout(set = 2, binding = 0) uniform sampler2D u_current;
layout(set = 2, binding = 1) uniform sampler2D u_history;
layout(set = 2, binding = 2) uniform sampler2D u_depth;
layout(set = 3, binding = 0) uniform Taa {
    mat4 reproject;  // current NDC -> previous clip (both unjittered)
    vec4 params;     // x: current-frame weight, y: history valid, z: clip gamma, w: unused
    vec4 screen;     // xy: 1 / render size, zw: 1 / history size
} u;
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
    vec2 px = u.screen.xy;
    vec3 current = safe(textureLod(u_current, v_uv, 0.0).rgb);
    // Neighbourhood moments (YCoCg of luminance-weighted colour) and closest depth.
    vec3 m1 = vec3(0.0), m2 = vec3(0.0);
    float closest = 1.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 uv = v_uv + vec2(x, y) * px;
            vec3 c = x == 0 && y == 0 ? current : safe(textureLod(u_current, uv, 0.0).rgb);
            c = to_ycocg(c / (1.0 + luma(c)));
            m1 += c;
            m2 += c * c;
            closest = min(closest, textureLod(u_depth, uv, 0.0).r);
        }
    }
    if (u.params.y < 0.5) { out_color = vec4(current, 1.0); return; }

    vec4 prev = u.reproject * vec4(v_uv.x * 2.0 - 1.0, 1.0 - v_uv.y * 2.0, closest, 1.0);
    vec2 prev_ndc = prev.xy / max(prev.w, 1e-6);
    vec2 prev_uv = vec2(prev_ndc.x * 0.5 + 0.5, 0.5 - prev_ndc.y * 0.5);
    if (prev.w <= 0.0 || any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0)))) {
        out_color = vec4(current, 1.0);
        return;
    }
    vec3 history = history_catmull_rom(prev_uv);

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
    vec3 r = from_ycocg(mix(hw, cw, alpha));
    r = r / max(1.0 - luma(r), 1e-4);
    out_color = vec4(safe(r), 1.0);
}
