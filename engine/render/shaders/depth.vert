#version 450
// SPDX-License-Identifier: Apache-2.0
// Depth pre-pass: same uniform layout and arithmetic as mesh.vert (invariant position).
layout(location = 0) in vec3 in_pos;
layout(set = 1, binding = 0) uniform Frame {
    mat4 view_proj;
} frame;
layout(set = 1, binding = 1) uniform Object {
    mat4 model;
    mat4 normal_matrix;
} object;
invariant gl_Position;
void main() {
    vec4 world = object.model * vec4(in_pos, 1.0);
    gl_Position = frame.view_proj * world;
}
