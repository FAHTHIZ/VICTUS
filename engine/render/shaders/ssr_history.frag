#version 450
// SPDX-License-Identifier: Apache-2.0
// Reflection history without TAA (ADR-0250). Writes this frame's HDR colour (a 2x2 box when the
// target is half size: bilinear at the texel corner) and linear depth, the two images TAA otherwise
// leaves for next frame's screen-space reflections (ADR-0199).
layout(location = 0) in vec2 v_uv;
layout(set = 2, binding = 0) uniform sampler2D u_current;
layout(set = 2, binding = 1) uniform sampler2D u_depth;
layout(set = 3, binding = 0) uniform SsrHistory {
    vec4 depth; // xy: linear depth 1 / (x d + y) of the 0..1 perspective depth d
} u;
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_depth;

void main() {
    vec3 c = textureLod(u_current, v_uv, 0.0).rgb;
    // Same guard as TAA: NaN or negative values must not reach the reflections.
    c = any(isnan(c)) || any(isinf(c)) ? vec3(0.0) : min(max(c, vec3(0.0)), vec3(64512.0));
    out_color = vec4(c, 1.0);
    // The farthest of the 2x2 block: a nearer sample would put a grazing floor in front of its own
    // reflected rays (self-hits) once the history is half size.
    vec4 d = textureGather(u_depth, v_uv, 0);
    vec4 lin = 1.0 / max(u.depth.x * d + u.depth.y, vec4(1e-6));
    float far_d = max(max(lin.x, lin.y), max(lin.z, lin.w));
    out_depth = vec2(min(far_d, 65000.0), 0.0);
}
