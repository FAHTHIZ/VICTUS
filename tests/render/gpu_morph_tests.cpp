// SPDX-License-Identifier: Apache-2.0
// Morph targets on the GPU path (ADR-0076): model meshes with targets are morphed on the CPU into
// per-entity vertex copies, re-uploaded only when the weights change, and released when unused.
#include "gpu_fixture.hpp"
#include "victus/animation/animation.hpp"
#include "victus/render/mesh.hpp"
#include "victus/runtime/scene_rendering.hpp"

using namespace victus;
using namespace victus::scene;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;

namespace {

constexpr std::uint32_t W = 96, H = 64;

// A 2x2 m red unlit square at the origin with target "Slide" moving it 3 m along +x.
assets::CookedModel make_slider() {
    assets::CookedModel m;
    assets::ModelMaterial mat;
    mat.name = "Red";
    mat.base_color = {1, 0.05f, 0.05f, 1};
    mat.unlit = true;
    m.materials.push_back(mat);
    render::MeshData mesh = render::make_plane({2, 2}, 1);
    render::MorphTarget slide{.name = "Slide", .vertices = {}, .positions = {}, .normals = {}};
    for (std::uint32_t i = 0; i < mesh.vertices.size(); ++i) {
        slide.vertices.push_back(i);
        slide.positions.push_back({3, 0, 0});
    }
    mesh.morphs.push_back(std::move(slide));
    mesh.morph_weights = {0.0f};
    m.meshes.push_back({"Slider#0", std::move(mesh), 0});
    assets::ModelNode node;
    node.name = "Slider";
    node.meshes = {0};
    m.nodes.push_back(node);
    return m;
}

struct Frame {
    std::vector<std::uint8_t> px;
    runtime::ExtractStats stats;
    [[nodiscard]] int red(std::uint32_t x) const { return channel(pixel(px, W, x, H / 2), 0); }
};

Frame draw(Scene& s, runtime::GpuAssetCache& cache) {
    auto& dev = gpu().ctx->device();
    auto sr = render::SceneRenderer::create(*gpu().ctx);
    REQUIRE(sr.ok());
    auto target = test::make_target(W, H);
    s.update_transforms();
    Frame f;
    f.stats = runtime::extract_scene(s, cache, **sr, {});
    auto cmd = dev.begin_commands();
    REQUIRE((*sr)->render(cmd, target, W, H, rhi::Format::RGBA8_UNORM).ok());
    cmd.submit_and_wait();
    auto px = dev.read_texture(target);
    REQUIRE(px.ok());
    test::save_output("morph_targets", W, H, *px);
    f.px = std::move(*px);
    dev.destroy(target);
    return f;
}

} // namespace

TEST_CASE("gpu: morph targets move model meshes and their copies follow the weights") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Scene s;
    const Entity cam = s.create("Camera");
    s.world().get<Transform>(cam)->position = {0, 8, 0};
    s.world().get<Transform>(cam)->rotation = math::Quat::axis_angle({1, 0, 0}, math::radians(-90));
    s.world().add<CameraComponent>(cam);
    runtime::GpuAssetCache cache(*gpu().ctx, nullptr);
    const Uuid model = Uuid::generate();
    REQUIRE(cache.add_model(model, make_slider()).ok());
    const Entity slider = s.create("Slider");
    auto& mr = s.world().add<MeshRendererComponent>(slider);
    mr.mesh_asset = model;
    mr.mesh_index = 0;

    // Weight 0 (mesh default): the base mesh at the centre, no copy.
    Frame base = draw(s, cache);
    CHECK(base.stats.meshes == 1);
    CHECK(base.stats.morphed == 0);
    CHECK(cache.morphed_count() == 0);
    const std::uint32_t centre = W / 2;
    // Find where +3 m lands on screen: the brighter side of the centre after morphing.
    const int base_centre = base.red(centre);
    MESSAGE("base centre red " << base_centre);
    CHECK(base_centre > 120);

    // Weight 1 on the entity: the square leaves the centre.
    s.world().add<animation::MorphWeightsComponent>(slider).weights = {1.0f};
    Frame moved = draw(s, cache);
    CHECK(moved.stats.morphed == 1);
    CHECK(cache.morphed_count() == 1);
    CHECK(cache.morph_uploads() == 1);
    const int side = std::max(moved.red(centre + 21), moved.red(centre - 21));
    MESSAGE("moved centre red " << moved.red(centre) << " side " << side);
    CHECK(moved.red(centre) + 60 < base_centre);
    CHECK(side > 120);

    // Unchanged weights reuse the copy; changed weights re-upload into it.
    (void)draw(s, cache);
    CHECK(cache.morph_uploads() == 1);
    s.world().get<animation::MorphWeightsComponent>(slider)->weights = {0.5f};
    (void)draw(s, cache);
    CHECK(cache.morph_uploads() == 2);
    CHECK(cache.morphed_count() == 1);

    // Weights on the parent (the Animator's node entity) apply to child mesh entities.
    s.world().remove<animation::MorphWeightsComponent>(slider);
    const Entity node = s.create("Node");
    s.set_parent(slider, node);
    s.world().add<animation::MorphWeightsComponent>(node).weights = {1.0f};
    moved = draw(s, cache);
    CHECK(moved.stats.morphed == 1);
    CHECK(moved.red(centre) + 60 < base_centre);

    // Back to zero: the base mesh draws again and the copy is released.
    s.world().get<animation::MorphWeightsComponent>(node)->weights = {0.0f};
    base = draw(s, cache);
    CHECK(base.stats.morphed == 0);
    CHECK(base.red(centre) > 120);
    CHECK(cache.morphed_count() == 0);

    // Unused copies are collected; evicting the model drops its copies.
    s.world().get<animation::MorphWeightsComponent>(node)->weights = {1.0f};
    (void)draw(s, cache);
    CHECK(cache.morphed_count() == 1);
    s.world().get<MeshRendererComponent>(slider)->visible = false;
    for (int i = 0; i < 10; ++i) (void)draw(s, cache);
    CHECK(cache.morphed_count() == 0);
    s.world().get<MeshRendererComponent>(slider)->visible = true;
    (void)draw(s, cache);
    CHECK(cache.morphed_count() == 1);
    cache.evict(model);
    CHECK(cache.morphed_count() == 0);

    // Static glTF node.weights apply to the node's entity without any component.
    assets::CookedModel posed = make_slider();
    posed.nodes[0].morph_weights = {1.0f};
    const Uuid posed_id = Uuid::generate();
    REQUIRE(cache.add_model(posed_id, std::move(posed)).ok());
    s.destroy(node);
    const Entity statue = s.create("Slider");
    auto& smr = s.world().add<MeshRendererComponent>(statue);
    smr.mesh_asset = posed_id;
    moved = draw(s, cache);
    CHECK(moved.stats.morphed == 1);
    CHECK(moved.red(centre) + 60 < base_centre);
    cache.clear();
}
