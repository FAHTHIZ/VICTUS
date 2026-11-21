#version 450
// SPDX-License-Identifier: Apache-2.0
// Responsive blended meshes (ADR-0187): writes the TAA reactive mask where an alpha-blended or
// additive mesh with Material::responsive > 0 covers a pixel. Blended meshes write no motion
// vectors, so without this the resolve blends a moving glass pane with a history that does not
// move with it. Drawn after the blended geometry into the R8 mask (alpha blending, like particles),
// with the mesh vertex stages; the pass has no depth attachment and tests against the sampled
// opaque depth instead, so it works with MSAA scene targets.
layout(location = 2) in vec2 v_uv;
layout(location = 4) in vec4 v_color; // linear rgb, alpha: opacity or coverage mask (params.z)
layout(set = 2, binding = 0) uniform sampler2D u_base_color;
layout(set = 2, binding = 1) uniform sampler2D u_depth;
layout(set = 3, binding = 0) uniform Params {
    vec4 base_color;   // material base colour (alpha multiplies coverage)
    vec4 uv_transform; // xy scale, zw offset
    vec4 params;       // x responsive 0..1, y alpha cutoff, z vertex alpha is a coverage mask, w additive
    vec4 screen;       // xy 1 / render size
} u;
layout(location = 0) out vec4 out_reactive;

void main() {
    vec2 suv = gl_FragCoord.xy * u.screen.xy;
    if (gl_FragCoord.z > textureLod(u_depth, suv, 0.0).r) discard; // behind opaque geometry
    vec4 base = texture(u_base_color, v_uv * u.uv_transform.xy + u.uv_transform.zw) * u.base_color;
    float coverage = u.params.z > 0.5 ? base.a : base.a * v_color.a;
    if (coverage < u.params.y) discard;
    // Additive surfaces cover by brightness rather than alpha (as additive particles do), so a
    // faint glow keeps most of its history and a bright one trusts the current frame.
    if (u.params.w > 0.5) coverage *= min(dot(base.rgb * v_color.rgb, vec3(0.2126, 0.7152, 0.0722)) * 4.0, 1.0);
    coverage = clamp(coverage, 0.0, 1.0);
    if (coverage <= 0.0) discard;
    float r = clamp(u.params.x, 0.0, 1.0);
    out_reactive = vec4(r, r, r, coverage);
}
