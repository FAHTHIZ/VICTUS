// SPDX-License-Identifier: Apache-2.0
#include "victus/runtime/surface_shader.hpp"

#include "victus/render/builtin_shaders.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/script/material_graph.hpp"

#include <format>
#include <string>

namespace victus::runtime {

Result<shadercompiler::CompiledShader> compile_surface_shader(std::string_view code, std::string_view name, bool want_msl,
                                                              std::vector<shadercompiler::Diagnostic>* diagnostics) {
    shadercompiler::CompileOptions options;
    options.name = std::string(name);
    options.want_msl = want_msl;
    auto compiled = shadercompiler::compile_glsl(render::surface_shader_source(code), shadercompiler::Stage::Fragment, options,
                                                 diagnostics);
    if (!compiled) return compiled;
    const render::BuiltinShader* pbr = render::find_builtin_shader("pbr.frag");
    if (!pbr) return make_error(ErrorCode::Unknown, "built-in pbr.frag is missing");
    const shadercompiler::ResourceCounts& c = compiled->counts;
    if (c.samplers != pbr->samplers || c.uniform_buffers != pbr->uniform_buffers || c.storage_buffers != pbr->storage_buffers ||
        c.storage_textures != pbr->storage_textures)
        return make_error(ErrorCode::InvalidArgument,
                          std::format("{}: surface shaders cannot declare their own uniforms, samplers or buffers "
                                      "(use i.params[0..3] for per-material values)",
                                      name));
    return compiled;
}

Result<std::string> load_surface_shader_code(const assets::AssetDatabase& db, const Uuid& uuid) {
    auto text = db.load_shader_source(uuid);
    if (!text) return text;
    const assets::AssetRecord* rec = db.find(uuid);
    if (!rec || !rec->path.ends_with(script::kMaterialGraphExtension)) return text;
    return script::material_graph_to_glsl(*text, rec->path);
}

std::string_view surface_shader_template() noexcept {
    return R"(// VICTUS surface shader. Runs per pixel before lighting; change any field of `s`.
//
// SurfaceInput i:  uv, mesh_uv, world_position, view_dir, vertex_normal, time (seconds),
//                  params[4] (vec4 values set per material in the Material Editor)
// Surface s:       base_color, alpha, normal (world), metallic, roughness, emissive,
//                  occlusion, unlit (bool)
// Material textures were already sampled into `s`; you can also sample them again with
// texture(u_base_color, i.uv) (also u_metal_rough, u_normal_map, u_emissive).

void surface(SurfaceInput i, inout Surface s) {
    // Pulse the emission with params[0].rgb as colour and params[0].w as speed.
    float pulse = 0.5 + 0.5 * sin(i.time * max(i.params[0].w, 0.1));
    s.emissive += i.params[0].rgb * pulse;
}
)";
}

} // namespace victus::runtime
