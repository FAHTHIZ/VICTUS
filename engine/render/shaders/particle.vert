#version 450
// SPDX-License-Identifier: Apache-2.0
// Camera-facing particle billboards, one instance per particle (vertex buffer slot 0,
// per-instance). The six quad corners come from gl_VertexIndex, so no vertex buffer is needed.
layout(location = 0) in vec4 in_pos_size; // xyz world position, w size (metres, full width)
layout(location = 1) in vec4 in_color;    // linear RGBA, rgb may exceed 1 (HDR)
layout(location = 2) in vec4 in_params;   // x rotation (radians), y flipbook frame, z flipbook grid
                                          // (columns * 16 + rows, 0 = whole texture), w height / width (0 = 1)
layout(set = 1, binding = 0) uniform Frame {
    mat4 view_proj;
    vec4 right; // camera right (world)
    vec4 up;    // camera up (world)
} frame;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out float v_distance; // view-space depth (clip w)
layout(location = 3) out vec3 v_world;     // lit particles (ADR-0214)
void main() {
    const vec2 corners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 c = corners[gl_VertexIndex % 6];
    vec2 q = vec2(c.x, c.y * (in_params.w > 0.0 ? in_params.w : 1.0)); // stretched along the rotated up axis
    float s = sin(in_params.x), k = cos(in_params.x);
    vec2 r = vec2(q.x * k - q.y * s, q.x * s + q.y * k);
    vec3 world = in_pos_size.xyz + (frame.right.xyz * r.x + frame.up.xyz * r.y) * (0.5 * in_pos_size.w);
    gl_Position = frame.view_proj * vec4(world, 1.0);
    v_world = world;
    v_uv = c * vec2(0.5, -0.5) + 0.5;
    if (in_params.z >= 17.0) { // flipbook: frames run left to right, top to bottom
        float cols = floor(in_params.z / 16.0), rows = in_params.z - cols * 16.0;
        float f = floor(in_params.y + 0.5);
        v_uv = (v_uv + vec2(mod(f, cols), floor(f / cols))) / vec2(cols, rows);
    }
    v_color = in_color;
    v_distance = gl_Position.w;
}
