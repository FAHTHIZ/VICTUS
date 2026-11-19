#version 450
// SPDX-License-Identifier: Apache-2.0
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(set = 2, binding = 0) uniform sampler2D u_texture;
layout(set = 3, binding = 0) uniform Params {
    vec4 flags; // x: 1 = the texture is sRGB; encode its decoded texels back to display space
                // y: 1 = the target is sRGB; output linear so the hardware encode gives display colours
} u;
layout(location = 0) out vec4 out_color;
// 2D draws in display space: tints and untextured sprites are display colours, so sRGB textures
// (decoded to linear by the sampler) are encoded back before the tint multiplies them.
vec3 linear_to_srgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}
vec3 srgb_to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
void main() {
    vec4 t = texture(u_texture, v_uv);
    if (u.flags.x > 0.5) t.rgb = linear_to_srgb(t.rgb);
    vec4 c = t * v_color;
    if (c.a <= 0.001) discard;
    if (u.flags.y > 0.5) c.rgb = srgb_to_linear(clamp(c.rgb, 0.0, 1.0));
    out_color = c;
}
