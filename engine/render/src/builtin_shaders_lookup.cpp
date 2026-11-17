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

Result<rhi::ComputePipelineHandle> create_builtin_compute_pipeline(rhi::Device& device, std::string_view name) {
    const BuiltinShader* s = find_builtin_shader(name);
    if (!s) return make_error(ErrorCode::NotFound, std::format("no built-in shader '{}'", name));
    if (s->stage != rhi::ShaderStage::Compute)
        return make_error(ErrorCode::InvalidArgument, std::format("built-in shader '{}' is not a compute shader", name));
    rhi::ComputePipelineDesc d;
    d.name = std::string(name);
    d.spirv = s->spirv;
    d.msl = s->msl;
    d.samplers = s->samplers;
    d.readonly_storage_textures = s->storage_textures;
    d.readonly_storage_buffers = s->storage_buffers;
    d.readwrite_storage_textures = s->readwrite_storage_textures;
    d.readwrite_storage_buffers = s->readwrite_storage_buffers;
    d.uniform_buffers = s->uniform_buffers;
    d.threads_x = s->threads[0];
    d.threads_y = s->threads[1];
    d.threads_z = s->threads[2];
    return device.create_compute_pipeline(d);
}

} // namespace victus::render
