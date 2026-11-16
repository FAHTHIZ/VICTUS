#version 450
// SPDX-License-Identifier: Apache-2.0
layout(location = 0) in vec3 in_pos;
layout(set = 1, binding = 0) uniform Shadow { mat4 light_view_proj; mat4 model; } u;
void main() { gl_Position = u.light_view_proj * u.model * vec4(in_pos, 1.0); }
