#version 450
// SPDX-License-Identifier: Apache-2.0
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(set = 1, binding = 0) uniform Frame { mat4 view_proj; } frame;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
void main() {
    v_uv = in_uv;
    v_color = in_color;
    gl_Position = frame.view_proj * vec4(in_pos, 0.0, 1.0);
}
