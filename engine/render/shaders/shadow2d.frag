#version 450
// SPDX-License-Identifier: Apache-2.0
// Writes 1 (shadow volume) or 0 (lit caster interior) into a light's shadow mask (ADR-0255).
layout(set = 3, binding = 0) uniform Params {
    vec4 value; // x: mask value
} u;
layout(location = 0) out vec4 out_mask;
void main() { out_mask = vec4(u.value.x, 0.0, 0.0, 1.0); }
