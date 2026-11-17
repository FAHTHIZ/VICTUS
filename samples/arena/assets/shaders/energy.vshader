// Energy surface shader (used by materials/energy_cube.vmat).
// Runs per pixel before lighting. See the Surface/SurfaceInput fields in the VICTUS docs:
//   i.uv, i.mesh_uv, i.world_position, i.view_dir, i.vertex_normal, i.time, i.params[0..3]
//   s.base_color, s.alpha, s.normal, s.metallic, s.roughness, s.emissive, s.occlusion, s.unlit
// Material parameters: params[0] = glow colour (rgb) + band density (w),
//                      params[1] = x glow strength, y scroll speed, z rim strength
void surface(SurfaceInput i, inout Surface s) {
    vec3 glow = i.params[0].rgb;
    float bands = 0.5 + 0.5 * sin(i.world_position.y * i.params[0].w - i.time * i.params[1].y);
    bands = smoothstep(0.7, 1.0, bands);
    float rim = pow(1.0 - clamp(dot(s.normal, i.view_dir), 0.0, 1.0), 3.0);
    s.emissive += glow * (bands * i.params[1].x + rim * i.params[1].z);
    s.roughness = mix(s.roughness, 0.2, bands); // the bands read as polished metal
    s.metallic = max(s.metallic, bands * 0.6);
}
