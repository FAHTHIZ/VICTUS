#version 450
// SPDX-License-Identifier: Apache-2.0
// Metallic-roughness PBR (glTF 2.0 model): GGX/Smith/Schlick, directional sun with cascaded
// shadow maps (2x2 atlas, PCF, dithered cascade transitions), forward point lights (up to 16),
// analytic sky environment lighting (diffuse + split-sum specular approximation), screen-space
// ambient occlusion and emissive.
//
// Project surface shaders (.vshader, ADR-0035) are spliced in at VICTUS_SURFACE_CODE: they
// define `void surface(SurfaceInput i, inout Surface s)`, which runs after the material's
// textures were sampled and before lighting. The interface below is a stable contract.
layout(location = 0) in vec3 v_world_pos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec4 v_tangent;

layout(set = 2, binding = 0) uniform sampler2D u_base_color;
layout(set = 2, binding = 1) uniform sampler2D u_metal_rough;
layout(set = 2, binding = 2) uniform sampler2D u_normal_map;
layout(set = 2, binding = 3) uniform sampler2D u_emissive;
layout(set = 2, binding = 4) uniform sampler2DShadow u_shadow;
layout(set = 2, binding = 5) uniform sampler2D u_ao;

struct PointLight { vec4 position_range; vec4 color_intensity; };
layout(set = 3, binding = 0) uniform Lighting {
    vec4 camera_pos;     // w: ambient occlusion enabled
    vec4 camera_forward; // xyz: view direction
    vec4 sun_dir;        // xyz: direction light travels, w: shadow enabled
    vec4 sun_color;      // rgb * intensity
    vec4 sky_color;      // rgb zenith ambient radiance, w: exposure
    vec4 horizon_color;  // rgb horizon ambient radiance
    vec4 ground_color;   // rgb ground ambient radiance, w: point light count
    vec4 shadow_params;  // x: atlas texel size, y: depth bias, z: normal bias (texels), w: cascade count
    vec4 cascade_params; // x: soft (3x3 PCF), y: atlas tile scale, z: transition fraction
    vec4 cascade_splits; // far view depth of each cascade
    vec4 cascade_texel;  // world-space texel size of each cascade
    vec4 screen;         // xy: 1 / render size, z: time in seconds (wraps every hour)
    mat4 cascade_vp[4];
    PointLight points[16];
} lighting;
layout(set = 3, binding = 1) uniform Material {
    vec4 base_color;
    vec4 emissive;       // rgb * strength
    vec4 params;         // x metallic, y roughness, z normal scale, w alpha cutoff
    vec4 flags;          // x has normal map, y unlit
    vec4 uv_transform;   // xy scale, zw offset (material tiling)
    vec4 custom[4];      // surface shader parameters (material "shader_params")
} material;

layout(location = 0) out vec4 out_color;

const float PI = 3.14159265359;

// What the lighting uses. Surface shaders may change any field.
struct Surface {
    vec3 base_color; // linear albedo
    float alpha;     // < the material's alpha cutoff discards the pixel
    vec3 normal;     // world space, unit length, facing the viewer
    float metallic;
    float roughness;
    vec3 emissive;   // linear radiance added after lighting
    float occlusion; // multiplies ambient light (1 = unoccluded)
    bool unlit;      // output base_color + emissive without lighting
};
// Read-only inputs for surface shaders.
struct SurfaceInput {
    vec2 uv;             // material-transformed texture coordinates
    vec2 mesh_uv;        // the mesh's own texture coordinates
    vec3 world_position;
    vec3 view_dir;       // unit vector towards the camera
    vec3 vertex_normal;  // interpolated geometric normal (world space)
    float time;          // seconds
    vec4 params[4];      // the material's shader parameters
};

// VICTUS_SURFACE_CODE

float distribution_ggx(float NdotH, float a) {
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d + 1e-7);
}
float geometry_smith(float NdotV, float NdotL, float roughness) {
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float gv = NdotV / (NdotV * (1.0 - k) + k);
    float gl = NdotL / (NdotL * (1.0 - k) + k);
    return gv * gl;
}
vec3 fresnel_schlick(float cos_theta, vec3 f0) { return f0 + (1.0 - f0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0); }

// Analytic environment: the same zenith/horizon/ground gradient as the sky, blurred towards
// the hemispherical average as roughness increases.
vec3 environment(vec3 dir, float roughness) {
    float h = clamp(dir.y, -1.0, 1.0);
    vec3 sharp = h > 0.0 ? mix(lighting.horizon_color.rgb, lighting.sky_color.rgb, sqrt(h))
                         : mix(lighting.horizon_color.rgb, lighting.ground_color.rgb, pow(-h, 0.4));
    vec3 blurred = mix(lighting.ground_color.rgb, lighting.sky_color.rgb, h * 0.5 + 0.5) * 0.5 + lighting.horizon_color.rgb * 0.5;
    return mix(sharp, blurred, roughness * roughness);
}
// Karis' analytic approximation of the split-sum environment BRDF (mobile).
vec2 env_brdf(float NdotV, float roughness) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

vec3 brdf(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float metallic, float roughness) {
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = max(dot(N, H), 0.0);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = fresnel_schlick(max(dot(H, V), 0.0), f0);
    float D = distribution_ggx(NdotH, roughness * roughness);
    float G = geometry_smith(NdotV, NdotL, roughness);
    vec3 spec = D * G * F / (4.0 * NdotV * NdotL + 1e-4);
    vec3 kd = (1.0 - F) * (1.0 - metallic);
    return (kd * albedo / PI + spec) * radiance * NdotL;
}

float interleaved_gradient_noise(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

float sample_cascade(int c, vec3 N, vec3 L) {
    float ndl = clamp(dot(N, L), 0.0, 1.0);
    vec3 offset = N * (lighting.cascade_texel[c] * lighting.shadow_params.z * (1.0 - ndl * 0.5));
    vec4 p4 = lighting.cascade_vp[c] * vec4(v_world_pos + offset, 1.0);
    vec3 p = p4.xyz / p4.w;
    vec2 uv = p.xy * vec2(0.5, -0.5) + 0.5;
    if (p.z > 1.0) return 1.0;
    float tile = lighting.cascade_params.y;
    vec2 origin = vec2(float(c & 1), float(c >> 1)) * tile;
    float texel = lighting.shadow_params.x;
    vec2 lo = origin + vec2(texel * 1.5), hi = origin + vec2(tile - texel * 1.5);
    vec2 auv = clamp(origin + uv * tile, lo, hi);
    float z = p.z - lighting.shadow_params.y;
    if (lighting.cascade_params.x < 0.5) return texture(u_shadow, vec3(auv, z));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            sum += texture(u_shadow, vec3(clamp(auv + vec2(x, y) * texel, lo, hi), z));
    return sum / 9.0;
}

float shadow_factor(vec3 N, vec3 L) {
    if (lighting.sun_dir.w < 0.5) return 1.0;
    int count = int(lighting.shadow_params.w);
    float depth = dot(v_world_pos - lighting.camera_pos.xyz, lighting.camera_forward.xyz);
    int c = 0;
    while (c < count - 1 && depth > lighting.cascade_splits[c]) ++c;
    float far_d = lighting.cascade_splits[c];
    if (depth > far_d) return 1.0;
    float near_d = c == 0 ? 0.0 : lighting.cascade_splits[c - 1];
    float t = (depth - near_d) / max(far_d - near_d, 1e-4);
    float band = lighting.cascade_params.z;
    float fade = clamp((1.0 - t) / max(band, 1e-4), 0.0, 1.0); // 1 inside, 0 at the far edge
    if (c < count - 1) {
        // Dithered transition into the next cascade hides the resolution seam.
        if (interleaved_gradient_noise(gl_FragCoord.xy) > fade) c += 1;
        return sample_cascade(c, N, L);
    }
    return mix(1.0, sample_cascade(c, N, L), fade); // last cascade fades out at shadow distance
}

void main() {
    vec2 uv = v_uv * material.uv_transform.xy + material.uv_transform.zw;
    vec4 base = texture(u_base_color, uv) * material.base_color;
    vec3 geometric_n = normalize(v_normal);
    if (!gl_FrontFacing) geometric_n = -geometric_n;
    Surface s;
    s.base_color = base.rgb;
    s.alpha = base.a;
    s.emissive = texture(u_emissive, uv).rgb * material.emissive.rgb;
    s.unlit = material.flags.y > 0.5;
    s.occlusion = 1.0;
    vec4 mr = texture(u_metal_rough, uv);
    s.metallic = clamp(material.params.x * mr.b, 0.0, 1.0);
    s.roughness = clamp(material.params.y * mr.g, 0.045, 1.0);
    vec3 N = geometric_n;
    if (material.flags.x > 0.5) {
        vec3 T = normalize(v_tangent.xyz - N * dot(N, v_tangent.xyz));
        vec3 B = cross(N, T) * v_tangent.w;
        vec3 tn = texture(u_normal_map, uv).xyz * 2.0 - 1.0;
        tn.xy *= material.params.z;
        N = normalize(mat3(T, B, N) * tn);
    }
    s.normal = N;
    vec3 V = normalize(lighting.camera_pos.xyz - v_world_pos);
#ifdef VICTUS_SURFACE_SHADER
    SurfaceInput si;
    si.uv = uv;
    si.mesh_uv = v_uv;
    si.world_position = v_world_pos;
    si.view_dir = V;
    si.vertex_normal = geometric_n;
    si.time = lighting.screen.z;
    si.params = material.custom;
    surface(si, s);
    s.normal = normalize(s.normal);
    s.metallic = clamp(s.metallic, 0.0, 1.0);
    s.roughness = clamp(s.roughness, 0.045, 1.0);
#endif
    if (s.alpha < material.params.w) discard;
    if (s.unlit) {
        out_color = vec4(s.base_color + s.emissive, s.alpha);
        return;
    }
    N = s.normal;
    vec3 albedo = s.base_color;
    float metallic = s.metallic;
    float roughness = s.roughness;

    vec3 L = normalize(-lighting.sun_dir.xyz);
    vec3 color = brdf(N, V, L, lighting.sun_color.rgb, albedo, metallic, roughness) * shadow_factor(N, L);

    int count = int(lighting.ground_color.w);
    for (int i = 0; i < count && i < 16; ++i) {
        vec3 to_light = lighting.points[i].position_range.xyz - v_world_pos;
        float dist = length(to_light);
        float range = lighting.points[i].position_range.w;
        float falloff = clamp(1.0 - pow(dist / range, 4.0), 0.0, 1.0);
        float atten = falloff * falloff / (dist * dist + 1.0);
        vec3 radiance = lighting.points[i].color_intensity.rgb * lighting.points[i].color_intensity.w * atten;
        color += brdf(N, V, to_light / max(dist, 1e-4), radiance, albedo, metallic, roughness);
    }

    float NdotV = max(dot(N, V), 1e-4);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 kd = (1.0 - fresnel_schlick(NdotV, f0)) * (1.0 - metallic);
    vec3 irradiance = environment(N, 1.0);
    vec2 ab = env_brdf(NdotV, roughness);
    vec3 R = reflect(-V, N);
    // Specular occlusion approximation: reflections below the local horizon are dimmed.
    float horizon = clamp(1.0 + dot(R, geometric_n), 0.0, 1.0);
    vec3 specular = environment(R, roughness) * (f0 * ab.x + ab.y) * horizon * horizon;
    float ao = lighting.camera_pos.w > 0.5 ? texture(u_ao, gl_FragCoord.xy * lighting.screen.xy).r : 1.0;
    ao *= s.occlusion;
    // Specular occlusion from AO (Lagarde & de Rousiers 2014 approximation).
    float spec_ao = clamp(pow(NdotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
    color += kd * albedo * irradiance * ao + specular * spec_ao;
    color += s.emissive;
    out_color = vec4(color, s.alpha);
}
