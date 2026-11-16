// SPDX-License-Identifier: Apache-2.0
// Shared renderer resources on one RHI device: default textures, samplers, built-in shader
// cache, texture and mesh uploads.
#pragma once
#include "victus/render/image.hpp"
#include "victus/render/mesh.hpp"
#include "victus/rhi/rhi.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace victus::render {

struct GpuMesh {
    rhi::BufferHandle vertices;
    rhi::BufferHandle indices;
    std::uint32_t vertex_count{0};
    std::uint32_t index_count{0};
    math::Aabb bounds;
    [[nodiscard]] bool valid() const noexcept { return vertices.valid() && indices.valid(); }
};

struct TextureOptions {
    bool srgb{true};
    bool mipmaps{true};
    std::string name;
};

class GpuContext {
public:
    [[nodiscard]] static Result<std::unique_ptr<GpuContext>> create(rhi::Device& device);
    ~GpuContext();
    GpuContext(const GpuContext&) = delete;
    GpuContext& operator=(const GpuContext&) = delete;

    [[nodiscard]] rhi::Device& device() noexcept { return device_; }

    [[nodiscard]] rhi::TextureHandle white() const noexcept { return white_; }
    [[nodiscard]] rhi::TextureHandle black() const noexcept { return black_; }
    [[nodiscard]] rhi::TextureHandle flat_normal() const noexcept { return flat_normal_; }
    [[nodiscard]] rhi::SamplerHandle linear_repeat() const noexcept { return linear_repeat_; }
    [[nodiscard]] rhi::SamplerHandle linear_clamp() const noexcept { return linear_clamp_; }
    [[nodiscard]] rhi::SamplerHandle nearest_clamp() const noexcept { return nearest_clamp_; }
    [[nodiscard]] rhi::SamplerHandle nearest_repeat() const noexcept { return nearest_repeat_; }
    [[nodiscard]] rhi::SamplerHandle shadow_compare() const noexcept { return shadow_; }

    // Built-in shader by source file name (e.g. "pbr.frag"); created once per context.
    [[nodiscard]] Result<rhi::ShaderHandle> shader(std::string_view name);

    // Uploads are recorded into their own command buffer and submitted immediately; queue
    // ordering guarantees they complete before later frame command buffers execute.
    [[nodiscard]] Result<rhi::TextureHandle> create_texture(const Image& image, const TextureOptions& options = {});
    // Uploads a complete pre-built mip chain (e.g. from the asset cooker); options.mipmaps is ignored.
    [[nodiscard]] Result<rhi::TextureHandle> create_texture_mips(std::span<const Image> chain, const TextureOptions& options = {});
    [[nodiscard]] Result<GpuMesh> create_mesh(const MeshData& mesh, std::string_view name = "mesh");
    void destroy(GpuMesh& mesh);
    // Best available depth format that can also be sampled (shadow maps).
    [[nodiscard]] rhi::Format sampled_depth_format() const noexcept { return sampled_depth_; }

private:
    explicit GpuContext(rhi::Device& d) : device_(d) {}
    rhi::Device& device_;
    rhi::TextureHandle white_, black_, flat_normal_;
    rhi::SamplerHandle linear_repeat_, linear_clamp_, nearest_clamp_, nearest_repeat_, shadow_;
    rhi::Format sampled_depth_{rhi::Format::D32_FLOAT};
    std::unordered_map<std::string, rhi::ShaderHandle> shaders_;
};

} // namespace victus::render
