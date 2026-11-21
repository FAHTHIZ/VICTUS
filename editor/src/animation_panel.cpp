// SPDX-License-Identifier: Apache-2.0
// Animation window (View > Animation): the selected Animator's controller (.vanim, ADR-0068) —
// current state, cross-fade, clip weights and parameter controls. During play it shows and drives
// the live session; while editing, Preview runs the controller on the selected character in place
// and restores its transforms when the preview ends (never saved, autosaved or played).
#include "victus/editor/editor.hpp"

#include <imgui.h>

#include <algorithm>
#include <format>

namespace victus::editor {

using namespace scene;

namespace {

// Range a float/int parameter's control spans: the thresholds of blend spaces that use it,
// widened a little; otherwise 0..1.
std::pair<float, float> param_range(const animation::Controller* c, const std::string& name) {
    float lo = 0, hi = 1;
    bool any = false;
    if (!c) return {lo, hi};
    for (const animation::ControllerState& st : c->states) {
        const bool x = st.param_x == name, y = st.param_y == name;
        if (!x && !y) continue;
        for (const animation::BlendPoint& p : st.points) {
            const float v = x ? p.at.x : p.at.y;
            lo = any ? std::min(lo, v) : v;
            hi = any ? std::max(hi, v) : v;
            any = true;
        }
    }
    if (any && hi - lo < 1e-3f) hi = lo + 1;
    if (any) {
        const float pad = (hi - lo) * 0.1f;
        lo -= pad;
        hi += pad;
    }
    return {lo, hi};
}

void collect_descendants(Scene& s, Entity e, std::vector<Entity>& out) {
    out.push_back(e);
    for (const Entity c : s.children(e)) collect_descendants(s, c, out);
}

} // namespace

animation::AnimationSystem* Editor::animation_inspected() {
    if (play_scene_) return play_animation_.get();
    return anim_backup_ ? edit_animation_.get() : nullptr;
}

Entity Editor::animation_preview_target() {
    if (!anim_backup_) return kNullEntity;
    const Entity e = edit_scene().find(anim_backup_->target);
    return !e.is_null() && edit_scene().world().get<animation::AnimatorComponent>(e) ? e : kNullEntity;
}

bool Editor::begin_animation_preview(Entity e) {
    end_animation_preview();
    if (play_scene_ || play_state_ != PlayState::Editing) return false;
    Scene& s = edit_scene();
    if (e.is_null() || !s.world().alive(e) || !s.world().get<animation::AnimatorComponent>(e)) return false;
    AnimBackup b;
    b.target = s.uuid(e);
    b.animator = *s.world().get<animation::AnimatorComponent>(e);
    std::vector<Entity> nodes;
    collect_descendants(s, e, nodes);
    for (const Entity n : nodes)
        if (const auto* t = s.world().get<Transform>(n)) b.transforms.emplace_back(s.uuid(n), *t);
    anim_backup_ = std::move(b);
    // A fresh system per preview: it reads the controller and model as they are now.
    edit_animation_ = std::make_unique<animation::AnimationSystem>(animation::AnimationSettings{
        .provider = db_ ? assets::animation_provider(*db_) : animation::SetProvider{},
        .controllers = db_ ? animation::directory_loader(db_->assets_dir()) : animation::ControllerLoader{},
    });
    anim_preview_paused_ = false;
    (void)edit_animation_->update_entity(s, e, 0.0f);
    return true;
}

void Editor::end_animation_preview() {
    if (!anim_backup_) {
        edit_animation_.reset();
        return;
    }
    Scene& s = edit_scene();
    for (const auto& [id, t] : anim_backup_->transforms) {
        const Entity n = s.find(id);
        if (auto* tr = n.is_null() ? nullptr : s.world().get<Transform>(n)) *tr = t;
    }
    const Entity e = s.find(anim_backup_->target);
    if (auto* a = e.is_null() ? nullptr : s.world().get<animation::AnimatorComponent>(e)) *a = anim_backup_->animator;
    anim_backup_.reset();
    edit_animation_.reset();
}

void Editor::update_animation_preview(float dt) {
    if (!anim_backup_) return;
    if (play_scene_ || play_state_ != PlayState::Editing) {
        end_animation_preview();
        return;
    }
    const Entity e = animation_preview_target();
    if (e.is_null()) { // deleted (or its Animator removed): nothing left to restore it into
        anim_backup_.reset();
        edit_animation_.reset();
        return;
    }
    (void)edit_animation_->update_entity(edit_scene(), e, anim_preview_paused_ ? 0.0f : dt);
}

bool Editor::set_animation_parameter(Entity e, std::string_view name, float value) {
    animation::AnimationSystem* sys = animation_inspected();
    return sys && sys->set_parameter(active_scene(), e, name, value);
}

bool Editor::fire_animation_trigger(Entity e, std::string_view name) {
    animation::AnimationSystem* sys = animation_inspected();
    return sys && sys->set_trigger(active_scene(), e, name);
}

bool Editor::set_animation_layer_weight(Entity e, std::string_view layer, float weight) {
    animation::AnimationSystem* sys = animation_inspected();
    return sys && sys->set_layer_weight(active_scene(), e, layer, std::clamp(weight, 0.0f, 1.0f));
}

bool Editor::enter_animation_state(Entity e, std::string_view state) {
    animation::AnimationSystem* sys = animation_inspected();
    if (!sys) return false;
    const animation::ControllerStatus st = sys->controller_status(active_scene(), e);
    if (!st.valid || std::find(st.states.begin(), st.states.end(), state) == st.states.end()) return false;
    return sys->play(active_scene(), e, state);
}

std::vector<std::string> Editor::animation_debug_lines(Entity e) {
    std::vector<std::string> out;
    animation::AnimationSystem* sys = animation_inspected();
    if (!sys || e.is_null()) return out;
    const animation::ControllerStatus st = sys->controller_status(active_scene(), e);
    if (!st.valid) {
        if (!st.error.empty()) out.push_back("error " + st.error);
        return out;
    }
    out.push_back(std::format("controller {} state {} time {:.2f}", st.controller, st.state, st.normalized_time));
    if (!st.previous_state.empty()) out.push_back(std::format("fade from {} {:.0f}%", st.previous_state, st.blend * 100.0f));
    for (const auto& w : st.weights)
        out.push_back(std::format("weight {} {:.2f}{}", w.clip, w.weight, w.missing ? " (missing)" : ""));
    for (const auto& p : st.parameters)
        out.push_back(std::format("param {} {} = {}", p.name, animation::param_type_name(p.type),
                                  p.type == animation::ParamType::Float ? std::format("{:.2f}", p.value)
                                                                        : std::format("{}", static_cast<int>(p.value))));
    for (const auto& l : st.layers)
        out.push_back(std::format("layer {} {} state {} weight {:.2f} nodes {}", l.name, animation::layer_blend_name(l.blend),
                                  l.state, l.effective_weight, l.affected_nodes));
    return out;
}

void Editor::draw_animation_panel() {
    if (!show_animation_) return;
    ImGui::SetNextWindowSize(ImVec2(360, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Animation", &show_animation_)) {
        ImGui::End();
        return;
    }
    Scene& s = active_scene();
    Entity e = kNullEntity;
    if (anim_backup_ && !play_scene_) e = animation_preview_target();
    else if (!selection_.empty()) e = s.find(selection_.front());
    const auto* a = e.is_null() ? nullptr : s.world().get<animation::AnimatorComponent>(e);
    if (!a) {
        ImGui::TextDisabled("Select an entity with an Animator.");
        ImGui::End();
        return;
    }
    const Name* nm = s.world().get<Name>(e);
    ImGui::Text("%s", nm ? nm->value.c_str() : "(entity)");
    if (a->controller.empty()) {
        ImGui::TextDisabled("No controller: set the Animator's controller to a .vanim asset");
        ImGui::TextDisabled("(drag one from the Assets panel onto the field).");
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("%s", a->controller.c_str());
    if (!play_scene_) {
        if (!anim_backup_) {
            if (ImGui::Button("Preview")) (void)begin_animation_preview(e);
            ImGui::SameLine();
            ImGui::TextDisabled("runs the controller here; the pose is restored afterwards");
        } else {
            if (ImGui::Button("End Preview")) end_animation_preview();
            ImGui::SameLine();
            ImGui::Checkbox("Pause", &anim_preview_paused_);
            ImGui::SameLine();
            if (ImGui::Button("Restart")) (void)begin_animation_preview(e);
        }
    }
    animation::AnimationSystem* sys = animation_inspected();
    if (!sys) {
        ImGui::End();
        return;
    }
    const animation::ControllerStatus st = sys->controller_status(s, e);
    if (!st.valid) {
        ImGui::TextColored({1.0f, 0.4f, 0.35f, 1}, "%s", st.error.empty() ? "controller not loaded" : st.error.c_str());
        ImGui::End();
        return;
    }
    const auto ctrl = sys->controller(a->controller);
    if (ImGui::CollapsingHeader("States", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (std::size_t i = 0; i < st.states.size(); ++i) {
            const std::string& name = st.states[i];
            const bool current = name == st.state;
            const bool root = i < st.root_motion_states.size() && st.root_motion_states[i] != 0;
            if (ImGui::Selectable(std::format("{}{}{}", name, root ? "  [root motion]" : "",
                                              current ? std::format("  {:.2f}", st.normalized_time) : "")
                                      .c_str(),
                                  current))
                (void)enter_animation_state(e, name);
        }
        if (!st.root_node.empty()) {
            const animation::RootMotion rm = sys->root_motion(s, e);
            ImGui::TextDisabled("root node %s%s", st.root_node.c_str(),
                                rm.active ? std::format("  moving {:.3f} m/frame", length(rm.position)).c_str() : "");
        }
        if (!st.previous_state.empty()) {
            ImGui::TextDisabled("fading from %s", st.previous_state.c_str());
            ImGui::ProgressBar(st.blend, ImVec2(-1, 0));
        }
        ImGui::TextDisabled("Click a state to jump to it.");
    }
    if (ImGui::CollapsingHeader("Clip Weights", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const auto& w : st.weights) {
            ImGui::ProgressBar(w.weight, ImVec2(140, 0), std::format("{:.2f}", w.weight).c_str());
            ImGui::SameLine();
            if (w.missing) ImGui::TextColored({1.0f, 0.65f, 0.25f, 1}, "%s (missing: rest pose)", w.clip.c_str());
            else ImGui::Text("%s", w.clip.c_str());
        }
    }
    if (!st.layers.empty() && ImGui::CollapsingHeader("Layers", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (std::size_t i = 0; i < st.layers.size(); ++i) {
            const auto& l = st.layers[i];
            const animation::ControllerLayer* def = ctrl && i < ctrl->layers.size() ? &ctrl->layers[i] : nullptr;
            ImGui::PushID(static_cast<int>(i));
            ImGui::SeparatorText(std::format("{}  ({}{})", l.name, animation::layer_blend_name(l.blend),
                                             def && def->sync ? ", synced to base" : "")
                                     .c_str());
            float w = l.weight;
            if (ImGui::SliderFloat("weight", &w, 0.0f, 1.0f, "%.2f")) (void)set_animation_layer_weight(e, l.name, w);
            if (def && !def->weight_parameter.empty())
                ImGui::TextDisabled("x %s = effective %.2f", def->weight_parameter.c_str(), l.effective_weight);
            ImGui::TextDisabled("state %s%s  %.2f  -  %u bones affected", l.state.empty() ? "(none)" : l.state.c_str(),
                                l.previous_state.empty() ? "" : std::format(" (from {})", l.previous_state).c_str(),
                                l.normalized_time, l.affected_nodes);
            if (!l.previous_state.empty()) ImGui::ProgressBar(l.blend_progress, ImVec2(-1, 0));
            if (def) {
                if (def->mask.empty() && def->mask_weights.empty()) ImGui::TextDisabled("mask: every animated bone");
                for (const std::string& n : def->mask) {
                    ImGui::ProgressBar(1.0f, ImVec2(90, 0), "1.00");
                    ImGui::SameLine();
                    ImGui::Text("%s", n.c_str());
                }
                for (const auto& [n, mw] : def->mask_weights) {
                    ImGui::ProgressBar(mw, ImVec2(90, 0), std::format("{:.2f}", mw).c_str());
                    ImGui::SameLine();
                    ImGui::Text("%s", n.c_str());
                }
                for (const std::string& n : def->exclude) ImGui::TextDisabled("excluded: %s", n.c_str());
            }
            ImGui::PopID();
        }
        ImGui::TextDisabled("Bars show how strongly each listed bone and its children follow the layer.");
    }
    if (ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const auto& p : st.parameters) {
            ImGui::PushID(p.name.c_str());
            switch (p.type) {
            case animation::ParamType::Float: {
                float v = p.value;
                const auto [lo, hi] = param_range(ctrl.get(), p.name);
                if (ImGui::SliderFloat(p.name.c_str(), &v, lo, hi, "%.2f")) (void)set_animation_parameter(e, p.name, v);
                break;
            }
            case animation::ParamType::Int: {
                int v = static_cast<int>(p.value);
                if (ImGui::DragInt(p.name.c_str(), &v, 0.1f)) (void)set_animation_parameter(e, p.name, static_cast<float>(v));
                break;
            }
            case animation::ParamType::Bool: {
                bool v = p.value != 0.0f;
                if (ImGui::Checkbox(p.name.c_str(), &v)) (void)set_animation_parameter(e, p.name, v ? 1.0f : 0.0f);
                break;
            }
            case animation::ParamType::Trigger:
                if (ImGui::Button(std::format("Fire {}", p.name).c_str())) (void)fire_animation_trigger(e, p.name);
                if (p.value != 0.0f) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(set)");
                }
                break;
            }
            ImGui::PopID();
        }
        if (play_scene_) ImGui::TextDisabled("Scripts may overwrite these every frame.");
    }
    ImGui::End();
}

} // namespace victus::editor
