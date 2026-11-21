#version 450
// SPDX-License-Identifier: Apache-2.0
// Depth-aware 4x4 blur matching the 4x4 period of the SSAO rotation noise. Red is ambient
// occlusion, green the sun contact shadow (ADR-0183); both share the weights.
layout(location = 0) in vec2 v_uv;
layout(set = 2, binding = 0) uniform sampler2D u_ao;
layout(set = 2, binding = 1) uniform sampler2D u_depth;
layout(set = 3, binding = 0) uniform Blur {
    vec4 proj;   // view z = proj.x / (depth * proj.y + proj.z)
    vec4 screen; // xy: 1 / ao size
} u;
layout(location = 0) out vec4 out_ao;
float view_z(vec2 uv) { return u.proj.x / (textureLod(u_depth, uv, 0.0).r * u.proj.y + u.proj.z); }
void main() {
    float z0 = view_z(v_uv);
    vec2 sum = vec2(0.0);
    float wsum = 0.0;
    for (int y = -2; y < 2; ++y)
        for (int x = -2; x < 2; ++x) {
            vec2 uv = v_uv + (vec2(x, y) + 0.5) * u.screen.xy;
            float w = 1.0 / (1e-3 + abs(view_z(uv) - z0) / max(abs(z0), 1e-3) * 40.0);
            sum += textureLod(u_ao, uv, 0.0).rg * w;
            wsum += w;
        }
    vec2 ao = sum / max(wsum, 1e-5);
    out_ao = vec4(ao, 0.0, 1.0);
}
