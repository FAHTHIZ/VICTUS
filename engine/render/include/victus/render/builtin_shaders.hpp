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
    std::uint32_t samplers, storage_textures, storage_buffers, uniform_buffers;
};

[[nodiscard]] std::span<const BuiltinShader> builtin_shaders() noexcept;
[[nodiscard]] const BuiltinShader* find_builtin_shader(std::string_view name) noexcept;
[[nodiscard]] Result<rhi::ShaderHandle> create_builtin_shader(rhi::Device& device, std::string_view name);

} // namespace victus::render
