#version 450
// SPDX-License-Identifier: Apache-2.0
// Rain splashes (ADR-0118): short-lived crowns where rain lands. Each instance repeatedly picks a
// random point within the rain radius around the camera, finds the first surface the rain meets
// there from the rain occlusion map (ADR-0114) and shows a growing, fading camera-facing crown on
// it. Points with nothing below them in the map are dropped. Shares precipitation.frag (type 2).
layout(set = 0, binding = 0) uniform sampler2D u_rain_depth; // rain occlusion depth
layout(set = 1, binding = 0) uniform Frame {
    mat4 view_proj;
    vec4 eye;    // xyz camera, w time (s, wrapped)
    vec4 right;  // camera right (world)
    vec4 up;     // camera up (world)
    vec4 params; // x radius (m), y crown size (m), z 1 / lifetime (s), w pixel size at 1 m
    vec4 color;  // rgb lit colour, a opacity
    mat4 rain_vp;  // world -> rain occlusion clip
    mat4 rain_inv; // rain occlusion clip -> world
} u;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out float v_distance;
layout(location = 3) out vec3 v_world;

vec3 hash3(uint n) {
    uvec3 v = uvec3(n * 1664525u + 1013904223u, n * 22695477u + 1u, n * 2891336453u + 747796405u);
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return vec3(v & uvec3(0xffffffu)) / 16777216.0;
}

void main() {
    const vec2 corners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 c = corners[gl_VertexIndex % 6];
    uint id = uint(gl_InstanceIndex);
    vec3 g = hash3(id * 7u + 3u);
    float cycle_f = u.eye.w * u.params.z + g.x; // each instance starts at its own phase
    float cycle = floor(cycle_f);
    float phase = cycle_f - cycle;
    vec3 h = hash3(id ^ (uint(int(cycle)) * 0x9e3779b9u));
    // A point on a disc around the camera (uniform by area), at camera height.
    float ang = h.x * 6.2831853, rad = sqrt(h.y) * u.params.x;
    vec3 column = vec3(u.eye.x + cos(ang) * rad, u.eye.y, u.eye.z + sin(ang) * rad);
    vec4 rc = u.rain_vp * vec4(column, 1.0);
    vec2 ndc = rc.xy / rc.w;
    vec2 uv = ndc * vec2(0.5, -0.5) + 0.5;
    float depth = textureLod(u_rain_depth, uv, 0.0).r;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || depth >= 0.99999) {
        gl_Position = vec4(0.0, 0.0, -2.0, 1.0); // nothing for the rain to land on: outside the clip volume
        v_uv = c;
        v_color = vec4(0.0);
        v_distance = 0.0;
        v_world = vec3(0.0);
        return;
    }
    vec4 hit4 = u.rain_inv * vec4(ndc, depth, 1.0);
    vec3 hit = hit4.xyz / hit4.w;
    float dist = length(hit - u.eye.xyz);
    float size = u.params.y * (0.6 + 0.8 * h.z) * (0.35 + 0.65 * phase);
    float alpha = u.color.a * (1.0 - phase) * (1.0 - phase) * (1.0 - smoothstep(0.8, 1.0, rad / max(u.params.x, 1e-3)));
    float min_size = max(dist, 0.1) * u.params.w;
    if (size < min_size) {
        alpha *= size / min_size;
        size = min_size;
    }
    // Crown standing on the surface: the quad's bottom edge touches the hit point.
    v_world = hit + (u.right.xyz * c.x + u.up.xyz * (c.y + 1.0)) * (0.5 * size);
    gl_Position = u.view_proj * vec4(v_world, 1.0);
    v_uv = c;
    v_color = vec4(u.color.rgb, alpha);
    v_distance = gl_Position.w;
}
