// SPDX-License-Identifier: Apache-2.0
// Vertex colours, distance fog and the coverage (snow) layer reach the screen.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;

namespace {
struct Shot {
    std::vector<std::uint8_t> px;
    std::uint32_t at(std::uint32_t x, std::uint32_t y) const { return pixel(px, 128, x, y); }
};

Shot draw(const Environment& env, const Material& mat, std::uint8_t alpha) {
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    constexpr std::uint32_t W = 128, H = 96;
    auto target = test::make_target(W, H);
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    MeshData plane = make_plane({400, 400});
    for (auto& v : plane.vertices) v.color = pack_vertex_color_srgb8(20, 200, 30, alpha);
    auto mesh = ctx.create_mesh(plane, "coloured plane");
    REQUIRE(mesh.ok());
    Camera3D cam = Camera3D::look_at({0, 3, 0}, {0, 0, -20});
    DirectionalLight sun;
    sun.direction = {0.2f, -1.0f, -0.3f};
    (*sr)->begin(cam, sun, env);
    (*sr)->submit(*mesh, mat, math::Mat4::identity());
    auto cmd = dev.begin_commands();
    REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    auto px = dev.read_texture(target);
    REQUIRE(px.ok());
    ctx.destroy(*mesh);
    dev.destroy(target);
    return {std::move(*px)};
}
} // namespace

TEST_CASE("vertex colour packing round trips") {
    const auto c = pack_vertex_color_srgb8(255, 128, 0, 77);
    CHECK((c & 0xFFu) == 255u);
    CHECK(((c >> 8) & 0xFFu) == 128u);
    CHECK((c >> 24) == 77u);
    const auto back = unpack_vertex_color(pack_vertex_color({1, 0, 0, 0.5f}));
    CHECK(back.r == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(back.a == doctest::Approx(0.5f).epsilon(0.01));
}

TEST_CASE("gpu: vertex colours tint, fog hides distance, coverage whitens masked faces") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Material m;
    m.base_color = {1, 1, 1, 1};
    m.roughness = 0.9f;
    Environment env;
    const Shot plain = draw(env, m, 255);
    const auto near = plain.at(64, 90);
    CHECK(channel(near, 1) > channel(near, 0) + 40); // green vertex colour
    CHECK(channel(near, 1) > channel(near, 2) + 40);

    Environment foggy = env;
    foggy.fog_mode = FogMode::Linear;
    foggy.fog_color = {1, 0, 0, 1};
    foggy.fog_start = 1.0f;
    foggy.fog_end = 8.0f;
    const Shot fog = draw(foggy, m, 255);
    const auto far = fog.at(64, 50); // close to the horizon: fully fogged
    MESSAGE("fogged " << channel(far, 0) << "," << channel(far, 1));
    CHECK(channel(far, 0) > channel(far, 1) + 60);

    Environment snowy = env;
    snowy.coverage_amount = 1.0f;
    snowy.coverage_color = {1, 1, 1, 1};
    Material masked = m;
    masked.vertex_alpha_coverage_mask = true;
    const Shot covered = draw(snowy, masked, 255);
    const Shot bare = draw(snowy, masked, 0);
    const auto cw = covered.at(64, 90), bw = bare.at(64, 90);
    MESSAGE("covered " << channel(cw, 0) << "," << channel(cw, 1) << " bare " << channel(bw, 0) << "," << channel(bw, 1));
    CHECK(channel(cw, 0) > channel(bw, 0) + 60); // white snow vs green grass
    CHECK(channel(bw, 1) > channel(bw, 0) + 40);
}
