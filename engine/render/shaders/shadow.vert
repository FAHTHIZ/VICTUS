#version 450
// SPDX-License-Identifier: Apache-2.0
// Shadow depth, instanced (model matrix per instance from vertex buffer slot 1).
layout(location = 0) in vec3 in_pos;
layout(location = 4) in vec4 in_model0;
layout(location = 5) in vec4 in_model1;
layout(location = 6) in vec4 in_model2;
layout(location = 7) in vec4 in_model3;
layout(set = 1, binding = 0) uniform Shadow { mat4 light_view_proj; } u;
void main() {
    mat4 model = mat4(in_model0, in_model1, in_model2, in_model3);
    gl_Position = u.light_view_proj * (model * vec4(in_pos, 1.0));
}
