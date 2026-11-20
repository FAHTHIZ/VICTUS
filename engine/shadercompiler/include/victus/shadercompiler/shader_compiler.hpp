// SPDX-License-Identifier: Apache-2.0
// Run-time shader compilation for project shaders: GLSL 450 (Vulkan rules) to SPIR-V with
// glslang, SPIR-V to Metal Shading Language 2.1 and GLSL ES 3.00 (ADR-0144) with SPIRV-Cross,
// and resource counts for SDL_GPU shader creation. Thread-safe; every call is independent.
#pragma once
#include "victus/core/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace victus::shadercompiler {

enum class Stage : std::uint8_t { Vertex, Fragment, Compute };

struct ResourceCounts {
    std::uint32_t samplers{0}, storage_textures{0}, storage_buffers{0}, uniform_buffers{0};
    friend bool operator==(const ResourceCounts&, const ResourceCounts&) = default;
};

// One problem reported by the compiler. `line` is 0 when the message has no location.
struct Diagnostic {
    std::uint32_t line{0};
    std::string message;
};

struct CompileOptions {
    std::string name{"shader"};                                  // used in messages
    std::vector<std::pair<std::string, std::string>> defines;     // -D name=value
    bool want_msl{true};                                          // also produce MSL
    // Also produce GLSL ES 3.00 for the GLES backend, compiled with VICTUS_ESSL=1 and following
    // tools/shaders/compile_shaders.py (GL clip space, vx_io_<location> varyings, per-stage
    // "vs_"/"fs_" names, "//! sampler|ubo|storage" lines). Graphics stages only.
    bool want_essl{false};
};

struct CompiledShader {
    std::vector<std::uint8_t> spirv; // SPIR-V words as bytes (little endian)
    std::string msl;                 // empty when not requested
    std::string glsl_es;             // empty when not requested
    ResourceCounts counts;
};

// Error message lists every diagnostic as "<name>:<line>: <message>"; `diagnostics` (when not
// null) receives them individually.
[[nodiscard]] Result<CompiledShader> compile_glsl(std::string_view source, Stage stage, const CompileOptions& options = {},
                                                  std::vector<Diagnostic>* diagnostics = nullptr);

// Resource counts of a SPIR-V module (bytes, as stored by CompiledShader::spirv).
[[nodiscard]] Result<ResourceCounts> reflect(const std::vector<std::uint8_t>& spirv);

} // namespace victus::shadercompiler
