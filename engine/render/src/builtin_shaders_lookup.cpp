// SPDX-License-Identifier: Apache-2.0
#include "victus/render/builtin_shaders.hpp"

#include <format>

namespace victus::render {

const BuiltinShader* find_builtin_shader(std::string_view name) noexcept {
    for (const auto& s : builtin_shaders())
        if (s.name == name) return &s;
    return nullptr;
}

Result<rhi::ShaderHandle> create_builtin_shader(rhi::Device& device, std::string_view name) {
    const BuiltinShader* s = find_builtin_shader(name);
    if (!s) return make_error(ErrorCode::NotFound, std::format("no built-in shader '{}'", name));
    rhi::ShaderDesc d;
    d.name = std::string(name);
    d.stage = s->stage;
    d.spirv = s->spirv;
    d.msl = s->msl;
    d.samplers = s->samplers;
    d.storage_textures = s->storage_textures;
    d.storage_buffers = s->storage_buffers;
    d.uniform_buffers = s->uniform_buffers;
    return device.create_shader(d);
}

} // namespace victus::render
