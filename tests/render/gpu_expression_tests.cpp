// SPDX-License-Identifier: Apache-2.0
// Facial expressions and equipment in the runtime renderer. Orion's face-display shapes, drawn
// through extract_scene, must change the image only inside the visor and morph only the visor's
// primitive; a prop attached to the right-hand socket of a running, GPU-skinned Orion must be
// drawn where the hand socket projects, following it from pose to pose.
#include "gpu_fixture.hpp"
#include "victus/animation/animation.hpp"
#include "victus/assets/importers.hpp"
#include "victus/assets/instantiate.hpp"
#include "victus/project/characters.hpp"
#include "victus/runtime/scene_rendering.hpp"

#include <chrono>

using namespace victus;
using victus::test::gpu;

namespace {

constexpr std::uint32_t W = 256, H = 192;

const assets::CookedModel& orion() {
    static const assets::CookedModel model = [] {
        const project::StarterCharacter* c = project::find_starter_character("orion");
        REQUIRE(c);
        assets::ModelImportSettings s;
        s.lods = 0; // textures stay: the face glyphs and lit piping come from the emissive maps
        auto m = assets::import_gltf_memory(project::starter_character_glb(*c), c->model_path, s);
        REQUIRE(m.ok());
        return std::move(m).value();
    }();
    return model;
}

struct View {
    scene::Scene s;
    scene::Entity root;
    render::Camera3D cam;
};

void light(scene::Scene& s) {
    const scene::Entity sun = s.create("Sun");
    s.world().add<scene::LightComponent>(sun).intensity = 3.0f;
    s.world().get<scene::Transform>(sun)->rotation = math::Quat::axis_angle({1, 0, 0}, -0.6f);
}

struct Frame {
    std::vector<std::uint8_t> px;
    runtime::ExtractStats stats;
    double extract_ms{0};
};

Frame draw(scene::Scene& s, runtime::GpuAssetCache& cache, const render::Camera3D& cam, const char* save) {
    auto& dev = gpu().ctx->device();
    auto sr = render::SceneRenderer::create(*gpu().ctx);
    REQUIRE(sr.ok());
    runtime::SceneViewSettings sv;
    sv.environment.draw_sky = false;
    sv.environment.clear_color = {0, 0, 0, 1};
    s.update_transforms();
    Frame f;
    const auto t0 = std::chrono::steady_clock::now();
    f.stats = runtime::extract_scene(s, cache, **sr, sv, &cam);
    f.extract_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    auto target = test::make_target(W, H);
    auto cmd = dev.begin_commands();
    REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    f.px = dev.read_texture(target).value();
    dev.destroy(target);
    if (save) test::save_output(save, W, H, f.px);
    return f;
}

// Pixel of a world point (x right, y down from the top row as read back), or {-1, -1} behind.
std::pair<int, int> to_pixel(const render::Camera3D& cam, math::Vec3 p) {
    const math::Vec4 c = cam.projection(float(W) / float(H)) * (cam.view * math::Vec4(p, 1.0f));
    if (c.w <= 0) return {-1, -1};
    const float x = (c.x / c.w * 0.5f + 0.5f) * float(W), y = (0.5f - c.y / c.w * 0.5f) * float(H);
    return {int(x), int(y)};
}

struct Diff {
    std::size_t changed{0}, outside{0};
    int x0{W}, y0{H}, x1{-1}, y1{-1};
};

// Pixels differing by more than 24/255, and how many fall outside [rx0, rx1] x [ry0, ry1].
Diff diff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, int rx0, int ry0, int rx1, int ry1) {
    Diff d;
    for (int y = 0; y < int(H); ++y)
        for (int x = 0; x < int(W); ++x) {
            const std::size_t i = (std::size_t(y) * W + std::size_t(x)) * 4;
            int m = 0;
            for (int k = 0; k < 3; ++k) m = std::max(m, std::abs(int(a[i + k]) - int(b[i + k])));
            if (m <= 24) continue;
            ++d.changed;
            d.outside += x < rx0 || x > rx1 || y < ry0 || y > ry1;
            d.x0 = std::min(d.x0, x);
            d.y0 = std::min(d.y0, y);
            d.x1 = std::max(d.x1, x);
            d.y1 = std::max(d.y1, y);
        }
    return d;
}

} // namespace

TEST_CASE("gpu expressions: face-display shapes change only the visor in the runtime renderer and morph only its primitive") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    animation::register_components();
    const assets::CookedModel& m = orion();
    runtime::GpuAssetCache cache(*gpu().ctx, nullptr);
    const Uuid id = Uuid::generate();
    REQUIRE(cache.add_model(id, m).ok());

    scene::Scene s;
    const scene::Entity root = assets::instantiate_model(s, id, m, "Orion");
    s.world().remove<animation::AnimatorComponent>(root);
    light(s);
    // The visor: the one primitive whose shapes move vertices.
    std::uint32_t visor = 0;
    for (std::uint32_t i = 0; i < m.meshes.size(); ++i)
        for (const auto& t : m.meshes[i].data.morphs)
            if (!t.vertices.empty()) visor = i;
    std::vector<scene::Entity> face; // every entity drawing a primitive with targets
    s.world().each<scene::MeshRendererComponent>([&](scene::Entity e, const scene::MeshRendererComponent& mr) {
        if (mr.mesh_index < m.meshes.size() && !m.meshes[mr.mesh_index].data.morphs.empty()) face.push_back(e);
    });
    REQUIRE(face.size() == m.meshes.size());
    const auto& names = m.meshes[visor].data.morphs;
    auto set = [&](std::initializer_list<std::pair<const char*, float>> shape) {
        std::vector<float> w(names.size(), 0.0f);
        for (const auto& [n, v] : shape)
            for (std::size_t i = 0; i < names.size(); ++i)
                if (names[i].name == n) w[i] = v;
        for (const scene::Entity e : face) s.world().add<animation::MorphWeightsComponent>(e).weights = w;
    };

    s.update_transforms();
    const scene::Entity head = s.find_descendant(root, "Socket.Face");
    REQUIRE_FALSE(head.is_null());
    const math::Vec3 at = s.world_matrix(head).transform_point({0, 0, 0});
    // The visor faces +Z (the character's front).
    const render::Camera3D cam = render::Camera3D::look_at(at + math::Vec3{0.0f, 0.02f, 0.55f}, at);
    // Screen rectangle of the visor's vertices (the model is at its bind pose).
    int rx0 = W, ry0 = H, rx1 = -1, ry1 = -1;
    for (const auto& v : m.meshes[visor].data.vertices) {
        const auto [x, y] = to_pixel(cam, v.position);
        rx0 = std::min(rx0, x), ry0 = std::min(ry0, y), rx1 = std::max(rx1, x), ry1 = std::max(ry1, y);
    }
    rx0 -= 2, ry0 -= 2, rx1 += 2, ry1 += 2;

    set({});
    const Frame neutral = draw(s, cache, cam, "expression_neutral");
    struct Case {
        const char* name;
        std::initializer_list<std::pair<const char*, float>> shape;
    };
    const Case cases[] = {{"expression_smile", {{"mouthSmile_L", 0.9f}, {"mouthSmile_R", 0.9f}, {"cheekSquint_L", 0.5f}, {"cheekSquint_R", 0.5f}}},
                          {"expression_blink", {{"eyeBlink_L", 1.0f}, {"eyeBlink_R", 1.0f}}},
                          {"expression_jaw_open", {{"jawOpen", 1.0f}}},
                          {"expression_surprise", {{"eyeWide_L", 0.9f}, {"eyeWide_R", 0.9f}, {"browInnerUp_L", 0.8f}, {"browInnerUp_R", 0.8f}, {"jawOpen", 0.4f}}}};
    for (const Case& c : cases) {
        set(c.shape);
        const Frame f = draw(s, cache, cam, c.name);
        const Diff d = diff(f.px, neutral.px, rx0, ry0, rx1, ry1);
        MESSAGE(std::string(c.name) << ": " << d.changed << " px changed, " << d.outside << " outside the visor rectangle [" << rx0 << "," << ry0 << "]-[" << rx1
                       << "," << ry1 << "], changes in [" << d.x0 << "," << d.y0 << "]-[" << d.x1 << "," << d.y1 << "]; primitives morphed " << f.stats.morphed
                       << " of " << face.size());
        CHECK(d.changed > 20);
        CHECK(d.outside == 0);
        CHECK(f.stats.morphed == 1); // only the visor is morphed and uploaded
    }

    // CPU cost of a changing expression (a blink animating every frame) against a held one.
    double changing = 0, held = 0;
    constexpr int kRuns = 40;
    auto& dev = gpu().ctx->device();
    auto sr = render::SceneRenderer::create(*gpu().ctx);
    REQUIRE(sr.ok());
    runtime::SceneViewSettings sv;
    for (int i = 0; i < kRuns * 2; ++i) {
        set({{"eyeBlink_L", 0.05f * float(i + 1)}, {"eyeBlink_R", 0.05f * float(i + 1)}});
        const auto t0 = std::chrono::steady_clock::now();
        (void)runtime::extract_scene(s, cache, **sr, sv, &cam);
        if (i >= kRuns) changing += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    for (int i = 0; i < kRuns; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        (void)runtime::extract_scene(s, cache, **sr, sv, &cam);
        held += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    (void)dev;
    MESSAGE("extract_scene with the expression changing every frame " << changing / kRuns << " ms, held " << held / kRuns << " ms ("
                                                                     << m.meshes[visor].data.vertices.size() << " visor vertices)");
}

TEST_CASE("gpu equipment: a prop on the right-hand socket is drawn where the GPU-skinned hand puts it, pose after pose") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    animation::register_components();
    const assets::CookedModel& m = orion();
    runtime::GpuAssetCache cache(*gpu().ctx, nullptr);
    const Uuid id = Uuid::generate();
    REQUIRE(cache.add_model(id, m).ok());
    scene::Scene s;
    const scene::Entity root = assets::instantiate_model(s, id, m, "Orion");
    {
        auto* a = s.world().get<animation::AnimatorComponent>(root);
        REQUIRE(a);
        a->clip = "Run";
        a->controller.clear();
        a->playing = false;
    }
    light(s);
    animation::AnimationSystem sys;
    sys.add_set(id, std::make_shared<animation::AnimationSet>(assets::make_animation_set(m)));
    // A bright green cube, 10 cm, held in the right hand.
    const scene::Entity prop = s.create("Prop");
    auto& mr = s.world().add<scene::MeshRendererComponent>(prop);
    mr.primitive = "cube";
    mr.base_color = {0.05f, 1.0f, 0.05f, 1.0f};
    s.world().get<scene::Transform>(prop)->scale = {0.1f, 0.1f, 0.1f};
    const scene::Entity sock = s.attach_to_socket(prop, root, "Socket.Hand.R");
    REQUIRE_FALSE(sock.is_null());
    const render::Camera3D cam = render::Camera3D::look_at({-1.3f, 1.2f, 1.7f}, {-0.2f, 0.95f, 0});

    std::pair<int, int> last{-1, -1};
    float moved = 0;
    for (const float t : {0.0f, 0.14f, 0.28f, 0.41f}) {
        s.world().get<animation::AnimatorComponent>(root)->time = t; // re-read: creating entities moves components
        sys.update(s, 0.0f);
        s.update_transforms();
        const math::Vec3 p = s.world_matrix(sock).transform_point({0, 0, 0});
        const auto [x, y] = to_pixel(cam, p);
        const Frame f = draw(s, cache, cam, t == 0.0f ? "equipment_hand_run_0" : nullptr);
        // Green-dominant pixels near the projected socket, and anywhere else.
        int near = 0, far = 0;
        for (int py = 0; py < int(H); ++py)
            for (int px = 0; px < int(W); ++px) {
                const std::size_t i = (std::size_t(py) * W + std::size_t(px)) * 4;
                const int r = f.px[i], g = f.px[i + 1], b = f.px[i + 2];
                if (g < 60 || g < 2 * r || g < 2 * b) continue;
                (std::abs(px - x) <= 12 && std::abs(py - y) <= 12 ? near : far) += 1;
            }
        MESSAGE("Run at " << t << " s: socket projects to (" << x << ", " << y << "); green prop pixels within 12 px " << near << ", elsewhere " << far
                          << "; skinned meshes " << f.stats.skinned);
        CHECK(near >= 4);
        CHECK(far <= near / 2);
        if (last.first >= 0) moved = std::max(moved, std::hypot(float(x - last.first), float(y - last.second)));
        last = std::pair{x, y};
    }
    MESSAGE("largest move of the hand socket between poses " << moved << " px");
    CHECK(moved > 8.0f);
}
