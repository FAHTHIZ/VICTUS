// SPDX-License-Identifier: Apache-2.0
// Lessons: build a whole small game step by step inside the Studio. Each step watches the scene
// (or the running game) and ticks itself off when what it asks for is there, however the user
// made it: by hand, with Add Component, with "Make it a..." or with the step's own button.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include "victus/gameplay/gameplay.hpp"
#include "victus/physics/physics.hpp"
#include "victus/project/project.hpp"
#include "victus/runtime/scene_view.hpp"
#include "victus/script/behaviours.hpp"
#include "victus/ui/style.hpp"

#include <imgui.h>

#include <array>
#include <filesystem>
#include <format>

namespace victus::editor {

namespace tk = ui::tokens;
using namespace scene;

namespace {

enum class LessonGoal : std::uint8_t { Floor, Player, Coin, Enemy, Goal, Win, Save };

struct StepText {
    LessonGoal goal;
    const char* title;
    const char* how_2d;
    const char* how_3d;
    const char* action;
};

constexpr std::array<StepText, 7> kSteps = {{
    {LessonGoal::Floor, "Add a floor",
     "Add a Sprite (Entity > Sprite), make it wide with Size in the Inspector, then Add Component > Collider 2D so "
     "things can stand on it.",
     "Add a Cube (Entity > Cube), make it flat and wide with Scale in the Inspector, then Add Component > Collider "
     "so things can stand on it.",
     "Add a floor for me"},
    {LessonGoal::Player, "Add the player",
     "Add another sprite above the floor, then press Make it a... > Player in the Inspector. It runs with A/D and "
     "jumps with Space.",
     "Add another cube above the floor, then press Make it a... > Player in the Inspector. It walks with WASD and "
     "jumps with Space.",
     "Add the player for me"},
    {LessonGoal::Coin, "Add a coin", "Add a small sprite and press Make it a... > Coin. Touching it scores a point.",
     "Add a small sphere and press Make it a... > Coin. Touching it scores a point.", "Add a coin for me"},
    {LessonGoal::Enemy, "Add an enemy",
     "Add a sprite and press Make it a... > Enemy. It walks back and forth; jump on it to defeat it.",
     "Add a cube and press Make it a... > Enemy. It walks back and forth; jump on it to defeat it.",
     "Add an enemy for me"},
    {LessonGoal::Goal, "Add a goal", "Put a sprite at the far end and press Make it a... > Goal.",
     "Put a cube at the far end and press Make it a... > Goal.", "Add a goal for me"},
    {LessonGoal::Win, "Play and reach the goal",
     "Press Play, click the viewport, then run with A/D and jump with Space. Jump over or onto the enemy and touch "
     "the goal.",
     "Press Play, click the viewport, then walk with WASD and jump with Space. Get past the enemy and touch the goal.",
     "Play now"},
    {LessonGoal::Save, "Save your game", "Press Ctrl+S. Then try File > Export Game to share it.",
     "Press Ctrl+S. Then try File > Export Game to share it.", "Save now"},
}};

bool any_behaviour_role(ecs::World& w, Entity e) {
    return w.has<script::PlatformerMovementComponent>(e) || w.has<script::TopDownMovementComponent>(e) ||
           w.has<script::HazardComponent>(e) || w.has<script::CollectibleComponent>(e) ||
           w.has<script::GoalComponent>(e) || w.has<script::PatrolComponent>(e);
}

bool has_floor(Scene& s, bool two_d) {
    auto& w = s.world();
    bool found = false;
    if (two_d) {
        w.each<physics::Collider2DComponent>([&](Entity e, physics::Collider2DComponent& c) {
            const auto* rb = w.get<physics::RigidBody2DComponent>(e);
            if (!c.is_trigger && (!rb || rb->type != physics::BodyType::Dynamic) && !any_behaviour_role(w, e)) found = true;
        });
    } else {
        w.each<physics::ColliderComponent>([&](Entity e, physics::ColliderComponent& c) {
            const auto* rb = w.get<physics::RigidBodyComponent>(e);
            if (!c.is_trigger && (!rb || rb->type != physics::BodyType::Dynamic) && !any_behaviour_role(w, e)) found = true;
        });
    }
    return found;
}

} // namespace

std::size_t lesson_step_count() noexcept { return kSteps.size(); }
const char* lesson_step_title(std::size_t i) noexcept { return i < kSteps.size() ? kSteps[i].title : ""; }

bool Editor::start_lesson(Lesson l) {
    if (!project_ || l == Lesson::None) return false;
    if (play_state_ != PlayState::Editing) stop();
    if (dirty()) {
        if (scene_path_.empty()) {
            notify("Save this scene first (Ctrl+S), then start the lesson", true);
            return false;
        }
        if (auto st = save_scene(); !st.ok()) {
            notify(st.error().message, true);
            return false;
        }
    }
    new_scene();
    const bool two_d = l == Lesson::First2D;
    if (two_d) {
        Entity cam;
        scene_->world().each<CameraComponent>([&](Entity e, CameraComponent&) { cam = e; });
        if (!cam.is_null()) {
            auto& c = *scene_->world().get<CameraComponent>(cam);
            c.projection = CameraComponent::Projection::Orthographic;
            c.ortho_height = 10.0f;
            auto& t = *scene_->world().get<Transform>(cam);
            t.position = {0, 0, 10};
            t.rotation = math::Quat{};
        }
        if (!cam.is_null()) {
            // A sky that moves with the camera, behind everything.
            const Entity sky = scene_->create("Sky", cam);
            scene_->world().get<Transform>(sky)->position = {0, 0, -11};
            auto& sp = scene_->world().add<SpriteComponent>(sky);
            sp.size = {40, 14};
            sp.tint = {0.42f, 0.66f, 0.95f, 1};
            sp.layer = -10;
        }
        scene_->update_transforms();
    }
    // Frame the whole level the steps build (x -9..9 in 2D, -11..11 in 3D).
    camera_.cam2d.position = {0, -1};
    camera_.cam2d.zoom = 1.0f;
    camera_.view_height = 11.0f;
    camera_.target = {0, 0, 0};
    camera_.distance = 20.0f;
    scene_->name = two_d ? "My First 2D Game" : "My First 3D Game";
    // A scene file of its own, never over an earlier lesson.
    const std::string base = two_d ? "scenes/lesson_2d" : "scenes/lesson_3d";
    std::string rel = base + std::string(project::kSceneExtension);
    for (int n = 2; std::filesystem::exists(project_->root() / rel) && n < 1000; ++n)
        rel = std::format("{}_{}{}", base, n, project::kSceneExtension);
    if (auto st = save_scene_as(rel); !st.ok()) notify(st.error().message, true);
    lesson_ = l;
    lesson_step_ = 0;
    lesson_won_ = false;
    show_panel(Panel::GettingStarted);
    notify(two_d ? "Lesson started: your first 2D game" : "Lesson started: your first 3D game");
    return true;
}

void Editor::stop_lesson() noexcept {
    lesson_ = Lesson::None;
    lesson_step_ = 0;
    lesson_won_ = false;
}

bool Editor::lesson_step_done(std::size_t i) {
    if (i >= kSteps.size() || !scene_) return false;
    const bool two_d = lesson_ == Lesson::First2D;
    auto& w = scene_->world();
    switch (kSteps[i].goal) {
    case LessonGoal::Floor: return has_floor(*scene_, two_d);
    case LessonGoal::Player:
        return w.count<script::PlatformerMovementComponent>() + w.count<script::TopDownMovementComponent>() > 0;
    case LessonGoal::Coin: return w.count<script::CollectibleComponent>() > 0;
    case LessonGoal::Enemy: {
        bool found = false;
        w.each<script::HazardComponent>([&](Entity e, script::HazardComponent&) {
            if (w.has<script::PatrolComponent>(e) || w.has<script::ChaseComponent>(e)) found = true;
        });
        return found;
    }
    case LessonGoal::Goal: return w.count<script::GoalComponent>() > 0;
    case LessonGoal::Win: return lesson_won_;
    case LessonGoal::Save: return play_state_ == PlayState::Editing && !dirty() && !scene_path_.empty();
    }
    return false;
}

void Editor::update_lesson() {
    if (lesson_ == Lesson::None || !project_) return;
    if (play_state_ != PlayState::Editing && !lesson_won_) {
        Scene& s = active_scene();
        s.world().each<script::GameRulesComponent>([&](script::GameRulesComponent& g) {
            if (g.state == script::GameState::Won) lesson_won_ = true;
        });
        s.world().each<script::GoalComponent>([&](script::GoalComponent& g) {
            if (g.reached) lesson_won_ = true;
        });
    }
    // Edits made while playing are thrown away on Stop, so only the edit scene counts.
    const bool editing = play_state_ == PlayState::Editing;
    while (lesson_step_ < kSteps.size()) {
        const LessonGoal g = kSteps[lesson_step_].goal;
        if (!editing && g != LessonGoal::Win) break;
        if (!lesson_step_done(lesson_step_)) break;
        ++lesson_step_;
        if (lesson_step_ == kSteps.size()) {
            notify("Lesson complete. You made a game!");
            complete_guide_step(GuideStep::AddBehaviour);
        } else if (g == LessonGoal::Win) {
            notify("You won! Press Stop to go back to editing");
        } else {
            notify(std::format("Step done: {}", kSteps[lesson_step_ - 1].title));
        }
    }
}

void Editor::run_lesson_step() {
    if (lesson_ == Lesson::None || lesson_step_ >= kSteps.size() || !project_) return;
    const bool two_d = lesson_ == Lesson::First2D;
    const LessonGoal goal = kSteps[lesson_step_].goal;
    if (goal == LessonGoal::Win) {
        if (play_state_ != PlayState::Playing) play();
        return;
    }
    if (play_state_ != PlayState::Editing) stop();
    if (goal == LessonGoal::Save) {
        if (auto st = save_scene(); !st.ok()) notify(st.error().message, true);
        update_lesson();
        return;
    }
    Scene& s = *scene_;
    auto& w = s.world();
    // Everything sits on one line so the level reads left to right: start, coin, enemy, goal.
    const auto place = [&](Entity e, math::Vec3 p2, math::Vec3 p3) { w.get<Transform>(e)->position = two_d ? p2 : p3; };
    const auto tint = [&](Entity e, math::Color c) {
        if (auto* sp = w.get<SpriteComponent>(e)) sp->tint = c;
        if (auto* m = w.get<MeshRendererComponent>(e)) m->base_color = c;
    };
    const auto make = [&](Primitive p) {
        const Entity e = create_entity(two_d ? Primitive::Sprite : p);
        if (!e.is_null()) select(e);
        return e;
    };
    switch (goal) {
    case LessonGoal::Floor: {
        const Entity e = make(Primitive::Cube);
        if (e.is_null()) return;
        auto before = capture(s, {s.uuid(e)});
        w.get<Name>(e)->value = "Floor";
        place(e, {0, -3.5f, 0}, {0, -0.5f, 0});
        if (two_d) {
            w.get<SpriteComponent>(e)->size = {18, 1};
            w.add<physics::RigidBody2DComponent>(e).type = physics::BodyType::Static;
            w.add<physics::Collider2DComponent>(e).size = {18, 1};
        } else {
            w.get<Transform>(e)->scale = {22, 1, 6};
            w.add<physics::RigidBodyComponent>(e).type = physics::BodyType::Static;
            w.add<physics::ColliderComponent>(e);
        }
        tint(e, {0.3f, 0.62f, 0.32f, 1});
        record_edit("Add Floor", std::move(before));
        break;
    }
    case LessonGoal::Player: {
        const Entity e = make(Primitive::Cube);
        if (e.is_null()) return;
        place(e, {-7, -2.5f, 0}, {-8, 0.5f, 0});
        if (two_d) w.get<SpriteComponent>(e)->size = {0.8f, 1.0f};
        tint(e, {0.25f, 0.5f, 0.95f, 1});
        (void)make_role(e, Role::Player);
        if (!two_d) {
            // Behind and above the player, looking ahead along the level.
            w.each<script::CameraFollowComponent>([](script::CameraFollowComponent& f) { f.offset = {0, 5, 9}; });
        }
        break;
    }
    case LessonGoal::Coin: {
        const Entity e = make(Primitive::Sphere);
        if (e.is_null()) return;
        place(e, {-3.5f, -2.2f, 0}, {-4, 0.8f, 0});
        if (two_d) w.get<SpriteComponent>(e)->size = {0.5f, 0.5f};
        else w.get<Transform>(e)->scale = {0.5f, 0.5f, 0.5f};
        tint(e, {1.0f, 0.82f, 0.15f, 1});
        (void)make_role(e, Role::Coin);
        break;
    }
    case LessonGoal::Enemy: {
        const Entity e = make(Primitive::Cube);
        if (e.is_null()) return;
        place(e, {1, -2.65f, 0}, {0, 0.4f, 0});
        if (two_d) w.get<SpriteComponent>(e)->size = {0.9f, 0.7f};
        else w.get<Transform>(e)->scale = {0.8f, 0.8f, 0.8f};
        tint(e, {0.9f, 0.25f, 0.2f, 1});
        (void)make_role(e, Role::Enemy);
        if (auto* p = w.get<script::PatrolComponent>(e)) p->offset = {3, 0, 0};
        break;
    }
    case LessonGoal::Goal: {
        const Entity e = make(Primitive::Cube);
        if (e.is_null()) return;
        place(e, {7.5f, -2.25f, 0}, {8.5f, 1, 0});
        if (two_d) w.get<SpriteComponent>(e)->size = {0.6f, 1.5f};
        else w.get<Transform>(e)->scale = {0.6f, 2, 0.6f};
        tint(e, {0.3f, 0.95f, 0.45f, 1});
        (void)make_role(e, Role::Goal);
        break;
    }
    case LessonGoal::Win:
    case LessonGoal::Save: break;
    }
    s.update_transforms();
    update_lesson();
}

void Editor::draw_lesson() {
    const bool two_d = lesson_ == Lesson::First2D;
    const std::size_t total = kSteps.size();
    kit::heading(two_d ? "Lesson: your first 2D game" : "Lesson: your first 3D game", tk::kTypeTitle);
    kit::subtitle("Build a small game from nothing. Each step ticks off as soon as the scene has what it asks for, "
                  "or press the button and the Studio does it while you watch.");
    ImGui::Dummy(ImVec2(0, tk::kSpace2));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, lesson_step_ == total ? tk::kSuccess : tk::kAccent);
    ImGui::ProgressBar(static_cast<float>(lesson_step_) / static_cast<float>(total), ImVec2(-1, 0),
                       std::format("{} of {} done", lesson_step_, total).c_str());
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, tk::kSpace3));
    for (std::size_t i = 0; i < total; ++i) {
        const bool ok = i < lesson_step_;
        const bool current = i == lesson_step_;
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushFont(kit::bold_font(), 0.0f);
        ImGui::TextColored(ok ? tk::kSuccess : (current ? tk::kText : tk::kTextDisabled), "%s %zu. %s", ok ? "Done" : "  ",
                           i + 1, kSteps[i].title);
        ImGui::PopFont();
        if (current) {
            ImGui::Indent(tk::kSpace4);
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(tk::kTextMuted, "%s", two_d ? kSteps[i].how_2d : kSteps[i].how_3d);
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, tk::kSpace1));
            if (kit::icon_text_button(kSteps[i].action, kit::Icon::Rocket, true)) run_lesson_step();
            ImGui::Unindent(tk::kSpace4);
            ImGui::Dummy(ImVec2(0, tk::kSpace2));
        }
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, tk::kSpace3));
    if (lesson_step_ == total) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(tk::kSuccess, "You made a game. Change anything you like: colours, speeds, more coins and "
                                         "enemies, or a second level with Goal > Next Scene.");
        ImGui::PopTextWrapPos();
    }
    if (ImGui::Button(lesson_step_ == total ? "Finish lesson" : "Leave lesson")) stop_lesson();
    kit::tooltip("Your scene stays as it is. Start a lesson again from Getting Started");
}

void Editor::draw_lesson_picker() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(tk::kTextMuted, "New here? Build a whole game with a lesson:");
    ImGui::PopTextWrapPos();
    if (ImGui::Button("2D platformer")) (void)start_lesson(Lesson::First2D);
    kit::tooltip("A side-view game: a floor, a player who runs and jumps, a coin, an enemy and a goal");
    ImGui::SameLine();
    if (ImGui::Button("3D adventure")) (void)start_lesson(Lesson::First3D);
    kit::tooltip("The same game in 3D, walked with WASD");
    ImGui::Dummy(ImVec2(0, tk::kSpace3));
}

} // namespace victus::editor
