// SPDX-License-Identifier: Apache-2.0
// Blended materials draw after opaque geometry, far to near, without writing depth.
#include "gpu_fixture.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/runtime/surface_shader.hpp"

#include <algorithm>

using namespace victus;
using namespace victus::render;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;

namespace {
struct Quad {
    Material mat;
    float z;
};

std::vector<std::uint8_t> draw(const std::vector<Quad>& quads) {
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();
    constexpr std::uint32_t W = 64, H = 64;
    auto target = test::make_target(W, H);
    auto sr = SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    auto mesh = ctx.create_mesh(make_plane({6, 6}), "quad");
    REQUIRE(mesh.ok());
    Camera3D cam = Camera3D::look_at({0, 0, 5}, {0, 0, 0});
    DirectionalLight sun;
    sun.direction = {0, 0, -1};
    sun.intensity = 3.0f;
    Environment env;
    env.draw_sky = false;
    env.clear_color = {0, 0, 0, 1};
    (*sr)->begin(cam, sun, env);
    // make_plane lies in XZ facing +Y: stand it up to face the camera (+Z).
    for (const Quad& q : quads)
        (*sr)->submit(*mesh, q.mat, math::Mat4::trs({0, 0, q.z}, math::Quat::axis_angle({1, 0, 0}, math::kPi * 0.5f), {1, 1, 1}));
    auto cmd = dev.begin_commands();
    REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    auto px = dev.read_texture(target);
    REQUIRE(px.ok());
    CHECK((*sr)->stats().transparent_draws == static_cast<std::uint32_t>(std::count_if(
              quads.begin(), quads.end(), [](const Quad& q) { return q.mat.blend != MaterialBlend::Opaque; })));
    const auto blended = static_cast<std::uint64_t>(
        std::count_if(quads.begin(), quads.end(), [](const Quad& q) { return q.mat.blend != MaterialBlend::Opaque; }));
    CHECK((*sr)->stats().transparent_triangles == blended * (mesh->lods[0].index_count / 3));
    CHECK((*sr)->stats().triangles >= (*sr)->stats().transparent_triangles);
    ctx.destroy(*mesh);
    dev.destroy(target);
    return std::move(*px);
}

Material flat(math::Color c, MaterialBlend blend) {
    Material m;
    m.base_color = c;
    m.unlit = true;
    m.blend = blend;
    m.cast_shadows = false;
    return m;
}
} // namespace

TEST_CASE("material blend names round trip") {
    for (auto b : {MaterialBlend::Opaque, MaterialBlend::Alpha, MaterialBlend::Additive})
        CHECK(parse_material_blend(material_blend_name(b)) == b);
    CHECK_FALSE(parse_material_blend("glass").has_value());
}

TEST_CASE("gpu: alpha materials blend over opaque geometry and do not hide what is behind") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    // Opaque red behind, half-transparent blue in front: the result mixes both.
    const auto mixed = draw({{flat({1, 0, 0, 1}, MaterialBlend::Opaque), -1.0f}, {flat({0, 0, 1, 0.5f}, MaterialBlend::Alpha), 1.0f}});
    const auto c = pixel(mixed, 64, 32, 32);
    MESSAGE("mixed " << channel(c, 0) << "," << channel(c, 1) << "," << channel(c, 2));
    CHECK(channel(c, 0) > 40);
    CHECK(channel(c, 2) > 40);
    // Submitted before the opaque quad it covers, the blended quad still lands on top.
    const auto order = draw({{flat({0, 0, 1, 0.5f}, MaterialBlend::Alpha), 1.0f}, {flat({1, 0, 0, 1}, MaterialBlend::Opaque), -1.0f}});
    const auto o = pixel(order, 64, 32, 32);
    CHECK(channel(o, 2) > 40);
    CHECK(channel(o, 0) > 40);
    // A blended quad behind an opaque one is hidden by depth testing.
    const auto hidden = draw({{flat({1, 0, 0, 1}, MaterialBlend::Opaque), 1.0f}, {flat({0, 0, 1, 0.5f}, MaterialBlend::Alpha), -1.0f}});
    const auto h = pixel(hidden, 64, 32, 32);
    CHECK(channel(h, 2) < 20);
    // Additive brightens.
    const auto add = draw({{flat({0.4f, 0, 0, 1}, MaterialBlend::Opaque), -1.0f}, {flat({0, 0.6f, 0, 1}, MaterialBlend::Additive), 1.0f}});
    const auto a = pixel(add, 64, 32, 32);
    CHECK(channel(a, 0) > 40);
    CHECK(channel(a, 1) > 40);
}

TEST_CASE("gpu: blended surface shaders read the vertex colour") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& dev = gpu().ctx->device();
    // Data in the vertex colour (here: white everywhere) drives colour and opacity.
    auto compiled = runtime::compile_surface_shader(
        "void surface(SurfaceInput i, inout Surface s) {\n    s.base_color = vec3(0.0, i.vertex_color.g, 0.0);\n    s.alpha = 0.5;\n}\n",
        "vertex_data.vshader", dev.capabilities().backend == rhi::Backend::Metal);
    REQUIRE(compiled.ok());
    rhi::ShaderDesc d;
    d.name = "vertex_data";
    d.stage = rhi::ShaderStage::Fragment;
    d.spirv = compiled->spirv;
    d.msl = compiled->msl;
    d.samplers = compiled->counts.samplers;
    d.storage_textures = compiled->counts.storage_textures;
    d.storage_buffers = compiled->counts.storage_buffers;
    d.uniform_buffers = compiled->counts.uniform_buffers;
    auto shader = dev.create_shader(d);
    REQUIRE(shader.ok());
    Material glass = flat({1, 1, 1, 1}, MaterialBlend::Alpha);
    glass.surface_shader = *shader;
    const auto px = draw({{flat({1, 0, 0, 1}, MaterialBlend::Opaque), -1.0f}, {glass, 1.0f}});
    const auto c = pixel(px, 64, 32, 32);
    MESSAGE("surface blend " << channel(c, 0) << "," << channel(c, 1) << "," << channel(c, 2));
    CHECK(channel(c, 0) > 40); // red shows through
    CHECK(channel(c, 1) > 40); // green from the vertex colour
    dev.destroy(*shader);
}
