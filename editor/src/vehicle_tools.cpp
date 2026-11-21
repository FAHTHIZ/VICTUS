// SPDX-License-Identifier: Apache-2.0
// The Inspector's "Drivable vehicle" card (ADR-0166): one undoable step turns a box or an
// imported car (wheel nodes named like "Wheel FL") into a VehicleComponent with VehicleWheel
// children (physics::make_vehicle), a toggle for keyboard/gamepad driving, and a live readout
// of speed, gear, engine rpm and wheel contacts in play mode.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <string>

namespace victus::editor {

namespace tk = ui::tokens;
using namespace scene;

namespace {

bool wheel_like(const Scene& s, Entity e) {
    for (const Entity c : s.children(e)) {
        if (s.world().get<physics::VehicleWheelComponent>(c)) return true;
        const auto* n = s.world().get<Name>(c);
        if (!n) continue;
        std::string v = n->value;
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (v.find("wheel") != std::string::npos || v.find("tire") != std::string::npos || v.find("tyre") != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

std::size_t Editor::make_vehicle(Entity chassis) {
    Scene& s = active_scene();
    if (!s.world().alive(chassis)) return 0;
    const std::vector<EntityState> before = capture_subtrees(s, {s.uuid(chassis)});
    const std::size_t wheels = physics::make_vehicle(s, chassis);
    s.update_transforms();
    if (play_state_ == PlayState::Editing) {
        // Wheels created here did not exist before: undo removes them.
        auto after = capture_subtrees(s, {s.uuid(chassis)});
        std::vector<EntityState> prior;
        prior.reserve(after.size());
        for (const EntityState& a : after) {
            const auto it = std::find_if(before.begin(), before.end(), [&](const EntityState& b) { return b.uuid == a.uuid; });
            prior.push_back(it != before.end() ? *it : EntityState{a.uuid, std::nullopt});
        }
        undo_.push({"Make Vehicle", std::move(prior), std::move(after)});
        update_title();
    }
    notify(std::format("Vehicle ready with {} wheels. Press Play and drive with W/A/S/D (Space is the handbrake).", wheels));
    return wheels;
}

bool Editor::set_vehicle_player_input(Entity chassis, bool on) {
    Scene& s = active_scene();
    auto* v = s.world().alive(chassis) ? s.world().get<physics::VehicleComponent>(chassis) : nullptr;
    if (!v) return false;
    if (v->player_input == on) return true;
    auto before = capture(s, {s.uuid(chassis)});
    v->player_input = on;
    record_edit(on ? "Drive With Keyboard On" : "Drive With Keyboard Off", std::move(before));
    return true;
}

void Editor::draw_vehicle_section(Entity e) {
    Scene& s = active_scene();
    auto& w = s.world();
    auto* vc = w.get<physics::VehicleComponent>(e);
    const auto* rb = w.get<physics::RigidBodyComponent>(e);
    const bool candidate = wheel_like(s, e) || (w.get<physics::ColliderComponent>(e) && rb && rb->type == physics::BodyType::Dynamic);
    if (!vc && !candidate) return;
    ImGui::PushID("##vehicle_card");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, tk::kSurfaceRaised);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, tk::kRadius);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(tk::kSpace4, tk::kSpace3));
    ImGui::BeginChild("##vehicle", ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
    kit::icon(kit::Icon::Play, tk::kAccent);
    ImGui::SameLine();
    ImGui::PushFont(kit::bold_font(), 0.0f);
    ImGui::TextUnformatted("Drivable vehicle");
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0.0f);
    if (!vc) {
        ImGui::TextColored(tk::kTextMuted, "Give this object wheels, suspension, an engine and a gearbox so it can be driven.");
        ImGui::PopTextWrapPos();
        if (kit::icon_text_button("Make Vehicle", kit::Icon::Plus, true, true, ImVec2(-1, 0))) (void)make_vehicle(e);
        kit::tooltip("Uses child objects named like a wheel (Wheel FL, tire_r...) or adds four wheels at the corners. "
                     "Front wheels steer. Undo removes it again.");
    } else {
        std::size_t wheels = 0;
        for (const Entity c : s.children(e))
            if (w.get<physics::VehicleWheelComponent>(c)) ++wheels;
        ImGui::SameLine();
        kit::pill(std::format("{} wheels", wheels), wheels > 0 ? ui::Tone::Success : ui::Tone::Warning);
        if (wheels == 0) ImGui::TextColored(tk::kWarning, "No wheels yet: press Make Vehicle or add VehicleWheel to child objects.");
        if (play_state_ != PlayState::Editing && play_physics_) {
            const physics::VehicleState st = play_physics_->vehicle_state(e);
            if (st.valid)
                ImGui::Text("%.0f km/h   gear %d   %.0f rpm   %u/%zu wheels on the ground", std::abs(st.speed) * 3.6f, st.gear,
                            st.engine_rpm, st.wheels_in_contact, st.wheels.size());
            else
                ImGui::TextColored(tk::kWarning, "Not simulated: %s", st.error.c_str());
        } else if (play_state_ == PlayState::Editing) {
            ImGui::TextColored(tk::kInfo, vc->player_input ? "Press Play and drive with W/A/S/D, Space is the handbrake."
                                                           : "Press Play; scripts drive it with entity:set_vehicle_input().");
        }
        ImGui::PopTextWrapPos();
        bool drive = vc->player_input;
        if (ImGui::Checkbox("Drive with keyboard and gamepad", &drive)) (void)set_vehicle_player_input(e, drive);
        kit::tooltip("W/S or the triggers accelerate and brake, A/D or the left stick steer, Space or A is the handbrake.");
        if (wheels == 0) {
            ImGui::SameLine();
            if (kit::icon_text_button("Make Vehicle", kit::Icon::Plus, true)) (void)make_vehicle(e);
        }
        ImGui::TextDisabled("Tune it in the Vehicle and VehicleWheel components; scripts: entity:vehicle_state()");
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::PopID();
    ImGui::Spacing();
}

} // namespace victus::editor
