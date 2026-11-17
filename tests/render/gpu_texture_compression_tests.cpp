// SPDX-License-Identifier: Apache-2.0
// Compressed textures on the GPU (Slice 49): cooked BC/ASTC variants upload when the device
// samples them, render like their RGBA8 source, and fall back to RGBA8 otherwise.
#include "gpu_fixture.hpp"
#include "victus/assets/importers.hpp"
#include "victus/assets/texture_compression.hpp"
#include "victus/render/sprite_batch.hpp"
#include "victus/runtime/scene_rendering.hpp"

#include <cmath>

using namespace victus;
using victus::test::gpu;

namespace {

constexpr std::uint32_t W = 64, H = 64;

render::Image source_image() {
    render::Image img;
    img.width = W;
    img.height = H;
    img.pixels.resize(std::size_t{W} * H * 4);
    for (std::uint32_t y = 0; y < H; ++y)
        for (std::uint32_t x = 0; x < W; ++x) {
            auto* p = img.pixels.data() + (std::size_t{y} * W + x) * 4;
            p[0] = std::uint8_t(x * 4);
            p[1] = std::uint8_t(y * 4);
            p[2] = std::uint8_t(((x / 8 + y / 8) % 2) ? 200 : 60);
            p[3] = 255;
        }
    return img;
}

// Draws `tex` over the whole target (1:1 texels, nearest) and reads it back.
render::Image draw(rhi::TextureHandle tex, const char* name) {
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    auto batch = render::SpriteBatch::create(ctx, rhi::Format::RGBA8_UNORM);
    REQUIRE(batch.ok());
    render::SpriteBatch& sb = **batch;
    sb.begin(math::Mat4::orthographic(-1, 1, -1, 1, -1, 1));
    render::Sprite s;
    s.texture = tex;
    s.size = {2, 2};
    s.nearest = true;
    sb.draw(s);
    auto target = test::make_target(W, H);
    auto cmd = dev.begin_commands();
    REQUIRE(sb.prepare(cmd).ok());
    rhi::RenderPassDesc rp;
    rp.colors.push_back({target, 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0, 0, 0, 1}, {}});
    auto pass = cmd.begin_render_pass(rp);
    sb.render(pass, cmd);
    pass.end();
    cmd.submit_and_wait();
    render::Image out;
    out.width = W;
    out.height = H;
    out.pixels = dev.read_texture(target).value();
    test::save_output(name, W, H, out.pixels);
    dev.destroy(target);
    return out;
}

} // namespace

TEST_CASE("gpu: cooked BC and ASTC variants upload, sample like the source and fall back to RGBA8") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    const auto& caps = dev.capabilities();
    MESSAGE("device " << caps.driver_name << ": bc=" << caps.bc_compression << " astc=" << caps.astc_compression);
    // Linear (UNORM) so the readback compares raw texel values without sRGB decode differences.
    assets::CookedTexture cooked = assets::cook_texture(source_image(), "texcomp", false, true, 4096);
    REQUIRE(assets::compress_cooked_texture(cooked, "bc7+bc1+astc4x4+astc6x6").ok());
    REQUIRE(cooked.compressed.size() == 4);

    std::optional<assets::TextureCodec> chosen;
    auto reference_tex = runtime::upload_cooked_texture(ctx, cooked, false, &chosen);
    REQUIRE(reference_tex.ok());
    CHECK_FALSE(chosen.has_value());
    const render::Image reference = draw(*reference_tex, "texcomp_rgba8");
    CHECK(assets::image_psnr(reference, source_image()) >= 45.0); // sprite path is near lossless
    dev.destroy(*reference_tex);

    std::uint32_t sampled = 0;
    for (const auto& variant : cooked.compressed) {
        const rhi::Format format = assets::codec_format(variant.codec, false);
        const bool supported = dev.supports_format(format, rhi::TextureUsageSampled);
        CAPTURE(std::string(assets::to_string(variant.codec)));
        auto tex = ctx.create_compressed_texture(format, W, H, variant.levels, {false, false, "texcomp_variant"});
        if (!supported) {
            CHECK_FALSE(tex.ok());
            continue;
        }
        REQUIRE(tex.ok());
        ++sampled;
        const render::Image got = draw(*tex, (std::string("texcomp_") + assets::to_string(variant.codec)).c_str());
        const double psnr = assets::image_psnr(reference, got);
        MESSAGE(std::string(assets::to_string(variant.codec)) << " on GPU: PSNR " << psnr << " dB vs RGBA8");
        CHECK(psnr >= 32.0);
        // The GPU's block decoder agrees with the cooker's CPU decoder.
        auto cpu = assets::decode_texture(variant.levels[0], W, H, variant.codec, false);
        REQUIRE(cpu.ok());
        CHECK(assets::image_psnr(*cpu, got) >= 50.0);
        dev.destroy(*tex);
    }
    // Wrong level sizes are rejected before touching the device.
    std::vector<std::vector<std::uint8_t>> bad{std::vector<std::uint8_t>(10)};
    CHECK_FALSE(ctx.create_compressed_texture(rhi::Format::BC7_RGBA_UNORM, W, H, bad).ok());
    CHECK_FALSE(ctx.create_compressed_texture(rhi::Format::RGBA8_UNORM, W, H, cooked.compressed[0].levels).ok());

    // upload_cooked_texture takes the first variant the device supports, in cooker order.
    auto tex = runtime::upload_cooked_texture(ctx, cooked, true, &chosen);
    REQUIRE(tex.ok());
    if (caps.bc_compression) {
        REQUIRE(chosen.has_value());
        CHECK(*chosen == assets::TextureCodec::BC7);
    } else if (caps.astc_compression) {
        REQUIRE(chosen.has_value());
        CHECK(*chosen == assets::TextureCodec::ASTC4x4);
    }
    dev.destroy(*tex);
    MESSAGE(sampled << " of 4 compressed variants sampled on this device");
    if (test::gpu_required() && (caps.bc_compression || caps.astc_compression)) CHECK(sampled >= 1);
}
