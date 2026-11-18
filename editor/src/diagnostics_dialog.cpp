// SPDX-License-Identifier: Apache-2.0
// Help > Create Diagnostics Bundle (player::create_diagnostics_bundle, the same as
// `victus diagnose`), the "closed unexpectedly last time" card and Help > About.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include "victus/core/build_info.hpp"
#include "victus/core/crash.hpp"
#include "victus/ui/style.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>

namespace victus::editor {

namespace tk = ui::tokens;
namespace sfs = std::filesystem;

namespace {

constexpr const char* kBundlePopup = "Create Diagnostics Bundle";
constexpr const char* kAboutPopup = "About VICTUS Studio";

std::string human_size(std::uint64_t bytes) {
    if (bytes >= 1024ull * 1024ull) return std::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    if (bytes >= 1024ull) return std::format("{:.0f} KB", static_cast<double>(bytes) / 1024.0);
    return std::format("{} bytes", bytes);
}

fs::Path home_dir() {
    for (const char* var : {"HOME", "USERPROFILE"})
        if (const char* v = std::getenv(var); v && *v) return v;
    return {};
}

void status_line(kit::Icon ic, ImVec4 col, const std::string& text) {
    kit::icon(ic, col);
    ImGui::SameLine();
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(col, "%s", text.c_str());
    ImGui::PopTextWrapPos();
}

void centre_next_popup(float width) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(width, vp->WorkSize.x - 40.0f), 0), ImGuiCond_Appearing);
}

} // namespace

fs::Path Editor::diagnostics_dir() const {
    return options_.diagnostics_dir.empty() ? crash::default_directory() : options_.diagnostics_dir;
}

fs::Path Editor::default_bundle_path() const {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const std::string name = std::format("victus-diagnostics-{:%Y%m%d-%H%M%S}.zip", now);
    const fs::Path home = home_dir();
    std::error_code ec;
    if (!home.empty() && sfs::is_directory(home / "Desktop", ec)) return home / "Desktop" / name;
    if (!home.empty() && sfs::is_directory(home, ec)) return home / name;
    return diagnostics_dir() / "bundles" / name;
}

void Editor::open_diagnostics_dialog() {
    if (bundle_output_.empty() || (!creating_bundle() && bundle_result_))
        bundle_output_ = default_bundle_path().string();
    if (!creating_bundle()) {
        bundle_result_.reset();
        bundle_error_.clear();
    }
    show_bundle_ = true;
}

Status Editor::start_diagnostics_bundle(const fs::Path& output, bool keep_paths) {
    if (bundle_future_.valid()) return make_error(ErrorCode::InvalidArgument, "a bundle is already being created");
    if (output.empty()) return make_error(ErrorCode::InvalidArgument, "choose where to save the bundle");
    if (output.extension() != ".zip")
        return make_error(ErrorCode::InvalidArgument, "the bundle file name must end in .zip");
    std::error_code ec;
    if (sfs::is_directory(output, ec))
        return make_error(ErrorCode::InvalidArgument, "that is a folder; add a file name ending in .zip");
    player::BundleOptions o;
    o.output = output;
    o.diagnostics_dir = diagnostics_dir();
    o.requested_by = "VICTUS Studio";
    o.redact_paths = !keep_paths;
    if (project_) {
        o.project = project_->root();
        // The check imports nothing, but refresh here (UI thread) so it never writes .meta files
        // at the same time as the editor's asset database.
        if (db_) (void)db_->refresh();
    }
    // What only the editor knows: the GPU it runs on and the state of the Studio.
    const auto& caps = gpu_.device().capabilities();
    std::string gpu = std::format("backend: {}\ndriver: {}\nmax texture size: {}\nmax MSAA: {}\ncompute: {}\n"
                                  "BC textures: {}\nASTC textures: {}\nanisotropy: {}\n",
                                  rhi::to_string(caps.backend), caps.driver_name, caps.max_texture_size, caps.max_msaa,
                                  caps.compute, caps.bc_compression, caps.astc_compression, caps.anisotropy);
    std::string studio = std::format("VICTUS Studio {} ({}, {})\nlayout: {}\nplay state: {}\n", build::version(),
                                     build::git_revision(), build::build_type(), layout_name(layout_),
                                     play_state_ == PlayState::Editing  ? "editing"
                                     : play_state_ == PlayState::Paused ? "paused"
                                                                        : "playing");
    if (project_) {
        studio +=
            std::format("project: {}\nscene: {}\nunsaved changes: {}\nentities: {}\n", project_->settings().name,
                        scene_path_.empty() ? "(unsaved)" : scene_path_, dirty() ? "yes" : "no", edit_scene().size());
        if (problems_)
            studio += std::format("problems: {} errors, {} warnings\n", problems_->errors(), problems_->warnings());
    }
    studio += "open panels:";
    for (int i = 0; i < static_cast<int>(Panel::Count); ++i)
        if (panel_open(static_cast<Panel>(i))) studio += std::format(" {},", panel_title(static_cast<Panel>(i)));
    if (studio.back() == ',') studio.pop_back();
    studio += '\n';
    o.extra = {{"gpu.txt", std::move(gpu)}, {"studio.txt", std::move(studio)}};
    bundle_result_.reset();
    bundle_error_.clear();
    bundle_future_ = std::async(std::launch::async, [o] { return player::create_diagnostics_bundle(o); });
    return {};
}

void Editor::poll_bundle() {
    if (!bundle_future_.valid()) return;
    if (bundle_future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    auto r = bundle_future_.get();
    if (r) {
        bundle_result_ = std::move(r).value();
        notify("Saved a diagnostics bundle to " + bundle_result_->path.string());
    } else {
        bundle_error_ = r.error().message;
        notify("could not create the diagnostics bundle: " + bundle_error_, true);
    }
}

void Editor::wait_for_bundle() {
    if (!bundle_future_.valid()) return;
    bundle_future_.wait();
    poll_bundle();
}

void Editor::draw_diagnostics_dialog() {
    poll_bundle();
    if (show_bundle_) {
        ImGui::OpenPopup(kBundlePopup);
        show_bundle_ = false;
    }
    centre_next_popup(600.0f);
    if (!ImGui::BeginPopupModal(kBundlePopup, nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    const bool running = creating_bundle();
    kit::heading("Report a problem", tk::kTypeHeading);
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(
        tk::kTextMuted,
        "Collects what helps someone find a bug into one .zip file: crash reports, recent logs, your "
        "computer's basic facts and graphics card%s. Nothing is sent anywhere - attach the file to a bug "
        "report yourself.",
        project_ ? ", and this project's settings and check results (no scenes, scripts or assets)" : "");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, tk::kSpace3));

    const std::size_t reports = crash::list_reports(diagnostics_dir()).size();
    if (reports == 0)
        status_line(kit::Icon::Check, tk::kTextMuted, "No crash reports on this computer.");
    else
        status_line(kit::Icon::Warning, tk::kWarning,
                    std::format("{} crash report{} will be included.", reports, reports == 1 ? "" : "s"));
    ImGui::Dummy(ImVec2(0, tk::kSpace2));

    ImGui::BeginDisabled(running);
    ImGui::TextUnformatted("Save as");
    const float browse_w = ImGui::CalcTextSize("Browse...").x + ImGui::GetFrameHeight() + tk::kSpace5;
    ImGui::SetNextItemWidth(-browse_w - tk::kSpace2);
    ImGui::InputText("##bundle_out", &bundle_output_);
    ImGui::SameLine(0, tk::kSpace2);
    if (kit::icon_text_button("Browse...", kit::Icon::Folder, false, platform_ != nullptr)) {
        const fs::Path current = bundle_output_;
        platform_->show_folder_dialog(window_, current.parent_path().string(), [this, current](std::string path) {
            if (!path.empty())
                bundle_output_ = (fs::Path(path) /
                                  (current.filename().empty() ? default_bundle_path().filename() : current.filename()))
                                     .string();
        });
    }
    bool hide = !bundle_keep_paths_;
    if (ImGui::Checkbox("Hide my user name and folder names", &hide)) bundle_keep_paths_ = !hide;
    kit::tooltip("Replaces your home folder, user name and project folder with <home>, <user> and <project> "
                 "in every file of the bundle.");
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, tk::kSpace3));

    if (running)
        status_line(kit::Icon::Info, tk::kInfo, "Creating the bundle...");
    else if (!bundle_error_.empty())
        status_line(kit::Icon::Error, tk::kDanger, "Could not create it: " + bundle_error_);
    else if (bundle_result_) {
        const auto& r = *bundle_result_;
        status_line(kit::Icon::Check, tk::kSuccess,
                    std::format("Saved {} ({}, {} files).", r.path.string(), human_size(r.bytes), r.files.size()));
        ImGui::TextColored(tk::kTextMuted, "Open the .zip to review it before you share it.");
        if (ImGui::SmallButton("Copy file path") && platform_) platform_->set_clipboard(r.path.string());
    }
    ImGui::Dummy(ImVec2(0, tk::kSpace3));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, tk::kSpace2));
    const ImVec2 big(200, ImGui::GetFrameHeight() + tk::kSpace2);
    if (kit::icon_text_button("Create Bundle", kit::Icon::Export, true, !running && !bundle_output_.empty(), big)) {
        if (auto st = start_diagnostics_bundle(bundle_output_, bundle_keep_paths_); !st.ok())
            bundle_error_ = st.error().message;
    }
    ImGui::SameLine();
    if (kit::icon_text_button("Close", kit::Icon::Close, false, true, ImVec2(0, big.y))) ImGui::CloseCurrentPopup();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void Editor::draw_crash_notice() {
    if (!crash_notice_) return;
    // A quiet card in the bottom-right corner, above the status bar; it never blocks work.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float w = std::min(420.0f, vp->WorkSize.x - 2 * tk::kSpace5);
    const float bottom = project_ ? ImGui::GetFrameHeight() + tk::kSpace4 : tk::kSpace5;
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x - tk::kSpace5, vp->WorkPos.y + vp->WorkSize.y - bottom), ImGuiCond_Always,
        ImVec2(1, 1));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(tk::kSpace5, tk::kSpace4));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, tk::kSurfacePanel);
    ImGui::PushStyleColor(ImGuiCol_Border, tk::kWarning);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##crash_notice", nullptr, flags)) {
        kit::icon(kit::Icon::Warning, tk::kWarning);
        ImGui::SameLine();
        ImGui::PushFont(kit::bold_font(), 0);
        ImGui::TextUnformatted("VICTUS Studio closed unexpectedly last time");
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(tk::kTextMuted,
                           "Unsaved scene changes are autosaved, so reopening the project restores them. "
                           "If it keeps happening, a diagnostics bundle helps fix it.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, tk::kSpace1));
        if (kit::icon_text_button("Report a problem...", kit::Icon::Help, true, true)) {
            crash_notice_ = false;
            open_diagnostics_dialog();
        }
        ImGui::SameLine();
        if (kit::icon_text_button("Dismiss", kit::Icon::Close, false, true)) crash_notice_ = false;
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

void Editor::draw_about_dialog() {
    if (show_about_) {
        ImGui::OpenPopup(kAboutPopup);
        show_about_ = false;
    }
    centre_next_popup(460.0f);
    if (!ImGui::BeginPopupModal(kAboutPopup, nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    ImGui::PushFont(kit::bold_font(), ImGui::GetStyle().FontSizeBase * tk::kTypeTitle);
    ImGui::TextColored(tk::kAccent, "VICTUS");
    ImGui::PopFont();
    ImGui::Text("Studio %s", build::version());
    ImGui::TextColored(tk::kTextMuted, "Build %s, %s, %s %s", build::git_revision(), build::build_type(),
                       build::platform(), build::architecture());
    const auto& caps = gpu_.device().capabilities();
    const std::string graphics =
        caps.driver_name.empty() ? std::string(rhi::to_string(caps.backend)) : caps.driver_name;
    ImGui::TextColored(tk::kTextMuted, "Graphics: %s", graphics.c_str());
    ImGui::Dummy(ImVec2(0, tk::kSpace2));
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(tk::kTextMuted, "Free and offline. No account needed. Licensed under Apache-2.0.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, tk::kSpace3));
    if (ImGui::SmallButton("Copy version info") && platform_)
        platform_->set_clipboard(std::format("VICTUS Studio {} ({}, {}, {} {}, {})", build::version(),
                                             build::git_revision(), build::build_type(), build::platform(),
                                             build::architecture(), graphics));
    ImGui::SameLine();
    if (ImGui::SmallButton("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace victus::editor
