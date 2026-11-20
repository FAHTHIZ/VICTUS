// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "victus/rhi/rhi.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace victus::render {

struct BuiltinShader {
    std::string_view name;
    rhi::ShaderStage stage;
    std::span<const std::uint8_t> spirv;
    std::string_view msl;
    std::string_view glsl_es; // empty when the shader needs more than GLSL ES 3.00 (ADR-0144)
    // For compute shaders storage_textures/storage_buffers count the read-only bindings.
    std::uint32_t samplers, storage_textures, storage_buffers, uniform_buffers;
    std::uint32_t readwrite_storage_textures{0}, readwrite_storage_buffers{0}; // compute only
    std::uint32_t threads[3]{0, 0, 0};                                          // compute workgroup size
};

[[nodiscard]] std::span<const BuiltinShader> builtin_shaders() noexcept;
[[nodiscard]] const BuiltinShader* find_builtin_shader(std::string_view name) noexcept;
[[nodiscard]] Result<rhi::ShaderHandle> create_builtin_shader(rhi::Device& device, std::string_view name);
// Compute shaders become pipelines directly (SDL_GPU has no separate compute shader objects).
[[nodiscard]] Result<rhi::ComputePipelineHandle> create_builtin_compute_pipeline(rhi::Device& device, std::string_view name);

} // namespace victus::render
