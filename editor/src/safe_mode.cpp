// SPDX-License-Identifier: Apache-2.0
// Editor safe mode (ADR-0101). Every project opening is recorded in
// <project>/.victus/editor_startup.json and the record is removed once the Studio has run
// EditorOptions::startup_frames frames, closed the project or quit. A record left behind means an
// opening crashed, hung or was killed; after safe_mode_after of them in a row the project opens in
// safe mode, which skips everything that runs project content automatically (like Godot's
// recovery mode). Safe mode is explained in a card with Restart Normally.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include "victus/core/build_info.hpp"
#include "victus/core/json.hpp"
#include "victus/core/log.hpp"
#include "victus/ui/style.hpp"

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <format>

namespace victus::editor {

namespace sfs = std::filesystem;
namespace tk = ui::tokens;

fs::Path startup_marker_path(const fs::Path& project_root) { return project_root / ".victus" / "editor_startup.json"; }

std::uint32_t unfinished_startups(const fs::Path& project_root) {
    auto text = fs::read_text(startup_marker_path(project_root), 64u << 10);
    if (!text) return 0;
    auto doc = json::parse(*text);
    if (!doc) return 1; // a torn marker still means an opening did not finish
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>((*doc)["unfinished"].as_int(1), 0, 1000));
}

const std::vector<std::string>& safe_mode_skipped() {
    static const std::vector<std::string> skipped = {
        "The startup scene was not opened; open scenes from the Assets panel.",
        "Scripts are not run in the editor, so the Inspector shows no script properties.",
        "Changed assets were not re-imported and thumbnails are off.",
        "The automatic project check is off (Check Project still works).",
        "Autosaved changes are kept on disk but not offered, and nothing is autosaved.",
        "Play mode is off, so the project's scripts and plugins never run.",
    };
    return skipped;
}

void Editor::begin_startup(const fs::Path& root) {
    const std::uint32_t previous = unfinished_startups(root);
    startup_marker_ = startup_marker_path(root);
    std::error_code ec;
    sfs::create_directories(startup_marker_.parent_path(), ec);
    json::Value marker = json::Value::object();
    marker.set("unfinished", static_cast<std::int64_t>(previous) + 1);
    marker.set("editor", std::string(build::version()));
    if (auto st = fs::write_text_atomic(startup_marker_, json::stringify(marker, true) + "\n"); !st) {
        VICTUS_LOG_WARN("editor", "cannot record the project opening ({}); crash detection is off for it", st.error().message);
        startup_marker_.clear();
    }
    startup_frames_left_ = std::max<std::uint32_t>(options_.startup_frames, 1);
    const std::uint32_t after = options_.safe_mode_after;
    if (options_.safe_mode || safe_request_) {
        safe_mode_ = true;
        safe_mode_reason_ = "you asked to open the project in safe mode.";
    } else if (after > 0 && previous >= after) {
        safe_mode_ = true;
        safe_mode_reason_ = std::format("the last {} attempts to open this project did not finish (a crash, hang or forced quit).", previous);
    }
    safe_request_ = false;
    if (safe_mode_) VICTUS_LOG_WARN("editor", "safe mode: {}", safe_mode_reason_);
}

void Editor::finish_startup() {
    startup_frames_left_ = 0;
    if (startup_marker_.empty()) return;
    std::error_code ec;
    sfs::remove(startup_marker_, ec);
    startup_marker_.clear();
}

void Editor::update_startup() {
    if (startup_frames_left_ == 0 || !project_) return;
    --startup_frames_left_;
    if (startup_frames_left_ == 0) finish_startup();
}

Status Editor::open_project_safe(const fs::Path& path) {
    safe_request_ = true;
    auto st = open_project(path);
    safe_request_ = false;
    return st;
}

Status Editor::reopen_normally() {
    if (!project_) return make_error(ErrorCode::InvalidArgument, "no project open");
    if (!safe_mode_) return {};
    if (dirty()) return make_error(ErrorCode::InvalidArgument, "save or undo your changes first");
    const fs::Path root = project_->root();
    options_.safe_mode = false; // --safe-mode covered this run until now
    close_project();
    return open_project(root);
}

void Editor::draw_safe_mode_card() {
    if (!safe_mode_card_visible() || !project_) return;
    // A card in the top-right corner below the toolbar; it stays until hidden or the project closes.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float w = std::min(460.0f, vp->WorkSize.x - 2 * tk::kSpace5);
    const float top = ImGui::GetFrameHeight() * 3.0f + tk::kSpace4;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - tk::kSpace5, vp->WorkPos.y + top), ImGuiCond_Always,
                            ImVec2(1, 0));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(tk::kSpace5, tk::kSpace4));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, tk::kSurfacePanel);
    ImGui::PushStyleColor(ImGuiCol_Border, tk::kWarning);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##safe_mode", nullptr, flags)) {
        kit::icon(kit::Icon::Warning, tk::kWarning);
        ImGui::SameLine();
        ImGui::PushFont(kit::bold_font(), 0);
        ImGui::TextUnformatted("Safe mode");
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(("The project opened in safe mode because " + safe_mode_reason_).c_str());
        for (const std::string& s : safe_mode_skipped()) {
            ImGui::Bullet();
            ImGui::TextColored(tk::kTextMuted, "%s", s.c_str());
        }
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, tk::kSpace1));
        if (kit::icon_text_button("Restart Normally", kit::Icon::Play, true, !dirty())) {
            if (auto st = reopen_normally(); !st.ok()) notify("cannot restart normally: " + st.error().message, true);
        }
        if (dirty()) kit::tooltip("Save or undo your changes first.");
        ImGui::SameLine();
        if (kit::icon_text_button("Report a problem...", kit::Icon::Help, false, true)) open_diagnostics_dialog();
        ImGui::SameLine();
        if (kit::icon_text_button("Hide", kit::Icon::Close, false, true)) safe_mode_card_hidden_ = true;
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

} // namespace victus::editor
