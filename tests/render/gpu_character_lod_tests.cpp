// SPDX-License-Identifier: Apache-2.0
// Character silhouette across poses and camera distances (roadmap 6b, Workstream H3 item 7): the
// Orion starter character goes through the real pipeline (GLB → asset database → scene →
// AnimationSystem → extraction → skinned draws) at several distances and poses, once with
// automatic mesh LODs and once at LOD 0. The LOD image must keep the silhouette: few pixels may
// change, and none of the face/hand detail may disappear at the distances where LODs engage.
#include "gpu_fixture.hpp"
#include "victus/animation/animation.hpp"
#include "victus/assets/database.hpp"
#include "victus/runtime/scene_rendering.hpp"

#include <cstdio>
#include <filesystem>

using namespace victus;
using victus::test::gpu;

namespace {

constexpr std::uint32_t W = 256, H = 256;

struct Frame {
    std::vector<std::uint8_t> px;
    render::SceneStats stats;
};

} // namespace

TEST_CASE("gpu character lods: Orion silhouette over poses and distances") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    auto& ctx = *gpu().ctx;
    auto& dev = ctx.device();

    const fs::Path project = std::filesystem::temp_directory_path() / ("victus_charlod_" + Uuid::generate().to_string().substr(0, 8));
    std::filesystem::create_directories(project / "assets" / "models");
    std::filesystem::copy_file(fs::Path(VICTUS_SOURCE_DIR) / "engine" / "project" / "data" / "characters" / "victus_orion.glb",
                               project / "assets" / "models" / "v01.glb");
    auto db = assets::AssetDatabase::open(project);
    REQUIRE(db.ok());
    const assets::AssetRecord* rec = (*db)->find_path("models/v01.glb");
    REQUIRE(rec);
    auto model = (*db)->load_model(rec->uuid);
    REQUIRE(model.ok());

    runtime::GpuAssetCache cache(ctx, db->get());
    auto sr = render::SceneRenderer::create(ctx);
    REQUIRE(sr.ok());
    render::RenderSettings base = render::RenderSettings::preset(render::QualityTier::High);
    base.taa = false;
    base.ssao = false;
    base.bloom = false;
    base.auto_exposure = false;

    auto render_pose = [&](const char* clip, float t, float distance, bool lods, const char* save) {
        scene::Scene s;
        if (clip) { // nullptr: the empty scene (sky and lighting only)
            const scene::Entity robot = assets::instantiate_model(s, rec->uuid, *model, "Robot");
            auto* an = s.world().get<animation::AnimatorComponent>(robot);
            REQUIRE(an);
            an->clip = clip;
            an->crossfade = 0.0f;
            animation::AnimationSettings as;
            as.provider = assets::animation_provider(**db);
            animation::AnimationSystem anim(as);
            anim.update(s, 0.0f);
            anim.update(s, t);
        }
        s.update_transforms();
        render::RenderSettings rs = base;
        rs.lods = lods;
        sr.value()->set_settings(rs);
        // Three-quarter front view aimed at the chest; FOV narrows with distance so the robot
        // keeps a usable size on screen while the projected LOD error still grows.
        const math::Vec3 target{0, 1.0f, 0};
        const math::Vec3 dir = math::normalize(math::Vec3{0.6f, 0.15f, 1.0f});
        render::Camera3D cam = render::Camera3D::look_at(target + dir * distance, target);
        cam.fov_y = math::radians(distance <= 3.0f ? 50.0f : 30.0f);
        runtime::SceneViewSettings view;
        (void)runtime::extract_scene(s, cache, **sr, view, &cam);
        auto rt = test::make_target(W, H);
        auto cmd = dev.begin_commands();
        REQUIRE(sr.value()->render(cmd, rt, W, H, rhi::Format::RGBA8_UNORM).ok());
        cmd.submit_and_wait();
        Frame f{dev.read_texture(rt).value(), sr.value()->stats()};
        if (save) test::save_output(save, W, H, f.px);
        dev.destroy(rt);
        return f;
    };

    struct Pose {
        const char* clip;
        float t;
    };
    const Pose poses[] = {{"Idle", 1.0f}, {"Run", 0.2f}, {"Jump", 0.3f}, {"Interact", 0.45f}};
    const float distances[] = {2.5f, 8.0f, 20.0f};
    std::uint32_t lod_frames = 0;
    std::vector<std::vector<std::uint8_t>> empty;
    for (float d : distances) empty.push_back(render_pose(nullptr, 0, d, false, nullptr).px);
    for (const Pose& p : poses)
        for (std::size_t di = 0; di < std::size(distances); ++di) {
            const float d = distances[di];
            char name[64];
            std::snprintf(name, sizeof name, "charlod_%s_%02d", p.clip, static_cast<int>(d));
            char full_name[72];
            std::snprintf(full_name, sizeof full_name, "%s_full", name);
            const Frame full = render_pose(p.clip, p.t, d, false, full_name);
            const Frame lod = render_pose(p.clip, p.t, d, true, name);
            INFO(std::string(p.clip) << " at " << d << " m: " << lod.stats.lod_draws << " LOD draws, " << lod.stats.triangles << "/"
                        << full.stats.triangles << " triangles");
            CHECK(full.stats.skinned_draws > 0);
            CHECK(lod.stats.visible == full.stats.visible);
            CHECK(lod.stats.triangles <= full.stats.triangles);
            if (lod.stats.lod_draws > 0) ++lod_frames;
            // Pixels covered by the character (differ from the empty scene), pixels the LOD image
            // changes by more than 48/255, and of those the ones with no match within one pixel of
            // the LOD 0 image (a 1 px LOD budget may move an edge by a pixel; it must not remove
            // or invent detail).
            const std::vector<std::uint8_t>& bg = empty[di];
            auto px_diff = [&](const std::vector<std::uint8_t>& a, std::size_t ia, const std::vector<std::uint8_t>& b,
                               std::size_t ib) {
                int m = 0;
                for (int c = 0; c < 3; ++c) m = std::max(m, std::abs(int(a[ia + c]) - int(b[ib + c])));
                return m;
            };
            std::size_t covered = 0, changed = 0, unmatched = 0;
            for (std::uint32_t y = 0; y < H; ++y)
                for (std::uint32_t x = 0; x < W; ++x) {
                    const std::size_t i = (std::size_t{y} * W + x) * 4;
                    if (px_diff(full.px, i, bg, i) > 8 || px_diff(lod.px, i, bg, i) > 8) ++covered;
                    if (px_diff(full.px, i, lod.px, i) <= 48) continue;
                    ++changed;
                    bool match = false;
                    for (int dy = -1; dy <= 1 && !match; ++dy)
                        for (int dx = -1; dx <= 1 && !match; ++dx) {
                            const int nx = int(x) + dx, ny = int(y) + dy;
                            if (nx < 0 || ny < 0 || nx >= int(W) || ny >= int(H)) continue;
                            match = px_diff(full.px, (std::size_t(ny) * W + std::size_t(nx)) * 4, lod.px, i) <= 48;
                        }
                    if (!match) ++unmatched;
                }
            INFO("covered " << covered << " changed " << changed << " unmatched " << unmatched);
            // The character must cover enough pixels for the comparison to mean something. Orion's
            // anatomical silhouette is slimmer than the old boxy robot's: about 290 px side-on at 20 m.
            CHECK(covered > 250);
            CHECK(static_cast<double>(changed) <= 0.06 * static_cast<double>(covered)); // edge pixels only
            // Textured characters (ADR-0179) lose a few more isolated pixels than flat-shaded ones:
            // the emissive piping and suit seams are high-contrast texture edges, and a simplified
            // triangle interpolates their UVs slightly differently. Measured worst case 0.31% of
            // the covered pixels (Interact at 2.5 m), all single pixels along piping and outlines.
            CHECK(unmatched <= 4 + covered / 250);
            std::printf("[charlod] %s %.1f m: lod_draws %u tris %llu/%llu covered %zu changed %zu unmatched %zu\n", p.clip, d,
                        lod.stats.lod_draws, static_cast<unsigned long long>(lod.stats.triangles),
                        static_cast<unsigned long long>(full.stats.triangles), covered, changed, unmatched);
        }
    CHECK(lod_frames > 0); // LODs engage at the longer distances
    cache.clear();
    std::error_code ec;
    std::filesystem::remove_all(project, ec);
}
