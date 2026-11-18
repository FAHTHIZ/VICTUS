#version 450
// SPDX-License-Identifier: Apache-2.0
// Maps the overdraw counter (overdraw.frag) to a heat ramp: 0 layers black, 1 dark blue, 2 green,
// 3 yellow, 4 orange, 5-7 red, 8 or more white.
layout(location = 0) in vec2 v_uv;
layout(set = 2, binding = 0) uniform sampler2D u_count;
layout(set = 3, binding = 0) uniform Heat {
    vec4 params; // x: 1 = decode to linear (sRGB target)
} u;
layout(location = 0) out vec4 out_color;

vec3 to_linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }

void main() {
    float n = floor(textureLod(u_count, v_uv, 0.0).r * 255.0 + 0.5);
    vec3 c = vec3(0.0);
    if (n >= 8.0) c = vec3(1.0);
    else if (n >= 5.0) c = vec3(0.9, 0.1, 0.05);
    else if (n >= 4.0) c = vec3(1.0, 0.5, 0.0);
    else if (n >= 3.0) c = vec3(0.95, 0.85, 0.1);
    else if (n >= 2.0) c = vec3(0.1, 0.75, 0.2);
    else if (n >= 1.0) c = vec3(0.1, 0.2, 0.6);
    out_color = vec4(u.params.x > 0.5 ? to_linear(c) : c, 1.0);
}
