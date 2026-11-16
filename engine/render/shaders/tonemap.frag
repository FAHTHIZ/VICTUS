#version 450
// SPDX-License-Identifier: Apache-2.0
// Exposure, ACES filmic tonemap (Narkowicz fit), vignette and sRGB encoding.
layout(location = 0) in vec2 v_uv;
layout(set = 2, binding = 0) uniform sampler2D u_hdr;
layout(set = 3, binding = 0) uniform Post { vec4 params; } post; // x exposure, y vignette, z encode sRGB, w tonemap on
layout(location = 0) out vec4 out_color;
vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
vec3 to_srgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }
void main() {
    vec4 hdr = texture(u_hdr, v_uv);
    vec3 c = hdr.rgb * post.params.x;
    if (post.params.w > 0.5) c = aces(c);
    vec2 d = v_uv - 0.5;
    c *= mix(1.0, 1.0 - dot(d, d) * 1.6, post.params.y);
    if (post.params.z > 0.5) c = to_srgb(clamp(c, 0.0, 1.0));
    out_color = vec4(c, 1.0);
}
