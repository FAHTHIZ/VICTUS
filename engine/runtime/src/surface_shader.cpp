// SPDX-License-Identifier: Apache-2.0
#include "victus/runtime/surface_shader.hpp"

#include "victus/render/builtin_shaders.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/graph/material_graph.hpp"

#include <format>
#include <string>

namespace victus::runtime {

namespace {
// A shader with only a vertex function keeps the built-in surface (an empty surface function).
std::string with_surface_function(std::string_view code) {
    std::string out(code);
    if (!render::surface_code_has_function(code, "surface")) out += "\nvoid surface(SurfaceInput i, inout Surface s) {}\n";
    return out;
}

Result<shadercompiler::CompiledShader> compile_vertex(std::string_view code, std::string_view name, render::SurfaceVertexVariant variant,
                                                      bool want_msl, bool want_essl,
                                                      std::vector<shadercompiler::Diagnostic>* diagnostics) {
    shadercompiler::CompileOptions options;
    options.name = std::string(name);
    options.want_msl = want_msl;
    options.want_essl = want_essl;
    auto compiled = shadercompiler::compile_glsl(render::surface_vertex_source(code, variant), shadercompiler::Stage::Vertex, options,
                                                 diagnostics);
    if (!compiled) return compiled;
    const bool skinned = variant == render::SurfaceVertexVariant::Skinned || variant == render::SurfaceVertexVariant::ShadowSkinned;
    const shadercompiler::ResourceCounts& c = compiled->counts;
    // Frame + VertexMaterial; skinned variants read the joint palette (a texture under ES).
    const std::uint32_t palette_samplers = (want_essl && skinned) ? 1u : 0u;
    if (c.uniform_buffers > 2 || c.storage_textures != 0 || c.storage_buffers > (skinned ? 1u : 0u) || c.samplers > palette_samplers)
        return make_error(ErrorCode::InvalidArgument,
                          std::format("{}: vertex functions cannot declare their own uniforms, samplers or buffers "
                                      "(use i.params[0..3] for per-material values)",
                                      name));
    return compiled;
}

Result<shadercompiler::CompiledShader> compile_surface(std::string_view code_in, std::string_view name, bool want_msl, bool want_essl,
                                                       std::vector<shadercompiler::Diagnostic>* diagnostics) {
    const std::string code = with_surface_function(code_in);
    shadercompiler::CompileOptions options;
    options.name = std::string(name);
    options.want_msl = want_msl;
    options.want_essl = want_essl;
    auto compiled = shadercompiler::compile_glsl(render::surface_shader_source(code), shadercompiler::Stage::Fragment, options,
                                                 diagnostics);
    if (!compiled) return compiled;
    // A vertex function is validated here too (victus check / export), in its static variant.
    if (render::surface_code_has_function(code, "vertex"))
        if (auto v = compile_vertex(code, name, render::SurfaceVertexVariant::Static, false, false, diagnostics); !v)
            return std::move(v).error();
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
} // namespace

Result<shadercompiler::CompiledShader> compile_surface_shader(std::string_view code, std::string_view name, bool want_msl,
                                                              std::vector<shadercompiler::Diagnostic>* diagnostics) {
    return compile_surface(code, name, want_msl, false, diagnostics);
}

Result<shadercompiler::CompiledShader> compile_surface_shader(std::string_view code, std::string_view name, rhi::Backend backend,
                                                              std::vector<shadercompiler::Diagnostic>* diagnostics) {
    return compile_surface(code, name, backend == rhi::Backend::Metal, backend == rhi::Backend::GLES, diagnostics);
}

Result<rhi::ShaderHandle> create_surface_shader(rhi::Device& device, std::string_view code, std::string_view name) {
    auto compiled = compile_surface_shader(code, name, device.capabilities().backend);
    if (!compiled) return std::move(compiled).error();
    rhi::ShaderDesc d;
    d.name = std::string(name);
    d.stage = rhi::ShaderStage::Fragment;
    d.spirv = compiled->spirv;
    d.msl = compiled->msl;
    d.glsl_es = compiled->glsl_es;
    d.samplers = compiled->counts.samplers;
    d.storage_textures = compiled->counts.storage_textures;
    d.storage_buffers = compiled->counts.storage_buffers;
    d.uniform_buffers = compiled->counts.uniform_buffers;
    return device.create_shader(d);
}

bool surface_shader_has_vertex(std::string_view code) { return render::surface_code_has_function(code, "vertex"); }

Result<render::SurfaceVertexStage> create_surface_vertex_stage(rhi::Device& device, std::string_view code, std::string_view name) {
    render::SurfaceVertexStage stage;
    const rhi::Backend backend = device.capabilities().backend;
    auto fail = [&](Error e) -> Result<render::SurfaceVertexStage> {
        for (auto& sh : stage.shaders)
            if (sh) device.destroy(sh);
        return e;
    };
    for (std::size_t i = 0; i < render::kSurfaceVertexVariants; ++i) {
        const auto variant = static_cast<render::SurfaceVertexVariant>(i);
        auto compiled = compile_vertex(code, name, variant, backend == rhi::Backend::Metal, backend == rhi::Backend::GLES, nullptr);
        if (!compiled) return fail(std::move(compiled).error());
        rhi::ShaderDesc d;
        d.name = std::format("{}#vertex{}", name, i);
        d.stage = rhi::ShaderStage::Vertex;
        d.spirv = compiled->spirv;
        d.msl = compiled->msl;
        d.glsl_es = compiled->glsl_es;
        d.samplers = compiled->counts.samplers;
        d.storage_textures = compiled->counts.storage_textures;
        d.storage_buffers = compiled->counts.storage_buffers;
        d.uniform_buffers = compiled->counts.uniform_buffers;
        auto sh = device.create_shader(d);
        if (!sh) return fail(std::move(sh).error());
        stage.shaders[i] = *sh;
    }
    return stage;
}

Result<std::string> load_surface_shader_code(const assets::AssetDatabase& db, const Uuid& uuid) {
    auto text = db.load_shader_source(uuid);
    if (!text) return text;
    const assets::AssetRecord* rec = db.find(uuid);
    if (!rec || !rec->path.ends_with(graph::kMaterialGraphExtension)) return text;
    return graph::material_graph_to_glsl(*text, rec->path);
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
//
// Optional vertex function, run per vertex in every pass (including shadows):
//   void vertex(VertexInput i, inout Vertex v)
// VertexInput i:  position, normal (object space), uv, color, object_origin (world), time,
//                 shadow (bool), params[4]
// Vertex v:       world_position, normal (world), uv, color
// Example (sway in the wind, more at the top):
//   void vertex(VertexInput i, inout Vertex v) {
//       v.world_position.x += sin(i.time * 2.0 + i.object_origin.x) * 0.1 * i.position.y;
//   }

void surface(SurfaceInput i, inout Surface s) {
    // Pulse the emission with params[0].rgb as colour and params[0].w as speed.
    float pulse = 0.5 + 0.5 * sin(i.time * max(i.params[0].w, 0.1));
    s.emissive += i.params[0].rgb * pulse;
}
)";
}

} // namespace victus::runtime
