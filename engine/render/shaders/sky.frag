#version 450
// SPDX-License-Identifier: Apache-2.0
// Sky background per view ray: the scene's environment cubemap (optionally blurred by sampling a
// prefiltered mip) or the procedural zenith/horizon/ground gradient plus sun disc.
layout(location = 0) in vec2 v_uv;
layout(set = 2, binding = 0) uniform samplerCube u_env;
layout(set = 3, binding = 0) uniform Sky {
    mat4 inv_view_proj;
    vec4 zenith;
    vec4 horizon;
    vec4 ground;
    vec4 sun_dir;   // direction towards the sun
    vec4 sun_color; // rgb * intensity, w: disc size (cos)
    vec4 env;       // x: environment map present (2: also draw the sun disc), y: mip (blur), z: intensity, w: rotation (radians about +Y)
} sky;
layout(location = 0) out vec4 out_color;
void main() {
    vec2 ndc = vec2(v_uv.x * 2.0 - 1.0, 1.0 - v_uv.y * 2.0);
    vec4 far_p = sky.inv_view_proj * vec4(ndc, 1.0, 1.0);
    vec4 near_p = sky.inv_view_proj * vec4(ndc, 0.0, 1.0);
    vec3 dir = normalize(far_p.xyz / far_p.w - near_p.xyz / near_p.w);
    if (sky.env.x > 0.5) {
        float c = cos(sky.env.w), s = sin(sky.env.w);
        vec3 d = vec3(c * dir.x + s * dir.z, dir.y, -s * dir.x + c * dir.z);
        vec3 e = textureLod(u_env, d, sky.env.y).rgb * sky.env.z;
        // Physical skies are baked without the sun disc (too small for the cube): add it here.
        if (sky.env.x > 1.5) e += sky.sun_color.rgb * smoothstep(sky.sun_color.w, 1.0, dot(dir, normalize(sky.sun_dir.xyz)));
        out_color = vec4(e, 1.0);
        return;
    }
    float h = dir.y;
    vec3 c = h > 0.0 ? mix(sky.horizon.rgb, sky.zenith.rgb, pow(h, 0.5)) : mix(sky.horizon.rgb, sky.ground.rgb, pow(-h, 0.4));
    float s = dot(dir, normalize(sky.sun_dir.xyz));
    c += sky.sun_color.rgb * smoothstep(sky.sun_color.w, 1.0, s) + sky.sun_color.rgb * 0.08 * pow(max(s, 0.0), 64.0);
    out_color = vec4(c, 1.0);
}
