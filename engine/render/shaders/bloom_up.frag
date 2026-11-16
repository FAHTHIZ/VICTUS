#version 450
// SPDX-License-Identifier: Apache-2.0
// Bloom upsample: 3x3 tent filter of the smaller level, added (blend) onto the larger one.
layout(location = 0) in vec2 v_uv;
layout(set = 2, binding = 0) uniform sampler2D u_src;
layout(set = 3, binding = 0) uniform Up {
    vec4 params; // xy: 1 / source size, z: filter radius in source texels, w: weight
} u;
layout(location = 0) out vec4 out_color;
void main() {
    vec2 t = u.params.xy * u.params.z;
    vec3 c = textureLod(u_src, v_uv, 0.0).rgb * 4.0;
    c += (textureLod(u_src, v_uv + vec2(-t.x, 0), 0.0).rgb + textureLod(u_src, v_uv + vec2(t.x, 0), 0.0).rgb +
          textureLod(u_src, v_uv + vec2(0, -t.y), 0.0).rgb + textureLod(u_src, v_uv + vec2(0, t.y), 0.0).rgb) * 2.0;
    c += textureLod(u_src, v_uv - t, 0.0).rgb + textureLod(u_src, v_uv + t, 0.0).rgb +
         textureLod(u_src, v_uv + vec2(-t.x, t.y), 0.0).rgb + textureLod(u_src, v_uv + vec2(t.x, -t.y), 0.0).rgb;
    out_color = vec4(c * (u.params.w / 16.0), 1.0);
}
