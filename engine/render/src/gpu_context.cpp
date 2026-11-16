// SPDX-License-Identifier: Apache-2.0
#include "victus/render/gpu_context.hpp"

#include "victus/render/builtin_shaders.hpp"

#include <algorithm>
#include <cstring>
#include <format>

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
    for (auto t : {white_, black_, flat_normal_})
        if (t) device_.destroy(t);
    for (auto s : {linear_repeat_, linear_clamp_, nearest_clamp_, nearest_repeat_, shadow_})
        if (s) device_.destroy(s);
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

Result<GpuMesh> GpuContext::create_mesh(const MeshData& mesh, std::string_view name) {
    if (mesh.vertices.empty() || mesh.indices.empty())
        return make_error(ErrorCode::InvalidArgument, std::format("mesh '{}' is empty", name));
    GpuMesh g;
    const auto vb_size = static_cast<std::uint32_t>(mesh.vertices.size() * sizeof(Vertex3D));
    const auto ib_size = static_cast<std::uint32_t>(mesh.indices.size() * sizeof(std::uint32_t));
    auto vb = device_.create_buffer({std::format("{}_vb", name), vb_size, rhi::BufferUsageVertex});
    if (!vb) return std::move(vb).error();
    auto ib = device_.create_buffer({std::format("{}_ib", name), ib_size, rhi::BufferUsageIndex});
    if (!ib) {
        device_.destroy(vb.value());
        return std::move(ib).error();
    }
    g.vertices = vb.value();
    g.indices = ib.value();
    g.vertex_count = static_cast<std::uint32_t>(mesh.vertices.size());
    g.index_count = static_cast<std::uint32_t>(mesh.indices.size());
    g.bounds = mesh.bounds();
    auto cmd = device_.begin_commands();
    cmd.upload_buffer(g.vertices, 0, {reinterpret_cast<const std::uint8_t*>(mesh.vertices.data()), vb_size});
    cmd.upload_buffer(g.indices, 0, {reinterpret_cast<const std::uint8_t*>(mesh.indices.data()), ib_size});
    cmd.submit();
    return g;
}

void GpuContext::destroy(GpuMesh& mesh) {
    if (mesh.vertices) device_.destroy(mesh.vertices);
    if (mesh.indices) device_.destroy(mesh.indices);
    mesh = {};
}

} // namespace victus::render
