#version 450
// SPDX-License-Identifier: Apache-2.0
// Camera-facing particle billboards, one instance per particle (vertex buffer slot 0,
// per-instance). The six quad corners come from gl_VertexIndex, so no vertex buffer is needed.
layout(location = 0) in vec4 in_pos_size; // xyz world position, w size (metres, full width)
layout(location = 1) in vec4 in_color;    // linear RGBA, rgb may exceed 1 (HDR)
layout(location = 2) in vec4 in_params;   // x rotation (radians)
layout(set = 1, binding = 0) uniform Frame {
    mat4 view_proj;
    vec4 right; // camera right (world)
    vec4 up;    // camera up (world)
} frame;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out float v_distance; // view-space depth (clip w)
void main() {
    const vec2 corners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 c = corners[gl_VertexIndex % 6];
    float s = sin(in_params.x), k = cos(in_params.x);
    vec2 r = vec2(c.x * k - c.y * s, c.x * s + c.y * k);
    vec3 world = in_pos_size.xyz + (frame.right.xyz * r.x + frame.up.xyz * r.y) * (0.5 * in_pos_size.w);
    gl_Position = frame.view_proj * vec4(world, 1.0);
    v_uv = c * vec2(0.5, -0.5) + 0.5;
    v_color = in_color;
    v_distance = gl_Position.w;
}
