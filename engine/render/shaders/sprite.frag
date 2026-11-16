#version 450
// SPDX-License-Identifier: Apache-2.0
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(set = 2, binding = 0) uniform sampler2D u_texture;
layout(location = 0) out vec4 out_color;
void main() {
    vec4 c = texture(u_texture, v_uv) * v_color;
    if (c.a <= 0.001) discard;
    out_color = c;
}
