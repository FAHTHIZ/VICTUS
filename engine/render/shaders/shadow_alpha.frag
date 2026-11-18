#version 450
// SPDX-License-Identifier: Apache-2.0
// Alpha-tested shadow casters (ADR-0067): discard where the base colour alpha is below the material's
// cutoff, so foliage casts dappled shade instead of a solid silhouette of its cards.
layout(location = 0) in vec2 v_uv;
layout(set = 2, binding = 0) uniform sampler2D u_base_color;
layout(set = 3, binding = 0) uniform AlphaTest {
    vec4 uv;     // xy: uv scale, zw: uv offset (Material::uv_scale / uv_offset)
    vec4 params; // x: alpha cutoff, y: base colour alpha
} m;
void main() {
    float a = texture(u_base_color, v_uv * m.uv.xy + m.uv.zw).a * m.params.y;
    if (a < m.params.x) discard;
}
