#version 450
// SPDX-License-Identifier: Apache-2.0
// Camera-facing ribbon strips (particle trails). The CPU emits a triangle list with two vertices
// per trail point (one per edge); each vertex is pushed sideways, perpendicular to the trail
// tangent and to the direction toward the eye, so the strip always faces the camera. Shares
// particle.frag (soft depth fade) and the particle pass.
layout(location = 0) in vec4 in_pos_half;  // xyz world position on the trail centre, w signed half width
layout(location = 1) in vec4 in_color;     // linear RGBA, rgb may exceed 1 (HDR)
layout(location = 2) in vec4 in_tangent_v; // xyz trail tangent (world), w texture v along the trail
layout(set = 1, binding = 0) uniform Frame {
    mat4 view_proj;
    vec4 right; // camera right (world), fallback when the trail points at the eye
    vec4 up;    // camera up (world)
    vec4 eye;   // camera position (world)
} frame;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out float v_distance; // view-space depth (clip w)
layout(location = 3) out vec3 v_world;     // lit particles (ADR-0214)
void main() {
    vec3 to_eye = frame.eye.xyz - in_pos_half.xyz;
    vec3 side = cross(in_tangent_v.xyz, to_eye);
    float len = length(side);
    side = len > 1e-6 * max(length(to_eye), 1e-3) ? side / len : frame.right.xyz;
    vec3 world = in_pos_half.xyz + side * in_pos_half.w;
    gl_Position = frame.view_proj * vec4(world, 1.0);
    v_world = world;
    v_uv = vec2(in_pos_half.w < 0.0 ? 0.0 : 1.0, in_tangent_v.w); // x across the trail, y along it
    v_color = in_color;
    v_distance = gl_Position.w;
}
