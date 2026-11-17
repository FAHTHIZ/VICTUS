// SPDX-License-Identifier: Apache-2.0
// AI Debugger window (View > AI Debug, play sessions): the selected entity's behaviour tree with
// the status of each node on the last tick, its blackboard and what its perception notices.
#include "victus/editor/editor.hpp"

#include <imgui.h>

#include <format>

namespace victus::editor {

using namespace scene;

namespace {
ImVec4 status_color(ai::NodeStatus s, bool active) {
    if (!active) return {0.55f, 0.55f, 0.55f, 1};
    switch (s) {
    case ai::NodeStatus::Running: return {1.0f, 0.8f, 0.25f, 1};
    case ai::NodeStatus::Success: return {0.4f, 0.9f, 0.45f, 1};
    case ai::NodeStatus::Failure: return {1.0f, 0.4f, 0.35f, 1};
    default: return {0.7f, 0.7f, 0.7f, 1};
    }
}
} // namespace

std::vector<std::string> Editor::ai_debug_lines(Entity e) {
    std::vector<std::string> out;
    if (!play_ai_ || play_state_ == PlayState::Editing) return out;
    Scene& s = active_scene();
    if (auto d = play_ai_->debug(s, e)) {
        out.push_back(std::format("tree {} ticks {} completions {} root {}", d->tree, d->ticks, d->completions,
                                  ai::status_name(d->last_root)));
        if (!d->error.empty()) out.push_back("error " + d->error);
        for (const auto& n : d->nodes)
            out.push_back(std::format("{}{} [{}]", std::string(n.depth * 2, ' '), n.label,
                                      n.active ? ai::status_name(n.status) : "-"));
    }
    if (const ai::Blackboard* bb = play_ai_->blackboard(s, e))
        for (const auto& [k, v] : bb->entries()) out.push_back(std::format("bb {} = {}", k, ai::to_string(v)));
    return out;
}

void Editor::draw_ai_debugger() {
    if (!show_ai_ || play_state_ == PlayState::Editing || !play_ai_) return;
    if (!ImGui::Begin("AI Debugger", &show_ai_)) {
        ImGui::End();
        return;
    }
    Scene& s = active_scene();
    Entity e = kNullEntity;
    if (!selection_.empty()) e = s.find(selection_.front());
    const auto debug = e.is_null() ? std::nullopt : play_ai_->debug(s, e);
    const auto* perception = e.is_null() ? nullptr : play_ai_->perception(s, e);
    if (e.is_null() || (!debug && !perception)) {
        ImGui::TextDisabled("Select an entity with a BehaviorTree or Perception component.");
        ImGui::Text("%u trees, %u perceivers", play_ai_->stats().trees, play_ai_->stats().perceivers);
        ImGui::End();
        return;
    }
    if (debug) {
        ImGui::Text("%s", debug->tree.c_str());
        ImGui::TextDisabled("ticks %llu, completions %u, last root %s", static_cast<unsigned long long>(debug->ticks),
                            debug->completions, std::string(ai::status_name(debug->last_root)).c_str());
        if (!debug->error.empty()) ImGui::TextColored({1.0f, 0.4f, 0.35f, 1}, "%s", debug->error.c_str());
        if (ImGui::CollapsingHeader("Tree", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const auto& n : debug->nodes) {
                ImGui::Indent(static_cast<float>(n.depth) * 14.0f + 1.0f);
                ImGui::TextColored(status_color(n.status, n.active), "%s%s", n.label.c_str(),
                                   n.active ? std::format("  [{}]", ai::status_name(n.status)).c_str() : "");
                ImGui::Unindent(static_cast<float>(n.depth) * 14.0f + 1.0f);
            }
        }
    }
    if (const ai::Blackboard* bb = play_ai_->blackboard(s, e); bb && ImGui::CollapsingHeader("Blackboard", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (bb->entries().empty()) ImGui::TextDisabled("(empty)");
        for (const auto& [k, v] : bb->entries()) ImGui::Text("%s = %s", k.c_str(), ai::to_string(v).c_str());
    }
    if (perception && ImGui::CollapsingHeader("Perception", ImGuiTreeNodeFlags_DefaultOpen)) {
        const Name* tn = perception->target.is_null() ? nullptr : s.world().get<Name>(perception->target);
        ImGui::Text("target: %s%s", perception->target.is_null() ? "none" : (tn ? tn->value.c_str() : "?"),
                    perception->target.is_null() ? "" : (perception->visible ? " (visible)" : " (remembered)"));
        ImGui::Text("seen: %zu", perception->seen.size());
        if (perception->heard)
            ImGui::Text("heard at (%.1f, %.1f, %.1f)", perception->noise_position.x, perception->noise_position.y,
                        perception->noise_position.z);
    }
    ImGui::End();
}

} // namespace victus::editor
