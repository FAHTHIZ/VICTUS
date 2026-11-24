#version 450
// SPDX-License-Identifier: Apache-2.0
// Writes a lit sprite's world normal into the 2D normal buffer (ADR-0255). Sprites without a
// normal map still overwrite the buffer (alpha 0) so lights ignore the normals of sprites they cover.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in float v_alpha;
layout(location = 2) in vec4 v_frame;
layout(set = 2, binding = 0) uniform sampler2D u_texture;
layout(set = 2, binding = 1) uniform sampler2D u_normal_map; // tangent space, green up
layout(set = 3, binding = 0) uniform Params {
    vec4 flags; // x: 1 = normal map bound
} u;
layout(location = 0) out vec4 out_normal;
void main() {
    if (texture(u_texture, v_uv).a * v_alpha < 0.5) discard;
    if (u.flags.x < 0.5) {
        out_normal = vec4(0.5, 0.5, 1.0, 0.0);
        return;
    }
    vec3 t = texture(u_normal_map, v_uv).xyz * 2.0 - 1.0;
    vec3 n = normalize(vec3(v_frame.xy * t.x + v_frame.zw * t.y, max(t.z, 0.0)));
    out_normal = vec4(n * 0.5 + 0.5, 1.0);
}
