#version 450
// SPDX-License-Identifier: Apache-2.0
// Fast approximate anti-aliasing after Lottes' FXAA 3.11 "quality" algorithm: local contrast
// test, edge orientation from a 3x3 luma neighbourhood, bidirectional end-of-edge search and
// sub-pixel blending. Input is sRGB-encoded colour with perceptual luma in alpha.
layout(location = 0) in vec2 v_uv;
layout(set = 2, binding = 0) uniform sampler2D u_ldr;
layout(set = 3, binding = 0) uniform Fxaa {
    vec4 params; // xy: 1 / size, z: decode to linear (sRGB target), w: sub-pixel quality
} u;
layout(location = 0) out vec4 out_color;

const int STEPS = 10;
const float STEP_SIZE[10] = float[](1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 4.0, 8.0);

float L(vec2 uv) { return textureLod(u_ldr, uv, 0.0).a; }
vec3 to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }
vec4 finish(vec3 c) { return vec4(u.params.z > 0.5 ? to_linear(c) : c, 1.0); }

void main() {
    vec2 px = u.params.xy;
    vec4 center = textureLod(u_ldr, v_uv, 0.0);
    float m = center.a;
    float n = L(v_uv + vec2(0, -px.y)), s = L(v_uv + vec2(0, px.y));
    float e = L(v_uv + vec2(px.x, 0)), w = L(v_uv + vec2(-px.x, 0));
    float lo = min(m, min(min(n, s), min(e, w)));
    float hi = max(m, max(max(n, s), max(e, w)));
    float range = hi - lo;
    if (range < max(0.0312, hi * 0.125)) { out_color = finish(center.rgb); return; }

    float nw = L(v_uv + vec2(-px.x, -px.y)), ne = L(v_uv + vec2(px.x, -px.y));
    float sw = L(v_uv + vec2(-px.x, px.y)), se = L(v_uv + vec2(px.x, px.y));
    // Second derivatives across the vertical axis (columns) detect horizontal edges.
    float horizontal = abs(nw + sw - 2.0 * w) + 2.0 * abs(n + s - 2.0 * m) + abs(ne + se - 2.0 * e);
    float vertical = abs(nw + ne - 2.0 * n) + 2.0 * abs(w + e - 2.0 * m) + abs(sw + se - 2.0 * s);
    bool is_h = horizontal >= vertical; // edge runs horizontally: blend vertically

    float l1 = is_h ? n : w, l2 = is_h ? s : e;
    float g1 = abs(l1 - m), g2 = abs(l2 - m);
    bool first = g1 >= g2;
    float gradient = 0.25 * max(g1, g2);
    float step_len = is_h ? px.y : px.x;
    float local_avg;
    if (first) { step_len = -step_len; local_avg = 0.5 * (l1 + m); }
    else local_avg = 0.5 * (l2 + m);

    vec2 uv = v_uv;
    if (is_h) uv.y += step_len * 0.5; else uv.x += step_len * 0.5;
    vec2 dir = is_h ? vec2(px.x, 0) : vec2(0, px.y);
    vec2 uv1 = uv - dir, uv2 = uv + dir;
    float e1 = L(uv1) - local_avg, e2 = L(uv2) - local_avg;
    bool done1 = abs(e1) >= gradient, done2 = abs(e2) >= gradient;
    for (int i = 1; i < STEPS && !(done1 && done2); ++i) {
        if (!done1) { uv1 -= dir * STEP_SIZE[i]; e1 = L(uv1) - local_avg; done1 = abs(e1) >= gradient; }
        if (!done2) { uv2 += dir * STEP_SIZE[i]; e2 = L(uv2) - local_avg; done2 = abs(e2) >= gradient; }
    }
    float d1 = is_h ? v_uv.x - uv1.x : v_uv.y - uv1.y;
    float d2 = is_h ? uv2.x - v_uv.x : uv2.y - v_uv.y;
    bool closer1 = d1 < d2;
    float dmin = min(d1, d2);
    float span = d1 + d2;
    float edge_offset = -dmin / span + 0.5;
    bool m_smaller = (m - local_avg) < 0.0;
    bool correct = ((closer1 ? e1 : e2) < 0.0) != m_smaller;
    float final_offset = correct ? edge_offset : 0.0;

    // Sub-pixel aliasing (single-pixel features).
    float avg = (2.0 * (n + s + e + w) + nw + ne + sw + se) / 12.0;
    float sub = clamp(abs(avg - m) / range, 0.0, 1.0);
    sub = (-2.0 * sub + 3.0) * sub * sub;
    float sub_offset = sub * sub * u.params.w;
    final_offset = max(final_offset, sub_offset);

    vec2 fuv = v_uv;
    if (is_h) fuv.y += final_offset * step_len; else fuv.x += final_offset * step_len;
    out_color = finish(textureLod(u_ldr, fuv, 0.0).rgb);
}
