// SPDX-License-Identifier: Apache-2.0
#include "victus/render/environment_map.hpp"

#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <vector>

namespace victus::render {
using namespace math;

namespace {
constexpr float kPi = 3.14159265358979f;
// Real SH basis, band 0..2.
std::array<float, 9> sh_basis(Vec3 n) noexcept {
    return {0.282095f,
            0.488603f * n.y,
            0.488603f * n.z,
            0.488603f * n.x,
            1.092548f * n.x * n.y,
            1.092548f * n.y * n.z,
            0.315392f * (3.0f * n.z * n.z - 1.0f),
            1.092548f * n.x * n.z,
            0.546274f * (n.x * n.x - n.y * n.y)};
}
struct FaceUniforms {
    Vec4 face;
};
struct PrefilterUniforms {
    Vec4 params, params2;
};
} // namespace

Vec3 equirect_direction(float u, float v) noexcept {
    const float phi = (u - 0.5f) * 2.0f * kPi; // 0 at -Z, +pi/2 at +X
    const float theta = v * kPi;              // 0 at +Y
    const float s = std::sin(theta);
    return {s * std::sin(phi), std::cos(theta), -s * std::cos(phi)};
}

Vec2 equirect_uv(Vec3 d) noexcept {
    d = normalize(d);
    return {std::atan2(d.x, -d.z) / (2.0f * kPi) + 0.5f, std::acos(std::clamp(d.y, -1.0f, 1.0f)) / kPi};
}

SphericalHarmonics9 SphericalHarmonics9::project(const ImageF& img) {
    VICTUS_PROFILE_SCOPE("SphericalHarmonics9::project");
    SphericalHarmonics9 out;
    if (img.empty() || img.width == 0 || img.height == 0) return out;
    // Projection is a smooth integral: a 256x128 version is exact to well below 8-bit precision.
    const ImageF small = downsample_to_fit(img, 256, 128);
    std::array<Vec3, 9> L{};
    double total_weight = 0.0;
    const float du = 1.0f / static_cast<float>(small.width), dv = 1.0f / static_cast<float>(small.height);
    for (std::uint32_t y = 0; y < small.height; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) * dv;
        // Solid angle of a texel: (2pi du) * (pi dv) * sin(theta).
        const float w = 2.0f * kPi * du * kPi * dv * std::sin(v * kPi);
        for (std::uint32_t x = 0; x < small.width; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) * du;
            const Vec4 c = small.at(x, y);
            const auto b = sh_basis(equirect_direction(u, v));
            for (int i = 0; i < 9; ++i) L[i] = L[i] + Vec3{c.x, c.y, c.z} * (b[i] * w);
            total_weight += w;
        }
    }
    // Normalise the discrete solid angles to exactly 4pi so constant environments stay exact.
    const float norm = total_weight > 0.0 ? static_cast<float>(4.0 * kPi / total_weight) : 0.0f;
    // Cosine-lobe convolution A_l = pi, 2pi/3, pi/4, divided by pi (radiance of a white surface).
    const float band[9] = {1.0f, 2.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
    for (int i = 0; i < 9; ++i) out.coefficients[i] = L[i] * (norm * band[i]);
    return out;
}

ExtractedSun extract_sun(ImageF& img, float min_peak_ratio) {
    VICTUS_PROFILE_SCOPE("extract_sun");
    ExtractedSun out;
    const std::size_t n = std::size_t{img.width} * img.height;
    if (img.width == 0 || img.height == 0 || img.pixels.size() < n * 4) return out;
    auto lum = [&](std::size_t i) {
        const float* p = &img.pixels[i * 4];
        const float l = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
        return std::isfinite(l) ? std::max(l, 0.0f) : 0.0f;
    };
    std::vector<float> l(n);
    float peak = 0.0f;
    for (std::size_t i = 0; i < n; ++i) peak = std::max(peak, l[i] = lum(i));
    std::vector<float> sorted = l;
    auto nth = [&](double q) {
        const auto k = static_cast<std::ptrdiff_t>(std::min<double>(static_cast<double>(n - 1), q * static_cast<double>(n)));
        std::nth_element(sorted.begin(), sorted.begin() + k, sorted.end());
        return sorted[static_cast<std::size_t>(k)];
    };
    const float median = nth(0.5);
    // A sun is a peak far above the median sky; overcast skies and interiors have none.
    if (!(median > 0.0f) || peak < median * std::max(min_peak_ratio, 1.0f)) return out;
    // Clamp to the 99.9th percentile (bright clouds survive; the solar disc and its aureole, a few
    // hundred texels of a 1K map at most, do not), and never below 16x the median.
    const float threshold = std::max(nth(0.999), 16.0f * median);
    if (!(peak > threshold)) return out;
    const float du = 1.0f / static_cast<float>(img.width), dv = 1.0f / static_cast<float>(img.height);
    Vec3 removed{0, 0, 0}, dir{0, 0, 0};
    for (std::uint32_t y = 0; y < img.height; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) * dv;
        const float solid_angle = 2.0f * kPi * du * kPi * dv * std::sin(v * kPi);
        for (std::uint32_t x = 0; x < img.width; ++x) {
            const std::size_t i = std::size_t{y} * img.width + x;
            if (l[i] <= threshold) continue;
            float* p = &img.pixels[i * 4];
            const float keep = threshold / l[i];
            const Vec3 cut = Vec3{p[0], p[1], p[2]} * ((1.0f - keep) * solid_angle);
            removed = removed + cut;
            dir = dir + equirect_direction((static_cast<float>(x) + 0.5f) * du, v) * ((l[i] - threshold) * solid_angle);
            p[0] *= keep;
            p[1] *= keep;
            p[2] *= keep;
            ++out.texels;
        }
    }
    const float len = length(dir);
    out.found = out.texels > 0 && len > 0.0f;
    out.direction = out.found ? dir * (1.0f / len) : Vec3{0, 1, 0};
    out.irradiance = removed;
    out.threshold = threshold;
    return out;
}

Vec3 SphericalHarmonics9::diffuse(Vec3 n) const noexcept {
    const auto b = sh_basis(normalize(n));
    Vec3 r{};
    for (int i = 0; i < 9; ++i) r = r + coefficients[i] * b[i];
    return {std::max(r.x, 0.0f), std::max(r.y, 0.0f), std::max(r.z, 0.0f)};
}

Vec3 SphericalHarmonics9::average() const noexcept { return coefficients[0] * 0.282095f; }

Result<std::unique_ptr<EnvironmentMap>> EnvironmentMap::create(GpuContext& ctx, const ImageF& equirect,
                                                               const EnvironmentBakeSettings& settings) {
    return create(ctx, equirect, SphericalHarmonics9::project(equirect), settings);
}

Result<std::unique_ptr<EnvironmentMap>> EnvironmentMap::create(GpuContext& ctx, const ImageF& equirect,
                                                               const SphericalHarmonics9& sh,
                                                               const EnvironmentBakeSettings& settings) {
    VICTUS_PROFILE_SCOPE("EnvironmentMap::create");
    if (equirect.empty() || equirect.width == 0 || equirect.height == 0 ||
        equirect.pixels.size() < std::size_t{equirect.width} * equirect.height * 4)
        return make_error(ErrorCode::InvalidArgument, "environment map: empty or malformed equirect image");
    auto& dev = ctx.device();
    const std::uint32_t max_tex = std::max(16u, dev.capabilities().max_texture_size);
    const ImageF src = downsample_to_fit(equirect, std::min(max_tex, 8192u), std::min(max_tex, 4096u));
    const std::uint32_t size = std::bit_floor(std::clamp(settings.cube_size, 16u, std::min(2048u, max_tex)));
    const std::uint32_t samples = std::clamp(settings.samples, 8u, 1024u);
    const std::uint32_t full_mips = static_cast<std::uint32_t>(std::bit_width(size)); // down to 1x1
    // Roughness levels stop at 8x8 faces: smaller mips cannot hold a rough lobe without seams.
    const std::uint32_t mips = std::max(1u, full_mips - 3u);

    std::unique_ptr<EnvironmentMap> env(new EnvironmentMap(ctx));
    env->size_ = size;
    env->mips_ = mips;
    env->sh_ = sh;

    rhi::TextureDesc ed;
    ed.name = "environment_equirect";
    ed.format = rhi::Format::RGBA16_FLOAT;
    ed.width = src.width;
    ed.height = src.height;
    auto eq = dev.create_texture(ed);
    if (!eq) return std::move(eq).error();
    rhi::TextureDesc sd;
    sd.name = "environment_source";
    sd.type = rhi::TextureType::Cube;
    sd.format = rhi::Format::RGBA16_FLOAT;
    sd.width = sd.height = size;
    sd.mip_levels = full_mips;
    sd.usage = rhi::TextureUsageSampled | rhi::TextureUsageColorTarget;
    auto source = dev.create_texture(sd);
    if (!source) {
        dev.destroy(*eq);
        return std::move(source).error();
    }
    rhi::TextureDesc cd = sd;
    cd.name = "environment_cube";
    cd.mip_levels = mips;
    auto cube = dev.create_texture(cd);
    if (!cube) {
        dev.destroy(*eq);
        dev.destroy(*source);
        return std::move(cube).error();
    }
    env->cube_ = *cube;

    auto vs = ctx.shader("fullscreen.vert");
    auto eq_fs = ctx.shader("env_equirect.frag");
    auto pf_fs = ctx.shader("env_prefilter.frag");
    rhi::PipelineHandle eq_pl, pf_pl;
    Status status;
    if (!vs || !eq_fs || !pf_fs) {
        status = make_error(ErrorCode::Unknown, "environment map: bake shaders unavailable");
    } else {
        rhi::PipelineDesc d;
        d.name = "env_equirect";
        d.vertex = *vs;
        d.fragment = *eq_fs;
        d.cull = rhi::CullMode::None;
        d.color_targets = {{rhi::Format::RGBA16_FLOAT, rhi::BlendMode::Opaque}};
        auto p = dev.create_pipeline(d);
        if (p) eq_pl = *p;
        else status = std::move(p).error();
        d.name = "env_prefilter";
        d.fragment = *pf_fs;
        auto q = dev.create_pipeline(d);
        if (q) pf_pl = *q;
        else if (status) status = std::move(q).error();
    }
    if (status) {
        auto cmd = dev.begin_commands();
        cmd.upload_texture(*eq, to_rgba16f(src));
        // Equirect -> source cube mip 0, then a box-filtered mip chain for filtered sampling.
        for (std::uint32_t face = 0; face < 6; ++face) {
            rhi::RenderPassDesc rp;
            rp.colors.push_back({*source, 0, face, rhi::LoadOp::DontCare, rhi::StoreOp::Store});
            auto pass = cmd.begin_render_pass(rp);
            pass.bind_pipeline(eq_pl);
            pass.set_viewport({0, 0, static_cast<float>(size), static_cast<float>(size), 0, 1});
            const FaceUniforms fu{{static_cast<float>(face), 0, 0, 0}};
            cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, fu);
            pass.bind_texture(rhi::ShaderStage::Fragment, 0, *eq, ctx.linear_repeat());
            pass.draw(3);
            pass.end();
        }
        cmd.generate_mipmaps(*source);
        // GGX prefilter: mip m holds roughness m / (mips - 1).
        for (std::uint32_t m = 0; m < mips; ++m) {
            const std::uint32_t ms = std::max(1u, size >> m);
            const float roughness = mips > 1 ? static_cast<float>(m) / static_cast<float>(mips - 1) : 0.0f;
            for (std::uint32_t face = 0; face < 6; ++face) {
                rhi::RenderPassDesc rp;
                rp.colors.push_back({*cube, m, face, rhi::LoadOp::DontCare, rhi::StoreOp::Store});
                auto pass = cmd.begin_render_pass(rp);
                pass.bind_pipeline(pf_pl);
                pass.set_viewport({0, 0, static_cast<float>(ms), static_cast<float>(ms), 0, 1});
                const PrefilterUniforms pu{{static_cast<float>(face), roughness, static_cast<float>(size), static_cast<float>(samples)},
                                           {static_cast<float>(full_mips), 0, 0, 0}};
                cmd.push_uniforms(rhi::ShaderStage::Fragment, 0, pu);
                pass.bind_texture(rhi::ShaderStage::Fragment, 0, *source, ctx.linear_clamp());
                pass.draw(3);
                pass.end();
            }
        }
        cmd.submit();
    }
    // SDL_GPU defers releases until submitted work that uses them has completed.
    if (eq_pl) dev.destroy(eq_pl);
    if (pf_pl) dev.destroy(pf_pl);
    dev.destroy(*eq);
    dev.destroy(*source);
    if (!status) return std::move(status).error();
    VICTUS_LOG_INFO("render", "environment map baked: {}x{} equirect -> {}^2 cube, {} mips, {} samples", src.width, src.height,
                    size, mips, samples);
    return env;
}

EnvironmentMap::~EnvironmentMap() {
    if (cube_) ctx_.device().destroy(cube_);
}

} // namespace victus::render
