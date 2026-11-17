// SPDX-License-Identifier: Apache-2.0
// Project surface shaders (.vshader): a GLSL function
//
//   void surface(SurfaceInput i, inout Surface s) { ... }
//
// spliced into the built-in PBR fragment shader (see pbr.frag for the Surface/SurfaceInput
// fields) and compiled at load time with glslang (SPIR-V) and SPIRV-Cross (MSL). A surface
// shader may change colours, normals, roughness, emission, alpha, etc. but cannot declare new
// uniforms or samplers; it reads material values through the existing material/textures and
// up to four per-material vec4 parameters (`i.params[0..3]`, MaterialAsset::shader_params).
#pragma once
#include "victus/assets/database.hpp"
#include "victus/core/result.hpp"
#include "victus/shadercompiler/shader_compiler.hpp"

#include <string_view>
#include <vector>

namespace victus::runtime {

// Splices `code` into pbr.frag and compiles it. `name` appears in error messages as
// "<name>:<line>: ..." with line numbers counted from the start of `code`. Fails when the
// result needs resources the built-in pipeline does not bind.
[[nodiscard]] Result<shadercompiler::CompiledShader> compile_surface_shader(std::string_view code, std::string_view name,
                                                                            bool want_msl = true,
                                                                            std::vector<shadercompiler::Diagnostic>* diagnostics = nullptr);

// The surface function of a shader asset: a .vshader's text, or the GLSL a .vmatgraph material
// graph compiles to (ADR-0062). Graph errors name the file and node.
[[nodiscard]] Result<std::string> load_surface_shader_code(const assets::AssetDatabase& db, const Uuid& uuid);

// Template written by the editor's "New Surface Shader" action.
[[nodiscard]] std::string_view surface_shader_template() noexcept;

} // namespace victus::runtime
