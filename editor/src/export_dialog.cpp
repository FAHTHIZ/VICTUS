// SPDX-License-Identifier: Apache-2.0
// File > Export Game: packages the project into a folder that runs without VICTUS
// (player::export_game, the same as `victus export`), on a worker thread.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include "victus/ui/style.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <chrono>
#include <filesystem>
#include <format>

namespace victus::editor {

namespace tk = ui::tokens;
namespace sfs = std::filesystem;

namespace {

constexpr const char* kExportPopup = "Export Game";

std::string human_size(std::uint64_t bytes) {
    if (bytes >= 1024ull * 1024ull) return std::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    if (bytes >= 1024ull) return std::format("{:.0f} KB", static_cast<double>(bytes) / 1024.0);
    return std::format("{} bytes", bytes);
}

} // namespace

fs::Path Editor::default_export_dir() const {
    if (!project_) return {};
    return project_->root() / "build" / player::executable_base_name(project_->settings().name);
}

void Editor::open_export_dialog() {
    if (!project_) return;
    if (export_output_.empty()) export_output_ = default_export_dir().string();
    if (export_name_.empty()) export_name_ = player::executable_base_name(project_->settings().name);
    show_export_ = true;
    // Show the latest state of the project: the check runs while the dialog opens.
    if (!checking_project()) check_project();
}

Status Editor::start_export(const fs::Path& output, std::string executable_name) {
    if (!project_) return make_error(ErrorCode::InvalidArgument, "no project open");
    if (export_future_.valid()) return make_error(ErrorCode::InvalidArgument, "an export is already running");
    if (play_state_ != PlayState::Editing) return make_error(ErrorCode::InvalidArgument, "stop play mode before exporting");
    if (output.empty()) return make_error(ErrorCode::InvalidArgument, "choose a folder for the game");
    std::error_code ec;
    if (sfs::exists(output, ec) && !sfs::is_directory(output, ec))
        return make_error(ErrorCode::InvalidArgument, "the export location is a file, not a folder");
    if (sfs::is_directory(output, ec) && !sfs::is_empty(output, ec) && !sfs::exists(output / "export.json", ec))
        return make_error(ErrorCode::AlreadyExists,
                          "the folder already has other files in it; pick an empty folder or a previous export");
    // Import anything stale here, on the UI thread, so the export does not write the asset
    // cache at the same time as the editor.
    if (db_) {
        (void)db_->refresh();
        (void)db_->import_all();
    }
    export_report_.reset();
    export_error_.clear();
    player::ExportOptions o;
    o.project = project_->root();
    o.output = output;
    o.executable_name = std::move(executable_name);
    o.player_executable = options_.player_executable;
    export_started_at_ = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    export_future_ = std::async(std::launch::async, [o] { return player::export_game(o); });
    return {};
}

void Editor::poll_export() {
    if (!export_future_.valid()) return;
    if (export_future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    auto r = export_future_.get();
    if (r) {
        export_report_ = std::move(r).value();
        complete_guide_step(GuideStep::Export);
        notify(std::format("Exported the game to {}", export_report_->executable.empty()
                                                           ? export_report_->pack.parent_path().string()
                                                           : export_report_->executable.parent_path().string()));
    } else {
        export_error_ = r.error().message;
        notify("export failed: " + export_error_, true);
    }
}

void Editor::wait_for_export() {
    if (!export_future_.valid()) return;
    export_future_.wait();
    poll_export();
}

void Editor::draw_export_dialog() {
    poll_export();
    if (show_export_) {
        ImGui::OpenPopup(kExportPopup);
        show_export_ = false;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(620.0f, vp->WorkSize.x - 40.0f), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kExportPopup, nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    const bool running = exporting();
    kit::heading("Export your game", tk::kTypeHeading);
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(tk::kTextMuted, "Builds a folder with your game in it. It runs on this kind of computer without "
                                       "VICTUS installed: share the whole folder, then players start the program inside.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, tk::kSpace3));

    ImGui::BeginDisabled(running);
    ImGui::TextUnformatted("Game name");
    ImGui::SameLine();
    ImGui::TextColored(tk::kTextMuted, "the name of the program players start");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##export_name", &export_name_);
    ImGui::TextUnformatted("Folder");
    ImGui::SameLine();
    ImGui::TextColored(tk::kTextMuted, "created if it does not exist");
    const float browse_w = ImGui::CalcTextSize("Browse...").x + ImGui::GetFrameHeight() + tk::kSpace5;
    ImGui::SetNextItemWidth(-browse_w - tk::kSpace2);
    ImGui::InputText("##export_out", &export_output_);
    ImGui::SameLine(0, tk::kSpace2);
    if (kit::icon_text_button("Browse...", kit::Icon::Folder, false, platform_ != nullptr)) {
        platform_->show_folder_dialog(window_, export_output_, [this](std::string path) {
            if (!path.empty()) export_output_ = std::move(path);
        });
    }
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, tk::kSpace3));

    // Readiness: unsaved changes and known problems, before anyone waits for an export to fail.
    const std::size_t errors = problems_ ? problems_->errors() : 0;
    const bool unsaved = dirty();
    const auto line = [](kit::Icon ic, ImVec4 col, const std::string& text) {
        kit::icon(ic, col);
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(col, "%s", text.c_str());
        ImGui::PopTextWrapPos();
    };
    if (unsaved) line(kit::Icon::Warning, tk::kWarning, "This scene has unsaved changes. Export saves it first.");
    if (checking_project()) line(kit::Icon::Info, tk::kTextMuted, "Checking the project...");
    else if (errors > 0) {
        line(kit::Icon::Error, tk::kDanger,
             std::format("The project has {} error{}. Fix {} before exporting.", errors, errors == 1 ? "" : "s",
                         errors == 1 ? "it" : "them"));
        if (ImGui::SmallButton("Show Problems")) {
            show_panel(Panel::Problems);
            ImGui::CloseCurrentPopup();
        }
    } else if (problems_) {
        line(kit::Icon::Check, tk::kSuccess, "No problems found in the project.");
    }
    if (running) {
        const double t =
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() - export_started_at_;
        line(kit::Icon::Export, tk::kInfo, std::format("Exporting... {:.0f} s", t));
    } else if (!export_error_.empty()) {
        line(kit::Icon::Error, tk::kDanger, "Export failed: " + export_error_);
    } else if (export_report_) {
        const auto& r = *export_report_;
        const fs::Path folder = r.executable.empty() ? r.pack.parent_path().parent_path() : r.executable.parent_path();
        line(kit::Icon::Check, tk::kSuccess,
             std::format("Done: {} files, {} in {}", r.files, human_size(r.pack_bytes), folder.string()));
        if (!r.executable.empty()) ImGui::TextColored(tk::kTextMuted, "Start %s to play.", r.executable.filename().string().c_str());
        for (const auto& w : r.warnings) line(kit::Icon::Warning, tk::kWarning, w);
        if (ImGui::SmallButton("Copy folder path") && platform_) platform_->set_clipboard(folder.string());
    }
    ImGui::Dummy(ImVec2(0, tk::kSpace3));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, tk::kSpace2));

    const bool can_export = !running && errors == 0 && !checking_project() && play_state_ == PlayState::Editing &&
                            !export_output_.empty();
    const char* label = unsaved ? "Save and Export" : "Export";
    if (kit::icon_text_button(label, kit::Icon::Export, true, can_export, ImVec2(180, ImGui::GetFrameHeight() + tk::kSpace2))) {
        Status st;
        if (unsaved) {
            if (scene_path_.empty()) st = make_error(ErrorCode::InvalidArgument, "save the scene with File > Save Scene As first");
            else st = save_scene();
        }
        if (st.ok()) st = start_export(export_output_, export_name_);
        if (!st.ok()) export_error_ = st.error().message;
    }
    if (!can_export && !running) kit::tooltip("Fix the problems above first");
    ImGui::SameLine();
    if (kit::icon_text_button(running ? "Close (keeps exporting)" : "Close", kit::Icon::Close, false, true,
                              ImVec2(0, ImGui::GetFrameHeight() + tk::kSpace2)))
        ImGui::CloseCurrentPopup();
    if (!running && ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace victus::editor
