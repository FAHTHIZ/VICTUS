#version 450
// SPDX-License-Identifier: Apache-2.0
// 2D point/spot light accumulated additively into the light target (ADR-0065). Falloff is a
// smooth window (1 - (d/r)^2)^2 raised to `falloff`, so light reaches exactly zero at the radius.
// Normal-mapped sprites (ADR-0255) take N.L from the screen-space normal buffer, with the light
// `height` above the sprite plane; shadow casters darken the pixels in this light's shadow mask.
layout(location = 0) in vec2 v_offset;
layout(set = 2, binding = 0) uniform sampler2D u_normals; // rgb normal * 0.5 + 0.5, a 1 = normal-mapped
layout(set = 2, binding = 1) uniform sampler2D u_shadow;  // r 1 = in this light's shadow
layout(set = 3, binding = 0) uniform Params {
    vec4 color;     // linear rgb * intensity
    vec4 direction; // xy spot direction (unit), z cos(outer), w cos(inner); z <= -1: point light
    vec4 params;    // x radius, y falloff exponent, z height above the sprite plane, w shadow strength (0 = none)
    vec4 screen;    // xy 1 / target size, z 1 = normal buffer bound
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
    vec2 suv = gl_FragCoord.xy * u.screen.xy;
    if (u.screen.z > 0.5) {
        vec4 n = textureLod(u_normals, suv, 0.0);
        if (n.a > 0.5) {
            vec3 N = normalize(n.xyz * 2.0 - 1.0);
            vec3 L = normalize(vec3(-v_offset, max(u.params.z, 1e-3)));
            att *= max(dot(N, L), 0.0);
        }
    }
    if (u.params.w > 0.0) att *= 1.0 - u.params.w * textureLod(u_shadow, suv, 0.0).r;
    out_color = vec4(u.color.rgb * att, 1.0);
}
