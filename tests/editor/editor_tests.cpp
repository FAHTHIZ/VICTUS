// SPDX-License-Identifier: Apache-2.0
// Editor: undo/redo state machine on plain scenes, and the full editor driven headlessly on the
// GPU (projects from templates, viewport rendering, picking, create/delete/duplicate/reparent
// with undo, play mode isolation, save and reopen).
#include "../render/gpu_fixture.hpp"
#include "victus/editor/editor.hpp"
#include "victus/animation/animation.hpp"
#include "victus/gameplay/capture.hpp"
#include "victus/editor/undo.hpp"
#include "victus/core/fs.hpp"
#include "victus/core/json.hpp"
#include "victus/scene/prefab.hpp"

#include "victus/render/image.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/gameui/gameui.hpp"
#include "victus/net/net.hpp"
#include "victus/physics/physics.hpp"
#include "victus/project/project.hpp"
#include "victus/runtime/surface_shader.hpp"
#include "victus/graph/material_graph.hpp"
#include "victus/script/script.hpp"
#include "victus/tilemap/tilemap.hpp"
#include <thread>

#include <algorithm>
#include <chrono>
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
    // The player's skinned Vanguard (its body primitives are the V01_Body[0..3] parts).
    CHECK((picked.count("V01_Body[0]") + picked.count("V01_Body[1]") + picked.count("V01_Body[2]") +
           picked.count("V01_Body[3]")) >= 1);

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
    CHECK(h.ed->play_scripts()->stats().instances == 8); // camera, player, buddy, sentry, 3 crates, HUD
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
    // Play > Record Capture: Stop saves a capture; replaying it reproduces every digest even
    // though the editor now ticks with a different frame time.
    CHECK(h.ed->captures().empty());
    h.ed->record();
    CHECK(h.ed->recording());
    CHECK(h.ed->play_state() == editor::PlayState::Playing);
    h.frames(40, 1.0f / 60.0f);
    h.frames(30, 1.0f / 30.0f);
    h.ed->stop();
    CHECK_FALSE(h.ed->recording());
    const auto caps = h.ed->captures();
    REQUIRE(caps.size() == 1);
    CHECK(caps[0] == h.ed->last_capture());
    CHECK(caps[0].parent_path() == h.dir / "Game" / ".victus" / "captures");
    {
        auto cap = gameplay::Capture::load(caps[0]);
        REQUIRE(cap);
        CHECK(cap->frames.size() == 70);
        CHECK(cap->scene == "scenes/main.vscene");
        CHECK(cap->seed != 0);
        REQUIRE(cap->digests.size() == 2); // frame 60 and the final 70
        CHECK_FALSE(cap->digests.back().state.empty());
        // A tampered copy (the player recorded elsewhere) must be reported by name.
        bool moved = false;
        for (auto& line : cap->digests.back().state)
            if (line.rfind("Player|", 0) == 0) {
                line = "Player|12345 0 0" + line.substr(line.find('|', 7));
                moved = true;
            }
        REQUIRE(moved);
        cap->digests.back().value = gameplay::digest_of(cap->digests.back().state);
        REQUIRE(cap->save(h.dir / "tampered.vcapture"));
    }
    REQUIRE(h.ed->replay(caps[0]).ok());
    CHECK(h.ed->replaying());
    for (int i = 0; i < 100 && h.ed->replay_result().empty(); ++i) h.frames(1, 1.0f / 45.0f);
    MESSAGE(h.ed->replay_result());
    CHECK_FALSE(h.ed->replay_diverged());
    CHECK(h.ed->replay_result() == "Replay matched: 70 frames, 2 digests");
    CHECK(h.ed->play_state() == editor::PlayState::Paused);
    h.ed->play(); // a finished replay stays paused
    CHECK(h.ed->play_state() == editor::PlayState::Paused);
    REQUIRE(h.ed->replay(h.dir / "tampered.vcapture").ok());
    for (int i = 0; i < 100 && h.ed->replay_result().empty(); ++i) h.frames(1);
    MESSAGE(h.ed->replay_result());
    CHECK(h.ed->replay_diverged());
    CHECK(h.ed->replay_result().find("frame 70: Player: position (") != std::string::npos);
    CHECK(h.ed->replay_result().find(") expected (1.2345, 0, 0)") != std::string::npos);
    h.ed->stop();
    CHECK(h.ed->captures().size() == 1); // replays never write captures
    CHECK_FALSE(h.ed->replay(h.dir / "missing.vcapture").ok());
    CHECK(h.ed->play_state() == editor::PlayState::Editing);
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

TEST_CASE("editor: overdraw view toggles the viewport debug view without touching the project") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Overdraw", "Overdraw", "3d").ok());
    CHECK(h.ed->debug_view() == render::DebugView::None);
    h.ed->set_debug_view(render::DebugView::Overdraw);
    h.frames(2);
    auto st = h.ed->read_overdraw();
    REQUIRE(st.ok());
    CHECK(st->covered_pixels > 0); // the 3D template's ground and cube
    CHECK(st->max_layers >= 1);
    // Switching quality keeps the debug view; it is never written to the project.
    REQUIRE(h.ed->set_quality(render::QualityTier::Low).ok());
    CHECK(h.ed->debug_view() == render::DebugView::Overdraw);
    h.ed->set_debug_view(render::DebugView::None);
    h.frames(2);
    st = h.ed->read_overdraw();
    REQUIRE(st.ok());
    CHECK(st->covered_pixels == 0);
    CHECK(h.ed->render_stats().main_batches > 0);
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

TEST_CASE("editor: assets changed on disk are hot-reloaded without a manual reimport") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Hot", "Hot", "3d").ok());
    h.frames(2);
    CHECK(h.ed->check_asset_changes() == 0);
    const auto assets_dir = h.dir / "Hot" / "assets";
    auto png = render::encode_png(render::make_checker(8, 2, 0xff0000ffu, 0xffffffffu));
    REQUIRE(png.ok());
    REQUIRE(fs::write_atomic(assets_dir / "decal.png", *png).ok());
    CHECK(h.ed->check_asset_changes() == 1);
    REQUIRE(h.ed->assets()->find_path("decal.png"));
    const auto& msgs = h.ed->messages();
    CHECK(std::find(msgs.begin(), msgs.end(), "Reloaded decal.png") != msgs.end());
    // Saving the scene from the editor is not reported as an outside change needing a restart.
    REQUIRE(h.ed->save_scene().ok());
    const std::size_t before = h.ed->messages().size();
    (void)h.ed->check_asset_changes();
    for (std::size_t i = before; i < h.ed->messages().size(); ++i)
        CHECK(h.ed->messages()[i].find("restart") == std::string::npos);
    // While playing, a re-import swaps the asset in the running session too.
    h.ed->play();
    h.frames(2);
    png = render::encode_png(render::make_checker(8, 2, 0xff00ff00u, 0xffffffffu));
    REQUIRE(png.ok());
    REQUIRE(fs::write_atomic(assets_dir / "decal.png", *png).ok());
    sfs::last_write_time(assets_dir / "decal.png", sfs::file_time_type::clock::now() + std::chrono::seconds(2));
    CHECK(h.ed->check_asset_changes() == 1);
    h.frames(2);
    h.ed->stop();
    h.frames(1);
}

TEST_CASE("editor: prefabs are created, instantiated, overridden, applied, reverted and hot-reloaded") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Pfb", "Pfb", "3d").ok());
    h.frames(2);
    auto& ed = *h.ed;
    Scene& s = ed.edit_scene();

    // A "Lamp" with a Bulb child becomes a prefab; the original turns into its first instance.
    const Entity lamp = ed.create_entity(editor::Primitive::Cube);
    ed.rename(lamp, "Lamp");
    const Entity bulb = ed.create_entity(editor::Primitive::PointLight, lamp);
    ed.rename(bulb, "Bulb");
    auto asset = ed.create_prefab(lamp);
    REQUIRE(asset);
    const auto* rec = ed.assets()->find(*asset);
    REQUIRE(rec);
    CHECK(rec->path == "prefabs/Lamp.vprefab");
    CHECK(rec->kind == assets::AssetKind::Prefab);
    CHECK(sfs::exists(h.dir / "Pfb" / "assets" / "prefabs" / "Lamp.vprefab.meta"));
    REQUIRE(s.world().get<PrefabInstance>(lamp));
    CHECK(s.world().get<PrefabPart>(bulb));
    // Undo removes the link, redo restores it.
    ed.undo();
    CHECK_FALSE(s.world().has<PrefabInstance>(s.find_by_name("Lamp")));
    ed.redo();
    const Entity lamp1 = s.find_by_name("Lamp");
    REQUIRE(s.world().get<PrefabInstance>(lamp1));

    // A second instance from the asset browser path.
    const Entity lamp2 = ed.instantiate_prefab(*asset);
    REQUIRE_FALSE(lamp2.is_null());
    CHECK(s.children(lamp2).size() == 1);
    const Entity bulb2 = s.children(lamp2)[0];

    // Renaming a part of instance 2 records a "name" override on its root.
    ed.rename(bulb2, "Red bulb");
    REQUIRE(s.world().get<PrefabInstance>(lamp2)->overrides.size() == 1);
    CHECK(s.world().get<PrefabInstance>(lamp2)->overrides[0].component == "name");

    // Apply from instance 1: brighter bulb -> instance 2 gets it, keeps its own name override.
    const Entity bulb1 = s.children(lamp1)[0];
    s.world().get<LightComponent>(bulb1)->intensity = 42.0f;
    REQUIRE(ed.apply_prefab(bulb1).ok()); // any entity of the instance works
    CHECK(s.world().get<LightComponent>(s.children(lamp2)[0])->intensity == doctest::Approx(42.0f));
    CHECK(s.world().get<Name>(s.children(lamp2)[0])->value == "Red bulb");
    auto saved = load_prefab_file(h.dir / "Pfb" / "assets" / "prefabs" / "Lamp.vprefab");
    REQUIRE(saved);
    CHECK(saved->size() == 2);
    // Our own write is not reported as an outside change.
    const std::size_t msgs_before = ed.messages().size();
    (void)ed.check_asset_changes();
    for (std::size_t i = msgs_before; i < ed.messages().size(); ++i) CHECK(ed.messages()[i].find("Reloaded prefab") == std::string::npos);

    // Revert drops the override.
    REQUIRE(ed.revert_prefab(lamp2).ok());
    CHECK(s.world().get<Name>(s.children(lamp2)[0])->value == "Bulb");
    CHECK(s.world().get<PrefabInstance>(lamp2)->overrides.empty());
    ed.undo();
    CHECK(s.world().get<Name>(s.children(lamp2)[0])->value == "Red bulb");

    // Editing the prefab file outside the editor updates both instances (hot reload).
    Prefab outside = *saved;
    {
        json::Value root = outside.entities[0];
        json::Value comps = root["components"];
        json::Value mesh = comps["MeshRenderer"];
        mesh.set("primitive", "sphere");
        comps.set("MeshRenderer", mesh);
        root.set("components", comps);
        outside.entities.as_array()[0] = root;
    }
    REQUIRE(save_prefab_file(outside, h.dir / "Pfb" / "assets" / "prefabs" / "Lamp.vprefab").ok());
    sfs::last_write_time(h.dir / "Pfb" / "assets" / "prefabs" / "Lamp.vprefab",
                         sfs::file_time_type::clock::now() + std::chrono::seconds(3));
    CHECK(ed.check_asset_changes() == 1);
    CHECK(s.world().get<MeshRendererComponent>(lamp1)->primitive == "sphere");
    CHECK(s.world().get<MeshRendererComponent>(lamp2)->primitive == "sphere");
    const auto& msgs = ed.messages();
    CHECK(std::find(msgs.begin(), msgs.end(), "Reloaded prefab prefabs/Lamp.vprefab (2 instances updated)") != msgs.end());

    // Unpack: lamp2 becomes ordinary entities. Scene save/reopen keeps lamp1's link.
    ed.unpack_prefab(lamp2);
    CHECK_FALSE(s.world().has<PrefabInstance>(lamp2));
    CHECK_FALSE(s.world().has<PrefabPart>(s.children(lamp2)[0]));
    REQUIRE(ed.save_scene().ok());
    h.frames(2);
    // Play mode: scripts can spawn the prefab too (scene.instantiate through the asset index).
    ed.play();
    h.frames(2);
    ed.stop();
    h.frames(1);
}

namespace {
// A second machine joining the editor's session: the project's startup scene with its scripts and
// physics, driven by ManualInput, over real UDP.
struct JoiningPlayer {
    Scene scene;
    std::unique_ptr<script::ScriptHost> host;
    std::unique_ptr<physics::PhysicsWorld> physics_world;
    gameplay::ManualInput input;
    gameplay::Simulation sim;
    std::unique_ptr<net::NetClient> client;

    void start(const project::Project& p, const std::string& scene_path, std::uint16_t port, const std::string& server_key) {
        REQUIRE(scene.load_file(p.root() / scene_path).ok());
        script::HostOptions so;
        so.assets_root = p.assets_dir();
        so.input_map = p.settings().input;
        so.save_directory = p.dev_save_directory();
        host = std::move(script::ScriptHost::create(so)).value();
        physics_world = std::move(physics::PhysicsWorld::create()).value();
        gameplay::add_builtin_systems(sim);
        gameui::add_ui_systems(sim);
        script::add_script_systems(sim, *host);
        physics::add_physics_systems(sim, *physics_world);
        sim.input = &input;
        auto t = net::create_udp_transport({});
        REQUIRE(t.ok());
        net::SecureOptions sec;
        auto key = net::key_from_hex(server_key);
        REQUIRE(key.ok());
        sec.expected_server_key = *key; // pinned: the key the editor shows
        net::ClientOptions co;
        co.host = "127.0.0.1";
        co.port = port;
        co.name = "guest";
        co.game_id = net::make_game_id(p.settings().name, scene_path);
        client = std::make_unique<net::NetClient>(net::with_encryption(std::move(*t), std::move(sec)), co);
        client->install(sim);
    }
    void tick() {
        sim.tick(scene, 1.0f / 60.0f);
        input.end_frame();
    }
};
} // namespace

TEST_CASE("editor: play as host, a player joins over UDP and the Network window inspects replication") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Arena", "Arena", "multiplayer").ok());
    h.frames(1);

    // Settings are validated and only change on success.
    editor::NetPlaySettings bad = h.ed->net_settings();
    bad.conditions = "latency=abc";
    CHECK_FALSE(h.ed->set_net_settings(bad).ok());
    bad = h.ed->net_settings();
    bad.insecure = true;
    bad.password = "secret";
    CHECK_FALSE(h.ed->set_net_settings(bad).ok());
    CHECK(h.ed->net_settings() == editor::NetPlaySettings{});

    // Offline Play has no server; the Network window says so.
    h.ed->set_show_network(true);
    h.ed->play();
    h.frames(3);
    CHECK(h.ed->play_server() == nullptr);
    h.ed->stop();
    h.frames(1);

    // Host on a random high port; the editor's character exists before anyone joins.
    const auto port = static_cast<std::uint16_t>(41000 + Random(static_cast<std::uint64_t>(
                                                                    std::chrono::steady_clock::now().time_since_epoch().count()))
                                                             .next_u32() % 9000);
    editor::NetPlaySettings ns;
    ns.port = port;
    ns.player_name = "Editor Host";
    REQUIRE(h.ed->set_net_settings(ns).ok());
    h.ed->play_host();
    REQUIRE(h.ed->play_state() == editor::PlayState::Playing);
    REQUIRE(h.ed->play_server() != nullptr);
    CHECK(h.ed->server_key().size() == 64);
    CHECK(h.ed->join_command().find(std::to_string(port)) != std::string::npos);
    CHECK(h.ed->join_command().find(h.ed->server_key()) != std::string::npos);
    h.frames(2);
    const net::NetServer& srv = *h.ed->play_server();
    const Entity host_player = srv.local_player();
    REQUIRE_FALSE(host_player.is_null());
    REQUIRE(srv.clients().size() == 1);
    CHECK(srv.clients()[0].name == "Editor Host");
    CHECK(srv.client_diagnostics().empty());

    // A second machine joins with the pinned key shown in the editor.
    JoiningPlayer guest;
    guest.start(*h.ed->project(), h.ed->scene_path(), port, h.ed->server_key());
    for (int i = 0; i < 400 && !guest.client->connected(); ++i) {
        guest.tick();
        h.frames(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(guest.client->connected());
    CHECK(guest.client->diagnostics().encrypted);
    // The guest walks right for a second while both machines keep ticking.
    guest.input.set_key("D", true);
    for (int i = 0; i < 90; ++i) {
        guest.tick();
        h.frames(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    guest.input.set_key("D", false);
    for (int i = 0; i < 20; ++i) {
        guest.tick();
        h.frames(1);
    }

    const auto clients = srv.client_diagnostics();
    REQUIRE(clients.size() == 1);
    CHECK(clients[0].name == "guest");
    CHECK(clients[0].encrypted);
    CHECK(clients[0].snapshot_ack > 0);
    const Entity guest_on_host = clients[0].player;
    REQUIRE(h.ed->active_scene().world().alive(guest_on_host));
    CHECK(h.ed->active_scene().world().get<Transform>(guest_on_host)->position.x > 1.5f); // walked right on the host

    // Replication rows name both players with their owners and count traffic.
    bool saw_host = false, saw_guest = false;
    std::uint64_t updates = 0;
    for (const auto& r : srv.replication()) {
        updates += r.updates;
        if (r.entity == host_player) {
            saw_host = true;
            CHECK(r.owner == net::kServerId);
            CHECK(r.local);
        }
        if (r.entity == guest_on_host) {
            saw_guest = true;
            CHECK(r.owner == clients[0].id);
            CHECK(r.player);
            CHECK(r.updates > 0);
            CHECK(r.relevant_clients == 1);
        }
    }
    CHECK(saw_host);
    CHECK(saw_guest);
    CHECK(updates > 0);

    // The Network window and the inspector's live Network section draw for the guest's character.
    h.ed->select(guest_on_host);
    h.frames(3);
    CHECK(h.ed->is_selected(guest_on_host));

    // Captures stay offline: while in Host mode, Record does not host.
    h.ed->stop();
    h.frames(1);
    CHECK(h.ed->play_server() == nullptr);
    for (int i = 0; i < 60 && guest.client->connected(); ++i) {
        guest.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK_FALSE(guest.client->connected());
    CHECK(guest.client->diagnostics().status.find("shutting down") != std::string::npos);
    h.ed->record();
    h.frames(2);
    CHECK(h.ed->recording());
    CHECK(h.ed->play_server() == nullptr);
    h.ed->stop();
    h.frames(1);

    // A port already in use: Play as Host reports the error and stays in edit mode.
    auto blocker = net::create_udp_transport({.port = port});
    REQUIRE(blocker.ok());
    h.ed->play_host();
    CHECK(h.ed->play_state() == editor::PlayState::Editing);
    CHECK(h.ed->play_server() == nullptr);
    CHECK(h.ed->messages().back().find("cannot host") != std::string::npos);
}

TEST_CASE("editor: in-editor clients join the hosted session and the view switches which player is driven") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Arena", "Arena", "multiplayer").ok());
    h.frames(1);

    editor::NetPlaySettings ns;
    ns.editor_clients = 4;
    CHECK_FALSE(h.ed->set_net_settings(ns).ok()); // 0-3
    ns.editor_clients = 3;
    ns.max_clients = 2;
    CHECK_FALSE(h.ed->set_net_settings(ns).ok()); // more than the server admits
    ns.max_clients = 8;
    ns.editor_clients = 2;
    ns.password = "letmein"; // in-editor clients use the session's key and password
    ns.port = static_cast<std::uint16_t>(
        41000 + Random(static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^ 0x9e37u).next_u32() % 9000);
    REQUIRE(h.ed->set_net_settings(ns).ok());

    gameplay::ManualInput keys;
    h.ed->set_game_input_override(&keys);
    h.ed->set_show_network(true);
    h.ed->play_host();
    REQUIRE(h.ed->play_server() != nullptr);
    REQUIRE(h.ed->editor_client_count() == 2);
    CHECK(h.ed->editor_client(0) == nullptr);
    CHECK(h.ed->editor_client(3) == nullptr);
    for (int i = 0; i < 400; ++i) {
        if (h.ed->editor_client(1)->connected() && h.ed->editor_client(2)->connected()) break;
        h.frames(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(h.ed->editor_client(1)->connected());
    REQUIRE(h.ed->editor_client(2)->connected());
    CHECK(h.ed->editor_client(1)->diagnostics().encrypted);
    const net::NetServer& srv = *h.ed->play_server();
    CHECK(srv.clients().size() == 3); // host + two in-editor players
    CHECK(h.ed->peer_label(0) == "Host (host)");
    CHECK(h.ed->peer_label(1) == "Client 1 (player 2)");
    CHECK(h.ed->peer_label(3).empty());
    h.frames(30); // everyone's spawns arrive everywhere

    // Server-side characters of each peer, by name.
    auto server_player = [&](const std::string& name) {
        for (const auto& c : srv.clients())
            if (c.name == name) return c.player;
        return Entity{};
    };
    const Entity host_p = srv.local_player(), p2 = server_player("player 2"), p3 = server_player("player 3");
    REQUIRE_FALSE(host_p.is_null());
    REQUIRE_FALSE(p2.is_null());
    REQUIRE_FALSE(p3.is_null());
    Scene& host_scene = *h.ed->play_scene();
    auto x_of = [&](Entity e) { return host_scene.world().get<Transform>(e)->position.x; };

    // View client 1: the held key moves only its character (through its own prediction and the
    // server), the host's and client 2's stay put.
    h.ed->set_view_peer(1);
    CHECK(h.ed->view_peer() == 1);
    CHECK(&h.ed->active_scene() == h.ed->editor_client_scene(1));
    const float h0 = x_of(host_p), a0 = x_of(p2), b0 = x_of(p3);
    keys.set_key("D", true);
    for (int i = 0; i < 60; ++i) {
        h.frames(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    keys.set_key("D", false);
    h.frames(20);
    CHECK(x_of(p2) > a0 + 1.0f);
    CHECK(std::abs(x_of(host_p) - h0) < 0.05f);
    CHECK(std::abs(x_of(p3) - b0) < 0.05f);
    // Client 1's own world shows its predicted player moved too; client 2 sees it remotely.
    const Entity local1 = h.ed->editor_client(1)->local_player();
    REQUIRE_FALSE(local1.is_null());
    CHECK(h.ed->editor_client_scene(1)->world().get<Transform>(local1)->position.x > a0 + 1.0f);
    bool seen_by_2 = false;
    for (const auto& c : h.ed->editor_client(2)->clients())
        if (c.name == "player 2" && !c.player.is_null())
            seen_by_2 = h.ed->editor_client_scene(2)->world().get<Transform>(c.player)->position.x > a0 + 0.8f;
    CHECK(seen_by_2);
    // The inspector's network section draws for the client's own player in its world.
    h.ed->select(local1);
    h.frames(2);
    CHECK(h.ed->is_selected(local1));

    // Back to the host: now the host's character moves.
    h.ed->set_view_peer(7); // out of range: ignored
    CHECK(h.ed->view_peer() == 1);
    h.ed->set_view_peer(0);
    CHECK(&h.ed->active_scene() == h.ed->play_scene());
    const float a1 = x_of(p2);
    keys.set_key("D", true);
    h.frames(60);
    keys.set_key("D", false);
    h.frames(10);
    CHECK(x_of(host_p) > h0 + 1.0f);
    CHECK(std::abs(x_of(p2) - a1) < 0.3f);

    // Stop ends every peer and returns the view to the edited scene.
    h.ed->stop();
    h.frames(1);
    CHECK(h.ed->editor_client_count() == 0);
    CHECK(h.ed->view_peer() == 0);
    CHECK(&h.ed->active_scene() == &h.ed->edit_scene());
    h.ed->set_game_input_override(nullptr);
}

TEST_CASE("editor: materials are created from a selection, edited, assigned with undo and hot-reloaded") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Mat", "Mat", "3d").ok());
    h.frames(2);
    auto& ed = *h.ed;
    Scene& s = ed.edit_scene();
    const auto assets_dir = h.dir / "Mat" / "assets";
    auto png = render::encode_png(render::make_checker(16, 2, 0xff0000ffu, 0xff00ff00u));
    REQUIRE(png.ok());
    REQUIRE(fs::write_atomic(assets_dir / "bricks.png", *png).ok());
    (void)ed.check_asset_changes();
    REQUIRE(ed.assets()->find_path("bricks.png"));
    const Uuid tex = ed.assets()->find_path("bricks.png")->uuid; // records move on refresh: keep the id

    const Entity box = ed.create_entity(editor::Primitive::Cube);
    ed.rename(box, "Crate");
    s.world().get<MeshRendererComponent>(box)->base_color = {0.2f, 0.4f, 0.6f, 1};
    ed.select(box);

    // New Material starts from the entity's look and takes over its colour.
    ed.new_material_for_selection();
    const Uuid mat = s.world().get<MeshRendererComponent>(box)->material_asset;
    REQUIRE_FALSE(mat.is_nil());
    const auto* rec = ed.assets()->find(mat);
    REQUIRE(rec);
    CHECK(rec->path == "materials/Crate.vmat");
    CHECK(rec->kind == assets::AssetKind::Material);
    CHECK(ed.material_editor_open());
    CHECK(ed.editing_material() == mat);
    auto loaded = ed.assets()->load_material(mat);
    REQUIRE(loaded);
    CHECK(loaded->base_color.b == doctest::Approx(0.6f));
    CHECK(s.world().get<MeshRendererComponent>(box)->base_color == math::Color{1, 1, 1, 1});
    h.frames(3); // Material Editor window drawn

    // Edit + save: texture and tiling persist; our own save is not reported as an outside change.
    assets::MaterialAsset edited = *loaded;
    edited.base_color_texture = tex;
    edited.uv_scale = {3, 3};
    REQUIRE(ed.save_material(mat, edited).ok());
    sfs::last_write_time(assets_dir / "materials" / "Crate.vmat", sfs::file_time_type::clock::now() + std::chrono::seconds(2));
    // (touching the mtime alone keeps our hash, so it still counts as our save)
    const std::size_t before = ed.messages().size();
    (void)ed.check_asset_changes();
    for (std::size_t i = before; i < ed.messages().size(); ++i) CHECK(ed.messages()[i].find("Crate.vmat") == std::string::npos);
    auto reread = assets::load_material_file(assets_dir / "materials" / "Crate.vmat");
    REQUIRE(reread);
    CHECK(reread->base_color_texture == tex);
    CHECK(reread->uv_scale == math::Vec2{3, 3});
    h.frames(2);

    // An outside edit is reported and picked up by the open editor.
    edited.roughness = 0.95f;
    REQUIRE(assets::save_material_file(assets_dir / "materials" / "Crate.vmat", edited).ok());
    sfs::last_write_time(assets_dir / "materials" / "Crate.vmat", sfs::file_time_type::clock::now() + std::chrono::seconds(4));
    CHECK(ed.check_asset_changes() >= 1);
    const auto& msgs = ed.messages();
    CHECK(std::find(msgs.begin(), msgs.end(), "Reloaded material materials/Crate.vmat") != msgs.end());
    h.frames(2);

    // Surface shaders: the template is created, assigned to the open material and compiled; a
    // broken edit is reported with its line.
    auto shader = ed.create_surface_shader();
    REQUIRE(shader.ok());
    const Uuid sh = *shader;
    CHECK(ed.assets()->find(sh)->path == "shaders/Surface.vshader");
    CHECK(ed.set_editing_material_shader(sh));
    CHECK_FALSE(ed.set_editing_material_shader(tex));
    auto with_shader = assets::load_material_file(assets_dir / "materials" / "Crate.vmat");
    REQUIRE(with_shader);
    CHECK(with_shader->shader == sh);
    h.frames(2); // Shader section drawn with the compile status
    {
        const std::string bad = "void surface(SurfaceInput i, inout Surface s) {\n  s.oops = 1.0;\n}\n";
        const auto file = assets_dir / "shaders" / "Surface.vshader";
        REQUIRE(fs::write_atomic(file, std::span(reinterpret_cast<const std::uint8_t*>(bad.data()), bad.size())).ok());
        sfs::last_write_time(file, sfs::file_time_type::clock::now() + std::chrono::seconds(6));
        CHECK(ed.check_asset_changes() >= 1);
        const auto& m2 = ed.messages();
        CHECK(std::any_of(m2.begin(), m2.end(), [](const std::string& m) {
            return m.find("Shader shaders/Surface.vshader") != std::string::npos && m.find("Surface.vshader:2") != std::string::npos;
        }));
        h.frames(2);
    }
    CHECK(ed.set_editing_material_shader({}));

    // Material instances: created next to the parent, inherit everything, save only overrides.
    auto inst = ed.create_material_instance(mat);
    REQUIRE(inst.ok());
    const Uuid inst_id = *inst;
    CHECK(ed.assets()->find(inst_id)->path == "materials/Crate_instance.vmat");
    ed.open_material_editor(inst_id);
    CHECK(ed.editing_material() == inst_id);
    h.frames(2); // instance header and inherited markers drawn
    auto inst_file = assets::load_material_file(assets_dir / "materials" / "Crate_instance.vmat");
    REQUIRE(inst_file);
    CHECK(inst_file->parent == mat);
    CHECK(inst_file->overrides == 0);
    auto inst_resolved = ed.assets()->resolve_material(inst_id);
    REQUIRE(inst_resolved);
    CHECK(inst_resolved->roughness == doctest::Approx(0.95f)); // the parent's value
    CHECK(inst_resolved->base_color_texture == tex);
    CHECK_FALSE(ed.create_material_instance(tex).ok());
    ed.open_material_editor(mat);

    // Assignment is undoable; non-material assets are rejected.
    CHECK(ed.assign_material(box, {}));
    CHECK(s.world().get<MeshRendererComponent>(box)->material_asset.is_nil());
    ed.undo();
    CHECK(s.world().get<MeshRendererComponent>(s.find_by_name("Crate"))->material_asset == mat);
    CHECK_FALSE(ed.assign_material(s.find_by_name("Crate"), tex));

    // Sprite sheets: sliced from a texture, assigned (undoably) and previewed while editing.
    {
        auto made = ed.create_sprite_sheet(tex, 2, 2, 8.0f);
        REQUIRE(made.ok());
        const Uuid sheet_id = *made;
        CHECK(ed.assets()->find(sheet_id)->path == "bricks.vsheet");
        CHECK(ed.assets()->find(sheet_id)->kind == assets::AssetKind::SpriteSheet);
        auto again = ed.create_sprite_sheet(tex, 1, 1);
        REQUIRE(again.ok());
        CHECK(ed.assets()->find(*again)->path == "bricks_2.vsheet");
        CHECK_FALSE(ed.create_sprite_sheet(mat, 2, 2).ok());
        CHECK_FALSE(ed.create_sprite_sheet(tex, 0, 2).ok());
        auto data = ed.assets()->load_sprite_sheet(sheet_id);
        REQUIRE(data.ok());
        CHECK(data->texture == tex);
        REQUIRE(data->find("default"));
        CHECK(data->find("default")->frames.size() == 4);
        const Entity coin = s.create("Coin");
        CHECK(ed.assign_sprite_sheet(coin, sheet_id));
        REQUIRE(s.world().has<animation::SpriteAnimatorComponent>(coin));
        CHECK(s.world().get<animation::SpriteAnimatorComponent>(coin)->clip == "default");
        CHECK(s.world().get<SpriteComponent>(coin)->texture_asset == tex);
        s.world().get<animation::SpriteAnimatorComponent>(coin)->clip.clear();
        s.world().get<animation::SpriteAnimatorComponent>(coin)->frame = 3;
        h.frames(2); // edit-mode preview shows frame 3 (bottom right)
        CHECK(s.world().get<SpriteComponent>(coin)->uv.x == doctest::Approx(0.5f));
        CHECK(s.world().get<SpriteComponent>(coin)->uv.y == doctest::Approx(0.5f));
        CHECK_FALSE(ed.assign_sprite_sheet(coin, tex));
        ed.undo();
        CHECK_FALSE(s.world().has<animation::SpriteAnimatorComponent>(s.find_by_name("Coin")));
        s.destroy(s.find_by_name("Coin"));
        // Pixel-art import settings: written to the .meta and re-imported.
        auto ts = assets::TextureImportSettings::from_json(ed.assets()->find(tex)->settings);
        ts.nearest = true;
        REQUIRE(ed.set_texture_import_settings(tex, ts).ok());
        CHECK(ed.assets()->find(tex)->settings["filter"].as_string() == "nearest");
        const auto meta = fs::read_text(assets_dir / "bricks.png.meta");
        REQUIRE(meta.ok());
        CHECK(meta->find("\"nearest\"") != std::string::npos);
        CHECK_FALSE(ed.set_texture_import_settings(mat, ts).ok());
        h.frames(2);
    }

    // The material reference survives a scene save/reopen.
    REQUIRE(ed.save_scene().ok());
    const std::string scene_file = ed.scene_path();
    REQUIRE(ed.open_scene(scene_file).ok());
    CHECK(ed.edit_scene().world().get<MeshRendererComponent>(ed.edit_scene().find_by_name("Crate"))->material_asset == mat);
    h.frames(2);
}

TEST_CASE("editor: tile sets are created from textures and painted in the viewport with undo per stroke") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Tiles", "Tiles", "2d").ok());
    h.frames(2);
    auto& ed = *h.ed;
    const auto assets_dir = h.dir / "Tiles" / "assets";
    auto png = render::encode_png(render::make_checker(32, 8, 0xff3080c0u, 0xff60c040u));
    REQUIRE(png.ok());
    REQUIRE(fs::write_atomic(assets_dir / "ground.png", *png).ok());
    (void)ed.check_asset_changes();
    REQUIRE(ed.assets()->find_path("ground.png"));
    const Uuid tex = ed.assets()->find_path("ground.png")->uuid;

    // Creating: default path next to the texture, unique names, validation.
    auto made = ed.create_tileset(tex, 4, 4);
    REQUIRE(made.ok());
    const Uuid ts_id = *made;
    CHECK(ed.assets()->find(ts_id)->path == "ground.vtileset");
    CHECK(ed.assets()->find(ts_id)->kind == assets::AssetKind::Tileset);
    auto again = ed.create_tileset(tex, 2, 2, tilemap::TileCollision::OneWay);
    REQUIRE(again.ok());
    CHECK(ed.assets()->find(*again)->path == "ground_2.vtileset");
    CHECK_FALSE(ed.create_tileset(tex, 0, 4).ok());
    CHECK_FALSE(ed.create_tileset(ts_id, 2, 2).ok());
    CHECK_FALSE(ed.create_tileset(tex, 1, 1, tilemap::TileCollision::Solid, "ground.vtileset").ok());
    auto file = tilemap::load_tileset_file(assets_dir / "ground.vtileset");
    REQUIRE(file);
    CHECK(file->texture == tex);
    CHECK(file->tile_count() == 16);
    CHECK(file->default_collision == tilemap::TileCollision::Solid);

    // Assigning: adds a Tilemap (undoable) and turns the tile tool on.
    Scene& s = ed.edit_scene();
    const Entity map = ed.create_entity(editor::Primitive::Empty);
    ed.rename(map, "TileLayer");
    ed.select(map);
    CHECK_FALSE(ed.assign_tileset(map, tex));
    REQUIRE(ed.assign_tileset(map, ts_id));
    CHECK(ed.tile_tool());
    REQUIRE(s.world().get<tilemap::TilemapComponent>(map));
    CHECK(s.world().get<tilemap::TilemapComponent>(map)->tileset == ts_id);
    ed.undo();
    CHECK_FALSE(s.world().has<tilemap::TilemapComponent>(s.find_by_name("TileLayer")));
    ed.redo();
    const Entity g = s.find_by_name("TileLayer");
    REQUIRE(s.world().has<tilemap::TilemapComponent>(g));
    ed.select(g);
    h.frames(2); // palette, grid overlay and tilemap rendering

    // Picking a cell from a viewport pixel.
    const math::Vec2 vp{float(Harness::VW), float(Harness::VH)};
    const auto cam = ed.camera().camera2d(float(Harness::VH));
    auto cell_at = [&](float wx, float wy) { return ed.tile_under(g, cam.world_to_screen({wx, wy}, vp.x, vp.y), vp); };
    auto c0 = cell_at(0.5f, 0.5f);
    REQUIRE(c0);
    CHECK((*c0)[0] == 0);
    CHECK((*c0)[1] == 0);
    auto cm = cell_at(-2.5f, 3.5f);
    REQUIRE(cm);
    CHECK((*cm)[0] == -3);
    CHECK((*cm)[1] == 3);

    auto tile = [&](std::int32_t x, std::int32_t y) {
        return s.world().get<tilemap::TilemapComponent>(s.find_by_name("TileLayer"))->tile(x, y);
    };
    // A dragged stroke fills the gaps between cells and is one undo entry.
    ed.tile_brush().mode = editor::Editor::TileMode::Paint;
    ed.tile_brush().tile = 5;
    REQUIRE(ed.begin_tile_stroke(g, 0, 0));
    CHECK(ed.tile_stroke_active());
    ed.continue_tile_stroke(4, 0);
    ed.continue_tile_stroke(4, 2);
    ed.end_tile_stroke();
    for (int x = 0; x <= 4; ++x) CHECK(tile(x, 0) == 5);
    CHECK(tile(4, 1) == 5);
    CHECK(tile(4, 2) == 5);
    CHECK(tile(3, 1) == -1);
    h.frames(2); // collision rebuilt by the edit-time tilemap system
    {
        const auto* shapes = s.world().get<physics::StaticShapes2DComponent>(s.find_by_name("TileLayer"));
        REQUIRE(shapes);
        CHECK_FALSE((shapes->boxes.empty() && shapes->loops.empty()));
    }
    ed.undo();
    CHECK(tile(0, 0) == -1);
    CHECK(tile(4, 2) == -1);
    ed.redo();
    CHECK(tile(2, 0) == 5);

    auto undo_label = [&] {
        const auto* c = ed.undo_stack().next_undo();
        return c ? c->label : std::string();
    };
    // A stroke that changes nothing records nothing.
    const auto undo_before = undo_label();
    REQUIRE(ed.begin_tile_stroke(s.find_by_name("TileLayer"), 1, 0));
    ed.end_tile_stroke();
    CHECK(undo_label() == undo_before);

    // Rectangle fill, flips, erase and the eyedropper.
    ed.tile_brush().mode = editor::Editor::TileMode::Rectangle;
    ed.tile_brush().tile = 2;
    REQUIRE(ed.begin_tile_stroke(s.find_by_name("TileLayer"), -2, -2));
    ed.continue_tile_stroke(0, -1);
    CHECK(tile(-1, -2) == -1); // filled on release
    ed.end_tile_stroke();
    for (int y = -2; y <= -1; ++y)
        for (int x = -2; x <= 0; ++x) CHECK(tile(x, y) == 2);
    ed.tile_brush().mode = editor::Editor::TileMode::Paint;
    ed.tile_brush().tile = 7;
    ed.tile_brush().flip_x = true;
    REQUIRE(ed.begin_tile_stroke(s.find_by_name("TileLayer"), 6, 6));
    ed.end_tile_stroke();
    ed.tile_brush() = {};
    ed.tile_brush().mode = editor::Editor::TileMode::Pick;
    REQUIRE(ed.begin_tile_stroke(s.find_by_name("TileLayer"), 6, 6));
    CHECK_FALSE(ed.tile_stroke_active());
    CHECK(ed.tile_brush().tile == 7);
    CHECK(ed.tile_brush().flip_x);
    CHECK(ed.tile_brush().mode == editor::Editor::TileMode::Paint);
    ed.tile_brush().mode = editor::Editor::TileMode::Erase;
    REQUIRE(ed.begin_tile_stroke(s.find_by_name("TileLayer"), -2, -2));
    ed.continue_tile_stroke(0, -2);
    ed.end_tile_stroke();
    CHECK(tile(-1, -2) == -1);
    CHECK(tile(-1, -1) == 2);
    CHECK(undo_label() == "Erase tiles");
    ed.undo();
    CHECK(tile(-1, -2) == 2);

    // Terrain painting picks tiles from the tile set's neighbour rule (hot-reloaded file).
    {
        auto ts = tilemap::load_tileset_file(assets_dir / "ground.vtileset");
        REQUIRE(ts);
        tilemap::Terrain t;
        t.name = "grass";
        for (int i = 0; i < 16; ++i) t.tiles[static_cast<std::size_t>(i)] = i;
        ts->terrains.push_back(t);
        REQUIRE(tilemap::save_tileset_file(assets_dir / "ground.vtileset", *ts).ok());
        (void)ed.check_asset_changes();
    }
    ed.tile_brush().mode = editor::Editor::TileMode::Terrain;
    ed.tile_brush().terrain = 0;
    REQUIRE(ed.begin_tile_stroke(s.find_by_name("TileLayer"), 10, 10));
    ed.continue_tile_stroke(12, 10);
    ed.end_tile_stroke();
    CHECK(tile(10, 10) == 2);  // east neighbour only
    CHECK(tile(11, 10) == 10); // east + west
    CHECK(tile(12, 10) == 8);  // west only
    ed.tile_brush().mode = editor::Editor::TileMode::Erase;
    REQUIRE(ed.begin_tile_stroke(s.find_by_name("TileLayer"), 12, 10));
    ed.end_tile_stroke();
    CHECK(tile(12, 10) == -1);
    CHECK(tile(11, 10) == 8); // re-picked after the neighbour was erased
    h.frames(2);

    // Saved with the scene and restored on reopen.
    REQUIRE(ed.save_scene().ok());
    const std::string scene_file = ed.scene_path();
    REQUIRE(ed.open_scene(scene_file).ok());
    Scene& s2 = ed.edit_scene();
    const auto* tm2 = s2.world().get<tilemap::TilemapComponent>(s2.find_by_name("TileLayer"));
    REQUIRE(tm2);
    CHECK(tm2->tile(2, 0) == 5);
    CHECK(tm2->cell(6, 6)->flip_x);
    CHECK(tm2->tile(11, 10) == 8);
    CHECK(tm2->tileset == ts_id);
}

namespace {
// Viewport pixel of a world point as the editor's gizmo sees it.
math::Vec2 gizmo_px(editor::Editor& ed, math::Vec3 p) {
    const auto s = ed.gizmo_view({float(Harness::VW), float(Harness::VH)}).project(p);
    REQUIRE(s.has_value());
    return *s;
}
float gizmo_length(editor::Editor& ed, math::Vec3 p) {
    return ed.gizmo_view({float(Harness::VW), float(Harness::VH)}).world_per_pixel(p) * editor::gizmo::Options{}.size_pixels;
}
} // namespace

TEST_CASE("editor: transform gizmo moves, rotates and scales the selection with undo, snapping and cancel") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Gizmo", "Gizmo", "3d").ok());
    editor::Editor& ed = *h.ed;
    const math::Vec2 vs{float(Harness::VW), float(Harness::VH)};
    Scene& s = ed.edit_scene();
    const Entity cube = ed.create_entity(editor::Primitive::Cube);
    const Uuid cube_id = s.uuid(cube);
    s.world().get<Transform>(cube)->position = {0, 0.5f, 0};
    s.update_transforms();
    ed.camera().target = {0, 0.5f, 0};
    ed.camera().distance = 8.0f;
    ed.select(cube);
    h.frames(2); // the gizmo draws over the viewport
    CHECK(ed.gizmo_settings().mode == editor::gizmo::Mode::Translate);
    REQUIRE(ed.gizmo_frame().has_value());
    CHECK(ed.gizmo_frame()->pivot == math::Vec3{0, 0.5f, 0});

    // Nothing under the pointer: no drag (the click selects instead).
    CHECK_FALSE(ed.gizmo_press({2, 2}, vs));

    // Move along X: one undo entry labelled "Move".
    const std::size_t depth = ed.undo_stack().size();
    float L = gizmo_length(ed, {0, 0.5f, 0});
    math::Vec3 grab{L * 0.7f, 0.5f, 0};
    CHECK(ed.gizmo_hover(gizmo_px(ed, grab), vs) == editor::gizmo::Handle::X);
    REQUIRE(ed.gizmo_press(gizmo_px(ed, grab), vs));
    CHECK(ed.gizmo_dragging());
    ed.gizmo_drag(gizmo_px(ed, grab + math::Vec3{1, 0, 0}), vs);
    ed.gizmo_drag(gizmo_px(ed, grab + math::Vec3{2, 0, 0}), vs);
    ed.gizmo_release();
    CHECK_FALSE(ed.gizmo_dragging());
    const Transform* t = s.world().get<Transform>(s.find(cube_id));
    CHECK(t->position.x == doctest::Approx(2.0f).epsilon(0.02));
    CHECK(t->position.y == doctest::Approx(0.5f));
    CHECK(t->position.z == doctest::Approx(0.0f).scale(1));
    REQUIRE(ed.undo_stack().size() == depth + 1);
    ed.undo();
    CHECK(s.world().get<Transform>(s.find(cube_id))->position == math::Vec3{0, 0.5f, 0});
    ed.redo();
    CHECK(s.world().get<Transform>(s.find(cube_id))->position.x == doctest::Approx(2.0f).epsilon(0.02));

    // Snapping: a 1.3 drag lands on 1.5 with 0.5 steps; Ctrl (invert) turns it off again.
    s.world().get<Transform>(s.find(cube_id))->position = {0, 0.5f, 0};
    s.update_transforms();
    ed.gizmo_settings().snap = true;
    ed.gizmo_settings().steps.translate = 0.5f;
    REQUIRE(ed.gizmo_press(gizmo_px(ed, grab), vs));
    ed.gizmo_drag(gizmo_px(ed, grab + math::Vec3{1.3f, 0, 0}), vs);
    CHECK(s.world().get<Transform>(s.find(cube_id))->position.x == doctest::Approx(1.5f));
    ed.gizmo_drag(gizmo_px(ed, grab + math::Vec3{1.3f, 0, 0}), vs, true);
    CHECK(s.world().get<Transform>(s.find(cube_id))->position.x == doctest::Approx(1.3f).epsilon(0.02));
    // Cancel (Escape / right click) restores the transform and records nothing.
    const std::size_t before_cancel = ed.undo_stack().size();
    ed.gizmo_cancel();
    CHECK(s.world().get<Transform>(s.find(cube_id))->position == math::Vec3{0, 0.5f, 0});
    CHECK(ed.undo_stack().size() == before_cancel);
    // A click on a handle without motion records nothing either.
    REQUIRE(ed.gizmo_press(gizmo_px(ed, grab), vs));
    ed.gizmo_release();
    CHECK(ed.undo_stack().size() == before_cancel);
    ed.gizmo_settings().snap = false;

    // Rotate about Y by 90 degrees, snapped to 15 degree steps.
    ed.gizmo_settings().mode = editor::gizmo::Mode::Rotate;
    ed.gizmo_settings().snap = true;
    ed.camera().pitch = -1.2f; // look down so the Y ring is open
    L = gizmo_length(ed, {0, 0.5f, 0});
    // Grab between X and -Z (away from where the X and Z rings cross the Y ring).
    const float a0 = math::radians(45.0f);
    const math::Vec3 ring{L * std::cos(a0), 0.5f, -L * std::sin(a0)};
    REQUIRE(ed.gizmo_hover(gizmo_px(ed, ring), vs) == editor::gizmo::Handle::Y);
    REQUIRE(ed.gizmo_press(gizmo_px(ed, ring), vs));
    const float a = math::radians(45.0f + 88.0f);
    ed.gizmo_drag(gizmo_px(ed, {L * std::cos(a), 0.5f, -L * std::sin(a)}), vs);
    h.frames(1); // draws the swept-angle pie while dragging
    ed.gizmo_release();
    const math::Vec3 fwd = s.world().get<Transform>(s.find(cube_id))->rotation.rotate({1, 0, 0});
    CHECK(fwd.x == doctest::Approx(0.0f).scale(1).epsilon(1e-3));
    CHECK(fwd.z == doctest::Approx(-1.0f).epsilon(1e-3));
    CHECK(ed.undo_stack().size() == before_cancel + 1);
    ed.gizmo_settings().snap = false;

    // Local space follows the rotated cube: its X handle now points along world -Z.
    ed.gizmo_settings().mode = editor::gizmo::Mode::Translate;
    ed.gizmo_settings().space = editor::gizmo::Space::Local;
    const math::Vec3 lx = ed.gizmo_frame()->axis(0);
    CHECK(lx.z == doctest::Approx(-1.0f).epsilon(1e-3));
    ed.gizmo_settings().space = editor::gizmo::Space::World;

    // Scale X (always local: the cube's own X axis) to 2x.
    ed.gizmo_settings().mode = editor::gizmo::Mode::Scale;
    ed.camera().pitch = -0.45f;
    L = gizmo_length(ed, {0, 0.5f, 0});
    const math::Vec3 sx = math::Vec3{0, 0.5f, 0} + lx * (L * 0.8f);
    REQUIRE(ed.gizmo_hover(gizmo_px(ed, sx), vs) == editor::gizmo::Handle::X);
    REQUIRE(ed.gizmo_press(gizmo_px(ed, sx), vs));
    ed.gizmo_drag(gizmo_px(ed, math::Vec3{0, 0.5f, 0} + lx * (L * 1.6f)), vs);
    ed.gizmo_release();
    const math::Vec3 sc = s.world().get<Transform>(s.find(cube_id))->scale;
    CHECK(sc.x == doctest::Approx(2.0f).epsilon(0.03));
    CHECK(sc.y == doctest::Approx(1.0f));
    ed.undo();
    CHECK(s.world().get<Transform>(s.find(cube_id))->scale == math::Vec3{1, 1, 1});

    // Hierarchies: with parent and child selected only the parent is moved (the child follows);
    // a child alone moves in world space even under a scaled, rotated parent.
    ed.gizmo_settings().mode = editor::gizmo::Mode::Translate;
    const Entity parent = ed.create_entity(editor::Primitive::Empty);
    const Uuid parent_id = s.uuid(parent);
    Transform* pt = s.world().get<Transform>(parent);
    pt->position = {0, 0.5f, 0};
    pt->rotation = math::Quat::axis_angle({0, 1, 0}, math::radians(30.0f));
    pt->scale = {2, 2, 2};
    REQUIRE(ed.reparent(s.find(cube_id), s.find(parent_id)));
    Transform* ct = s.world().get<Transform>(s.find(cube_id));
    ct->position = {0, 0, 0};
    ct->rotation = {};
    ct->scale = {1, 1, 1};
    s.update_transforms();
    ed.select(s.find(parent_id));
    ed.select(s.find(cube_id), true); // cube is the active entity
    REQUIRE(ed.gizmo_press(gizmo_px(ed, grab), vs));
    ed.gizmo_drag(gizmo_px(ed, grab + math::Vec3{1, 0, 0}), vs);
    ed.gizmo_release();
    CHECK(s.world().get<Transform>(s.find(parent_id))->position.x == doctest::Approx(1.0f).epsilon(0.02));
    CHECK(s.world().get<Transform>(s.find(cube_id))->position == math::Vec3{0, 0, 0});
    ed.select(s.find(cube_id));
    const math::Vec3 world0 = s.world_matrix(s.find(cube_id)).c[3].xyz();
    L = gizmo_length(ed, world0);
    const math::Vec3 g2 = world0 + math::Vec3{L * 0.7f, 0, 0};
    REQUIRE(ed.gizmo_press(gizmo_px(ed, g2), vs));
    ed.gizmo_drag(gizmo_px(ed, g2 + math::Vec3{1, 0, 0}), vs);
    ed.gizmo_release();
    const math::Vec3 world1 = s.world_matrix(s.find(cube_id)).c[3].xyz();
    CHECK(world1.x - world0.x == doctest::Approx(1.0f).epsilon(0.02));
    CHECK(world1.z == doctest::Approx(world0.z).epsilon(1e-3).scale(1));
    CHECK(s.world().get<Transform>(s.find(cube_id))->scale == math::Vec3{1, 1, 1});

    // No gizmo while playing.
    ed.play();
    CHECK_FALSE(ed.gizmo_frame().has_value());
    CHECK_FALSE(ed.gizmo_press(gizmo_px(ed, g2), vs));
    ed.stop();
    h.frames(1);
}

TEST_CASE("editor: transform gizmo works in 2D scenes on the XY plane") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Gizmo2D", "Gizmo2D", "2d").ok());
    editor::Editor& ed = *h.ed;
    const math::Vec2 vs{float(Harness::VW), float(Harness::VH)};
    Scene& s = ed.edit_scene();
    const Entity sprite = ed.create_entity(editor::Primitive::Sprite);
    // Every primitive has its own name (the table once missed "Spot Light" and read past its end).
    CHECK(s.world().get<Name>(sprite)->value == "Sprite");
    const Entity spot = ed.create_entity(editor::Primitive::SpotLight);
    CHECK(s.world().get<Name>(spot)->value == "Spot Light");
    s.destroy(spot);
    const Uuid id = s.uuid(sprite);
    s.world().get<Transform>(sprite)->position = {0, 0, 0};
    s.update_transforms();
    ed.camera().cam2d.position = {0, 0};
    ed.select(sprite);
    h.frames(2);
    REQUIRE(ed.gizmo_view(vs).orthographic());
    const float L = gizmo_length(ed, {0, 0, 0});
    const math::Vec3 plane{L * 0.34f, L * 0.34f, 0};
    REQUIRE(ed.gizmo_hover(gizmo_px(ed, plane), vs) == editor::gizmo::Handle::XY);
    REQUIRE(ed.gizmo_press(gizmo_px(ed, plane), vs));
    ed.gizmo_drag(gizmo_px(ed, plane + math::Vec3{1.5f, -0.75f, 0}), vs);
    ed.gizmo_release();
    const Transform* t = s.world().get<Transform>(s.find(id));
    CHECK(t->position.x == doctest::Approx(1.5f).epsilon(0.01));
    CHECK(t->position.y == doctest::Approx(-0.75f).epsilon(0.01));
    CHECK(t->position.z == 0.0f);
    // Rotation in 2D is about Z only.
    ed.gizmo_settings().mode = editor::gizmo::Mode::Rotate;
    const math::Vec3 p0 = t->position;
    REQUIRE(ed.gizmo_hover(gizmo_px(ed, p0 + math::Vec3{L, 0, 0}), vs) == editor::gizmo::Handle::Z);
    REQUIRE(ed.gizmo_press(gizmo_px(ed, p0 + math::Vec3{L, 0, 0}), vs));
    ed.gizmo_drag(gizmo_px(ed, p0 + math::Vec3{0, L, 0}), vs);
    ed.gizmo_release();
    const math::Vec3 right = s.world().get<Transform>(s.find(id))->rotation.rotate({1, 0, 0});
    CHECK(right.y == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(right.z == doctest::Approx(0.0f).scale(1));
    h.frames(1);
    ed.undo();
    ed.undo();
    CHECK(s.world().get<Transform>(s.find(id))->position == math::Vec3{0, 0, 0});
}

TEST_CASE("editor: thumbnail camera frames the whole box") {
    for (const math::Aabb box : {math::Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}}, math::Aabb{{10, 0, -3}, {14, 9, 1}},
                                 math::Aabb{{-0.01f, -0.01f, -0.01f}, {0.01f, 0.01f, 0.01f}}}) {
        const render::Camera3D cam = editor::thumbnail_camera(box);
        const math::Mat4 vp = cam.projection(1.0f) * cam.view;
        for (int i = 0; i < 8; ++i) {
            const math::Vec3 p{i & 1 ? box.max.x : box.min.x, i & 2 ? box.max.y : box.min.y, i & 4 ? box.max.z : box.min.z};
            const math::Vec4 c = vp * math::Vec4{p.x, p.y, p.z, 1};
            REQUIRE(c.w > 0);
            CHECK(std::abs(c.x / c.w) <= 1.0f);
            CHECK(std::abs(c.y / c.w) <= 1.0f);
            CHECK(c.z / c.w >= 0.0f);
            CHECK(c.z / c.w <= 1.0f);
        }
        // Looking down at the box from above and in front.
        CHECK(cam.position.y > box.center().y);
        CHECK(cam.forward().y < 0.0f);
    }
}

TEST_CASE("editor: asset thumbnails render models, materials and prefabs and refresh on change") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Thumbs", "Thumbs", "3d").ok());
    editor::Editor& ed = *h.ed;
    REQUIRE(ed.thumbnails() != nullptr);
    auto& db = *ed.assets();
    const auto* robot = db.find_path("models/characters/victus_vanguard_01.glb");
    const auto* ground = db.find_path("materials/ground.vmat");
    const auto* energy = db.find_path("materials/energy_cube.vmat");
    const auto* crate = db.find_path("prefabs/crate.vprefab");
    const auto* grid = db.find_path("textures/ground_grid.png");
    const auto* script = db.find_path("scripts/player.luau");
    REQUIRE(robot);
    REQUIRE(ground);
    REQUIRE(energy);
    REQUIRE(crate);
    REQUIRE(grid);
    REQUIRE(script);
    const Uuid robot_id = robot->uuid, ground_id = ground->uuid, energy_id = energy->uuid, crate_id = crate->uuid;
    const Uuid grid_id = grid->uuid, script_id = script->uuid;

    // Textures come straight from the asset cache; scripts have no picture.
    auto tex = ed.asset_thumbnail(grid_id);
    REQUIRE(tex.has_value());
    CHECK(tex->texture.valid());
    CHECK_FALSE(tex->rendered);
    CHECK(tex->aspect == doctest::Approx(1.0f));
    CHECK_FALSE(ed.asset_thumbnail(script_id).has_value());

    // Rendered kinds are queued on first request and rendered within the per-frame budget.
    ed.set_thumbnail_budget(0);
    for (const Uuid& id : {robot_id, ground_id, energy_id, crate_id}) CHECK_FALSE(ed.asset_thumbnail(id).has_value());
    h.frames(2);
    CHECK(ed.thumbnails()->pending() >= 4);
    ed.set_thumbnail_budget(1);
    const auto before = ed.thumbnails()->rendered_total();
    h.frames(1);
    CHECK(ed.thumbnails()->rendered_total() == before + 1);
    ed.set_thumbnail_budget(8);
    h.frames(2);
    CHECK(ed.thumbnails()->pending() == 0);

    const auto read = [&](const Uuid& id) {
        auto t = ed.asset_thumbnail(id);
        REQUIRE(t.has_value());
        CHECK(t->rendered);
        auto px = gpu().ctx->device().read_texture(t->texture);
        REQUIRE(px.ok());
        return std::move(px).value();
    };
    const std::uint32_t n = ed.thumbnails()->size();
    const auto distinct = [&](const std::vector<std::uint8_t>& px) {
        // The subject in the middle differs from the backdrop in the corner.
        const auto corner = pixel(px, n, 2, 2);
        int count = 0;
        for (std::uint32_t y = n / 4; y < 3 * n / 4; y += 4)
            for (std::uint32_t x = n / 4; x < 3 * n / 4; x += 4)
                if (diff(pixel(px, n, x, y), corner) > 30) ++count;
        return count;
    };
    const auto robot_px = read(robot_id);
    const auto ground_px = read(ground_id);
    const auto energy_px = read(energy_id);
    const auto crate_px = read(crate_id);
    test::save_output("editor_thumbnail_model", n, n, robot_px);
    test::save_output("editor_thumbnail_material", n, n, energy_px);
    test::save_output("editor_thumbnail_prefab", n, n, crate_px);
    CHECK(distinct(robot_px) > 20);
    CHECK(distinct(ground_px) > 20);
    CHECK(distinct(energy_px) > 20);
    CHECK(distinct(crate_px) > 20);
    CHECK(ground_px != energy_px); // different materials, different spheres

    // Saving a material re-renders material previews with the new look.
    auto mat = db.load_material(energy_id);
    REQUIRE(mat.ok());
    assets::MaterialAsset red = *mat;
    // Plain rough red PBR: without the energy shader's orange glow (or a specular highlight at
    // the centre) the result does not depend on how a driver saturates bright colours.
    red.base_color = {1, 0, 0, 1};
    red.emissive = {0, 0, 0, 1};
    red.metallic = 0;
    red.roughness = 1;
    red.shader = Uuid{};
    red.shader_params = {};
    REQUIRE(ed.save_material(energy_id, red).ok());
    CHECK_FALSE(ed.asset_thumbnail(energy_id).has_value()); // queued again
    h.frames(3);
    const auto red_px = read(energy_id);
    CHECK(red_px != energy_px);
    {
        // The centre of the sphere is now clearly red.
        const auto c = pixel(red_px, n, n / 2, n / 2);
        CHECK(channel(c, 0) > channel(c, 1) + 20);
        CHECK(channel(c, 0) > channel(c, 2) + 20);
    }
    // ...and leaves model previews alone.
    CHECK(ed.asset_thumbnail(robot_id).has_value());

    // Reimporting with changes, invalidation and closing drop previews.
    ed.thumbnails()->invalidate(robot_id);
    CHECK_FALSE(ed.asset_thumbnail(robot_id).has_value());
    h.frames(2);
    CHECK(ed.asset_thumbnail(robot_id).has_value());
    ed.close_project();
    CHECK(ed.thumbnails() == nullptr);
}

TEST_CASE("editor: assets panel draws list and grid views with thumbnails") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "ThumbsUi", "ThumbsUi", "3d").ok());
    editor::Editor& ed = *h.ed;
    ed.set_asset_view(true, 1000.0f);
    CHECK(ed.asset_grid());
    CHECK(ed.asset_tile_size() == doctest::Approx(256.0f));
    ed.set_asset_view(true, 64.0f);
    h.frames(4);
    ed.set_asset_view(false, 64.0f);
    h.frames(4);
    CHECK_FALSE(ed.asset_grid());
    // Drawing the panel requested previews; the budget renders them over the frames.
    CHECK(ed.thumbnails()->rendered_total() + ed.thumbnails()->pending() > 0);
}

TEST_CASE("editor: visual script graphs are created, edited with undo, saved and run in play mode") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Graphs", "Graphs", "3d").ok());
    h.frames(2);
    auto& ed = *h.ed;
    const auto assets_dir = h.dir / "Graphs" / "assets";

    // A new graph is a starter (On Start -> Log) that compiles, registered as a script asset.
    auto id = ed.create_graph();
    REQUIRE(id.ok());
    const auto* rec = ed.assets()->find(*id);
    REQUIRE(rec);
    CHECK(rec->path == "scripts/Graph.vgraph");
    CHECK(rec->kind == assets::AssetKind::Script);
    auto second = ed.create_graph();
    REQUIRE(second.ok());
    CHECK(ed.assets()->find(*second)->path == "scripts/Graph_2.vgraph");
    CHECK_FALSE(ed.create_graph("scripts/Graph").ok()); // exists

    REQUIRE(ed.open_graph_editor(*id).ok());
    CHECK(ed.graph_editor_open());
    CHECK(ed.editing_graph_asset() == *id);
    CHECK(ed.editing_graph().nodes.size() == 2);
    CHECK(ed.editing_graph().links.size() == 1);
    CHECK(ed.graph_diagnostics().empty());
    CHECK_FALSE(ed.graph_dirty());
    CHECK_FALSE(ed.graph_can_undo());
    h.frames(2); // draws the canvas and side panel

    // Build: On Start -> Set Position(Make Vector(0, speed, 0)).
    CHECK(ed.graph_add_node("no.such_node") == 0);
    const auto set_pos = ed.graph_add_node("entity.set_position", 520, 0);
    const auto make = ed.graph_add_node("vector.make", 260, 120);
    REQUIRE(set_pos != 0);
    REQUIRE(make != 0);
    CHECK(ed.graph_dirty());
    CHECK(ed.graph_add_variable({"speed", graph::PinType::Number, json::Value(3.0), true}));
    CHECK_FALSE(ed.graph_add_variable({"speed", graph::PinType::Number, {}, true})); // duplicate
    const auto get = ed.graph_add_node("variable.get", 0, 120);
    REQUIRE(ed.graph_set_config(get, "variable", json::Value("speed")));
    CHECK(ed.graph_connect(2, "out", set_pos, "in")); // after the log
    CHECK(ed.graph_connect(get, "value", make, "y"));
    CHECK(ed.graph_connect(make, "vector", set_pos, "position"));
    CHECK_FALSE(ed.graph_connect(make, "vector", set_pos, "in")); // data into exec
    CHECK_FALSE(ed.graph_connect(make, "vector", make, "x"));     // self link
    CHECK(ed.graph_set_literal(make, "z", json::Value(2.0)));
    CHECK_FALSE(ed.graph_set_literal(set_pos, "in", json::Value(1.0))); // exec pins take no literal
    CHECK(ed.graph_diagnostics().empty());
    h.frames(2);

    // Undo steps back one edit at a time; redo restores it.
    const std::string built = graph::serialize_graph(ed.editing_graph());
    REQUIRE(ed.graph_undo()); // literal z
    CHECK(ed.editing_graph().node(make)->inputs.find("z") == nullptr);
    REQUIRE(ed.graph_undo()); // vector -> position link
    CHECK(ed.editing_graph().links.size() == 3);
    REQUIRE(ed.graph_redo());
    REQUIRE(ed.graph_redo());
    CHECK(graph::serialize_graph(ed.editing_graph()) == built);
    CHECK_FALSE(ed.graph_redo());

    // Consecutive literal edits of one pin are one undo step.
    CHECK(ed.graph_set_literal(make, "x", json::Value(1.0)));
    CHECK(ed.graph_set_literal(make, "x", json::Value(1.5)));
    REQUIRE(ed.graph_undo());
    CHECK(ed.editing_graph().node(make)->inputs.find("x") == nullptr);

    // Renaming a variable retargets its nodes; removing one leaves an error on them.
    CHECK(ed.graph_update_variable("speed", {"height", graph::PinType::Number, json::Value(3.0), true}));
    CHECK(ed.editing_graph().node(get)->config["variable"].as_string_or("") == "height");
    CHECK(ed.graph_diagnostics().empty());
    CHECK(ed.graph_remove_variable("height"));
    CHECK_FALSE(ed.graph_diagnostics().empty());
    CHECK(ed.graph_diagnostics().front().node == get);
    REQUIRE(ed.graph_undo());
    CHECK(ed.graph_diagnostics().empty());

    // Removing a node drops its links; undo brings both back.
    const std::size_t links = ed.editing_graph().links.size();
    CHECK(ed.graph_remove_node(make));
    CHECK(ed.editing_graph().node(make) == nullptr);
    CHECK(ed.editing_graph().links.size() == links - 2);
    REQUIRE(ed.graph_undo());
    CHECK(ed.editing_graph().links.size() == links);
    CHECK(ed.graph_move_node(make, 300, 140));
    CHECK(ed.editing_graph().node(make)->x == 300.0f);

    // Save writes the file; the inspector discovers the exposed variable as a field.
    REQUIRE(ed.save_graph().ok());
    CHECK_FALSE(ed.graph_dirty());
    auto text = fs::read_text(assets_dir / "scripts" / "Graph.vgraph");
    REQUIRE(text.ok());
    CHECK(*text == graph::serialize_graph(ed.editing_graph()));

    // Assign to an entity with a per-entity override and run it.
    Scene& s = ed.edit_scene();
    const Entity box = ed.create_entity(editor::Primitive::Cube);
    ed.rename(box, "GraphBox");
    ed.select(box);
    REQUIRE(ed.assign_graph_to(box));
    auto* sc = s.world().get<script::ScriptComponent>(box);
    REQUIRE(sc);
    CHECK(sc->script == "scripts/Graph.vgraph");
    sc->properties = json::Value::object();
    sc->properties.set("height", json::Value(5.0));
    h.frames(2); // the inspector shows the graph's fields
    ed.play();
    h.frames(3);
    REQUIRE(ed.play_scene());
    const Entity played = ed.play_scene()->find_by_name("GraphBox");
    REQUIRE_FALSE(played.is_null());
    const auto pos = ed.play_scene()->world().get<Transform>(played)->position;
    CHECK(pos.y == doctest::Approx(5.0f));
    CHECK(pos.z == doctest::Approx(2.0f));
    ed.stop();
    h.frames(1);

    // Undo of the assignment is a scene edit.
    ed.undo();
    CHECK_FALSE(s.world().has<script::ScriptComponent>(s.find_by_name("GraphBox")));

    // A file changed on disk reloads into the editor when there are no unsaved edits.
    auto changed = graph::parse_graph(*text);
    REQUIRE(changed.ok());
    changed->nodes.front().x = 400;
    REQUIRE(fs::write_text_atomic(assets_dir / "scripts" / "Graph.vgraph", graph::serialize_graph(*changed)).ok());
    h.frames(70); // the editor polls once a second
    CHECK(ed.editing_graph().nodes.front().x == 400.0f);
    CHECK_FALSE(ed.graph_dirty());

    // Graph files reopen as saved.
    REQUIRE(ed.open_graph_editor(*second).ok());
    CHECK(ed.editing_graph().nodes.size() == 2);
    REQUIRE(ed.open_graph_editor(*id).ok());
    CHECK(ed.editing_graph().nodes.size() == 5);
}

TEST_CASE("editor: graph debugging highlights running nodes and the failing node while playing") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "GraphDebug", "GraphDebug", "3d").ok());
    h.frames(2);
    auto& ed = *h.ed;
    auto id = ed.create_graph("scripts/Debug.vgraph");
    REQUIRE(id.ok());
    REQUIRE(ed.open_graph_editor(*id).ok());
    // Starter: 1 On Start -> 2 Log. Add On Update -> Translate.
    const auto update = ed.graph_add_node("event.update", 0, 160);
    const auto move = ed.graph_add_node("entity.translate", 260, 160);
    REQUIRE(ed.graph_connect(update, "out", move, "in"));
    REQUIRE(ed.save_graph().ok());
    const Entity box = ed.create_entity(editor::Primitive::Cube);
    ed.rename(box, "Traced");
    REQUIRE(ed.assign_graph_to(box));
    CHECK(ed.graph_node_heat(update) == 0.0f);

    ed.play();
    h.frames(4);
    CHECK(ed.graph_node_runs(1) == 1); // start ran once
    CHECK(ed.graph_node_runs(update) >= 3);
    CHECK(ed.graph_node_runs(move) == ed.graph_node_runs(update));
    CHECK(ed.graph_node_heat(update) > 0.9f); // runs every frame
    CHECK(ed.graph_node_heat(1) < 1.0f);      // ran once, fading
    CHECK_FALSE(ed.graph_runtime_error());

    // A live edit that fails at run time: the editor names the node after the hot reload.
    const auto add = ed.graph_add_node("math.add", 0, 300);
    REQUIRE(ed.graph_set_literal(add, "a", json::Value("text")));
    REQUIRE(ed.graph_set_literal(add, "b", json::Value(true)));
    const auto log = ed.graph_add_node("debug.log", 520, 160);
    REQUIRE(ed.graph_connect(move, "out", log, "in"));
    REQUIRE(ed.graph_connect(add, "result", log, "message"));
    REQUIRE(ed.save_graph().ok());
    h.frames(3);
    REQUIRE(ed.graph_runtime_error());
    CHECK(ed.graph_runtime_error()->node == log);
    CHECK(ed.graph_runtime_error()->message.find("[node") != std::string::npos);

    // After Stop the error stays visible and the highlights fade out.
    ed.stop();
    h.frames(90);
    CHECK(ed.graph_runtime_error());
    CHECK(ed.graph_node_heat(update) == 0.0f);
    // The next session starts clean once the graph is fixed.
    REQUIRE(ed.graph_remove_node(add));
    REQUIRE(ed.save_graph().ok());
    ed.play();
    h.frames(3);
    CHECK_FALSE(ed.graph_runtime_error());
    CHECK(ed.graph_node_runs(log) >= 1);
    ed.stop();
    h.frames(1);
}

TEST_CASE("editor: profiler window reports frame percentiles, scopes and records a trace") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Prof", "Prof", "3d").ok());
    h.ed->set_show_profiler(true);
    CHECK(h.ed->show_profiler());
    h.ed->play();
    h.frames(12);
    auto r = h.ed->profiler_report();
    CHECK(r.frames >= 10);
    CHECK(r.p99_ms >= r.p50_ms);
    auto has = [&](std::string_view name) {
        return std::any_of(r.scopes.begin(), r.scopes.end(), [&](const auto& s) { return s.name == name; });
    };
    CHECK(has("simulation.tick"));
    CHECK(std::any_of(r.scopes.begin(), r.scopes.end(), [](const auto& s) { return s.name.starts_with("system."); }));
    CHECK(std::any_of(r.scopes.begin(), r.scopes.end(), [](const auto& s) { return s.name.starts_with("pass."); }));

    // Paused: the window keeps its numbers.
    h.ed->set_profiler_paused(true);
    CHECK(h.ed->profiler_paused());
    const auto frozen = h.ed->profiler_report().frames;
    h.frames(3);
    CHECK(h.ed->profiler_report().frames == frozen);
    h.ed->set_profiler_paused(false);

    // Trace recording writes a Chrome trace into the project.
    h.ed->record_profile_trace(4);
    CHECK(h.ed->recording_profile_trace());
    h.frames(6);
    CHECK_FALSE(h.ed->recording_profile_trace());
    const auto trace = h.ed->last_profile_trace();
    REQUIRE_FALSE(trace.empty());
    CHECK(trace.parent_path() == h.ed->project()->root() / ".victus" / "profiles");
    auto text = fs::read_text(trace);
    REQUIRE(text);
    auto parsed = json::parse(*text);
    REQUIRE(parsed);
    CHECK((*parsed)["traceEvents"].size() > 10);
    h.ed->stop();
    h.frames(1);
}

TEST_CASE("editor: fuzzy matching ranks word starts and contiguous runs") {
    CHECK(editor::fuzzy_match_score("prof", "View: Toggle Profiler") >= 0);
    CHECK(editor::fuzzy_match_score("tog prof", "View: Toggle Profiler") >= 0);
    CHECK(editor::fuzzy_match_score("xyz", "View: Toggle Profiler") == -1);
    CHECK(editor::fuzzy_match_score("", "anything") >= -100);
    // Word-start/contiguous beats a scattered match.
    CHECK(editor::fuzzy_match_score("save", "File: Save Scene") >
          editor::fuzzy_match_score("save", "Assets: New Surface Shader (verbose)"));
    CHECK(editor::fuzzy_match_score("cube", "Entity: Create Cube") > editor::fuzzy_match_score("cube", "Select: Crate Cube Base"));
}

TEST_CASE("editor: command palette finds and runs actions, scenes, assets and entities") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Cmds", "Cmds", "3d").ok());
    h.frames(1);

    auto names = [&](std::string_view q) {
        std::vector<std::string> out;
        for (const auto& c : h.ed->find_commands(q)) out.push_back(c.name);
        return out;
    };
    // Empty query lists actions only.
    const auto all = names("");
    CHECK_FALSE(all.empty());
    CHECK(std::none_of(all.begin(), all.end(), [](const std::string& n) { return n.starts_with("Select: "); }));
    // Disabled commands are hidden: nothing to undo yet.
    CHECK(std::find(all.begin(), all.end(), "Edit: Undo") == all.end());

    REQUIRE_FALSE(names("toggle profiler").empty());
    CHECK(names("toggle profiler").front() == "View: Toggle Profiler");
    CHECK_FALSE(h.ed->show_profiler());
    CHECK(h.ed->run_command("view: toggle profiler"));
    CHECK(h.ed->show_profiler());

    const std::size_t before = h.ed->edit_scene().world().alive_count();
    REQUIRE_FALSE(names("create cube").empty());
    CHECK(names("create cube").front() == "Entity: Create Cube");
    CHECK(h.ed->run_command("Entity: Create Cube"));
    CHECK(h.ed->edit_scene().world().alive_count() == before + 1);
    CHECK(h.ed->run_command("Edit: Undo"));
    CHECK(h.ed->edit_scene().world().alive_count() == before);
    CHECK_FALSE(h.ed->run_command("No Such Command"));

    // Entities and scenes are searchable.
    const auto players = names("select player");
    REQUIRE_FALSE(players.empty());
    CHECK(players.front() == "Select: Player");
    CHECK(h.ed->run_command("Select: Player"));
    REQUIRE_FALSE(h.ed->selected().is_null());
    CHECK(h.ed->edit_scene().world().get<scene::Name>(h.ed->selected())->value == "Player");
    const auto scenes = names("open scene");
    CHECK(std::any_of(scenes.begin(), scenes.end(), [](const std::string& n) { return n.starts_with("Open Scene: "); }) ==
          false); // the only scene is already open
    REQUIRE(h.ed->create_graph().ok());
    CHECK_FALSE(names("open visual script").empty());

    // The palette window opens, takes input and closes on Escape without side effects.
    h.ed->open_command_palette();
    CHECK(h.ed->command_palette_open());
    h.frames(2);
    CHECK(h.ed->command_palette_open());
}

TEST_CASE("editor: a fatal error (GPU device lost) autosaves unsaved edits for recovery") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Fatal", "Fatal", "3d").ok());
    h.ed->set_autosave_interval(0); // periodic autosave off: only the fatal path writes
    h.frames(1);
    const std::string scene = h.ed->scene_path();
    const auto copy = h.ed->autosave_path(scene);
    CHECK(h.ed->save_for_fatal_error("GPU device lost: test") == "no unsaved edits");
    CHECK_FALSE(std::filesystem::exists(copy));

    const std::size_t base = h.ed->edit_scene().world().alive_count();
    (void)h.ed->create_entity(editor::Primitive::Cube);
    REQUIRE(h.ed->dirty());
    const std::string summary = h.ed->save_for_fatal_error("GPU device lost: test");
    CHECK(summary.find("autosaved") != std::string::npos);
    REQUIRE(std::filesystem::exists(copy));

    // The next session recovers the edit.
    h.ed->close_project();
    std::filesystem::last_write_time(copy, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5));
    REQUIRE(h.ed->open_project(h.dir / "Fatal").ok());
    REQUIRE(h.ed->recovery_available());
    REQUIRE(h.ed->recover_autosave().ok());
    CHECK(h.ed->edit_scene().world().alive_count() == base + 1);
}

TEST_CASE("editor: unsaved edits are autosaved and recovered after a crash") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Auto", "Auto", "3d").ok());
    h.ed->set_autosave_interval(0.5f);
    h.frames(1);
    const std::string scene = h.ed->scene_path();
    const auto copy = h.ed->autosave_path(scene);
    CHECK(copy.parent_path() == h.ed->project()->root() / ".victus" / "autosave");
    const std::size_t base = h.ed->edit_scene().world().alive_count();
    CHECK_FALSE(h.ed->recovery_available());

    // Not dirty: nothing is written; dirty: written after the interval.
    h.frames(3, 0.25f);
    CHECK_FALSE(std::filesystem::exists(copy));
    (void)h.ed->create_entity(editor::Primitive::Cube);
    REQUIRE(h.ed->dirty());
    h.frames(1, 0.25f);
    CHECK_FALSE(std::filesystem::exists(copy));
    h.frames(2, 0.25f);
    REQUIRE(std::filesystem::exists(copy));

    // Undoing every edit removes the stale copy.
    h.ed->undo();
    h.frames(1);
    CHECK_FALSE(std::filesystem::exists(copy));

    // "Crash": the project closes while dirty with the copy on disk.
    (void)h.ed->create_entity(editor::Primitive::Sphere);
    h.frames(3, 0.25f);
    REQUIRE(std::filesystem::exists(copy));
    h.ed->close_project();
    CHECK(std::filesystem::exists(copy));
    // Make sure the copy is newer than the scene file even on coarse file clocks.
    std::filesystem::last_write_time(copy, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5));

    REQUIRE(h.ed->open_project(h.dir / "Auto").ok());
    REQUIRE(h.ed->recovery_available());
    CHECK(h.ed->recovery_scene() == scene);
    h.frames(2); // the recovery prompt is shown
    CHECK(h.ed->edit_scene().world().alive_count() == base);
    REQUIRE(h.ed->recover_autosave().ok());
    CHECK_FALSE(h.ed->recovery_available());
    CHECK(h.ed->edit_scene().world().alive_count() == base + 1);
    CHECK(h.ed->dirty());
    CHECK(std::filesystem::exists(copy)); // kept until saved or discarded
    REQUIRE(h.ed->save_scene().ok());
    CHECK_FALSE(h.ed->dirty());
    CHECK_FALSE(std::filesystem::exists(copy));

    // Discarding a recovery deletes the copy and keeps the scene file.
    (void)h.ed->create_entity(editor::Primitive::Cube);
    REQUIRE(h.ed->autosave_now().ok());
    std::filesystem::last_write_time(copy, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5));
    h.ed->close_project();
    REQUIRE(h.ed->open_project(h.dir / "Auto").ok());
    REQUIRE(h.ed->recovery_available());
    h.ed->discard_autosave();
    CHECK_FALSE(h.ed->recovery_available());
    CHECK_FALSE(std::filesystem::exists(copy));
    CHECK(h.ed->edit_scene().world().alive_count() == base + 1);

    // A copy older than the scene file (saved later elsewhere) is ignored and removed.
    (void)h.ed->create_entity(editor::Primitive::Cube);
    REQUIRE(h.ed->autosave_now().ok());
    std::filesystem::last_write_time(copy, std::filesystem::file_time_type::clock::now() - std::chrono::hours(1));
    h.ed->close_project();
    REQUIRE(h.ed->open_project(h.dir / "Auto").ok());
    CHECK_FALSE(h.ed->recovery_available());
    CHECK_FALSE(std::filesystem::exists(copy));

    // Untitled scenes autosave too, and Save As removes the untitled copy.
    h.ed->new_scene();
    const auto untitled = h.ed->autosave_path("");
    (void)h.ed->create_entity(editor::Primitive::Cube);
    REQUIRE(h.ed->autosave_now().ok());
    REQUIRE(std::filesystem::exists(untitled));
    REQUIRE(h.ed->save_scene_as("scenes/second").ok());
    CHECK_FALSE(std::filesystem::exists(untitled));
}

TEST_CASE("editor: merge_edit applies only the fields changed on the primary") {
    auto parse = [](const char* t) { return *json::parse(t); };
    const auto before = parse(R"({"Transform":{"position":[0,0,0],"scale":[1,1,1]},"Rotator":{"degrees_per_second":10}})");
    const auto after = parse(R"({"Transform":{"position":[0,5,0],"scale":[1,1,1]},"MeshRenderer":{"primitive":"cube"}})");
    const auto target = parse(R"({"Transform":{"position":[1,2,3],"scale":[2,2,2]},"Rotator":{"degrees_per_second":99},"Light":{"type":"point"}})");
    const auto out = editor::merge_edit(before, after, target);
    CHECK(out["Transform"]["position"][0].as_number() == 1);
    CHECK(out["Transform"]["position"][1].as_number() == 5); // only y changed
    CHECK(out["Transform"]["position"][2].as_number() == 3);
    CHECK(out["Transform"]["scale"][0].as_number() == 2);    // untouched
    CHECK(out.find("Rotator") == nullptr);                   // removed on the primary
    CHECK(out["MeshRenderer"]["primitive"].as_string() == "cube"); // added on the primary
    CHECK(out["Light"]["type"].as_string() == "point");      // the target's own component stays
    CHECK(editor::merge_edit(before, before, target) == target);
}

TEST_CASE("editor: range, box and multi-entity editing of the selection") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Multi", "Multi", "3d").ok());
    h.frames(2);
    scene::Scene& s = h.ed->edit_scene();
    const auto order = h.ed->hierarchy_order();
    REQUIRE(order.size() > 6);
    CHECK(order.size() == s.world().alive_count());

    // Shift+click range in hierarchy order; the clicked entity is primary.
    h.ed->select(order[1]);
    h.ed->select_range(order[4]);
    CHECK(h.ed->selection().size() == 4);
    CHECK(h.ed->selected() == order[4]);
    h.ed->select_range(order[2]); // extending again keeps the earlier picks
    CHECK(h.ed->selection().size() == 4);
    CHECK(h.ed->selected() == order[2]);

    // Box select: the whole viewport picks many objects, an empty corner none; additive keeps.
    const math::Vec2 size{800, 450};
    h.ed->clear_selection();
    CHECK(h.ed->box_select({0, 0}, {1, 1}, size) == 0);
    const std::size_t all = h.ed->box_select({0, 0}, size, size);
    CHECK(all >= 4);
    CHECK(h.ed->selection().size() == all);
    // Prefab parts collapse to their instance: no selected entity is a part of a selected instance.
    for (const Uuid& id : h.ed->selection()) {
        const auto e = s.find(id);
        const auto root = scene::instance_root(s, e);
        CHECK((root.is_null() || root == e));
    }
    h.ed->clear_selection();
    const std::size_t left = h.ed->box_select({0, 0}, {size.x * 0.5f, size.y}, size);
    const std::size_t right = h.ed->box_select({size.x * 0.5f, 0}, size, size, true);
    CHECK(left + right >= all - 1);
    CHECK(h.ed->selection().size() == left + right);

    // Multi edit: changing the primary's position.y moves every selected entity's y only.
    const auto a = h.ed->create_entity(editor::Primitive::Cube);
    const auto b = h.ed->create_entity(editor::Primitive::Sphere);
    s.world().get<scene::Transform>(a)->position = {1, 0, 0};
    s.world().get<scene::Transform>(b)->position = {-3, 2, 7};
    h.ed->select(b);
    h.ed->select(a, true); // a is primary
    const json::Value before = s.save_entity(a);
    s.world().get<scene::Transform>(a)->position.y = 4;
    s.world().add<gameplay::RotatorComponent>(a);
    h.ed->apply_multi_edit(a, before);
    const auto* tb = s.world().get<scene::Transform>(b);
    CHECK(tb->position.x == doctest::Approx(-3));
    CHECK(tb->position.y == doctest::Approx(4));
    CHECK(tb->position.z == doctest::Approx(7));
    CHECK(s.world().has<gameplay::RotatorComponent>(b));
}

TEST_CASE("editor: material graphs are created, edited on the canvas and shade the open material") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "MatGraph", "MatGraph", "3d").ok());
    h.frames(2);
    auto& ed = *h.ed;
    const Entity box = ed.create_entity(editor::Primitive::Cube);
    ed.rename(box, "Orb");
    ed.select(box);
    ed.new_material_for_selection();
    const Uuid mat = ed.edit_scene().world().get<MeshRendererComponent>(box)->material_asset;
    REQUIRE_FALSE(mat.is_nil());

    // A new material graph is a shader asset with the starter graph; it opens in the Graph Editor
    // with the material node library.
    auto id = ed.create_material_graph();
    REQUIRE(id.ok());
    const Uuid graph = *id;
    const auto* rec = ed.assets()->find(graph);
    REQUIRE(rec);
    CHECK(rec->path == "shaders/MaterialGraph.vmatgraph");
    CHECK(rec->kind == assets::AssetKind::Shader);
    REQUIRE(ed.open_graph_editor(graph).ok());
    CHECK(ed.editing_graph().domain == graph::GraphDomain::Material);
    CHECK(ed.graph_diagnostics().empty());
    h.frames(2);

    // Script nodes and variables do not belong in material graphs.
    CHECK(ed.graph_add_node("debug.log") == 0);
    CHECK_FALSE(ed.graph_add_variable({"speed", graph::PinType::Number, json::Value(1.0), true}));

    // Fresnel rim * Parameter 0 colour -> emission.
    std::uint32_t out = 0;
    for (const auto& n : ed.editing_graph().nodes)
        if (n.type == graph::kMaterialOutputNode) out = n.id;
    REQUIRE(out != 0);
    const auto fres = ed.graph_add_node("mat.fresnel", 0, 300);
    const auto param = ed.graph_add_node("mat.param0", 0, 420);
    const auto mul = ed.graph_add_node("mat.multiply", 260, 340);
    REQUIRE(fres);
    REQUIRE(param);
    REQUIRE(mul);
    CHECK(ed.graph_connect(fres, "value", mul, "a"));
    CHECK(ed.graph_connect(param, "rgb", mul, "b"));
    CHECK(ed.graph_connect(mul, "result", out, "emissive"));
    CHECK(ed.graph_set_literal(fres, "power", json::Value(2.0)));
    CHECK(ed.graph_set_literal(out, "roughness", json::Value(0.3)));
    CHECK(ed.graph_diagnostics().empty());
    h.frames(2);

    // Undo/redo keep the material domain.
    REQUIRE(ed.graph_undo());
    CHECK(ed.editing_graph().domain == graph::GraphDomain::Material);
    REQUIRE(ed.graph_redo());
    CHECK(ed.editing_graph().domain == graph::GraphDomain::Material);

    // Assigned to the open material and saved: the renderer compiles the graph.
    ed.open_material_editor(mat);
    CHECK(ed.set_editing_material_shader(graph));
    REQUIRE(ed.save_graph().ok());
    const auto glsl = runtime::load_surface_shader_code(*ed.assets(), graph);
    REQUIRE(glsl.ok());
    CHECK(glsl->find("s.roughness = 0.3;") != std::string::npos);
    CHECK(glsl->find("s.emissive = ") != std::string::npos);
    auto mat_file = ed.assets()->load_material(mat);
    REQUIRE(mat_file);
    CHECK(mat_file->shader == graph);
    h.frames(3); // Material Editor shows the compile status, viewport renders with the graph
    const auto& msgs = ed.messages();
    CHECK(std::none_of(msgs.begin(), msgs.end(), [](const std::string& m) { return m.find("does not compile") != std::string::npos; }));

    // A broken graph is saved but reported.
    REQUIRE(ed.graph_remove_node(out));
    CHECK_FALSE(ed.graph_diagnostics().empty());
    REQUIRE(ed.save_graph().ok());
    CHECK(std::any_of(ed.messages().begin(), ed.messages().end(), [](const std::string& m) { return m.find("1 error") != std::string::npos; }));
    h.frames(2);
}

TEST_CASE("editor: terrain is created as an asset and entity, collides in play mode and undoes") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Land", "Land", "3d").ok());
    h.frames(2);
    auto& ed = *h.ed;
    auto made = ed.create_terrain();
    REQUIRE(made.ok());
    Scene& s = ed.edit_scene();
    const auto* comp = s.world().get<terrain::TerrainComponent>(*made);
    REQUIRE(comp);
    const assets::AssetRecord* rec = ed.assets()->find(comp->data);
    REQUIRE(rec);
    CHECK(rec->path == "terrain/Terrain.vterrain");
    CHECK(rec->kind == assets::AssetKind::Terrain);
    CHECK(ed.assets()->load_terrain(comp->data).ok());
    h.frames(3); // edit mode builds and draws the chunks
    const auto* rt = s.world().get<terrain::TerrainRuntime>(*made);
    REQUIRE(rt);
    CHECK(rt->chunks.size() == 16);
    auto second = ed.create_terrain();
    REQUIRE(second.ok());
    CHECK(ed.assets()->find(s.world().get<terrain::TerrainComponent>(*second)->data)->path == "terrain/Terrain_2.vterrain");
    ed.undo();
    CHECK(s.find(s.uuid(*made)) == *made);
    std::size_t terrains = 0;
    s.world().each<terrain::TerrainComponent>([&](Entity, terrain::TerrainComponent&) { ++terrains; });
    CHECK(terrains == 1);

    // Saved scenes keep the component; play mode gives the terrain a height-field collider.
    REQUIRE(ed.save_scene().ok());
    ed.play();
    h.frames(4);
    REQUIRE(ed.play_scene());
    bool collider = false;
    ed.play_scene()->world().each<physics::StaticHeightfieldComponent>(
        [&](Entity, physics::StaticHeightfieldComponent& c) { collider = c.resolution == 129; });
    CHECK(collider);
    ed.stop();
}

TEST_CASE("editor: terrain strokes sculpt and paint, undo and redo, and are written on save") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Sculpt", "Sculpt", "3d").ok());
    h.frames(2);
    auto& ed = *h.ed;
    auto made = ed.create_terrain();
    REQUIRE(made.ok());
    Scene& s = ed.edit_scene();
    const Entity t = *made;
    const auto* comp = s.world().get<terrain::TerrainComponent>(t);
    REQUIRE(comp);
    const Uuid data_id = comp->data;
    const std::filesystem::path file = ed.assets()->assets_dir() / ed.assets()->find(data_id)->path;
    h.frames(2);
    auto data = s.world().get<terrain::TerrainRuntime>(t)->data;
    REQUIRE(data);
    const auto original = data->heights;
    const auto original_splat = data->splat;
    const std::size_t centre = data->index(64, 64);

    // The editor camera looks at the terrain centre: the ray under the viewport centre hits it.
    const auto hover = ed.terrain_under({400, 300}, {800, 600});
    REQUIRE(hover);
    CHECK(hover->entity == t);
    CHECK(ed.pick({400, 300}, {800, 600}) == t);

    ed.select(t, false);
    ed.set_terrain_tool(true);
    ed.terrain_brush() = {.mode = terrain::BrushMode::Raise, .radius = 10, .strength = 1};
    const std::size_t undo_before = ed.undo_stack().size();
    const math::Vec3 p = s.world().get<scene::Transform>(t)->position;
    REQUIRE(ed.begin_terrain_stroke(t, p));
    CHECK(ed.terrain_stroke_active());
    for (int i = 0; i < 5; ++i) ed.continue_terrain_stroke(p + math::Vec3{static_cast<float>(i), 0, 0}, 0.1f);
    ed.end_terrain_stroke();
    CHECK_FALSE(ed.terrain_stroke_active());
    CHECK(ed.undo_stack().size() == undo_before + 1);
    const float raised = data->heights[centre];
    CHECK(raised > original[centre]);
    CHECK(ed.unsaved_terrains() == 1);

    // An empty stroke records nothing; Shift-inverted strokes lower.
    REQUIRE(ed.begin_terrain_stroke(t, p));
    ed.end_terrain_stroke();
    CHECK(ed.undo_stack().size() == undo_before + 1);
    REQUIRE(ed.begin_terrain_stroke(t, p, true));
    ed.continue_terrain_stroke(p, 0.05f);
    ed.end_terrain_stroke();
    CHECK(data->heights[centre] < raised);
    ed.undo();
    CHECK(data->heights[centre] == doctest::Approx(raised));
    ed.undo();
    CHECK(data->heights == original);
    ed.redo();
    CHECK(data->heights[centre] == doctest::Approx(raised));

    // Painting, then whole-terrain operations, all undoable.
    ed.terrain_brush() = {.mode = terrain::BrushMode::Paint, .radius = 8, .strength = 1, .layer = 3};
    REQUIRE(ed.begin_terrain_stroke(t, p));
    for (int i = 0; i < 10; ++i) ed.continue_terrain_stroke(p, 0.25f);
    ed.end_terrain_stroke();
    CHECK(terrain::unpack_weights(data->splat[centre]).w > 0.9f);
    REQUIRE(ed.generate_terrain(t, {.seed = 9}).ok());
    const auto generated = data->heights;
    CHECK(generated != original);
    REQUIRE(ed.auto_paint_terrain(t).ok());
    ed.undo();
    ed.undo();
    CHECK(terrain::unpack_weights(data->splat[centre]).w > 0.9f);
    CHECK(data->heights[centre] == doctest::Approx(raised));
    ed.redo();
    CHECK(data->heights == generated);

    // Saving writes the heightfield; the file reloads with the edits (16-bit quantized).
    REQUIRE(ed.save_scene().ok());
    CHECK(ed.unsaved_terrains() == 0);
    auto loaded = terrain::load_file(file);
    REQUIRE(loaded.ok());
    CHECK(loaded->heights[centre] == doctest::Approx(generated[centre]).epsilon(1e-4));
    // Asset reloads keep the edited data.
    h.frames(3);
    CHECK(s.world().get<terrain::TerrainRuntime>(t)->data == data);
    (void)original_splat;

    // Foliage scatters on the generated terrain as an undoable child entity.
    auto bush = ed.add_foliage(t, -1);
    REQUIRE(bush.ok());
    CHECK(s.children(t).size() == 1);
    CHECK(s.world().get<terrain::FoliageComponent>(*bush)->layer == -1);
    CHECK(s.world().has<MeshRendererComponent>(*bush));
    h.frames(2);
    const auto* frt = s.world().get<terrain::FoliageRuntime>(*bush);
    REQUIRE(frt);
    CHECK(frt->instances > 100u);
    ed.undo();
    CHECK(s.children(t).empty());
    ed.redo();
    CHECK(s.children(t).size() == 1);
    Entity none = s.create("Empty");
    CHECK_FALSE(ed.add_foliage(none).ok());
}

TEST_CASE("editor: play sessions run behaviour trees and expose them to the AI debugger") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Brains", "Brains", "3d").ok());
    h.frames(2);
    auto& ed = *h.ed;
    const auto tree_file = ed.assets()->assets_dir() / "ai" / "counter.vbt";
    std::filesystem::create_directories(tree_file.parent_path());
    REQUIRE(fs::write_text_atomic(tree_file, R"({"blackboard": {"mood": "curious"},
        "root": {"type": "sequence", "children": [{"type": "wait", "seconds": 0.01}, {"type": "set", "key": "done", "value": true}]}})").ok());
    Scene& s = ed.edit_scene();
    const Entity e = s.create("Thinker");
    s.world().add<ai::BehaviorTreeComponent>(e, ai::BehaviorTreeComponent{.tree = "ai/counter.vbt"});
    s.world().add<ai::PerceptionComponent>(e, ai::PerceptionComponent{});
    CHECK(ed.play_ai() == nullptr);
    CHECK(ed.ai_debug_lines(e).empty());
    ed.play();
    h.frames(10);
    REQUIRE(ed.play_ai() != nullptr);
    Scene& play = ed.active_scene();
    const Entity pe = play.find(s.uuid(e));
    REQUIRE_FALSE(pe.is_null());
    const auto d = ed.play_ai()->debug(play, pe);
    REQUIRE(d);
    CHECK(d->error.empty());
    CHECK(d->ticks >= 5);
    CHECK(d->completions >= 1);
    const auto lines = ed.ai_debug_lines(pe);
    REQUIRE(lines.size() >= 4);
    CHECK(lines[0].find("ai/counter.vbt") != std::string::npos);
    CHECK(lines[1].find("sequence") != std::string::npos);
    CHECK(std::find(lines.begin(), lines.end(), "bb mood = curious") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), "bb done = true") != lines.end());
    ed.set_show_ai(true);
    ed.select(e, false);
    h.frames(2); // debugger window and perception overlay draw without errors
    ed.stop();
    CHECK(ed.play_ai() == nullptr);
}

TEST_CASE("editor: the Animation window previews and drives animation controllers") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Anim", "Anim", "3d").ok());
    h.frames(2);
    auto& ed = *h.ed;
    Scene& s = ed.edit_scene();
    const Entity robot = s.find_by_name("Robot");
    REQUIRE_FALSE(robot.is_null());
    const auto same_rotation = [](math::Quat a, math::Quat b) {
        return std::abs(std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) - 1.0f) <= 1e-4f;
    };
    const Entity leg = s.find_by_name("UpperLeg.L");
    const Transform authored = *s.world().get<Transform>(leg);
    const std::string clip_before = s.world().get<animation::AnimatorComponent>(robot)->clip;
    ed.set_show_animation(true);
    ed.select(robot, false);
    h.frames(2); // window with the Preview button
    CHECK(ed.animation_inspected() == nullptr);
    CHECK(ed.animation_debug_lines(robot).empty());

    // Edit-mode preview: the controller runs on the selected robot only.
    REQUIRE(ed.begin_animation_preview(robot));
    CHECK(ed.animation_previewing());
    CHECK(ed.animation_preview_target() == robot);
    auto lines = ed.animation_debug_lines(robot);
    REQUIRE_FALSE(lines.empty());
    CHECK(lines[0] == "controller anim/characters/vanguard_locomotion.vanim state Locomotion time 0.00");
    CHECK(std::find(lines.begin(), lines.end(), "weight Idle 1.00") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), "param speed float = 0.00") != lines.end());
    REQUIRE(ed.set_animation_parameter(robot, "speed", 5.0f));
    CHECK_FALSE(ed.set_animation_parameter(robot, "nope", 1.0f));
    h.frames(10);
    lines = ed.animation_debug_lines(robot);
    CHECK(std::find(lines.begin(), lines.end(), "weight Run 1.00") != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), "param speed float = 5.00") != lines.end());
    CHECK_FALSE(same_rotation(s.world().get<Transform>(leg)->rotation, authored.rotation)); // striding
    CHECK_FALSE(ed.dirty()); // previews are not edits
    REQUIRE(ed.fire_animation_trigger(robot, "jump"));
    h.frames(2);
    CHECK(ed.animation_debug_lines(robot)[0].find("state Jump") != std::string::npos);
    REQUIRE(ed.enter_animation_state(robot, "Locomotion"));
    CHECK_FALSE(ed.enter_animation_state(robot, "NoSuchState"));
    ed.set_animation_preview_paused(true);
    h.frames(3); // paused preview still draws
    // Saving ends the preview and restores the authored pose first.
    REQUIRE(ed.save_scene().ok());
    CHECK_FALSE(ed.animation_previewing());
    CHECK(same_rotation(s.world().get<Transform>(leg)->rotation, authored.rotation));
    CHECK(math::length(s.world().get<Transform>(leg)->position - authored.position) < 1e-6f);
    CHECK(s.world().get<animation::AnimatorComponent>(robot)->clip == clip_before);

    // Play: the window reads and drives the live session (starting play ends a preview).
    REQUIRE(ed.begin_animation_preview(robot));
    ed.play();
    CHECK_FALSE(ed.animation_previewing());
    h.frames(5);
    Scene& play = ed.active_scene();
    const Entity probot = play.find(s.uuid(robot));
    REQUIRE_FALSE(probot.is_null());
    REQUIRE(ed.animation_inspected() != nullptr);
    CHECK_FALSE(ed.begin_animation_preview(probot)); // no edit previews while playing
    ed.select(probot, false);
    h.frames(2);
    lines = ed.animation_debug_lines(probot);
    REQUIRE_FALSE(lines.empty());
    CHECK(lines[0].find("state Locomotion") != std::string::npos);
    CHECK(ed.fire_animation_trigger(probot, "jump"));
    h.frames(2);
    CHECK(ed.animation_debug_lines(probot)[0].find("state Jump") != std::string::npos);
    ed.stop();
    CHECK(ed.animation_inspected() == nullptr);
}

TEST_CASE("editor: the sequencer panel edits, previews, records and saves sequences") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Cine", "Cine", "3d").ok());
    h.frames(2);
    auto& ed = *h.ed;
    Scene& s = ed.edit_scene();
    using sequencer::TrackType;
    const Entity rig = s.create("Rig");
    const Entity door = s.create("Door", rig);
    s.world().get<Transform>(door)->position = {1, 0, 0};
    const Entity cam = s.create("ShotCam");
    s.world().add<CameraComponent>(cam, CameraComponent{.primary = false});
    auto door_pos = [&] { return s.world().get<Transform>(door)->position; };
    ed.select(rig);

    // Create, open, assign.
    auto path = ed.create_sequence();
    REQUIRE(path.ok());
    CHECK(*path == "sequences/Sequence.vseq");
    REQUIRE(ed.assets()->find_path(*path));
    CHECK(ed.assets()->find_path(*path)->kind == assets::AssetKind::Sequence);
    REQUIRE(ed.open_sequence(*path).ok());
    CHECK(ed.sequencer_open());
    CHECK(ed.editing_sequence_path() == *path);
    CHECK(ed.editing_sequence()->duration == doctest::Approx(5));
    CHECK(ed.sequence_preview_player().is_null());
    h.frames(2); // panel with "No preview player"
    REQUIRE(ed.assign_sequence(rig));
    CHECK(s.world().get<sequencer::SequencePlayerComponent>(rig)->sequence == *path);
    CHECK(ed.sequence_preview_player() == rig);

    // Record a transform key, move the door, record another; scrubbing interpolates.
    const int tr = ed.sequence_add_track(TrackType::Transform, "Door");
    CHECK(tr == 0);
    const auto k0 = ed.sequence_record_transform(tr, 0);
    CHECK(k0.valid());
    ed.sequence_seek(0);
    CHECK(ed.sequence_previewing());
    s.world().get<Transform>(door)->position = {1, 4, 0};
    const auto k1 = ed.sequence_record_transform(tr, 2.0f);
    REQUIRE(k1.valid());
    REQUIRE(ed.editing_sequence()->tracks[0].position.size() == 2);
    CHECK(ed.editing_sequence()->tracks[0].rotation.size() == 2);
    ed.sequence_seek(1.0f);
    CHECK(door_pos().y == doctest::Approx(2));
    CHECK(ed.sequence_dirty());

    // Event and camera tracks.
    const int ev = ed.sequence_add_track(TrackType::Event);
    const auto ek = ed.sequence_add_key(ev, 1.5f, "boom");
    CHECK(ek.valid());
    CHECK(ed.editing_sequence()->tracks[1].events.at(0).name == "boom");
    const int ct = ed.sequence_add_track(TrackType::Camera);
    CHECK(ed.sequence_add_key(ct, 0.5f, "ShotCam").valid());
    ed.sequence_seek(1.0f);
    CHECK(s.world().get<CameraComponent>(cam)->primary);

    // Moving a transform key moves its rotation and scale keys too.
    const auto moved = ed.sequence_move_key(k1, 3.0f);
    REQUIRE(moved.valid());
    CHECK(*ed.sequence_key_time(moved) == doctest::Approx(3));
    CHECK(ed.editing_sequence()->tracks[0].rotation.back().time == doctest::Approx(3));
    CHECK(ed.editing_sequence()->tracks[0].scale.back().time == doctest::Approx(3));
    ed.sequence_seek(1.5f);
    CHECK(door_pos().y == doctest::Approx(2));

    // Delete, undo, redo.
    CHECK(ed.sequence_delete_key(ek));
    CHECK(ed.editing_sequence()->tracks[1].events.empty());
    CHECK(ed.sequence_undo());
    CHECK(ed.editing_sequence()->tracks[1].events.size() == 1);
    CHECK(ed.sequence_redo());
    CHECK(ed.editing_sequence()->tracks[1].events.empty());
    CHECK(ed.sequence_undo());

    // Real-time preview and the panel drawing with a selected key.
    ed.sequence_preview_play(true);
    h.frames(6);
    CHECK(ed.sequence_time() > 1.5f);
    ed.sequence_preview_play(false);
    (void)ed.sequence_add_key(ev, 2.5f, "late");
    h.frames(3);

    // Ending the preview restores the authored scene.
    ed.end_sequence_preview();
    CHECK_FALSE(ed.sequence_previewing());
    CHECK(door_pos() == math::Vec3{1, 0, 0});
    CHECK_FALSE(s.world().get<CameraComponent>(cam)->primary);

    // Save: the runtime accepts the file and our own write is not reported as an outside change.
    REQUIRE(ed.save_sequence().ok());
    CHECK_FALSE(ed.sequence_dirty());
    const auto file = ed.assets()->assets_dir() / *path;
    auto text = fs::read_text(file);
    REQUIRE(text.ok());
    auto parsed = sequencer::Sequence::parse(*text);
    REQUIRE(parsed.ok());
    CHECK((*parsed)->tracks.size() == 3);
    CHECK((*parsed)->tracks[0].position.back().time == doctest::Approx(3));
    CHECK((*parsed)->tracks[1].events.size() == 2);
    sfs::last_write_time(file, sfs::file_time_type::clock::now() + std::chrono::seconds(2));
    std::size_t before = ed.messages().size();
    (void)ed.check_asset_changes();
    for (std::size_t i = before; i < ed.messages().size(); ++i) CHECK(ed.messages()[i].find("Reloaded sequence") == std::string::npos);

    // An outside edit reloads the open (clean) sequence.
    REQUIRE(fs::write_text_atomic(file, R"({"duration": 1, "tracks": [{"type": "transform", "target": "Door",
        "position": [{"t": 0, "v": [0, 0, 0]}, {"t": 0.1, "v": [0, 5, 0]}]}]})").ok());
    sfs::last_write_time(file, sfs::file_time_type::clock::now() + std::chrono::seconds(4));
    before = ed.messages().size();
    (void)ed.check_asset_changes();
    bool reported = false;
    for (std::size_t i = before; i < ed.messages().size(); ++i) reported |= ed.messages()[i] == "Reloaded sequence " + *path;
    CHECK(reported);
    REQUIRE(ed.editing_sequence()->tracks.size() == 1);

    // Saving the scene while previewing stores the authored pose.
    ed.sequence_seek(0.1f);
    CHECK(door_pos().y == doctest::Approx(5));
    REQUIRE(ed.save_scene_as("scenes/cine.vscene").ok());
    CHECK_FALSE(ed.sequence_previewing());
    CHECK(door_pos() == math::Vec3{1, 0, 0});

    // Play sessions run the saved sequence.
    ed.play();
    h.frames(20);
    REQUIRE(ed.play_sequencer() != nullptr);
    Scene& play = ed.active_scene();
    const Entity pd = play.find(s.uuid(door));
    REQUIRE_FALSE(pd.is_null());
    CHECK(play.world().get<Transform>(pd)->position.y == doctest::Approx(5));
    ed.stop();
    CHECK(ed.play_sequencer() == nullptr);
    CHECK(door_pos() == math::Vec3{1, 0, 0});

    // Relative tracks record offsets from the pre-preview transform.
    const int rel = ed.sequence_add_track(TrackType::Transform, "Door");
    REQUIRE(ed.edit_sequence([&](sequencer::Sequence& q) { q.tracks[static_cast<std::size_t>(rel)].relative = true; }));
    ed.sequence_seek(0);
    s.world().get<Transform>(door)->position = {1, 0, 2};
    REQUIRE(ed.sequence_record_transform(rel, 0.5f).valid());
    CHECK(ed.editing_sequence()->tracks[static_cast<std::size_t>(rel)].position.at(0).value == math::Vec3{0, 0, 2});
    ed.close_sequence();
    CHECK(ed.editing_sequence() == nullptr);
    CHECK(door_pos() == math::Vec3{1, 0, 0});
}
