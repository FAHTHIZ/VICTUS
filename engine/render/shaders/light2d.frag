#version 450
// SPDX-License-Identifier: Apache-2.0
// 2D point/spot light accumulated additively into the light target (ADR-0065). Falloff is a
// smooth window (1 - (d/r)^2)^2 raised to `falloff`, so light reaches exactly zero at the radius.
layout(location = 0) in vec2 v_offset;
layout(set = 3, binding = 0) uniform Params {
    vec4 color;     // linear rgb * intensity
    vec4 direction; // xy spot direction (unit), z cos(outer), w cos(inner); z <= -1: point light
    vec4 params;    // x radius, y falloff exponent
} u;
layout(location = 0) out vec4 out_color;
void main() {
    float d = length(v_offset) / u.params.x;
    if (d >= 1.0) discard;
    float w = 1.0 - d * d;
    float att = pow(w * w, u.params.y);
    if (u.direction.z > -1.0) {
        vec2 l = length(v_offset) > 1e-5 ? normalize(v_offset) : u.direction.xy;
        att *= smoothstep(u.direction.z, max(u.direction.w, u.direction.z + 1e-4), dot(l, u.direction.xy));
    }
    out_color = vec4(u.color.rgb * att, 1.0);
}
