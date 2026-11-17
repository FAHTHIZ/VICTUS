#version 450
// SPDX-License-Identifier: Apache-2.0
// Lit 2D sprites (ADR-0065): the sprite colour is multiplied by the accumulated 2D light at this
// pixel (ambient plus point/spot lights).
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(set = 2, binding = 0) uniform sampler2D u_texture;
layout(set = 2, binding = 1) uniform sampler2D u_light;
layout(set = 3, binding = 0) uniform Params {
    vec4 screen; // xy: 1 / target size
} u;
layout(location = 0) out vec4 out_color;
void main() {
    vec4 c = texture(u_texture, v_uv) * v_color;
    if (c.a <= 0.001) discard;
    vec3 light = textureLod(u_light, gl_FragCoord.xy * u.screen.xy, 0.0).rgb;
    out_color = vec4(c.rgb * light, c.a);
}
