// SPDX-License-Identifier: Apache-2.0
// Native plugins in the Studio (ADR-0192, docs/PLUGINS.md): editor-target plugins load with the
// project and add commands (each run is one undo step); the Plugins panel lists every manifest
// with its Studio and last-Play state, errors, systems, functions and commands, and can enable,
// disable, reload and create plugins.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include "victus/core/json.hpp"
#include "victus/editor/undo.hpp"
#include "victus/ui/style.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <format>

namespace victus::editor {

namespace tk = ui::tokens;

namespace {

// Every entity, parents before children (the order undo restores them in).
std::vector<Uuid> preorder(const scene::Scene& s) {
    std::vector<Uuid> out;
    std::vector<scene::Entity> stack = s.roots();
    std::reverse(stack.begin(), stack.end());
    while (!stack.empty()) {
        const scene::Entity e = stack.back();
        stack.pop_back();
        out.push_back(s.uuid(e));
        auto kids = s.children(e);
        stack.insert(stack.end(), kids.rbegin(), kids.rend());
    }
    return out;
}

std::string state_text(const EntityState& st) { return st.snapshot ? json::stringify(*st.snapshot, false) : std::string(); }

ImVec4 state_color(plugin::State s) {
    switch (s) {
    case plugin::State::Loaded: return tk::kSuccess;
    case plugin::State::Failed:
    case plugin::State::Rejected:
    case plugin::State::Skipped: return tk::kDanger;
    default: return tk::kTextMuted;
    }
}

} // namespace

void Editor::unload_plugins() {
    editor_plugins_.reset();
    plugins_suspended_ = false;
}

void Editor::reload_plugins() {
    if (play_state_ != PlayState::Editing) return; // Stop reloads them
    editor_plugins_.reset();
    plugin_manifests_.clear();
    plugin_issues_.clear();
    if (!project_) return;
    const fs::Path dir = project_->root() / "plugins";
    plugin_manifests_ = plugin::discover(dir, &plugin_issues_);
    if (safe_mode_) return; // listed, never loaded
    plugin::HostOptions options{.target = "editor"};
    options.notify = [this](std::string_view name, std::string_view message, bool error) {
        notify(std::format("{}: {}", name, message), error);
    };
    editor_plugins_ = std::make_unique<plugin::PluginManager>(std::move(options));
    editor_plugins_->load(plugin_manifests_);
    for (const auto& st : editor_plugins_->statuses())
        if (st.state == plugin::State::Failed || st.state == plugin::State::Rejected || st.state == plugin::State::Skipped)
            notify(std::format("Plugin {} {}: {}", st.name, plugin::to_string(st.state), st.error), true);
    for (const auto& i : plugin_issues_) notify(std::format("Plugin manifest {}: {}", i.path.filename().string(), i.message), true);
}

Status Editor::run_plugin_command(std::string_view id) {
    if (play_state_ != PlayState::Editing)
        return make_error(ErrorCode::InvalidArgument, "Stop Play first: plugin commands change the scene you are building");
    if (!editor_plugins_) return make_error(ErrorCode::NotFound, safe_mode_ ? "plugins are off in safe mode" : "no plugins are loaded");
    std::string label;
    for (const auto& c : editor_plugins_->commands())
        if (c.id == id) label = c.label;
    if (label.empty()) return make_error(ErrorCode::NotFound, std::format("no plugin command '{}'", id));
    // Snapshot everything: a command may create, change or destroy any entity.
    if (pending_dirty_ && !pending_before_.empty()) record_edit("Edit", std::move(pending_before_));
    pending_before_.clear();
    pending_dirty_ = false;
    const std::vector<Uuid> before_ids = preorder(*scene_);
    const std::vector<EntityState> before_all = capture(*scene_, before_ids);
    Status st = editor_plugins_->run_command(id, *scene_);
    // Entities now present first (pre-order), then the ones the command destroyed.
    std::vector<Uuid> ids = preorder(*scene_);
    for (const Uuid& u : before_ids)
        if (scene_->find(u).is_null()) ids.push_back(u);
    std::vector<EntityState> before;
    before.reserve(ids.size());
    for (const Uuid& u : ids) {
        const auto it = std::find(before_ids.begin(), before_ids.end(), u);
        before.push_back(it == before_ids.end() ? EntityState{u, std::nullopt}
                                                : before_all[static_cast<std::size_t>(it - before_ids.begin())]);
    }
    std::vector<EntityState> after = capture(*scene_, ids);
    bool changed = false;
    for (std::size_t i = 0; i < ids.size() && !changed; ++i) changed = state_text(before[i]) != state_text(after[i]);
    if (changed) {
        undo_.push(SceneCommand{label, std::move(before), std::move(after), {}});
        update_title();
    }
    prune_selection();
    return st;
}

Status Editor::set_plugin_enabled(std::string_view name, bool enabled) {
    const auto it = std::find_if(plugin_manifests_.begin(), plugin_manifests_.end(),
                                 [&](const plugin::Manifest& m) { return m.name == name; });
    if (it == plugin_manifests_.end()) return make_error(ErrorCode::NotFound, std::format("no plugin '{}'", name));
    if (auto st = plugin::set_enabled(it->manifest_path, enabled); !st) return st;
    reload_plugins();
    return {};
}

Status Editor::create_plugin(std::string_view name) {
    if (!project_) return make_error(ErrorCode::InvalidArgument, "open a project first");
    if (auto st = plugin::scaffold(project_->root() / "plugins", name); !st) return st;
    reload_plugins();
    return {};
}

void Editor::draw_plugin_menu_items() {
    const bool editing = play_state_ == PlayState::Editing;
    const auto cmds = editor_plugins_ ? editor_plugins_->commands() : std::vector<plugin::Command>{};
    if (cmds.empty()) ImGui::TextDisabled(safe_mode_ ? "Plugins are off in safe mode" : "No plugin commands");
    for (const auto& c : cmds) {
        const std::string item = std::format("{}##{}", c.label, c.id);
        if (ImGui::MenuItem(item.c_str(), nullptr, false, editing))
            if (auto st = run_plugin_command(c.id); !st.ok()) notify(st.error().message, true);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s (plugin %s)%s", c.id.c_str(), c.plugin.c_str(), editing ? "" : "\nStop Play to run it");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Manage Plugins...", nullptr, show_plugins_)) show_panel(Panel::Plugins);
    if (ImGui::MenuItem("Reload Plugins", nullptr, false, editing && project_ && !safe_mode_)) reload_plugins();
    kit::tooltip("Unload and load the project's editor plugins again (after rebuilding a library)");
}

void Editor::draw_plugins_panel() {
    if (!show_plugins_) return;
    if (!ImGui::Begin("Plugins", &show_plugins_)) {
        ImGui::End();
        return;
    }
    if (!project_) {
        ImGui::TextDisabled("Open a project to see its plugins.");
        ImGui::End();
        return;
    }
    const bool editing = play_state_ == PlayState::Editing;
    ImGui::TextDisabled("Plugin API %u.%u, %s. Plugins are native code with full access to this computer.", VICTUS_PLUGIN_API_MAJOR,
                        VICTUS_PLUGIN_API_MINOR, std::string(plugin::platform_key()).c_str());
    if (safe_mode_) ImGui::TextColored(tk::kWarning, "Safe mode: plugins are listed but not loaded.");
    if (kit::icon_text_button("Reload", kit::Icon::Rotate, false, editing && !safe_mode_)) reload_plugins();
    kit::tooltip("Unload the editor plugins and load them again, picking up rebuilt libraries");
    ImGui::SameLine();
    if (kit::icon_text_button("Unload", kit::Icon::Stop, false, editing && editor_plugins_ != nullptr)) unload_plugins();
    kit::tooltip("Unload the editor plugins (Windows locks a loaded .dll, so unload before rebuilding)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    const bool enter = ImGui::InputTextWithHint("##newplugin", "new_plugin_name", &plugin_new_name_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((kit::icon_text_button("New Plugin", kit::Icon::Plus, false, !plugin_new_name_.empty()) || (enter && !plugin_new_name_.empty()))) {
        if (auto st = create_plugin(plugin_new_name_); !st.ok()) notify("New Plugin: " + st.error().message, true);
        else {
            notify(std::format("Created plugins/{} - build it with CMake, then Reload", plugin_new_name_));
            plugin_new_name_.clear();
        }
    }
    kit::tooltip("Create plugins/<name> with a C template (a system, a script function and a Studio command) and a CMake file");
    for (const auto& i : plugin_issues_) ImGui::TextColored(tk::kDanger, "%s: %s", i.path.filename().string().c_str(), i.message.c_str());
    if (plugin_manifests_.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("No plugins in %s. Create one above or with `victus plugins <project> --new NAME`.",
                           (project_->root() / "plugins").string().c_str());
        ImGui::End();
        return;
    }
    const auto status_of = [](const std::vector<plugin::PluginStatus>& list, const std::string& name) -> const plugin::PluginStatus* {
        for (const auto& s : list)
            if (s.name == name) return &s;
        return nullptr;
    };
    for (const auto& m : plugin_manifests_) {
        ImGui::PushID(m.name.c_str());
        ImGui::Separator();
        bool enabled = m.enabled;
        ImGui::BeginDisabled(!editing);
        if (ImGui::Checkbox("##enabled", &enabled))
            if (auto st = set_plugin_enabled(m.name, enabled); !st.ok()) notify(st.error().message, true);
        ImGui::EndDisabled();
        kit::tooltip("Enabled: written to the manifest, so exported games and `victus run` follow it too");
        ImGui::SameLine();
        kit::heading(m.name.c_str(), tk::kTypeBody);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", plugin::to_string(m.version).c_str());
        if (!m.description.empty()) ImGui::TextWrapped("%s", m.description.c_str());
        std::string targets;
        for (const auto& t : m.targets) targets += (targets.empty() ? "" : ", ") + t;
        ImGui::TextDisabled("Targets: %s   API %s", targets.c_str(), plugin::to_string(m.api).c_str());
        const auto row = [&](const char* where, const plugin::PluginStatus* st, const char* none) {
            ImGui::TextUnformatted(where);
            ImGui::SameLine(110.0f);
            if (!st) {
                ImGui::TextDisabled("%s", none);
                return;
            }
            ImGui::TextColored(state_color(st->state), "%s", std::string(plugin::to_string(st->state)).c_str());
            if (!st->error.empty()) {
                ImGui::SameLine();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(st->state == plugin::State::Disabled ? tk::kTextMuted : tk::kDanger, "%s", st->error.c_str());
                ImGui::PopTextWrapPos();
            }
        };
        const plugin::PluginStatus* in_studio = editor_plugins_ ? status_of(editor_plugins_->statuses(), m.name) : nullptr;
        row("In Studio", in_studio, safe_mode_ ? "not loaded (safe mode)" : (editing ? "not loaded" : "unloaded during Play"));
        const plugin::PluginStatus* in_play =
            play_plugins_ ? status_of(play_plugins_->statuses(), m.name) : status_of(play_plugin_statuses_, m.name);
        row(play_plugins_ ? "In Play" : "Last Play", in_play, "not run yet");
        // Building hint when the library for this platform is missing.
        if (const auto lib = m.libraries.find(std::string(plugin::platform_key())); lib == m.libraries.end()) {
            ImGui::TextColored(tk::kWarning, "No library listed for %s in the manifest.", std::string(plugin::platform_key()).c_str());
        } else if (std::error_code ec; !std::filesystem::is_regular_file(m.dir / lib->second, ec)) {
            const std::string cmd = std::format("cmake -S \"{0}\" -B \"{0}/build\" && cmake --build \"{0}/build\" --config Release",
                                                m.dir.generic_string());
            ImGui::TextColored(tk::kWarning, "%s is not built yet.", lib->second.c_str());
            if (ImGui::SmallButton("Copy Build Command") && platform_) platform_->set_clipboard(cmd);
            kit::tooltip(cmd.c_str());
        }
        const plugin::PluginStatus* shown = in_studio && in_studio->state == plugin::State::Loaded ? in_studio : in_play;
        if (shown && !(shown->systems.empty() && shown->functions.empty() && shown->commands.empty())) {
            if (ImGui::TreeNode("##details", "%zu systems, %zu functions, %zu commands", shown->systems.size(), shown->functions.size(),
                                shown->commands.size())) {
                for (const auto& s : shown->systems) ImGui::BulletText("system %s (runs during Play)", s.c_str());
                for (const auto& f : shown->functions) ImGui::BulletText("function %s (plugins.call in scripts)", f.c_str());
                ImGui::TreePop();
            }
        }
        if (in_studio && editor_plugins_) {
            for (const auto& c : editor_plugins_->commands()) {
                if (c.plugin != m.name) continue;
                const std::string b = std::format("{}##{}", c.label, c.id);
                ImGui::BeginDisabled(!editing);
                if (ImGui::Button(b.c_str()))
                    if (auto st = run_plugin_command(c.id); !st.ok()) notify(st.error().message, true);
                ImGui::EndDisabled();
                kit::tooltip(c.id.c_str());
                ImGui::SameLine();
            }
            ImGui::NewLine();
        }
        ImGui::PopID();
    }
    ImGui::End();
}

} // namespace victus::editor
