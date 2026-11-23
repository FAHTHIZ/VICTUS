// SPDX-License-Identifier: Apache-2.0
// Procedural trees with the starter foliage wind (ADR-0243): the trees draw lit with their bark
// and leaf colours, and the wind vertex function moves the crown and its shadow over time while
// a material with zero wind holds still.
#include "gpu_fixture.hpp"
#include "victus/procgen/city.hpp"
#include "victus/procgen/tree.hpp"
#include "victus/project/starter_materials.hpp"
#include "victus/render/scene_renderer.hpp"
#include "victus/runtime/scene_rendering.hpp"

#include <chrono>
#include <filesystem>

using namespace victus;
using namespace victus::scene;
using victus::test::gpu;
namespace sfs = std::filesystem;

namespace {

constexpr std::uint32_t W = 160, H = 120;

struct TreeProject {
    fs::Path dir;
    std::unique_ptr<assets::AssetDatabase> db;
    TreeProject() {
        dir = sfs::temp_directory_path() / ("victus_trees_" + Uuid::generate().to_string().substr(0, 8));
        std::vector<project::TemplateFile> files;
        project::add_starter_material_files(files);
        for (const auto& f : files) {
            // Only the foliage shader and materials; the texture sets are not needed here.
            if (f.path.find("foliage_wind") == std::string::npos && f.path.find("leaves.vmat") == std::string::npos &&
                f.path.find("bark.vmat") == std::string::npos)
                continue;
            const fs::Path p = dir / f.path;
            sfs::create_directories(p.parent_path());
            REQUIRE(fs::write_atomic(p, std::span(reinterpret_cast<const std::uint8_t*>(f.content.data()), f.content.size())).ok());
        }
        auto opened = assets::AssetDatabase::open(dir);
        REQUIRE(opened.ok());
        db = std::move(opened).value();
        (void)db->refresh();
    }
    ~TreeProject() {
        db.reset();
        std::error_code ec;
        sfs::remove_all(dir, ec);
    }
};

std::vector<std::uint8_t> shoot(Scene& s, runtime::GpuAssetCache& cache, double time, const char* name) {
    auto& dev = gpu().ctx->device();
    auto sr = render::SceneRenderer::create(*gpu().ctx);
    REQUIRE(sr.ok());
    auto settings = render::RenderSettings::preset(render::QualityTier::Medium);
    settings.taa = false; // compare single frames
    (*sr)->set_settings(settings);
    (*sr)->set_time(time);
    auto target = test::make_target(W, H);
    for (int i = 0; i < 2; ++i) {
        (void)runtime::extract_scene(s, cache, **sr, {});
        auto cmd = dev.begin_commands();
        REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
    }
    auto px = dev.read_texture(target);
    REQUIRE(px.ok());
    test::save_output(name, W, H, *px);
    dev.destroy(target);
    return std::move(*px);
}

// Pixels whose colour differs by more than 24 in any channel.
std::size_t changed(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    std::size_t n = 0;
    for (std::size_t i = 0; i + 3 < a.size(); i += 4)
        for (int c = 0; c < 3; ++c)
            if (std::abs(int(a[i + c]) - int(b[i + c])) > 24) {
                ++n;
                break;
            }
    return n;
}

} // namespace

TEST_CASE("gpu trees: starter foliage wind sways the crown over time, zero wind holds still (ADR-0243)") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    procgen::register_components();
    TreeProject proj;
    REQUIRE(proj.db->find(project::starter_leaves_uuid()));
    REQUIRE(proj.db->find(project::starter_bark_uuid()));

    Scene s;
    const Entity cam = s.create("Camera");
    s.world().get<Transform>(cam)->position = {0, 4, 11};
    s.world().add<CameraComponent>(cam);
    const Entity sun = s.create("Sun");
    s.world().get<Transform>(sun)->rotation = math::Quat::axis_angle({0, 1, 0}, math::radians(30)) * math::Quat::axis_angle({1, 0, 0}, math::radians(-50));
    auto& light = s.world().add<LightComponent>(sun);
    light.type = LightComponent::Type::Directional;
    light.intensity = 3.0f;
    light.shadows = true;
    const Entity ground = s.create("Ground");
    s.world().get<Transform>(ground)->scale = {30, 1, 30};
    s.world().add<MeshRendererComponent>(ground).primitive = "plane";
    const Entity tree = s.create("Tree");
    auto& tc = s.world().add<procgen::TreeComponent>(tree);
    tc.height = 7.0f;
    tc.crown_radius = 2.6f;
    tc.leaf_material = project::starter_leaves_uuid();
    tc.bark_material = project::starter_bark_uuid();
    REQUIRE(procgen::build_trees(s) == 1);
    s.update_transforms();

    runtime::GpuAssetCache cache(*gpu().ctx, proj.db.get());
    const auto a = shoot(s, cache, 0.0, "tree_wind_t0");
    // The tree draws: green leaves above the ground line in the middle of the frame.
    std::size_t green = 0;
    for (std::uint32_t y = 10; y < H / 2; ++y)
        for (std::uint32_t x = W / 4; x < 3 * W / 4; ++x) {
            const std::uint8_t* p = &a[(y * W + x) * 4];
            if (p[1] > p[0] + 8 && p[1] > p[2] + 8) ++green;
        }
    MESSAGE("green crown pixels: " << green);
    CHECK(green > 600);

    const auto b = shoot(s, cache, 2.3, "tree_wind_t1");
    const std::size_t moved = changed(a, b);
    MESSAGE("pixels changed by the wind between t = 0 s and 2.3 s: " << moved);
    CHECK(moved > 40); // a gentle sway: ~0.15 m at the crown of a 7 m tree

    // Zero wind (bend strength and flutter 0): the same two times render the same frame.
    for (const char* name : {"leaves.vmat", "bark.vmat"}) {
        const fs::Path file = proj.dir / "assets" / "materials" / "starter" / name;
        auto m = assets::load_material_file(file);
        REQUIRE(m.ok());
        m->shader_params[0].w = 0.0f;
        m->shader_params[1].y = 0.0f;
        REQUIRE(assets::save_material_file(file, *m).ok());
        sfs::last_write_time(file, sfs::file_time_type::clock::now() + std::chrono::seconds(10));
    }
    (void)proj.db->refresh();
    cache.evict_materials();
    const auto c = shoot(s, cache, 0.0, "tree_calm_t0");
    const auto d = shoot(s, cache, 2.3, "tree_calm_t1");
    const std::size_t still = changed(c, d);
    MESSAGE("pixels changed without wind: " << still);
    CHECK(still < 10);
    cache.clear();
}
