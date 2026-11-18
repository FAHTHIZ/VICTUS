// SPDX-License-Identifier: Apache-2.0
// Shared renderer resources on one RHI device: default textures, samplers, built-in shader
// cache, texture and mesh uploads.
#pragma once
#include "victus/render/image.hpp"
#include "victus/render/mesh.hpp"
#include "victus/rhi/rhi.hpp"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace victus::render {

// One level of detail inside GpuMesh::indices.
struct GpuMeshLod {
    std::uint32_t first_index{0}, index_count{0};
    float error{0.0f}; // geometric error in mesh units (0 for LOD 0)
};

struct GpuMesh {
    rhi::BufferHandle vertices;
    rhi::BufferHandle indices;
    rhi::BufferHandle skin; // SkinVertex stream (slot 2); valid only for skinned meshes
    std::uint32_t vertex_count{0};
    std::uint32_t index_count{0}; // LOD 0
    math::Aabb bounds;
    std::array<GpuMeshLod, kMaxMeshLods> lods{}; // lods[0] is the full mesh
    std::uint32_t lod_count{1};
    // Elements the buffers can hold (>= the counts above after update_mesh grew them with headroom).
    std::uint32_t vertex_capacity{0}, index_capacity{0};
    [[nodiscard]] bool valid() const noexcept { return vertices.valid() && indices.valid(); }
    [[nodiscard]] bool skinned() const noexcept { return skin.valid(); }
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
    // 1x1 black cubemap (no environment map).
    [[nodiscard]] rhi::TextureHandle black_cube() const noexcept { return black_cube_; }
    // 64x64 white disc with a smooth radial alpha falloff: the default particle texture.
    [[nodiscard]] rhi::TextureHandle soft_particle() const noexcept { return soft_particle_; }
    [[nodiscard]] rhi::SamplerHandle linear_repeat() const noexcept { return linear_repeat_; }
    [[nodiscard]] rhi::SamplerHandle linear_clamp() const noexcept { return linear_clamp_; }
    [[nodiscard]] rhi::SamplerHandle nearest_clamp() const noexcept { return nearest_clamp_; }
    [[nodiscard]] rhi::SamplerHandle nearest_repeat() const noexcept { return nearest_repeat_; }
    [[nodiscard]] rhi::SamplerHandle shadow_compare() const noexcept { return shadow_; }
    // Trilinear repeat sampler for material textures with up to `max_anisotropy` anisotropic taps
    // (rounded down to 1, 2, 4, 8 or 16; 1 = off). Created on first use and cached per level.
    // Devices without anisotropic filtering get the plain trilinear sampler.
    [[nodiscard]] Result<rhi::SamplerHandle> material_sampler(std::uint32_t max_anisotropy);
    // The level material_sampler(max_anisotropy) actually applies on this device.
    [[nodiscard]] std::uint32_t effective_anisotropy(std::uint32_t max_anisotropy) const noexcept;

    // Built-in shader by source file name (e.g. "pbr.frag"); created once per context.
    [[nodiscard]] Result<rhi::ShaderHandle> shader(std::string_view name);

    // Uploads are recorded into their own command buffer and submitted immediately; queue
    // ordering guarantees they complete before later frame command buffers execute.
    [[nodiscard]] Result<rhi::TextureHandle> create_texture(const Image& image, const TextureOptions& options = {});
    // Uploads a complete pre-built mip chain (e.g. from the asset cooker); options.mipmaps is ignored.
    [[nodiscard]] Result<rhi::TextureHandle> create_texture_mips(std::span<const Image> chain, const TextureOptions& options = {});
    // Uploads a block-compressed mip chain (BC/ASTC). Level m holds rhi::mip_size_bytes(format,
    // max(1, width >> m), max(1, height >> m)) bytes; options.srgb and options.mipmaps are ignored.
    [[nodiscard]] Result<rhi::TextureHandle> create_compressed_texture(rhi::Format format, std::uint32_t width, std::uint32_t height,
                                                                       std::span<const std::vector<std::uint8_t>> levels,
                                                                       const TextureOptions& options = {});
    [[nodiscard]] Result<GpuMesh> create_mesh(const MeshData& mesh, std::string_view name = "mesh");
    // Replaces the contents of a mesh that changes often (animated crowds, rebuilt chunks) without
    // recreating its GPU buffers each time: unskinned meshes without LODs are rewritten in place
    // (`discard` uploads, so a frame still in flight keeps the old data) while they fit, and regrown
    // with 50% headroom when they do not. Skinned or LOD meshes, and invalid `mesh`, are recreated.
    // On failure `mesh` is left unchanged.
    [[nodiscard]] Status update_mesh(GpuMesh& mesh, const MeshData& data, std::string_view name = "mesh");
    void destroy(GpuMesh& mesh);
    // Vertex variants: a mesh with its own vertex buffer (e.g. CPU-morphed shapes) sharing
    // `base`'s index, LOD and skin buffers. `vertices` must hold base.vertex_count entries.
    // update_vertex_variant rewrites them with a `discard` upload; destroy_vertex_variant frees
    // only the variant's vertex buffer (destroy `base` separately, after its variants).
    [[nodiscard]] Result<GpuMesh> create_vertex_variant(const GpuMesh& base, std::span<const Vertex3D> vertices,
                                                        std::string_view name = "variant");
    [[nodiscard]] Status update_vertex_variant(GpuMesh& variant, std::span<const Vertex3D> vertices);
    void destroy_vertex_variant(GpuMesh& variant);
    // Best available depth format that can also be sampled (shadow maps).
    [[nodiscard]] rhi::Format sampled_depth_format() const noexcept { return sampled_depth_; }

private:
    explicit GpuContext(rhi::Device& d) : device_(d) {}
    rhi::Device& device_;
    rhi::TextureHandle white_, black_, flat_normal_, soft_particle_, black_cube_;
    rhi::SamplerHandle linear_repeat_, linear_clamp_, nearest_clamp_, nearest_repeat_, shadow_;
    std::array<rhi::SamplerHandle, 5> material_samplers_{}; // 1x, 2x, 4x, 8x, 16x
    rhi::Format sampled_depth_{rhi::Format::D32_FLOAT};
    std::unordered_map<std::string, rhi::ShaderHandle> shaders_;
};

} // namespace victus::render
