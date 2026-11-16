#version 450
// SPDX-License-Identifier: Apache-2.0
// Scalable ambient obscurance (McGuire et al. 2012, "Alchemy" estimator) from the depth
// buffer alone: view-space positions are reconstructed from depth, normals from the smaller
// of the neighbouring depth differences, and samples are spread on a per-pixel rotated
// golden-angle spiral whose screen radius shrinks with distance.
layout(location = 0) in vec2 v_uv;
layout(set = 2, binding = 0) uniform sampler2D u_depth;
layout(set = 3, binding = 0) uniform Ssao {
    mat4 inv_proj;
    vec4 params;  // x: world radius, y: intensity, z: bias, w: sample count
    vec4 screen;  // xy: 1 / depth size, z: projection scale (pixels per unit at depth 1), w: fade distance
} u;
layout(location = 0) out vec4 out_ao;

vec3 view_pos(vec2 uv) {
    float d = textureLod(u_depth, uv, 0.0).r;
    vec4 p = u.inv_proj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
    return p.xyz / p.w;
}

void main() {
    float d = textureLod(u_depth, v_uv, 0.0).r;
    if (d >= 1.0) { out_ao = vec4(1.0); return; }
    vec3 P = view_pos(v_uv);
    vec2 px = u.screen.xy;
    vec3 r = view_pos(v_uv + vec2(px.x, 0.0)) - P, l = P - view_pos(v_uv - vec2(px.x, 0.0));
    vec3 t = view_pos(v_uv + vec2(0.0, px.y)) - P, b = P - view_pos(v_uv - vec2(0.0, px.y));
    vec3 dx = abs(r.z) < abs(l.z) ? r : l;
    vec3 dy = abs(t.z) < abs(b.z) ? t : b;
    vec3 N = normalize(cross(dy, dx));
    if (dot(N, -P) < 0.0) N = -N;

    float radius = u.params.x;
    float screen_radius = radius * u.screen.z / max(-P.z, 1e-3); // pixels
    int count = int(u.params.w);
    float angle0 = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float sum = 0.0;
    float r2 = radius * radius;
    for (int i = 0; i < count; ++i) {
        float a = (float(i) + 0.5) / float(count);
        float ang = angle0 + float(i) * 2.39996323; // golden angle
        vec2 offset = vec2(cos(ang), sin(ang)) * sqrt(a) * max(screen_radius, 1.0) * px;
        vec3 v = view_pos(v_uv + offset) - P;
        float vv = dot(v, v);
        float vn = dot(v, N);
        float falloff = max(r2 - vv, 0.0) / r2;
        sum += falloff * max(vn - u.params.z * -P.z, 0.0) / (vv + 0.01);
    }
    float ao = max(0.0, 1.0 - 2.0 * u.params.y * sum / float(count));
    // Fade out with distance: far away the reconstruction is undersampled and grazing.
    ao = mix(ao, 1.0, smoothstep(u.screen.w * 0.5, u.screen.w, -P.z));
    out_ao = vec4(ao, ao, ao, 1.0);
}
