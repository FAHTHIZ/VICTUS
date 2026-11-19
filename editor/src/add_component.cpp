// SPDX-License-Identifier: Apache-2.0
// Inspector > Add Component: one searchable list of every component, grouped by category, each
// with a one-line description written for someone new to game engines.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include "victus/gameplay/gameplay.hpp"
#include "victus/scene/reflect.hpp"
#include "victus/ui/style.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <string_view>

namespace victus::editor {

namespace tk = ui::tokens;
using namespace scene;

namespace {

struct Blurb {
    std::string_view id, description;
};

// Plain-language descriptions by serialized component name.
constexpr Blurb kBlurbs[] = {
    {"Camera", "A view the game can look through"},
    {"Light", "Gives off light: a sun, a bulb or a spot"},
    {"MeshRenderer", "Draws a 3D shape or model here"},
    {"Sprite", "Draws a 2D image here"},
    {"Rotator", "Spins the entity at a steady speed, no code needed"},
    {"Oscillator", "Moves the entity back and forth, no code needed"},
    {"PlatformerMovement", "Run and jump like a platformer hero (arrow keys / WASD + Space), no code needed"},
    {"TopDownMovement", "Walk in any direction, seen from above or in 3D, no code needed"},
    {"CameraFollow", "Makes a camera follow the player smoothly"},
    {"Health", "Hit points: take damage, flash, respawn or disappear at zero"},
    {"Hazard", "Hurts the player on touch: spikes, lava, enemies (stomp them if stompable)"},
    {"Collectible", "A pickup: coins for score, hearts for health or items for the inventory"},
    {"Spawner", "Creates copies of a prefab every few seconds"},
    {"Patrol", "Walks back and forth between two points"},
    {"MovingPlatform", "Travels back and forth and carries whoever stands on it: lifts, moving ledges, saws"},
    {"Chase", "Follows the player when they come close"},
    {"Projectile", "Flies straight and damages what it hits"},
    {"Shooter", "Fires projectiles when you press fire, or by itself like a turret"},
    {"Lifetime", "Removes the entity after a few seconds"},
    {"Checkpoint", "Touch it to respawn here instead of at the start"},
    {"Goal", "The end of the level: win or go to the next scene"},
    {"GameRules", "Score, lives, a timer, win and game over, with an on-screen HUD"},
    {"Script", "Runs a Luau or visual script on this entity"},
    {"Animator", "Plays animations from an animation controller (.vanim)"},
    {"AudioListener", "The ears of the scene, usually on the camera or player"},
    {"AudioSource", "Plays a sound from where this entity is"},
    {"BehaviorTree", "Makes AI decisions from a behaviour tree"},
    {"CharacterController", "Walks, jumps and climbs steps without tipping over"},
    {"CharacterController2D", "Platformer movement for a 2D character"},
    {"Collider", "A solid shape that things bump into or pass through as a trigger"},
    {"Collider2D", "A 2D shape that things bump into or pass through as a trigger"},
    {"Decal", "Projects a texture onto surfaces, like a sticker"},
    {"Environment", "Sky, ambient light, fog and screen effects for the scene"},
    {"FogVolume", "A box of extra volumetric fog, like mist in a valley"},
    {"IKConstraint", "Points a limb or head at a target, e.g. feet on the ground"},
    {"IrradianceVolume", "Soft bounced light for an area"},
    {"Joint", "Links two physics bodies with a hinge, spring or fixed joint"},
    {"Joint2D", "Links two 2D physics bodies with a hinge, spring or fixed joint"},
    {"Light2D", "Lights sprites in a 2D scene"},
    {"MaterialOverrides", "Changes material colours and values just for this entity"},
    {"NavAgent", "Finds paths around obstacles and walks them"},
    {"NavMeshSurface", "Marks the ground AI can walk on"},
    {"NetworkIdentity", "Shares this entity with the other players online"},
    {"ParticleEmitter", "Sparks, smoke, fire, dust and other effects"},
    {"Perceptible", "Lets AI with Perception see or hear this entity"},
    {"Perception", "Lets AI see and hear what is around it"},
    {"ReflectionProbe", "Captures the surroundings for shiny reflections"},
    {"RigidBody", "Falls, bounces and gets pushed by physics"},
    {"RigidBody2D", "Falls, bounces and gets pushed by 2D physics"},
    {"SequencePlayer", "Plays a cinematic sequence (.vseq)"},
    {"SkinnedMesh", "Bends a mesh with a skeleton for character animation"},
    {"SpriteAnimator", "Flipbook animation from a sprite sheet"},
    {"UiButton", "A button players can click or tap"},
    {"UiCanvas", "The root of an on-screen interface"},
    {"UiImage", "An image in the interface"},
    {"UiLayout", "Arranges child elements in a row or column"},
    {"UiPanel", "A background panel for other elements"},
    {"UiRect", "Position and size on the screen"},
    {"UiScroll", "An area that scrolls when its content is too big"},
    {"UiText", "Text on the screen"},
};

std::string_view blurb(std::string_view id) {
    for (const auto& b : kBlurbs)
        if (b.id == id) return b.description;
    return {};
}

} // namespace

std::vector<ComponentChoice> Editor::component_choices(Entity e) const {
    std::vector<ComponentChoice> out;
    const Scene& s = const_cast<Editor*>(this)->active_scene();
    const ecs::World& w = s.world();
    const auto builtin = [&](std::string id, std::string name, std::string category, bool present) {
        out.push_back({id, std::move(name), std::move(category), std::string(blurb(id)), present});
    };
    builtin("MeshRenderer", "Mesh Renderer", "Rendering", w.has<MeshRendererComponent>(e));
    builtin("Sprite", "Sprite", "Rendering", w.has<SpriteComponent>(e));
    builtin("Light", "Light", "Rendering", w.has<LightComponent>(e));
    builtin("Camera", "Camera", "Rendering", w.has<CameraComponent>(e));
    builtin("Script", "Script", "Scripting", w.has<script::ScriptComponent>(e));
    builtin("Rotator", "Rotator", "Motion", w.has<gameplay::RotatorComponent>(e));
    builtin("Oscillator", "Oscillator", "Motion", w.has<gameplay::OscillatorComponent>(e));
    for (const auto* schema : scene::schemas())
        out.push_back({schema->name, kit::humanize(schema->name), schema->category, std::string(blurb(schema->name)),
                       w.get_raw(e, schema->id) != nullptr});
    // Categories in a fixed, beginner-first order; anything new goes last, alphabetically.
    static constexpr std::string_view order[] = {"Rendering", "Behaviours", "Scripting",  "Motion",    "Physics 3D", "Physics 2D",
                                                 "Animation", "Audio",      "Effects",   "AI",         "Navigation",
                                                 "UI",        "Cinematics", "Networking"};
    const auto rank = [](const std::string& c) {
        const auto it = std::find(std::begin(order), std::end(order), c);
        return static_cast<std::size_t>(it - std::begin(order));
    };
    std::stable_sort(out.begin(), out.end(), [&](const ComponentChoice& a, const ComponentChoice& b) {
        const auto ra = rank(a.category), rb = rank(b.category);
        return ra != rb ? ra < rb : (ra == std::size(order) && a.category < b.category);
    });
    return out;
}

bool Editor::add_component_raw(Entity e, std::string_view id) {
    ecs::World& w = active_scene().world();
    if (id == "MeshRenderer") {
        if (w.has<MeshRendererComponent>(e)) return false;
        w.add<MeshRendererComponent>(e).primitive = "cube";
    } else if (id == "Sprite") {
        if (w.has<SpriteComponent>(e)) return false;
        w.add<SpriteComponent>(e);
    } else if (id == "Light") {
        if (w.has<LightComponent>(e)) return false;
        w.add<LightComponent>(e);
    } else if (id == "Camera") {
        if (w.has<CameraComponent>(e)) return false;
        w.add<CameraComponent>(e).primary = false;
    } else if (id == "Script") {
        if (w.has<script::ScriptComponent>(e)) return false;
        w.add<script::ScriptComponent>(e);
    } else if (id == "Rotator") {
        if (w.has<gameplay::RotatorComponent>(e)) return false;
        w.add<gameplay::RotatorComponent>(e);
    } else if (id == "Oscillator") {
        if (w.has<gameplay::OscillatorComponent>(e)) return false;
        w.add<gameplay::OscillatorComponent>(e);
    } else {
        const auto* schema = scene::find_schema(id);
        if (!schema || w.get_raw(e, schema->id) != nullptr) return false;
        (void)w.add_raw(e, schema->id);
    }
    return true;
}

bool Editor::add_component(Entity e, std::string_view id) {
    Scene& s = active_scene();
    if (e.is_null() || !s.world().alive(e)) return false;
    auto before = capture(s, {s.uuid(e)});
    if (!add_component_raw(e, id)) return false;
    record_edit("Add " + kit::humanize(id), std::move(before));
    if (const auto* schema = scene::find_schema(id); schema && schema->category == "Behaviours")
        complete_guide_step(GuideStep::AddBehaviour);
    return true;
}

bool Editor::draw_add_component(Entity e) {
    bool changed = false;
    ImGui::Dummy(ImVec2(0, tk::kSpace3));
    bool auto_open = false; // open_add_component(): after the workspace settles
    if (add_component_open_ > 0) auto_open = --add_component_open_ == 0;
    if (kit::icon_text_button("Add Component", kit::Icon::Plus, false, true,
                              ImVec2(-1, ImGui::GetFrameHeight() + tk::kSpace2)) ||
        auto_open) {
        add_component_filter_.clear();
        ImGui::OpenPopup("##addcomp");
    }
    kit::tooltip("Give this entity more abilities: physics, sound, AI, effects, UI and more");
    ImGui::SetNextWindowSizeConstraints(ImVec2(340, 0), ImVec2(420, 480));
    if (!ImGui::BeginPopup("##addcomp")) return false;
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    kit::search_input("addcomp_search", "Search components", add_component_filter_);
    const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    ImGui::Separator();
    ImGui::BeginChild("##addcomp_list", ImVec2(0, std::min(400.0f, ImGui::GetTextLineHeightWithSpacing() * 22)),
                      ImGuiChildFlags_None);
    const auto choices = component_choices(e);
    std::string last_category;
    std::size_t shown = 0;
    bool done = false;
    for (const auto& c : choices) {
        if (c.present) continue;
        if (!kit::contains_icase(c.name, add_component_filter_) &&
            !kit::contains_icase(c.category, add_component_filter_) &&
            !kit::contains_icase(c.description, add_component_filter_) &&
            !kit::contains_icase(c.id, add_component_filter_))
            continue;
        if (c.category != last_category) {
            ImGui::SeparatorText(c.category.c_str());
            last_category = c.category;
        }
        ImGui::PushID(c.id.c_str());
        const float line = ImGui::GetTextLineHeight();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const bool two = !c.description.empty();
        const bool clicked =
            ImGui::Selectable("##pick", shown == 0 && !add_component_filter_.empty(), ImGuiSelectableFlags_None,
                              ImVec2(0, two ? line * 2 + tk::kSpace1 : line));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(kit::bold_font(), ImGui::GetFontSize(), pos, ImGui::GetColorU32(tk::kText), c.name.c_str());
        if (two)
            dl->AddText(nullptr, ImGui::GetFontSize() * 0.92f, ImVec2(pos.x, pos.y + line + tk::kSpace1),
                        ImGui::GetColorU32(tk::kTextMuted), c.description.c_str());
        if ((clicked || (enter && shown == 0)) && !done) {
            changed |= add_component_raw(e, c.id);
            done = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopID();
        ++shown;
    }
    if (shown == 0) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(tk::kTextMuted,
                           add_component_filter_.empty() ? "This entity already has every component."
                                                         : "No component matches \"%s\".",
                           add_component_filter_.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return changed;
}

} // namespace victus::editor
