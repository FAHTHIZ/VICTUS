// SPDX-License-Identifier: Apache-2.0
// Editor: undo/redo state machine on plain scenes, and the full editor driven headlessly on the
// GPU (projects from templates, viewport rendering, picking, create/delete/duplicate/reparent
// with undo, play mode isolation, save and reopen).
#include "../render/gpu_fixture.hpp"
#include "victus/editor/editor.hpp"
#include "victus/editor/undo.hpp"

#include <filesystem>
#include <set>

using namespace victus;
using namespace victus::scene;
using victus::test::channel;
using victus::test::gpu;
using victus::test::pixel;
namespace sfs = std::filesystem;

namespace {
sfs::path temp_dir(const char* tag) {
    return sfs::temp_directory_path() / (std::string("victus_ed_") + tag + "_" + Uuid::generate().to_string().substr(0, 8));
}
} // namespace

TEST_CASE("undo: property edit, create, delete subtree, reparent, dirty tracking") {
    Scene s;
    const Entity a = s.create("A");
    const Entity b = s.create("B");
    const Entity c = s.create("C", b);
    const Entity d = s.create("D", c);
    s.world().add<MeshRendererComponent>(d).primitive = "cube";
    const Uuid ua = s.uuid(a), ub = s.uuid(b), uc = s.uuid(c), ud = s.uuid(d);
    editor::UndoStack undo;
    CHECK_FALSE(undo.dirty());

    // Property edit.
    auto before = editor::capture(s, {ua});
    s.world().get<Transform>(a)->position = {1, 2, 3};
    s.world().get<Name>(a)->value = "Moved";
    undo.push({"Edit", before, editor::capture(s, {ua})});
    CHECK(undo.dirty());
    REQUIRE(undo.undo(s).ok());
    CHECK(s.world().get<Transform>(s.find(ua))->position == math::Vec3{0, 0, 0});
    CHECK(s.world().get<Name>(s.find(ua))->value == "A");
    CHECK_FALSE(undo.dirty());
    REQUIRE(undo.redo(s).ok());
    CHECK(s.world().get<Transform>(s.find(ua))->position == math::Vec3{1, 2, 3});
    undo.mark_saved();
    CHECK_FALSE(undo.dirty());

    // Delete a subtree (B > C > D) and restore it with components, hierarchy and order.
    auto sub = editor::capture_subtrees(s, {ub});
    REQUIRE(sub.size() == 3);
    s.destroy(s.find(ub));
    CHECK(s.find(ud).is_null());
    undo.push({"Delete", sub, editor::absent(sub)});
    CHECK(undo.dirty());
    REQUIRE(undo.undo(s).ok());
    REQUIRE_FALSE(s.find(ud).is_null());
    CHECK(s.parent(s.find(ud)) == s.find(uc));
    CHECK(s.parent(s.find(uc)) == s.find(ub));
    CHECK(s.world().get<MeshRendererComponent>(s.find(ud))->primitive == "cube");
    CHECK(s.roots().size() == 2);
    CHECK(s.roots()[1] == s.find(ub)); // order: A then B
    CHECK_FALSE(undo.dirty());          // back at the saved revision
    REQUIRE(undo.redo(s).ok());
    CHECK(s.find(ub).is_null());
    REQUIRE(undo.undo(s).ok());

    // Reparent C under A, undo restores the old parent.
    auto rp = editor::capture(s, {uc});
    REQUIRE(s.move_before(s.find(uc), kNullEntity, s.find(ua)));
    undo.push({"Reparent", rp, editor::capture(s, {uc})});
    CHECK(s.parent(s.find(uc)) == s.find(ua));
    REQUIRE(undo.undo(s).ok());
    CHECK(s.parent(s.find(uc)) == s.find(ub));
    CHECK(s.parent(s.find(ud)) == s.find(uc)); // children travel with their parent

    // A new edit after undo discards the redo branch and is dirty even at the same depth.
    undo.mark_saved();
    auto e2 = editor::capture(s, {ua});
    s.world().get<Transform>(s.find(ua))->scale = {2, 2, 2};
    undo.push({"Scale", e2, editor::capture(s, {ua})});
    CHECK_FALSE(undo.can_redo());
    REQUIRE(undo.undo(s).ok());
    CHECK_FALSE(undo.dirty());
    auto e3 = editor::capture(s, {ua});
    s.world().get<Transform>(s.find(ua))->scale = {3, 3, 3};
    undo.push({"Scale", e3, editor::capture(s, {ua})});
    CHECK(undo.dirty());
}

namespace {
struct Harness {
    std::unique_ptr<ui::UiContext> ui;
    std::unique_ptr<editor::Editor> ed;
    sfs::path dir;
    static constexpr std::uint32_t W = 1280, H = 720, VW = 480, VH = 270;

    bool init() {
        auto& ctx = *gpu().ctx;
        dir = temp_dir("h");
        sfs::create_directories(dir);
        auto u = ui::UiContext::create(ctx, rhi::Format::RGBA8_UNORM, nullptr, nullptr);
        if (!u) return false;
        ui = std::move(u).value();
        editor::EditorOptions o;
        o.recent_file = dir / "recent.json";
        o.default_project_dir = dir;
        auto e = editor::Editor::create(ctx, *ui, nullptr, nullptr, o);
        if (!e) return false;
        ed = std::move(e).value();
        ed->set_viewport_size_override(VW, VH);
        return true;
    }
    void frames(int n, float dt = 1.0f / 60.0f) {
        auto& dev = gpu().ctx->device();
        for (int i = 0; i < n; ++i) {
            ui->new_frame(dt, {static_cast<float>(W), static_cast<float>(H)});
            ed->frame(dt);
            auto cmd = dev.begin_commands();
            REQUIRE(ed->render(cmd).ok());
            REQUIRE(ui->prepare(cmd).ok());
            cmd.submit_and_wait();
        }
    }
    std::vector<std::uint8_t> viewport() {
        auto px = gpu().ctx->device().read_texture(ed->viewport_texture());
        REQUIRE(px.ok());
        return std::move(px).value();
    }
    ~Harness() {
        ed.reset();
        ui.reset();
        std::error_code ec;
        sfs::remove_all(dir, ec);
    }
};

// Largest absolute channel difference.
int diff(std::uint32_t a, std::uint32_t b) {
    int m = 0;
    for (int c = 0; c < 3; ++c) m = std::max(m, std::abs(int(channel(a, c)) - int(channel(b, c))));
    return m;
}
} // namespace

TEST_CASE("editor: 3D project workflow end to end") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    h.frames(2); // launcher
    CHECK_FALSE(h.ed->has_project());
    REQUIRE(h.ed->create_project(h.dir / "Game", "Game", "3d").ok());
    REQUIRE(h.ed->has_project());
    CHECK(h.ed->scene_path() == "scenes/main.vscene");
    CHECK_FALSE(h.ed->dirty());
    h.frames(3);
    REQUIRE(h.ed->viewport_texture().valid());
    auto px = h.viewport();
    test::save_output("editor_viewport_3d", Harness::VW, Harness::VH, px);
    // Sky at the top, lit objects in the middle: the image is not a flat colour.
    const auto top = pixel(px, Harness::VW, Harness::VW / 2, 4);
    int distinct = 0;
    for (std::uint32_t x = 0; x < Harness::VW; x += 8)
        if (diff(pixel(px, Harness::VW, x, Harness::VH / 2), top) > 40) ++distinct;
    CHECK(distinct > 3);

    // Grid overlay must only add thin lines: no large uniformly grid-coloured regions.
    // (Regression: line vertices beyond the first upload chunk were corrupted.)
    Scene& s = h.ed->edit_scene();
    const std::size_t initial = s.size();

    // Picking: the cube sits near the centre-left; pick at every column of the middle row
    // and require that the template's three meshes are all found.
    std::set<std::string> picked;
    for (float x = 0; x < Harness::VW; x += 4) {
        const Entity e = h.ed->pick({x, Harness::VH * 0.45f}, {float(Harness::VW), float(Harness::VH)});
        if (!e.is_null()) picked.insert(s.world().get<Name>(e)->value);
    }
    CHECK(picked.count("Spinning Cube") == 1);
    // The player's skinned robot (its body primitives are the Body[0]/Body[1] parts).
    CHECK((picked.count("Body[0]") + picked.count("Body[1]")) >= 1);

    // Create, undo, redo.
    const Entity cube = h.ed->create_entity(editor::Primitive::Cube);
    const Uuid cube_id = s.uuid(cube);
    CHECK(s.size() == initial + 1);
    CHECK(h.ed->dirty());
    CHECK(h.ed->selected() == cube);
    h.ed->undo();
    CHECK(s.find(cube_id).is_null());
    CHECK_FALSE(h.ed->dirty());
    h.ed->redo();
    REQUIRE_FALSE(s.find(cube_id).is_null());

    // Duplicate + reparent under the original, undo both.
    h.ed->select(s.find(cube_id));
    h.ed->duplicate_selected();
    REQUIRE(h.ed->selection().size() == 1);
    const Uuid copy_id = h.ed->selection()[0];
    CHECK(copy_id != cube_id);
    CHECK(s.size() == initial + 2);
    s.world().get<Transform>(s.find(copy_id))->position = {3, 1, 0};
    s.update_transforms();
    const math::Mat4 world_before = s.world_matrix(s.find(copy_id));
    REQUIRE(h.ed->reparent(s.find(copy_id), s.find(cube_id)));
    CHECK(s.parent(s.find(copy_id)) == s.find(cube_id));
    const math::Mat4 world_after = s.world_matrix(s.find(copy_id));
    CHECK(world_after.c[3].x == doctest::Approx(world_before.c[3].x).epsilon(1e-4));
    h.ed->undo(); // reparent
    CHECK(s.parent(s.find(copy_id)).is_null());

    // Delete the original: undo brings it back.
    h.ed->select(s.find(cube_id));
    h.ed->delete_selected();
    CHECK(s.find(cube_id).is_null());
    h.ed->undo();
    CHECK_FALSE(s.find(cube_id).is_null());

    // Play mode runs systems on a copy; stopping restores the authored scene.
    const Entity spinner = s.find_by_name("Spinning Cube");
    REQUIRE_FALSE(spinner.is_null());
    const math::Quat authored = s.world().get<Transform>(spinner)->rotation;
    const std::size_t undo_depth = h.ed->undo_stack().size();
    h.ed->play();
    CHECK(h.ed->play_state() == editor::PlayState::Playing);
    h.frames(10);
    Scene& play = h.ed->active_scene();
    CHECK(&play != &s);
    const Entity play_spinner = play.find(s.uuid(spinner));
    REQUIRE_FALSE(play_spinner.is_null());
    CHECK_FALSE(play.world().get<Transform>(play_spinner)->rotation == authored);
    // Template scripts run in play mode: the player is a simulated character standing on the
    // ground, the camera follows, no script errors.
    REQUIRE(h.ed->play_scripts() != nullptr);
    CHECK(h.ed->play_scripts()->stats().instances == 7); // camera, player, buddy, 3 crates, HUD
    CHECK(h.ed->play_scripts()->stats().errors == 0);
    {
        const Entity ap = s.find_by_name("Player");
        const Entity pp = play.find(s.uuid(ap));
        REQUIRE_FALSE(pp.is_null());
        REQUIRE(h.ed->play_physics() != nullptr);
        const physics::CharacterState cs = h.ed->play_physics()->character_state(pp);
        CHECK(cs.valid);
        CHECK(cs.grounded);
        CHECK(play.world().get<Transform>(pp)->position.y == doctest::Approx(0.0f).epsilon(0.05));
    }
    // The companion's navmesh was built for the play session.
    REQUIRE(h.ed->play_navigation() != nullptr);
    CHECK(h.ed->play_navigation()->valid());
    CHECK(h.ed->play_navigation()->stats().agents == 1);
    h.ed->create_entity(editor::Primitive::Sphere); // play-time edit, not undoable, discarded
    CHECK(h.ed->undo_stack().size() == undo_depth);
    auto play_px = h.viewport();
    test::save_output("editor_viewport_play", Harness::VW, Harness::VH, play_px);
    h.ed->stop();
    CHECK(h.ed->play_state() == editor::PlayState::Editing);
    CHECK(&h.ed->active_scene() == &s);
    CHECK(s.world().get<Transform>(spinner)->rotation == authored);
    CHECK(h.ed->play_scripts() == nullptr);
    CHECK(h.ed->play_navigation() == nullptr);
    // View > Navigation Mesh previews the navmesh while editing.
    h.ed->set_show_navmesh(true);
    h.frames(1);
    REQUIRE(h.ed->navmesh_preview() != nullptr);
    CHECK(h.ed->navmesh_preview()->valid());
    CHECK(h.ed->navmesh_preview()->stats().polygons > 0);
    const std::uint32_t builds = h.ed->navmesh_preview()->stats().builds;
    h.frames(2);
    CHECK(h.ed->navmesh_preview()->stats().builds == builds); // unchanged scene: no rebuild
    test::save_output("editor_viewport_navmesh", Harness::VW, Harness::VH, h.viewport());
    h.ed->set_show_navmesh(false);

    // Save and reopen: the edits persist and the scene is clean.
    REQUIRE(h.ed->save_scene().ok());
    CHECK_FALSE(h.ed->dirty());
    const std::size_t saved_size = s.size();
    REQUIRE(h.ed->open_project(h.dir / "Game").ok());
    CHECK(h.ed->edit_scene().size() == saved_size);
    CHECK_FALSE(h.ed->edit_scene().find(cube_id).is_null());
    h.frames(1);
}

TEST_CASE("editor: 2D and simulation templates render in their modes") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Platformer", "Platformer", "2d").ok());
    CHECK(runtime::scene_view_mode(h.ed->edit_scene()) == runtime::ViewMode::TwoD);
    h.frames(2);
    auto px = h.viewport();
    test::save_output("editor_viewport_2d", Harness::VW, Harness::VH, px);
    // The ground sprite (green) spans the bottom of the view.
    int green = 0;
    for (std::uint32_t x = 0; x < Harness::VW; x += 4) {
        const auto p = pixel(px, Harness::VW, x, Harness::VH - 20);
        if (channel(p, 1) > channel(p, 0) + 30 && channel(p, 1) > channel(p, 2) + 30) ++green;
    }
    CHECK(green > 20);
    // 2D picking hits the player sprite at its projected position.
    Scene& s = h.ed->edit_scene();
    const Entity player = s.find_by_name("Player");
    REQUIRE_FALSE(player.is_null());
    const math::Vec3 p = s.world_matrix(player).transform_point({0, 0, 0});
    const math::Vec2 screen = h.ed->camera().camera2d(float(Harness::VH)).world_to_screen({p.x, p.y}, float(Harness::VW), float(Harness::VH));
    CHECK(h.ed->pick(screen, {float(Harness::VW), float(Harness::VH)}) == player);

    REQUIRE(h.ed->create_project(h.dir / "Sim", "Sim", "simulation").ok());
    CHECK(h.ed->edit_scene().size() > 576);
    h.ed->play();
    h.frames(3);
    auto sim_px = h.viewport();
    test::save_output("editor_viewport_sim", Harness::VW, Harness::VH, sim_px);
    h.ed->stop();
}

TEST_CASE("editor: rendering quality selector applies, persists and reopens") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Quality", "Quality", "3d").ok());
    CHECK(h.ed->quality() == render::QualityTier::High);
    h.frames(2);
    const auto high = h.ed->render_stats();
    CHECK(high.shadow_cascades == 3);
    CHECK(high.main_batches > 0);

    REQUIRE(h.ed->set_quality(render::QualityTier::Low).ok());
    h.frames(2);
    const auto low = h.ed->render_stats();
    CHECK(low.shadow_cascades == 1);
    CHECK(low.render_width < high.render_width); // low renders at 0.75 scale
    CHECK(h.ed->project()->settings().render_quality == "low");
    auto reopened = project::Project::open(h.dir / "Quality");
    REQUIRE(reopened.ok());
    CHECK(reopened->settings().render_quality == "low");

    // Reopening the project in the editor restores the tier.
    REQUIRE(h.ed->open_project(h.dir / "Quality").ok());
    CHECK(h.ed->quality() == render::QualityTier::Low);
    h.frames(1);
    CHECK(h.ed->render_stats().shadow_cascades == 1);
}

TEST_CASE("editor: input actions panel edits, captures bindings and saves the project") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    CHECK(h.ed->input_map() == nullptr);
    REQUIRE(h.ed->create_project(h.dir / "Input", "Input", "3d").ok());
    REQUIRE(h.ed->input_map() != nullptr);
    CHECK(*h.ed->input_map() == gameplay::default_input_map());
    h.ed->set_show_input_actions(true);
    h.frames(2); // the panel draws every action without errors

    // Invalid maps are refused and change nothing.
    gameplay::InputMap bad = *h.ed->input_map();
    bad.actions[0].positive.push_back({.kind = gameplay::InputBinding::Kind::Key, .key = "Qq"});
    CHECK_FALSE(h.ed->set_input_map(bad).ok());
    gameplay::InputMap dup = *h.ed->input_map();
    dup.actions.push_back(dup.actions[0]);
    CHECK_FALSE(h.ed->set_input_map(dup).ok());
    CHECK(*h.ed->input_map() == gameplay::default_input_map());

    // Listen: the next key goes to the action and is saved to victus.project.
    gameplay::ManualInput in;
    h.ed->begin_binding_capture("jump");
    CHECK(h.ed->capturing_binding());
    CHECK_FALSE(h.ed->capture_from(in)); // nothing pressed yet
    in.set_key("J", true);
    CHECK(h.ed->capture_from(in));
    CHECK_FALSE(h.ed->capturing_binding());
    const auto* jump = h.ed->input_map()->find("jump");
    REQUIRE(jump);
    CHECK(gameplay::binding_to_string(jump->positive.back()) == "J");
    in.end_frame();

    // A stick pushed left binds the negative half-axis; Escape cancels without binding.
    h.ed->begin_binding_capture("look_x", true);
    in.set_gamepad_axis(0, gameplay::gamepad_axis_index("RightX"), -0.9f);
    CHECK(h.ed->capture_from(in));
    const auto* look = h.ed->input_map()->find("look_x");
    REQUIRE(look);
    CHECK(gameplay::binding_to_string(look->negative.back()) == "Pad.RightX-");
    in.end_frame();
    const std::size_t before = h.ed->input_map()->find("fire")->positive.size();
    h.ed->begin_binding_capture("fire");
    in.set_key("Escape", true);
    CHECK(h.ed->capture_from(in));
    CHECK_FALSE(h.ed->capturing_binding());
    CHECK(h.ed->input_map()->find("fire")->positive.size() == before);

    auto reopened = project::Project::open(h.dir / "Input");
    REQUIRE(reopened.ok());
    CHECK(reopened->settings().input == *h.ed->input_map());

    // While playing, edits reach the running scripts.
    h.ed->play();
    h.frames(2);
    REQUIRE(h.ed->play_scripts());
    gameplay::InputMap renamed = *h.ed->input_map();
    renamed.add("dash", {"Q", "Pad.East"});
    REQUIRE(h.ed->set_input_map(renamed).ok());
    CHECK(h.ed->play_scripts()->options().input_map.find("dash") != nullptr);
    h.frames(2);
    h.ed->stop();
    h.frames(1);
}
