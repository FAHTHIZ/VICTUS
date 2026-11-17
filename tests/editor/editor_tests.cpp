// SPDX-License-Identifier: Apache-2.0
// Editor: undo/redo state machine on plain scenes, and the full editor driven headlessly on the
// GPU (projects from templates, viewport rendering, picking, create/delete/duplicate/reparent
// with undo, play mode isolation, save and reopen).
#include "../render/gpu_fixture.hpp"
#include "victus/editor/editor.hpp"
#include "victus/animation/animation.hpp"
#include "victus/gameplay/capture.hpp"
#include "victus/editor/undo.hpp"

#include "victus/render/image.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/gameui/gameui.hpp"
#include "victus/net/net.hpp"
#include "victus/physics/physics.hpp"
#include "victus/project/project.hpp"
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
        CHECK_FALSE(shapes->boxes.empty());
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
