#version 450
// SPDX-License-Identifier: Apache-2.0
// Scalable ambient obscurance (McGuire et al. 2012, "Alchemy" estimator) from the depth
// buffer alone: view-space positions are reconstructed from depth, normals from the smaller
// of the neighbouring depth differences, and samples are spread on a per-pixel rotated
// golden-angle spiral whose screen radius shrinks with distance.
// The green channel carries screen-space contact shadows for the sun (ADR-0183): a short ray
// march from each pixel towards the sun through the depth buffer catches the small-scale
// occlusion (feet on the ground, cracks, bolts, foliage) that cascaded shadow maps are too coarse
// to resolve, in the spirit of Unreal's Contact Shadows and Days Gone's screen-space shadows.
layout(location = 0) in vec2 v_uv;
layout(set = 2, binding = 0) uniform sampler2D u_depth;
layout(set = 3, binding = 0) uniform Ssao {
    mat4 inv_proj;
    vec4 params;  // x: world radius, y: intensity, z: bias, w: sample count
    vec4 screen;  // xy: 1 / depth size, z: projection scale (pixels per unit at depth 1), w: fade distance
    mat4 proj;
    vec4 light;   // contact shadows: xyz direction towards the sun (view space), w: ray length (m)
    vec4 contact; // x: steps, y: surface thickness (m), z: enabled, w: fade distance (m)
} u;
layout(location = 0) out vec4 out_ao;

vec3 view_pos(vec2 uv) {
    float d = textureLod(u_depth, uv, 0.0).r;
    vec4 p = u.inv_proj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
    return p.xyz / p.w;
}

// Fraction of sunlight reaching P past nearby depth-buffer occluders (1 = unoccluded).
float contact_shadow(vec3 P, vec3 N, float noise) {
    vec3 Ld = u.light.xyz;
    if (u.contact.z < 0.5 || dot(N, Ld) <= 0.0) return 1.0; // N.L already darkens back faces
    int steps = int(u.contact.x);
    float len = u.light.w;
    vec3 start = P + N * (0.002 * -P.z); // lift off the surface against self-shadowing
    for (int i = 0; i < steps; ++i) {
        float t = (float(i) + noise) / float(steps) * len;
        vec3 Q = start + Ld * t;
        vec4 c = u.proj * vec4(Q, 1.0);
        if (c.w <= 1e-4) break;
        vec2 uv = vec2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
        if (uv.x <= 0.0 || uv.y <= 0.0 || uv.x >= 1.0 || uv.y >= 1.0) break;
        float d = textureLod(u_depth, uv, 0.0).r;
        if (d >= 1.0) continue; // sky
        vec4 s4 = u.inv_proj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
        float dz = s4.z / s4.w - Q.z; // > 0: the visible surface is in front of the ray sample
        // Thickness grows with distance so far pixels (coarser depth) still find their occluders.
        float thickness = u.contact.y * (1.0 + 0.02 * -Q.z);
        if (dz > 0.0015 * -Q.z && dz < thickness) {
            // Soften occluders at the end of the ray and near the screen edges.
            float edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
            return 1.0 - smoothstep(0.0, 0.05, edge) * (1.0 - t / len * 0.5);
        }
    }
    return 1.0;
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
    int count = u.params.y > 0.0 ? int(u.params.w) : 0; // contact shadows alone skip the AO taps
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float angle0 = 6.2831853 * noise;
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
    float ao = max(0.0, 1.0 - 2.0 * u.params.y * sum / float(max(count, 1)));
    // Fade out with distance: far away the reconstruction is undersampled and grazing.
    ao = mix(ao, 1.0, smoothstep(u.screen.w * 0.5, u.screen.w, -P.z));
    float cs = contact_shadow(P, N, noise);
    cs = mix(cs, 1.0, smoothstep(u.contact.w * 0.5, u.contact.w, -P.z));
    out_ao = vec4(ao, cs, 0.0, 1.0);
}
