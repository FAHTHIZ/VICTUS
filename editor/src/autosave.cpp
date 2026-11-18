// SPDX-License-Identifier: Apache-2.0
// Autosave and crash recovery: unsaved scene edits are written periodically to
// <project>/.victus/autosave/<scene>.vscene; a copy newer than its scene file is offered for
// recovery when the scene is opened again.
#include "victus/editor/editor.hpp"

#include "victus/core/log.hpp"
#include "victus/project/project.hpp"

#include <imgui.h>

#include <chrono>
#include <format>

namespace victus::editor {

namespace sfs = std::filesystem;

fs::Path Editor::autosave_path(std::string_view scene_path) const {
    if (!project_) return {};
    std::string key = scene_path.empty() ? std::string("untitled") : std::string(scene_path);
    for (char& c : key)
        if (c == '/' || c == '\\' || c == ':') c = '~';
    if (!key.ends_with(project::kSceneExtension)) key += project::kSceneExtension;
    return project_->root() / ".victus" / "autosave" / key;
}

Status Editor::autosave_now() {
    if (!project_ || !scene_) return make_error(ErrorCode::InvalidArgument, "no project open");
    const fs::Path path = autosave_path(scene_path_);
    std::error_code ec;
    sfs::create_directories(path.parent_path(), ec);
    VICTUS_TRY_STATUS(scene_->save_file(path));
    autosaved_revision_ = undo_.revision();
    autosave_written_ = true;
    autosave_scene_ = scene_path_;
    autosave_timer_ = 0;
    return {};
}

std::string Editor::save_for_fatal_error(std::string_view reason) {
    VICTUS_LOG_ERROR("editor", "{}", reason);
    std::string summary = "no unsaved edits";
    if (project_ && scene_ && dirty()) {
        if (auto st = autosave_now(); st.ok())
            summary = std::format("unsaved edits were autosaved to {}; reopen the scene to recover them",
                                  autosave_path(scene_path_).string());
        else
            summary = "autosave failed: " + st.error().message;
    }
    VICTUS_LOG_ERROR("editor", "{}", summary);
    return summary;
}

void Editor::remove_autosave(std::string_view scene_path) {
    if (!project_) return;
    std::error_code ec;
    sfs::remove(autosave_path(scene_path), ec);
    if (recovery_ && recovery_->scene == scene_path) recovery_.reset();
}

void Editor::update_autosave(float dt) {
    if (!project_ || !scene_) return;
    if (!dirty()) {
        // Saved, or every edit undone: the copy is stale.
        if (autosave_written_) {
            remove_autosave(autosave_scene_);
            autosave_written_ = false;
        }
        autosave_timer_ = 0;
        return;
    }
    if (autosave_seconds_ <= 0 || sequence_previewing()) return; // previewed poses are not authored edits
    autosave_timer_ += dt;
    if (autosave_timer_ < autosave_seconds_ || (autosave_written_ && autosaved_revision_ == undo_.revision())) return;
    if (auto st = autosave_now(); !st.ok()) {
        autosave_timer_ = 0; // retry after another interval
        notify("autosave failed: " + st.error().message, true);
    } else {
        VICTUS_LOG_INFO("editor", "autosaved {}", scene_path_.empty() ? std::string("untitled scene") : scene_path_);
    }
}

void Editor::check_recovery() {
    recovery_.reset();
    autosave_written_ = false;
    autosave_timer_ = 0;
    if (!project_) return;
    const fs::Path copy = autosave_path(scene_path_);
    std::error_code ec;
    if (!sfs::is_regular_file(copy, ec)) return;
    const auto copy_time = sfs::last_write_time(copy, ec);
    if (ec) return;
    if (!scene_path_.empty()) {
        if (auto file = project_->resolve(scene_path_); file && sfs::exists(*file, ec)) {
            const auto scene_time = sfs::last_write_time(*file, ec);
            if (!ec && scene_time >= copy_time) {
                sfs::remove(copy, ec); // the scene was saved after the copy (e.g. by another tool)
                return;
            }
        }
    }
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(sfs::file_time_type::clock::now() - copy_time).count();
    recovery_ = Recovery{scene_path_, copy, std::max<std::int64_t>(age, 0)};
}

Status Editor::recover_autosave() {
    if (!recovery_) return make_error(ErrorCode::NotFound, "no autosave to recover");
    const Recovery r = *recovery_;
    auto s = std::make_unique<scene::Scene>();
    if (auto st = s->load_file(r.file); !st.ok()) {
        recovery_.reset();
        return st;
    }
    set_scene(std::move(s), r.scene);
    undo_.mark_unsaved(); // recovered edits are not in the scene file yet
    autosave_written_ = true; // the copy stays until the user saves or discards
    autosave_scene_ = r.scene;
    autosaved_revision_ = undo_.revision();
    recovery_.reset();
    update_title();
    notify("Recovered unsaved changes to " + (r.scene.empty() ? std::string("the untitled scene") : r.scene));
    return {};
}

void Editor::discard_autosave() {
    if (!recovery_) return;
    std::error_code ec;
    sfs::remove(recovery_->file, ec);
    recovery_.reset();
}

void Editor::draw_recovery_modal() {
    if (!recovery_ || pending_action_ != Pending::None) return;
    if (!ImGui::IsPopupOpen("Recover Unsaved Changes")) ImGui::OpenPopup("Recover Unsaved Changes");
    if (!ImGui::BeginPopupModal("Recover Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const std::int64_t age = recovery_ ? recovery_->age_s : 0;
    const std::string when = age < 10 ? std::string("just now")
                             : age < 90 ? std::format("{} seconds ago", age)
                             : age < 5400 ? std::format("{} minutes ago", age / 60)
                                          : std::format("{} hours ago", age / 3600);
    ImGui::Text("VICTUS found unsaved changes to %s (autosaved %s).",
                recovery_->scene.empty() ? "an untitled scene" : recovery_->scene.c_str(), when.c_str());
    ImGui::TextDisabled("The editor closed without saving them (crash or forced quit).");
    ImGui::Spacing();
    if (ImGui::Button("Recover", ImVec2(110, 0))) {
        ImGui::CloseCurrentPopup();
        if (auto st = recover_autosave(); !st.ok()) notify("recovery failed: " + st.error().message, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard", ImVec2(110, 0))) {
        ImGui::CloseCurrentPopup();
        discard_autosave();
    }
    ImGui::EndPopup();
}

} // namespace victus::editor
