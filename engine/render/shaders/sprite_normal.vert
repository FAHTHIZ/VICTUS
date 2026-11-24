#version 450
// SPDX-License-Identifier: Apache-2.0
// Normal buffer pass for lit 2D sprites (ADR-0255): the sprite quad with its world tangent frame
// packed as one float: the tangent angle, plus 8 when the flips mirror the frame.
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 3) in float in_frame;
layout(set = 1, binding = 0) uniform Frame { mat4 view_proj; } frame;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out float v_alpha;
layout(location = 2) out vec4 v_frame;
void main() {
    v_uv = in_uv;
    v_alpha = in_color.a;
    float mirrored = in_frame > 4.0 ? -1.0 : 1.0;
    float a = in_frame > 4.0 ? in_frame - 8.0 : in_frame;
    vec2 t = vec2(cos(a), sin(a));
    v_frame = vec4(t, mirrored * vec2(-t.y, t.x));
    gl_Position = frame.view_proj * vec4(in_pos, 0.0, 1.0);
}
