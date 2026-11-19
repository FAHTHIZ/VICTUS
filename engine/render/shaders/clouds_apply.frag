#version 450
// SPDX-License-Identifier: Apache-2.0
// Volumetric clouds, pass 2 of 2 (ADR-0111): upsamples the cloud target bilinearly over the
// scene with premultiplied-alpha blending (result = scene * transmittance + cloud light).
// Pixels whose geometry is nearer than where the view ray enters the cloud layer keep the
// scene untouched, so silhouettes in front of the clouds stay sharp at full resolution.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D u_clouds;
layout(set = 2, binding = 1) uniform sampler2D u_depth;
layout(set = 3, binding = 0) uniform Params {
    mat4 inv_view_proj;
    vec4 camera; // xyz world position
    vec4 layer;  // x bottom altitude, y top altitude
    vec4 screen; // xy: 1 / render size
} u;

vec3 world_at(vec2 uv, float d) {
    vec4 p = u.inv_view_proj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
    return p.xyz / p.w;
}

void main() {
    vec2 uv = gl_FragCoord.xy * u.screen.xy;
    float d = textureLod(u_depth, uv, 0.0).r;
    if (d < 1.0) {
        vec3 o = u.camera.xyz;
        vec3 p = world_at(uv, d);
        vec3 ray = p - o;
        float dist = length(ray);
        vec3 dir = ray / max(dist, 1e-6);
        float entry;
        if (o.y > u.layer.x && o.y < u.layer.y) entry = 0.0;
        else if (abs(dir.y) < 1e-5) entry = 1e30;
        else {
            float ta = (u.layer.x - o.y) / dir.y, tb = (u.layer.y - o.y) / dir.y;
            entry = min(ta, tb) >= 0.0 ? min(ta, tb) : 1e30;
        }
        if (dist <= entry) { out_color = vec4(0.0); return; }
    }
    vec4 c = textureLod(u_clouds, uv, 0.0);
    out_color = vec4(c.rgb, clamp(c.a, 0.0, 1.0));
}
