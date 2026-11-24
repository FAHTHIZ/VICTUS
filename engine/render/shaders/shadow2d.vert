#version 450
// SPDX-License-Identifier: Apache-2.0
// 2D shadow geometry (ADR-0255): caster edges extruded away from one light on the CPU, and the
// caster interiors that must stay lit, in world space.
layout(location = 0) in vec2 in_pos;
layout(set = 1, binding = 0) uniform Frame { mat4 view_proj; } frame;
void main() { gl_Position = frame.view_proj * vec4(in_pos, 0.0, 1.0); }
