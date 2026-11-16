#version 450
// SPDX-License-Identifier: Apache-2.0
// Depth pre-pass: same inputs and arithmetic as mesh.vert (invariant position), instanced.
layout(location = 0) in vec3 in_pos;
layout(location = 4) in vec4 in_model0;
layout(location = 5) in vec4 in_model1;
layout(location = 6) in vec4 in_model2;
layout(location = 7) in vec4 in_model3;
layout(set = 1, binding = 0) uniform Frame {
    mat4 view_proj;
} frame;
invariant gl_Position;
void main() {
    mat4 model = mat4(in_model0, in_model1, in_model2, in_model3);
    vec4 world = model * vec4(in_pos, 1.0);
    gl_Position = frame.view_proj * world;
}
