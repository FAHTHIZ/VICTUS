// SPDX-License-Identifier: Apache-2.0
// Problems panel: the project validation `victus check` runs (player::validate_project), shown
// inside the Studio with filters and a way to jump to each problem.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include "victus/ui/style.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <optional>

namespace victus::editor {

namespace tk = ui::tokens;
using player::Category;
using player::Diagnostic;
using player::Severity;

namespace {

bool contains_icase(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    const auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != hay.end();
}

kit::Icon category_icon(Category c) {
    switch (c) {
    case Category::Project:
        return kit::Icon::Gear;
    case Category::Script:
        return kit::Icon::Script;
    case Category::Material:
        return kit::Icon::Material;
    case Category::Shader:
        return kit::Icon::Shader;
    case Category::SpriteSheet:
        return kit::Icon::Sprite;
    case Category::Tileset:
        return kit::Icon::Tileset;
    case Category::Terrain:
        return kit::Icon::Terrain;
    case Category::BehaviorTree:
        return kit::Icon::Tree;
    case Category::Sequence:
        return kit::Icon::Sequence;
    case Category::AnimController:
        return kit::Icon::Animation;
    case Category::Prefab:
        return kit::Icon::Prefab;
    case Category::Scene:
        return kit::Icon::Scene;
    case Category::Entity:
        return kit::Icon::Entity;
    }
    return kit::Icon::File;
}

// What "Go to" does for a problem, as a button label; empty when only copying makes sense.
std::string go_to_label(const Diagnostic& d) {
    if ((!d.entity_uuid.is_nil() || !d.entity_name.empty()) && d.path.ends_with(project::kSceneExtension))
        return "Select entity";
    if (d.category == Category::Material && !d.asset.is_nil()) return "Open material";
    if (d.path.ends_with(".vgraph") || d.path.ends_with(".vmatgraph")) return "Open graph";
    if (d.category == Category::Sequence && !d.path.empty()) return "Open sequence";
    if (d.category == Category::Scene && d.path.ends_with(project::kSceneExtension)) return "Open scene";
    return {};
}

} // namespace

void Editor::check_project() {
    if (!project_) return;
    if (check_future_.valid()) {
        check_again_ = true;
        return;
    }
    // Give new files their .meta sidecars here, on the UI thread, so the check (which opens its
    // own asset database) never has to create them at the same time as the editor.
    if (db_) (void)db_->refresh();
    check_future_ =
        std::async(std::launch::async, [root = project_->root()] { return player::validate_project(root); });
}

void Editor::poll_project_check() {
    if (!check_future_.valid()) return;
    if (check_future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    auto r = check_future_.get();
    if (r) {
        problems_ = std::move(r).value();
        problems_error_.clear();
    } else {
        problems_.reset();
        problems_error_ = r.error().message;
    }
    problems_checked_at_ = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (check_again_) {
        check_again_ = false;
        check_project();
    }
}

void Editor::wait_for_project_check() {
    while (check_future_.valid()) {
        check_future_.wait();
        poll_project_check();
    }
}

bool Editor::go_to_problem(const Diagnostic& d) {
    if (!project_) return false;
    if (!d.entity_uuid.is_nil() || !d.entity_name.empty()) {
        // Entity problems name a scene (project-relative) or a prefab (assets-relative).
        if (d.path.ends_with(project::kSceneExtension) && d.path != scene_path_ && play_state_ == PlayState::Editing) {
            if (dirty()) {
                notify("Save this scene first, then pick the problem again to open " + d.path, true);
                return false;
            }
            if (auto st = open_scene(d.path); !st.ok()) {
                notify("cannot open " + d.path + ": " + st.error().message, true);
                return false;
            }
        }
        if (d.path == scene_path_) {
            scene::Scene& s = active_scene();
            scene::Entity e = d.entity_uuid.is_nil() ? scene::kNullEntity : s.find(d.entity_uuid);
            if (e.is_null() && !d.entity_name.empty()) e = s.find_by_name(d.entity_name);
            if (!e.is_null()) {
                select(e);
                frame_selected();
                show_panel(Panel::Inspector);
                return true;
            }
        }
    }
    if (d.category == Category::Material && !d.asset.is_nil()) {
        open_material_editor(d.asset);
        return true;
    }
    if ((d.path.ends_with(".vgraph") || d.path.ends_with(".vmatgraph")) && db_) {
        if (const auto* rec = db_->find_path(d.path)) {
            if (auto st = open_graph_editor(rec->uuid); st.ok()) {
                show_panel(Panel::GraphEditor);
                return true;
            }
        }
    }
    if (d.category == Category::Sequence && !d.path.empty()) {
        if (auto st = open_sequence(d.path); st.ok()) {
            show_panel(Panel::Sequencer);
            return true;
        }
    }
    if (d.category == Category::Scene && d.path.ends_with(project::kSceneExtension) &&
        play_state_ == PlayState::Editing && !dirty()) {
        if (auto st = open_scene(d.path); st.ok()) return true;
    }
    if (!d.path.empty() && platform_) {
        platform_->set_clipboard(d.path);
        notify("Copied " + d.path);
        return true;
    }
    return false;
}

void Editor::draw_problems() {
    poll_project_check();
    if (!core_open_[static_cast<int>(Panel::Problems)]) return;
    const std::string title = problems_ && !problems_->diagnostics.empty()
                                  ? std::format("Problems ({})###Problems", problems_->diagnostics.size())
                                  : std::string("Problems###Problems");
    if (!ImGui::Begin(title.c_str(), &core_open_[static_cast<int>(Panel::Problems)])) {
        ImGui::End();
        return;
    }
    const bool running = checking_project();
    if (kit::icon_text_button(running ? "Checking..." : "Check Project", kit::Icon::Problems, false,
                              project_.has_value() && !running))
        check_project();
    kit::tooltip("Checks every scene, prefab, script, material, shader and asset on disk - the same checks "
                 "as `victus check`. It also runs when the project opens and after each save.");
    std::size_t errors = 0, warnings = 0;
    if (problems_) {
        errors = problems_->errors();
        warnings = problems_->warnings();
    }
    ImGui::SameLine(0, tk::kSpace4);
    {
        const std::string e = std::format("{} error{}", errors, errors == 1 ? "" : "s");
        const std::string w = std::format("{} warning{}", warnings, warnings == 1 ? "" : "s");
        ImGui::PushStyleColor(ImGuiCol_Text, errors ? tk::kDanger : tk::kTextMuted);
        ImGui::Checkbox(e.c_str(), &problems_show_errors_);
        ImGui::PopStyleColor();
        kit::tooltip("Show errors (the game may not run or export until they are fixed)");
        ImGui::SameLine(0, tk::kSpace4);
        ImGui::PushStyleColor(ImGuiCol_Text, warnings ? tk::kWarning : tk::kTextMuted);
        ImGui::Checkbox(w.c_str(), &problems_show_warnings_);
        ImGui::PopStyleColor();
        kit::tooltip("Show warnings (the game runs, but something probably is not what you meant)");
    }
    ImGui::SameLine(0, tk::kSpace4);
    ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x * 0.35f));
    ImGui::InputTextWithHint("##problems_filter", "Filter by text or file", &problems_filter_);
    if (problems_ && problems_checked_at_ > 0) {
        const double ago =
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() - problems_checked_at_;
        const std::string when = ago < 5     ? "checked just now"
                                 : ago < 120 ? std::format("checked {:.0f} s ago", ago)
                                             : std::format("checked {:.0f} min ago", ago / 60);
        const float w = ImGui::CalcTextSize(when.c_str()).x;
        ImGui::SameLine();
        const float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w;
        if (ImGui::GetCursorPosX() < x) ImGui::SetCursorPosX(x);
        ImGui::TextColored(tk::kTextDisabled, "%s", when.c_str());
    }
    ImGui::Separator();

    if (!project_) {
        (void)kit::empty_state(kit::Icon::Problems, "No project open", "Open or create a project to check it.");
        ImGui::End();
        return;
    }
    if (!problems_error_.empty()) {
        kit::icon(kit::Icon::Error, tk::kDanger);
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(tk::kDanger, "The project could not be checked: %s", problems_error_.c_str());
        ImGui::PopTextWrapPos();
        ImGui::End();
        return;
    }
    if (!problems_) {
        (void)kit::empty_state(kit::Icon::Problems, running ? "Checking the project..." : "Not checked yet",
                               "Problems in scenes, scripts, materials and other assets show up here.");
        ImGui::End();
        return;
    }
    if (problems_->diagnostics.empty()) {
        (void)kit::empty_state(kit::Icon::Check, "No problems found",
                               std::format("{} scenes, {} prefabs, {} scripts and {} materials checked.",
                                           problems_->scenes, problems_->prefabs, problems_->scripts,
                                           problems_->materials));
        ImGui::End();
        return;
    }

    ImGui::BeginChild("##problems_list");
    std::size_t shown = 0;
    std::optional<Diagnostic> go;
    for (std::size_t i = 0; i < problems_->diagnostics.size(); ++i) {
        const Diagnostic& d = problems_->diagnostics[i];
        const bool is_error = d.severity == Severity::Error;
        if ((is_error && !problems_show_errors_) || (!is_error && !problems_show_warnings_)) continue;
        if (!contains_icase(d.message, problems_filter_) && !contains_icase(d.path, problems_filter_) &&
            !contains_icase(d.entity_name, problems_filter_))
            continue;
        ++shown;
        ImGui::PushID(static_cast<int>(i));
        ImGui::BeginGroup();
        kit::icon(is_error ? kit::Icon::Error : kit::Icon::Warning, is_error ? tk::kDanger : tk::kWarning);
        ImGui::SameLine();
        kit::icon(category_icon(d.category));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x > 260
                                   ? ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 150
                                   : 0.0f);
        ImGui::TextUnformatted(d.message.c_str());
        if (!d.suggested_fix.empty()) ImGui::TextColored(tk::kTextMuted, "%s", d.suggested_fix.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::EndGroup();
        const bool row_hovered = ImGui::IsItemHovered();
        if (row_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) go = d;
        // Actions on the right.
        const std::string label = go_to_label(d);
        ImGui::SameLine();
        const float bw = 140.0f;
        const float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw;
        if (ImGui::GetCursorPosX() < x) ImGui::SetCursorPosX(x);
        if (!label.empty()) {
            if (ImGui::SmallButton(label.c_str())) go = d;
        } else if (!d.path.empty()) {
            if (ImGui::SmallButton("Copy path") && platform_) platform_->set_clipboard(d.path);
        }
        if (ImGui::BeginPopupContextItem("##problem_ctx")) {
            if (ImGui::MenuItem("Copy message", nullptr, false, platform_ != nullptr))
                platform_->set_clipboard(d.line());
            if (ImGui::MenuItem("Copy path", nullptr, false, platform_ != nullptr && !d.path.empty()))
                platform_->set_clipboard(d.path);
            ImGui::EndPopup();
        }
        ImGui::PopID();
        ImGui::Separator();
    }
    if (shown == 0)
        ImGui::TextColored(tk::kTextMuted, "Nothing matches the filter (%zu hidden).", problems_->diagnostics.size());
    ImGui::EndChild();
    ImGui::End();
    if (go) (void)go_to_problem(*go);
}

} // namespace victus::editor
