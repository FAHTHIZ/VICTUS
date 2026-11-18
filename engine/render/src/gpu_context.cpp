// SPDX-License-Identifier: Apache-2.0
#include "victus/render/gpu_context.hpp"

#include "victus/render/builtin_shaders.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <vector>

namespace victus::render {

namespace {
Result<rhi::TextureHandle> solid(GpuContext& ctx, std::uint32_t rgba, bool srgb, const char* name) {
    Image img;
    img.width = img.height = 4;
    img.pixels.resize(4 * 4 * 4);
    for (int i = 0; i < 16; ++i) std::memcpy(img.pixels.data() + i * 4, &rgba, 4);
    return ctx.create_texture(img, {srgb, false, name});
}
} // namespace

Result<std::unique_ptr<GpuContext>> GpuContext::create(rhi::Device& device) {
    std::unique_ptr<GpuContext> ctx(new GpuContext(device));
    auto w = solid(*ctx, 0xFFFFFFFFu, true, "default_white");
    if (!w) return std::move(w).error();
    ctx->white_ = w.value();
    auto b = solid(*ctx, 0xFF000000u, true, "default_black");
    if (!b) return std::move(b).error();
    ctx->black_ = b.value();
    auto n = solid(*ctx, 0xFFFF8080u, false, "default_flat_normal");
    if (!n) return std::move(n).error();
    ctx->flat_normal_ = n.value();
    {
        // Alpha = (1 - r^2)^2: smooth to the edge with no visible rim when scaled up.
        Image disc;
        disc.width = disc.height = 64;
        disc.pixels.resize(64 * 64 * 4);
        for (std::uint32_t y = 0; y < 64; ++y)
            for (std::uint32_t x = 0; x < 64; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f) / 32.0f - 1.0f, dy = (static_cast<float>(y) + 0.5f) / 32.0f - 1.0f;
                const float f = std::max(0.0f, 1.0f - (dx * dx + dy * dy));
                std::uint8_t* px = disc.pixels.data() + (y * 64 + x) * 4;
                px[0] = px[1] = px[2] = 255;
                px[3] = static_cast<std::uint8_t>(std::lround(255.0f * f * f));
            }
        auto sp = ctx->create_texture(disc, {false, true, "default_soft_particle"});
        if (!sp) return std::move(sp).error();
        ctx->soft_particle_ = sp.value();
    }

    {
        // 1x1 black cubemap bound wherever a scene has no environment map.
        rhi::TextureDesc cd;
        cd.name = "default_black_cube";
        cd.type = rhi::TextureType::Cube;
        cd.format = rhi::Format::RGBA8_UNORM;
        auto cube = device.create_texture(cd);
        if (!cube) return std::move(cube).error();
        ctx->black_cube_ = cube.value();
        const std::uint8_t px[4] = {0, 0, 0, 255};
        auto cmd = device.begin_commands();
        for (std::uint32_t face = 0; face < 6; ++face) cmd.upload_texture(ctx->black_cube_, px, 0, face);
        cmd.submit();
    }

    auto make_sampler = [&](rhi::Filter f, rhi::AddressMode a, bool compare) -> Result<rhi::SamplerHandle> {
        rhi::SamplerDesc s;
        s.min_filter = s.mag_filter = s.mip_filter = f;
        s.address_u = s.address_v = s.address_w = a;
        s.compare = compare;
        s.max_anisotropy = (f == rhi::Filter::Linear && !compare && device.capabilities().anisotropy) ? 8.0f : 0.0f;
        return device.create_sampler(s);
    };
    struct {
        rhi::SamplerHandle* out;
        rhi::Filter f;
        rhi::AddressMode a;
        bool cmp;
    } samplers[] = {
        {&ctx->linear_repeat_, rhi::Filter::Linear, rhi::AddressMode::Repeat, false},
        {&ctx->linear_clamp_, rhi::Filter::Linear, rhi::AddressMode::ClampToEdge, false},
        {&ctx->nearest_clamp_, rhi::Filter::Nearest, rhi::AddressMode::ClampToEdge, false},
        {&ctx->nearest_repeat_, rhi::Filter::Nearest, rhi::AddressMode::Repeat, false},
        {&ctx->shadow_, rhi::Filter::Linear, rhi::AddressMode::ClampToEdge, true},
    };
    for (auto& s : samplers) {
        auto r = make_sampler(s.f, s.a, s.cmp);
        if (!r) return std::move(r).error();
        *s.out = r.value();
    }
    const std::uint32_t depth_usage = rhi::TextureUsageDepthTarget | rhi::TextureUsageSampled;
    if (!device.supports_format(rhi::Format::D32_FLOAT, depth_usage)) ctx->sampled_depth_ = rhi::Format::D16_UNORM;
    return ctx;
}

GpuContext::~GpuContext() {
    for (auto& [name, s] : shaders_) device_.destroy(s);
    for (auto t : {white_, black_, flat_normal_, soft_particle_, black_cube_})
        if (t) device_.destroy(t);
    for (auto s : {linear_repeat_, linear_clamp_, nearest_clamp_, nearest_repeat_, shadow_})
        if (s) device_.destroy(s);
    for (auto s : material_samplers_)
        if (s) device_.destroy(s);
}

std::uint32_t GpuContext::effective_anisotropy(std::uint32_t max_anisotropy) const noexcept {
    if (!device_.capabilities().anisotropy) return 1;
    std::uint32_t level = 1;
    while (level < 16 && level * 2 <= max_anisotropy) level *= 2;
    return level;
}

Result<rhi::SamplerHandle> GpuContext::material_sampler(std::uint32_t max_anisotropy) {
    const std::uint32_t level = effective_anisotropy(max_anisotropy);
    std::size_t index = 0;
    while ((1u << index) < level) ++index;
    if (material_samplers_[index]) return material_samplers_[index];
    rhi::SamplerDesc s;
    s.min_filter = s.mag_filter = s.mip_filter = rhi::Filter::Linear;
    s.address_u = s.address_v = s.address_w = rhi::AddressMode::Repeat;
    s.max_anisotropy = level > 1 ? static_cast<float>(level) : 0.0f;
    auto made = device_.create_sampler(s);
    if (!made) return std::move(made).error();
    material_samplers_[index] = made.value();
    return made.value();
}

Result<rhi::ShaderHandle> GpuContext::shader(std::string_view name) {
    std::string key(name);
    if (auto it = shaders_.find(key); it != shaders_.end()) return it->second;
    auto s = create_builtin_shader(device_, name);
    if (!s) return std::move(s).error();
    shaders_.emplace(key, s.value());
    return s.value();
}

Result<rhi::TextureHandle> GpuContext::create_texture(const Image& image, const TextureOptions& options) {
    if (image.empty() || image.width == 0 || image.height == 0)
        return make_error(ErrorCode::InvalidArgument, std::format("texture '{}' has no pixels", options.name));
    if (options.mipmaps) return create_texture_mips(build_mip_chain(image, options.srgb), options);
    return create_texture_mips(std::span<const Image>(&image, 1), options);
}

Result<rhi::TextureHandle> GpuContext::create_texture_mips(std::span<const Image> chain, const TextureOptions& options) {
    if (chain.empty() || chain[0].empty() || chain[0].width == 0 || chain[0].height == 0)
        return make_error(ErrorCode::InvalidArgument, std::format("texture '{}' has no pixels", options.name));
    for (std::size_t m = 0; m < chain.size(); ++m) {
        const std::uint32_t w = std::max(1u, chain[0].width >> m), h = std::max(1u, chain[0].height >> m);
        if (chain[m].width != w || chain[m].height != h || chain[m].pixels.size() != std::size_t{w} * h * 4)
            return make_error(ErrorCode::InvalidArgument, std::format("texture '{}' mip {} has wrong size", options.name, m));
    }
    rhi::TextureDesc d;
    d.name = options.name;
    d.format = options.srgb ? rhi::Format::RGBA8_SRGB : rhi::Format::RGBA8_UNORM;
    d.width = chain[0].width;
    d.height = chain[0].height;
    d.mip_levels = static_cast<std::uint32_t>(chain.size());
    d.usage = rhi::TextureUsageSampled;
    auto tex = device_.create_texture(d);
    if (!tex) return tex;
    auto cmd = device_.begin_commands();
    for (std::uint32_t m = 0; m < chain.size(); ++m) cmd.upload_texture(tex.value(), chain[m].pixels, m);
    cmd.submit();
    return tex;
}

Result<rhi::TextureHandle> GpuContext::create_compressed_texture(rhi::Format format, std::uint32_t width, std::uint32_t height,
                                                                  std::span<const std::vector<std::uint8_t>> levels,
                                                                  const TextureOptions& options) {
    if (!rhi::is_compressed(format)) return make_error(ErrorCode::InvalidArgument, std::format("texture '{}': format is not block-compressed", options.name));
    if (levels.empty() || width == 0 || height == 0)
        return make_error(ErrorCode::InvalidArgument, std::format("texture '{}' has no blocks", options.name));
    if (!device_.supports_format(format, rhi::TextureUsageSampled))
        return make_error(ErrorCode::Unsupported, std::format("texture '{}': compressed format not supported by the device", options.name));
    for (std::size_t m = 0; m < levels.size(); ++m) {
        const std::uint32_t w = std::max(1u, width >> m), h = std::max(1u, height >> m);
        if (levels[m].size() != rhi::mip_size_bytes(format, w, h))
            return make_error(ErrorCode::InvalidArgument, std::format("texture '{}' mip {} has wrong size", options.name, m));
    }
    rhi::TextureDesc d;
    d.name = options.name;
    d.format = format;
    d.width = width;
    d.height = height;
    d.mip_levels = static_cast<std::uint32_t>(levels.size());
    d.usage = rhi::TextureUsageSampled;
    auto tex = device_.create_texture(d);
    if (!tex) return tex;
    auto cmd = device_.begin_commands();
    for (std::uint32_t m = 0; m < levels.size(); ++m) cmd.upload_texture(tex.value(), levels[m], m);
    cmd.submit();
    return tex;
}

Result<GpuMesh> GpuContext::create_mesh(const MeshData& mesh, std::string_view name) {
    if (mesh.vertices.empty() || mesh.indices.empty())
        return make_error(ErrorCode::InvalidArgument, std::format("mesh '{}' is empty", name));
    GpuMesh g;
    // LOD 0 and every coarser level share one index buffer, back to back.
    std::vector<std::uint32_t> all_lods;
    const std::vector<std::uint32_t>* indices = &mesh.indices;
    g.lods[0] = {0, static_cast<std::uint32_t>(mesh.indices.size()), 0.0f};
    g.lod_count = 1;
    if (!mesh.lods.empty()) {
        all_lods = mesh.indices;
        for (const MeshLod& lod : mesh.lods) {
            if (g.lod_count >= kMaxMeshLods || lod.indices.empty()) break;
            g.lods[g.lod_count++] = {static_cast<std::uint32_t>(all_lods.size()), static_cast<std::uint32_t>(lod.indices.size()),
                                     lod.error};
            all_lods.insert(all_lods.end(), lod.indices.begin(), lod.indices.end());
        }
        indices = &all_lods;
    }
    const auto vb_size = static_cast<std::uint32_t>(mesh.vertices.size() * sizeof(Vertex3D));
    const auto ib_size = static_cast<std::uint32_t>(indices->size() * sizeof(std::uint32_t));
    auto vb = device_.create_buffer({std::format("{}_vb", name), vb_size, rhi::BufferUsageVertex});
    if (!vb) return std::move(vb).error();
    auto ib = device_.create_buffer({std::format("{}_ib", name), ib_size, rhi::BufferUsageIndex});
    if (!ib) {
        device_.destroy(vb.value());
        return std::move(ib).error();
    }
    g.vertices = vb.value();
    g.indices = ib.value();
    g.vertex_count = g.vertex_capacity = static_cast<std::uint32_t>(mesh.vertices.size());
    g.index_count = static_cast<std::uint32_t>(mesh.indices.size());
    g.index_capacity = static_cast<std::uint32_t>(indices->size());
    g.bounds = mesh.bounds();
    const auto sb_size = static_cast<std::uint32_t>(mesh.skin.size() * sizeof(SkinVertex));
    if (mesh.skinned()) {
        auto sb = device_.create_buffer({std::format("{}_skin", name), sb_size, rhi::BufferUsageVertex});
        if (!sb) {
            device_.destroy(g.vertices);
            device_.destroy(g.indices);
            return std::move(sb).error();
        }
        g.skin = sb.value();
    }
    auto cmd = device_.begin_commands();
    cmd.upload_buffer(g.vertices, 0, {reinterpret_cast<const std::uint8_t*>(mesh.vertices.data()), vb_size});
    cmd.upload_buffer(g.indices, 0, {reinterpret_cast<const std::uint8_t*>(indices->data()), ib_size});
    if (g.skin) cmd.upload_buffer(g.skin, 0, {reinterpret_cast<const std::uint8_t*>(mesh.skin.data()), sb_size});
    cmd.submit();
    return g;
}

Status GpuContext::update_mesh(GpuMesh& mesh, const MeshData& data, std::string_view name) {
    if (data.vertices.empty() || data.indices.empty())
        return make_error(ErrorCode::InvalidArgument, std::format("mesh '{}' is empty", name));
    const auto nv = static_cast<std::uint32_t>(data.vertices.size());
    const auto ni = static_cast<std::uint32_t>(data.indices.size());
    if (data.skinned() || !data.lods.empty() || !mesh.valid() || mesh.skinned()) {
        auto g = create_mesh(data, name);
        if (!g) return std::move(g).error();
        destroy(mesh);
        mesh = g.value();
        return {};
    }
    GpuMesh next = mesh;
    rhi::BufferHandle new_vb, new_ib;
    if (nv > mesh.vertex_capacity) {
        next.vertex_capacity = nv + nv / 2;
        auto vb = device_.create_buffer({std::format("{}_vb", name), next.vertex_capacity * static_cast<std::uint32_t>(sizeof(Vertex3D)),
                                         rhi::BufferUsageVertex});
        if (!vb) return std::move(vb).error();
        new_vb = next.vertices = vb.value();
    }
    if (ni > mesh.index_capacity) {
        next.index_capacity = ni + ni / 2;
        auto ib = device_.create_buffer({std::format("{}_ib", name), next.index_capacity * static_cast<std::uint32_t>(sizeof(std::uint32_t)),
                                         rhi::BufferUsageIndex});
        if (!ib) {
            if (new_vb) device_.destroy(new_vb);
            return std::move(ib).error();
        }
        new_ib = next.indices = ib.value();
    }
    auto cmd = device_.begin_commands();
    cmd.upload_buffer(next.vertices, 0, {reinterpret_cast<const std::uint8_t*>(data.vertices.data()), nv * sizeof(Vertex3D)}, true);
    cmd.upload_buffer(next.indices, 0, {reinterpret_cast<const std::uint8_t*>(data.indices.data()), ni * sizeof(std::uint32_t)}, true);
    cmd.submit();
    // Old buffers that were outgrown: the device defers their release until the GPU is done with them.
    if (new_vb) device_.destroy(mesh.vertices);
    if (new_ib) device_.destroy(mesh.indices);
    next.vertex_count = nv;
    next.index_count = ni;
    next.lods = {};
    next.lods[0] = {0, ni, 0.0f};
    next.lod_count = 1;
    next.bounds = data.bounds();
    mesh = next;
    return {};
}

void GpuContext::destroy(GpuMesh& mesh) {
    if (mesh.vertices) device_.destroy(mesh.vertices);
    if (mesh.indices) device_.destroy(mesh.indices);
    if (mesh.skin) device_.destroy(mesh.skin);
    mesh = {};
}

} // namespace victus::render
