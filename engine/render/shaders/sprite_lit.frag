#version 450
// SPDX-License-Identifier: Apache-2.0
// Lit 2D sprites (ADR-0065): the sprite colour is multiplied by the accumulated 2D light at this
// pixel (ambient plus point/spot lights).
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(set = 2, binding = 0) uniform sampler2D u_texture;
layout(set = 2, binding = 1) uniform sampler2D u_light;
layout(set = 3, binding = 0) uniform Params {
    vec4 screen; // xy: 1 / target size, z: 1 = the texture is sRGB, w: 1 = sRGB target (see sprite.frag)
} u;
layout(location = 0) out vec4 out_color;
vec3 linear_to_srgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}
vec3 srgb_to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
void main() {
    vec4 t = texture(u_texture, v_uv);
    if (u.screen.z > 0.5) t.rgb = linear_to_srgb(t.rgb);
    vec4 c = t * v_color;
    if (c.a <= 0.001) discard;
    vec3 light = textureLod(u_light, gl_FragCoord.xy * u.screen.xy, 0.0).rgb;
    vec3 rgb = c.rgb * light;
    if (u.screen.w > 0.5) rgb = srgb_to_linear(clamp(rgb, 0.0, 1.0));
    out_color = vec4(rgb, c.a);
}
