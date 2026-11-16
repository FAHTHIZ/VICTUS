// SPDX-License-Identifier: Apache-2.0
// Material assets (.vmat) end to end: a material referencing a texture by UUID is resolved by the
// GPU asset cache, tiling changes what reaches the screen, broken/missing materials degrade to the
// component's own look, and hot reload picks up edited material files.
#include "gpu_fixture.hpp"
#include "victus/render/image.hpp"
#include "victus/runtime/hot_reload.hpp"
#include "victus/runtime/scene_rendering.hpp"

#include <chrono>
#include <filesystem>

using namespace victus;
using namespace victus::scene;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;
namespace sfs = std::filesystem;

namespace {
constexpr std::uint32_t kRed = 0xff0000ffu, kGreen = 0xff00ff00u;

struct MaterialScene {
    fs::Path project;
    std::unique_ptr<assets::AssetDatabase> db;
    Uuid texture, material;
    Scene scene;
    Entity plane;

    MaterialScene() {
        project = sfs::temp_directory_path() / ("victus_mat_" + Uuid::generate().to_string().substr(0, 8));
        sfs::create_directories(project / "assets" / "materials");
        // 2x2 red/green checker (cells of 8 px): one cell per quadrant of UV space.
        auto png = render::encode_png(render::make_checker(16, 2, kRed, kGreen));
        REQUIRE(png.ok());
        REQUIRE(fs::write_atomic(project / "assets" / "checker.png", *png).ok());
        auto opened = assets::AssetDatabase::open(project);
        REQUIRE(opened.ok());
        db = std::move(opened).value();
        texture = db->find_path("checker.png")->uuid;
        (void)db->import_all();
        write_material(1.0f, 0);

        const Entity cam = scene.create("Camera");
        scene.world().get<Transform>(cam)->position = {0, 6, 0};
        scene.world().get<Transform>(cam)->rotation = math::Quat::axis_angle({1, 0, 0}, math::radians(-90));
        scene.world().add<CameraComponent>(cam);
        plane = scene.create("Floor");
        scene.world().get<Transform>(plane)->scale = {20, 1, 20};
        auto& mr = scene.world().add<MeshRendererComponent>(plane);
        mr.primitive = "plane";
        mr.material_asset = material;
        scene.update_transforms();
    }
    ~MaterialScene() {
        db.reset();
        std::error_code ec;
        sfs::remove_all(project, ec);
    }
    void write_material(float tiling, int bump_seconds) {
        assets::MaterialAsset m;
        m.unlit = true; // exact texture colours on screen, independent of lighting
        m.filter = assets::TextureFilter::Nearest;
        m.base_color_texture = texture;
        m.uv_scale = {tiling, tiling};
        const fs::Path file = project / "assets" / "materials" / "floor.vmat";
        REQUIRE(assets::save_material_file(file, m).ok());
        sfs::last_write_time(file, sfs::file_time_type::clock::now() + std::chrono::seconds(bump_seconds));
        (void)db->refresh();
        material = db->find_path("materials/floor.vmat")->uuid;
    }
};

// Renders the scene and counts red<->green changes along the middle scanline.
int transitions(MaterialScene& s, runtime::GpuAssetCache& cache, runtime::ExtractStats* out = nullptr) {
    auto& dev = gpu().ctx->device();
    auto sr = render::SceneRenderer::create(*gpu().ctx);
    REQUIRE(sr.ok());
    constexpr std::uint32_t W = 160, H = 96;
    auto target = test::make_target(W, H);
    const auto stats = runtime::extract_scene(s.scene, cache, **sr, {});
    if (out) *out = stats;
    auto cmd = dev.begin_commands();
    REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    auto px = dev.read_texture(target);
    REQUIRE(px.ok());
    test::save_output("material_tiling", W, H, *px);
    int changes = 0, last = -1;
    for (std::uint32_t x = 0; x < W; ++x) {
        const auto p = pixel(*px, W, x, H / 2);
        const int c = channel(p, 0) > channel(p, 1) + 60 ? 0 : channel(p, 1) > channel(p, 0) + 60 ? 1 : -1;
        if (c < 0) continue;
        if (last >= 0 && c != last) ++changes;
        last = c;
    }
    dev.destroy(target);
    return changes;
}
} // namespace

TEST_CASE("material asset: JSON round trip, defaults and strict types") {
    assets::MaterialAsset m;
    m.base_color = {0.5f, 0.25f, 1.0f, 0.75f};
    m.metallic = 0.3f;
    m.uv_scale = {4, 2};
    m.uv_offset = {0.5f, 0};
    m.filter = assets::TextureFilter::Nearest;
    m.double_sided = true;
    m.normal_texture = Uuid::generate();
    auto back = assets::MaterialAsset::from_json(m.to_json());
    REQUIRE(back.ok());
    CHECK(*back == m);

    auto minimal = json::parse(R"({"victus_material": 1, "roughness": 0.9})");
    REQUIRE(minimal.ok());
    auto mm = assets::MaterialAsset::from_json(*minimal);
    REQUIRE(mm.ok());
    CHECK(mm->roughness == doctest::Approx(0.9f));
    CHECK(mm->uv_scale == math::Vec2{1, 1});
    CHECK(mm->base_color_texture.is_nil());

    for (const char* bad : {R"({"roughness": 0.5})", R"({"victus_material": 1, "metallic": "shiny"})",
                            R"({"victus_material": 1, "uv_scale": [1]})", R"({"victus_material": 1, "filter": "cubic"})",
                            R"({"victus_material": 1, "normal_texture": "not-a-uuid"})"}) {
        auto doc = json::parse(bad);
        REQUIRE(doc.ok());
        CHECK_FALSE(assets::MaterialAsset::from_json(*doc).ok());
    }
    auto future = json::parse(R"({"victus_material": 99})");
    REQUIRE(future.ok());
    auto fm = assets::MaterialAsset::from_json(*future);
    REQUIRE_FALSE(fm.ok());
    CHECK(fm.error().code == ErrorCode::VersionMismatch);
    CHECK(assets::kind_for_extension(".VMAT") == assets::AssetKind::Material);
}

TEST_CASE("gpu: material assets texture meshes, tile, degrade and hot reload") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    MaterialScene s;
    runtime::GpuAssetCache cache(*gpu().ctx, s.db.get());

    // Tiling 1: the 2x2 checker spans the whole 20 m floor; the camera sees ~1 cell boundary.
    runtime::ExtractStats stats;
    const int base = transitions(s, cache, &stats);
    CHECK(stats.missing_assets == 0);
    auto resolved = cache.material(s.material);
    REQUIRE(resolved.ok());
    CHECK(resolved->base_color_texture);
    CHECK(resolved->nearest_filter);
    CHECK(base >= 1);
    CHECK(base <= 2);

    // Tiling 8 via hot reload: many more cells cross the screen.
    runtime::AssetHotReloader reloader(*s.db);
    s.write_material(8.0f, 5);
    const auto changes = reloader.check();
    REQUIRE(changes.materials.size() == 1);
    CHECK(changes.materials[0] == "materials/floor.vmat");
    CHECK(changes.restart_needed.empty());
    runtime::AssetHotReloader::apply(changes, &cache, nullptr);
    const int tiled = transitions(s, cache);
    CHECK(tiled >= base + 4);

    // Broken file: counted as missing, still drawn with the component's own look (no crash).
    {
        const fs::Path file = s.project / "assets" / "materials" / "floor.vmat";
        const std::string text = "{ not json";
        REQUIRE(fs::write_atomic(file, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())).ok());
        cache.evict_materials();
        CHECK_FALSE(cache.material(s.material).ok());
        (void)transitions(s, cache, &stats);
        CHECK(stats.missing_assets == 1);
        CHECK(stats.meshes == 1);
    }
    // Unknown UUID: same graceful degradation.
    s.scene.world().get<MeshRendererComponent>(s.plane)->material_asset = Uuid::generate();
    (void)transitions(s, cache, &stats);
    CHECK(stats.missing_assets == 1);
    CHECK(stats.meshes == 1);
    cache.clear();
}

TEST_CASE("scene: MeshRenderer material reference survives save/load") {
    Scene a;
    const Entity e = a.create("Box");
    auto& mr = a.world().add<MeshRendererComponent>(e);
    mr.primitive = "cube";
    mr.material_asset = Uuid::generate();
    const auto text = json::stringify(a.save());
    Scene b;
    auto doc = json::parse(text);
    REQUIRE(doc.ok());
    REQUIRE(b.load(*doc).ok());
    const Entity be = b.find_by_name("Box");
    REQUIRE(b.world().get<MeshRendererComponent>(be));
    CHECK(b.world().get<MeshRendererComponent>(be)->material_asset == mr.material_asset);
}
