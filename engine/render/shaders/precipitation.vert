#version 450
// SPDX-License-Identifier: Apache-2.0
// Rain streaks and snow flakes (ADR-0113), six vertices per instance and no vertex buffer: each
// drop's place in a box around the camera comes from a hash of its instance index, it moves by
// its velocity x time, and the box wraps around the camera so drops stay fixed in the world
// while the camera moves. Rain is a quad stretched along the velocity, snow a camera-facing disc
// that sways. Drops narrower than a pixel are widened to one pixel with their alpha lowered so
// distant rain does not flicker.
layout(set = 1, binding = 0) uniform Frame {
    mat4 view_proj;
    vec4 eye;      // xyz camera position, w time (seconds, wrapped)
    vec4 velocity; // xyz fall velocity incl. wind (m/s), w type (0 rain, 1 snow)
    vec4 volume;   // x box size (m), y drop width (m), z streak (s), w pixel size at 1 m
    vec4 color;    // rgb lit colour, a opacity
    vec4 right;    // camera right (world)
    vec4 up;       // camera up (world)
} u;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out float v_distance;

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
    vec3 h = hash3(id);
    vec3 g = hash3(id + 0x9e3779b9u);
    float size = u.volume.x;
    bool snow = u.velocity.w > 0.5;
    float t = u.eye.w;
    vec3 vel = u.velocity.xyz * (0.85 + 0.3 * g.x);
    vec3 p = h * size + vel * t;
    if (snow) { // flakes flutter around their path
        float ph = g.y * 6.2831853;
        p += vec3(sin(t * (0.9 + g.z) + ph), 0.0, cos(t * (0.7 + g.x) + ph * 1.7)) * 0.35;
    }
    vec3 local = mod(p - u.eye.xyz, vec3(size)) - 0.5 * size;
    vec3 world = u.eye.xyz + local;
    float dist = length(local);
    // Fade towards the box faces (no popping as drops wrap) and right in front of the camera.
    vec3 edge = abs(local) / (0.5 * size);
    float fade = (1.0 - smoothstep(0.7, 1.0, max(edge.x, max(edge.y, edge.z)))) * smoothstep(0.3, 1.2, dist);
    float width = u.volume.y * (0.7 + 0.6 * g.z);
    float min_width = max(dist, 0.1) * u.volume.w;
    float alpha = u.color.a * fade;
    if (width < min_width) {
        alpha *= width / min_width;
        width = min_width;
    }
    vec3 offset;
    if (snow) {
        offset = (u.right.xyz * c.x + u.up.xyz * c.y) * (0.5 * width);
    } else {
        float speed = max(length(vel), 1e-3);
        vec3 axis = vel / speed;
        vec3 to_eye = normalize(u.eye.xyz - world);
        vec3 side = cross(axis, to_eye);
        float sl = length(side);
        side = sl > 1e-4 ? side / sl : u.right.xyz;
        float len = max(speed * u.volume.z, width);
        offset = side * (c.x * 0.5 * width) + axis * (c.y * 0.5 * len);
    }
    gl_Position = u.view_proj * vec4(world + offset, 1.0);
    v_uv = c;
    v_color = vec4(u.color.rgb, alpha);
    v_distance = gl_Position.w;
}
