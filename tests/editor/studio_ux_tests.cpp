// SPDX-License-Identifier: Apache-2.0
// VICTUS Studio UX: design tokens (WCAG contrast), typography, and the workspace chrome (toolbar,
// status bar, console counts) driven headlessly on the GPU.
#include "../render/gpu_fixture.hpp"
#include "victus/core/log.hpp"
#include "victus/editor/editor.hpp"
#include "victus/gameplay/framework.hpp"
#include "victus/core/fs.hpp"
#include "victus/core/zip.hpp"
#include "victus/core/json.hpp"
#include "victus/physics/physics.hpp"
#include "victus/project/project.hpp"
#include "victus/runtime/scene_view.hpp"
#include "victus/script/behaviours.hpp"
#include "victus/ui/style.hpp"
#include "victus/ui/ui.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <fstream>
#include <filesystem>
#include <functional>

using namespace victus;
using victus::test::gpu;
namespace sfs = std::filesystem;
namespace tk = victus::ui::tokens;

namespace {

struct Studio {
    std::unique_ptr<ui::UiContext> ui;
    std::unique_ptr<editor::Editor> ed;
    sfs::path dir;
    static constexpr std::uint32_t W = 1280, H = 720;

    bool init(const sfs::path& samples_dir = {}, bool previous_crash = false,
              const std::function<void(editor::EditorOptions&)>& tweak = {}) {
        auto& ctx = *gpu().ctx;
        dir = sfs::temp_directory_path() / ("victus_ux_" + Uuid::generate().to_string().substr(0, 8));
        sfs::create_directories(dir);
        auto u = ui::UiContext::create(ctx, rhi::Format::RGBA8_UNORM, nullptr, nullptr);
        if (!u) return false;
        ui = std::move(u).value();
        editor::EditorOptions o;
        o.recent_file = dir / "recent.json";
        o.default_project_dir = dir;
        o.samples_dir = samples_dir;
        o.diagnostics_dir = dir / "diagnostics";
        o.previous_session_crashed = previous_crash;
        if (tweak) tweak(o);
        auto e = editor::Editor::create(ctx, *ui, nullptr, nullptr, o);
        if (!e) return false;
        ed = std::move(e).value();
        ed->set_viewport_size_override(320, 180);
        return true;
    }
    void frames(int n) {
        auto& dev = gpu().ctx->device();
        for (int i = 0; i < n; ++i) {
            ui->new_frame(1.0f / 60.0f, {static_cast<float>(W), static_cast<float>(H)});
            ed->frame(1.0f / 60.0f);
            auto cmd = dev.begin_commands();
            REQUIRE(ed->render(cmd).ok());
            REQUIRE(ui->prepare(cmd).ok());
            cmd.submit_and_wait();
        }
    }
    ~Studio() {
        ed.reset();
        ui.reset();
        std::error_code ec;
        sfs::remove_all(dir, ec);
    }
};

} // namespace

TEST_CASE("studio tokens: text and accents meet WCAG contrast on every surface") {
    const ImVec4 surfaces[] = {tk::kSurfaceBase, tk::kSurfacePanel, tk::kSurfaceRaised, tk::kSurfaceHover};
    for (const ImVec4& s : surfaces) {
        CHECK(ui::contrast_ratio(tk::kText, s) >= 7.0f);         // AAA body text
        CHECK(ui::contrast_ratio(tk::kTextMuted, s) >= 4.5f);    // AA secondary text
        CHECK(ui::contrast_ratio(tk::kTextDisabled, s) >= 2.0f); // disabled text is exempt, but visible
        for (const ImVec4 tone : {tk::kAccent, tk::kSuccess, tk::kWarning, tk::kDanger, tk::kInfo})
            CHECK(ui::contrast_ratio(tone, s) >= 4.5f); // status text and icons
    }
    // Text on the accent fill (primary buttons) uses the base surface colour.
    CHECK(ui::contrast_ratio(tk::kSurfaceBase, tk::kAccent) >= 4.5f);
    CHECK(ui::contrast_ratio(ImVec4(1, 1, 1, 1), ImVec4(0, 0, 0, 1)) == doctest::Approx(21.0f));
    CHECK(ui::tone(ui::Tone::Danger).x == tk::kDanger.x);
}

TEST_CASE("studio: Inter typography, toolbar and status bar, console counts") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Studio h;
    REQUIRE(h.init());
    ImGui::SetCurrentContext(h.ui->imgui());
    ImFont* body = ImGui::GetIO().FontDefault;
    REQUIRE(body != nullptr);
    CHECK(std::strcmp(body->GetDebugName(), "Inter Medium") == 0);
    bool bold = false;
    for (ImFont* f : ImGui::GetIO().Fonts->Fonts)
        bold = bold || std::strcmp(f->GetDebugName(), "Inter Bold") == 0;
    CHECK(bold);
    // Theme comes from the tokens.
    CHECK(ImGui::GetStyle().Colors[ImGuiCol_WindowBg].x == doctest::Approx(tk::kSurfacePanel.x));
    CHECK(ImGui::GetStyle().Colors[ImGuiCol_CheckMark].x == doctest::Approx(tk::kAccent.x));

    REQUIRE(h.ed->create_project(h.dir / "Ux", "Ux", "3d").ok());
    h.frames(3);
    ImGui::SetCurrentContext(h.ui->imgui());
    ImGuiWindow* toolbar = ImGui::FindWindowByName("##toolbar");
    ImGuiWindow* status = ImGui::FindWindowByName("##statusbar");
    REQUIRE(toolbar != nullptr);
    REQUIRE(status != nullptr);
    CHECK(toolbar->Active);
    CHECK(status->Active);
    CHECK(toolbar->Size.y > ImGui::GetFrameHeight()); // room for padded icon buttons
    CHECK(status->Pos.y + status->Size.y == doctest::Approx(static_cast<float>(Studio::H)));

    // Console counts follow the log and the Clear action.
    h.ed->clear_console();
    CHECK(h.ed->console_counts().errors == 0);
    VICTUS_LOG_WARN("ux_test", "a warning");
    VICTUS_LOG_ERROR("ux_test", "an error");
    VICTUS_LOG_ERROR("ux_test", "another error");
    auto c = h.ed->console_counts();
    CHECK(c.errors == 2);
    CHECK(c.warnings == 1);
    h.ed->clear_console();
    c = h.ed->console_counts();
    CHECK(c.errors == 0);
    CHECK(c.warnings == 0);

    // The status bar's counters bring the Console to the front.
    h.ed->focus_console();
    h.frames(2);
    ImGui::SetCurrentContext(h.ui->imgui());
    ImGuiContext& g = *ImGui::GetCurrentContext();
    REQUIRE(g.NavWindow != nullptr);
    CHECK(std::strcmp(g.NavWindow->Name, "Console") == 0);
}

TEST_CASE("studio launcher: new-project validation explains every problem in plain words") {
    const sfs::path root = sfs::temp_directory_path() / ("victus_np_" + Uuid::generate().to_string().substr(0, 8));
    sfs::create_directories(root / "Busy");
    { std::ofstream(root / "Busy" / "notes.txt") << "x"; }
    sfs::create_directories(root / "Empty");
    { std::ofstream(root / "Taken.txt") << "x"; }
    using editor::Editor;
    CHECK(Editor::new_project_problem(root, "").find("name") != std::string::npos);
    CHECK(Editor::new_project_problem(root, "   ").find("name") != std::string::npos);
    CHECK(Editor::new_project_problem(root, " Lead").find("space") != std::string::npos);
    CHECK(Editor::new_project_problem(root, "Dot.").find("dot") != std::string::npos);
    CHECK(Editor::new_project_problem(root, "a/b").find("'/'") != std::string::npos);
    CHECK(Editor::new_project_problem(root, "What?").find("'?'") != std::string::npos);
    CHECK(Editor::new_project_problem(root, "CON").find("reserved") != std::string::npos);
    CHECK(Editor::new_project_problem(root, "lpt1").find("reserved") != std::string::npos);
    CHECK(Editor::new_project_problem(root, std::string(101, 'a')).find("long") != std::string::npos);
    CHECK(Editor::new_project_problem({}, "Game").find("where") != std::string::npos);
    CHECK(Editor::new_project_problem(root / "Taken.txt", "Game").find("file") != std::string::npos);
    CHECK(Editor::new_project_problem(root, "Busy").find("not empty") != std::string::npos);
    CHECK(Editor::new_project_problem(root, "Taken.txt").find("file") != std::string::npos);
    CHECK(Editor::new_project_problem(root, "Empty").empty());
    CHECK(Editor::new_project_problem(root, "My Game").empty());
    CHECK(Editor::new_project_problem(root / "not-yet-created", "My Game").empty());
    REQUIRE(project::Project::create(root / "Existing", "Existing", "2d").ok());
    CHECK(Editor::new_project_problem(root, "Existing").find("already exists") != std::string::npos);
    std::error_code ec;
    sfs::remove_all(root, ec);
}

TEST_CASE("studio launcher: pages, samples list and opening a sample copy") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    const sfs::path samples =
        sfs::temp_directory_path() / ("victus_samples_" + Uuid::generate().to_string().substr(0, 8));
    REQUIRE(project::Project::create(samples / "zeta_demo", "Zeta Demo", "2d").ok());
    REQUIRE(project::Project::create(samples / "alpha_demo", "Alpha Demo", "2d").ok());
    {
        std::ofstream(samples / "alpha_demo" / "README.md")
            << "# Alpha Demo\n\nA tiny test sample.\nIt has two lines.\n\n## Controls\nNone.\n";
    }
    sfs::create_directories(samples / "not_a_project");
    sfs::create_directories(samples / "alpha_demo" / ".victus" / "cache");
    { std::ofstream(samples / "alpha_demo" / ".victus" / "cache" / "stale.bin") << "x"; }

    Studio s;
    REQUIRE(s.init(samples));
    // No recent projects yet: the launcher starts on New Project.
    CHECK(s.ed->launcher_page() == editor::LauncherPage::New);
    const auto& list = s.ed->samples();
    REQUIRE(list.size() == 2); // the plain folder is skipped
    CHECK(list[0].name == "Alpha Demo");
    CHECK(list[1].name == "Zeta Demo");
    CHECK(list[0].summary == "A tiny test sample. It has two lines.");

    for (const auto page : {editor::LauncherPage::New, editor::LauncherPage::Recent, editor::LauncherPage::Samples,
                            editor::LauncherPage::Learn}) {
        s.ed->set_launcher_page(page);
        CHECK(s.ed->launcher_page() == page);
        s.frames(2);
        CHECK_FALSE(s.ed->has_project());
    }

    const sfs::path projects = s.dir / "Projects";
    REQUIRE(s.ed->open_sample_copy(list[0].dir, projects).ok());
    REQUIRE(s.ed->has_project());
    CHECK(s.ed->project()->root().string() == (projects / "alpha_demo").string());
    CHECK(sfs::exists(projects / "alpha_demo" / "README.md"));
    CHECK_FALSE(sfs::exists(projects / "alpha_demo" / ".victus" / "cache" / "stale.bin")); // caches are not copied
    s.frames(2);
    s.ed->close_project();
    // A second copy never overwrites the first one.
    REQUIRE(s.ed->open_sample_copy(list[0].dir, projects).ok());
    CHECK(s.ed->project()->root().string() == (projects / "alpha_demo 2").string());
    s.ed->close_project();
    CHECK_FALSE(s.ed->open_sample_copy(samples / "not_a_project", projects).ok());
    // The original sample is untouched.
    CHECK(project::Project::is_project_dir(samples / "alpha_demo"));
    CHECK(sfs::exists(samples / "alpha_demo" / ".victus" / "cache" / "stale.bin"));

    // With recent projects the launcher opens on Recent (unless a page was picked).
    editor::EditorOptions o;
    o.recent_file = s.dir / "recent.json";
    o.default_project_dir = s.dir;
    auto e2 = editor::Editor::create(*gpu().ctx, *s.ui, nullptr, nullptr, o);
    REQUIRE(e2);
    CHECK(e2.value()->launcher_page() == editor::LauncherPage::Recent);
    CHECK(e2.value()->samples().empty()); // no samples folder configured
    e2.value().reset();
    std::error_code ec;
    sfs::remove_all(samples, ec);
}

TEST_CASE("studio workspace: every panel reopens from the Window menu, layout presets dock them") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Studio h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Proj", "Proj", "3d").ok());
    ImGui::SetCurrentContext(h.ui->imgui());
    h.frames(3);
    const auto docked = [](const char* name) {
        ImGuiWindow* w = ImGui::FindWindowByName(name);
        return w && w->DockNode != nullptr && w->Active;
    };
    for (const char* name : {"Viewport", "Hierarchy", "Inspector", "Assets", "Console"})
        CHECK(docked(name));

    // The viewport cannot be closed; core panels close and come back.
    h.ed->set_panel_open(editor::Panel::Viewport, false);
    CHECK(h.ed->panel_open(editor::Panel::Viewport));
    h.ed->set_panel_open(editor::Panel::Hierarchy, false);
    h.ed->set_panel_open(editor::Panel::Console, false);
    h.frames(2);
    CHECK_FALSE(h.ed->panel_open(editor::Panel::Hierarchy));
    CHECK_FALSE(ImGui::FindWindowByName("Hierarchy")->Active);
    h.ed->show_panel(editor::Panel::Hierarchy);
    h.ed->focus_console(); // the status bar's error count reopens a closed Console too
    h.frames(3);
    CHECK(docked("Hierarchy"));
    CHECK(h.ed->panel_open(editor::Panel::Console));
    CHECK(docked("Console"));

    // show_panel brings a docked tab to the front.
    h.ed->show_panel(editor::Panel::Assets);
    h.frames(3);
    ImGuiWindow* assets = ImGui::FindWindowByName("Assets");
    REQUIRE(assets);
    REQUIRE(assets->DockNode);
    CHECK(assets->DockNode->SelectedTabId == assets->TabId);

    // Every panel has a title and a palette command; every layout docks its tools.
    for (int i = 0; i < static_cast<int>(editor::Panel::Count); ++i)
        CHECK(std::strlen(editor::panel_title(static_cast<editor::Panel>(i))) > 0);
    std::size_t window_cmds = 0;
    for (const auto& c : h.ed->commands())
        if (c.name.starts_with("Window: ")) ++window_cmds;
    CHECK(window_cmds ==
          static_cast<std::size_t>(editor::Panel::Count) + static_cast<std::size_t>(editor::Layout::Count) + 1);

    h.ed->apply_layout(editor::Layout::Animation);
    h.frames(4);
    CHECK(h.ed->layout() == editor::Layout::Animation);
    CHECK(h.ed->panel_open(editor::Panel::Animation));
    CHECK(h.ed->panel_open(editor::Panel::Sequencer));
    CHECK(docked("Animation"));
    CHECK(docked("Sequencer"));

    h.ed->apply_layout(editor::Layout::Scripting);
    h.frames(4);
    CHECK_FALSE(h.ed->panel_open(editor::Panel::Animation)); // presets close tools they do not use
    CHECK(h.ed->panel_open(editor::Panel::GraphEditor));
    ImGuiWindow* graph = ImGui::FindWindowByName("###GraphEditor");
    REQUIRE(graph);
    REQUIRE(graph->DockNode);
    CHECK(graph->DockNode->SelectedTabId == graph->TabId); // in front of the Viewport
    CHECK(graph->DockNode == ImGui::FindWindowByName("Viewport")->DockNode);

    h.ed->apply_layout(editor::Layout::Debugging);
    h.frames(4);
    CHECK(h.ed->show_profiler());
    CHECK(docked("Profiler"));
    CHECK(docked("Network"));

    // Reset Layout puts an undocked panel back.
    ImGui::DockContextQueueUndockWindow(ImGui::GetCurrentContext(), ImGui::FindWindowByName("Inspector"));
    h.frames(2);
    CHECK(ImGui::FindWindowByName("Inspector")->DockNode == nullptr);
    h.ed->apply_layout(h.ed->layout());
    h.frames(3);
    CHECK(docked("Inspector"));
    for (int i = 0; i < static_cast<int>(editor::Layout::Count); ++i) {
        h.ed->apply_layout(static_cast<editor::Layout>(i));
        h.frames(2);
        for (const char* name : {"Viewport", "Hierarchy", "Inspector", "Assets", "Console"})
            CHECK(docked(name));
    }
    h.ed->apply_layout(editor::Layout::Default);
    h.frames(2);
}

TEST_CASE("studio problems: project checks run in the background and jump to the problem") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Studio h;
    REQUIRE(h.init());
    ImGui::SetCurrentContext(h.ui->imgui());
    CHECK_FALSE(h.ed->problems().has_value());
    h.ed->check_project(); // no project: nothing happens
    CHECK_FALSE(h.ed->checking_project());

    REQUIRE(h.ed->create_project(h.dir / "Proj", "Proj", "3d").ok());
    // Opening a project starts a check by itself.
    h.ed->wait_for_project_check();
    REQUIRE(h.ed->problems().has_value());
    CHECK(h.ed->problems()->diagnostics.empty());
    CHECK(h.ed->problems()->scenes >= 1);
    CHECK(h.ed->problems_error().empty());
    h.ed->show_panel(editor::Panel::Problems);
    h.frames(3);
    ImGuiWindow* panel = ImGui::FindWindowByName("###Problems");
    REQUIRE(panel);
    CHECK(panel->DockNode != nullptr);
    CHECK(panel->DockNode == ImGui::FindWindowByName("Console")->DockNode); // tabbed with the Console
    CHECK(panel->DockNode->SelectedTabId == panel->TabId);

    // Break a script used by scene entities and save: the save re-checks.
    const sfs::path script = h.dir / "Proj" / "assets" / "scripts" / "crate.luau";
    REQUIRE(sfs::exists(script));
    { std::ofstream(script, std::ios::app) << "\nlocal x = = 1\n"; }
    REQUIRE(h.ed->save_scene().ok());
    CHECK(h.ed->checking_project());
    h.frames(1); // drawing while the check runs is fine
    h.ed->wait_for_project_check();
    REQUIRE(h.ed->problems().has_value());
    const auto& report = *h.ed->problems();
    CHECK(report.errors() >= 2);
    const player::Diagnostic* entity_problem = nullptr;
    for (const auto& d : report.diagnostics)
        if (d.path == h.ed->scene_path() && !d.entity_name.empty()) entity_problem = &d;
    REQUIRE(entity_problem);
    const std::string entity_name = entity_problem->entity_name;
    h.frames(3); // the list renders with problems in it
    CHECK(std::string(panel->Name).starts_with("Problems ("));

    // Going to an entity problem selects that entity.
    h.ed->clear_selection();
    REQUIRE(h.ed->go_to_problem(*entity_problem));
    const scene::Entity picked = h.ed->selected();
    REQUIRE_FALSE(picked.is_null());
    CHECK(h.ed->edit_scene().world().get<scene::Name>(picked)->value == entity_name);

    // Two quick requests while one runs collapse into one more check, not a pile of threads.
    h.ed->check_project();
    h.ed->check_project();
    h.ed->check_project();
    h.ed->wait_for_project_check();
    CHECK_FALSE(h.ed->checking_project());

    // Fixing the script and checking again clears the list.
    {
        std::string text = *fs::read_text(script);
        text.erase(text.find("\nlocal x = = 1\n"));
        std::ofstream(script, std::ios::trunc) << text;
    }
    h.ed->check_project();
    h.ed->wait_for_project_check();
    CHECK(h.ed->problems()->diagnostics.empty());
    h.frames(2);

    // Closing the project waits for a running check and forgets the result.
    h.ed->check_project();
    h.ed->close_project();
    CHECK_FALSE(h.ed->checking_project());
    CHECK_FALSE(h.ed->problems().has_value());
}

TEST_CASE("studio export: Export Game builds a runnable folder off the UI thread") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Studio h;
    REQUIRE(h.init());
    ImGui::SetCurrentContext(h.ui->imgui());
    CHECK_FALSE(h.ed->start_export(h.dir / "out", "Game").ok()); // no project
    REQUIRE(h.ed->create_project(h.dir / "Tiny Game", "Tiny Game", "2d").ok());
    CHECK(h.ed->default_export_dir() == h.dir / "Tiny Game" / "build" / "Tiny_Game");

    // The dialog opens with defaults and renders while the project check runs.
    h.ed->open_export_dialog();
    h.frames(2);
    CHECK(ImGui::FindWindowByName("Export Game") != nullptr);
    h.ed->wait_for_project_check();
    h.frames(2);

    // Wrong folders are refused before any work starts.
    { std::ofstream(h.dir / "a_file") << "x"; }
    CHECK_FALSE(h.ed->start_export(h.dir / "a_file", "Game").ok());
    sfs::create_directories(h.dir / "busy");
    { std::ofstream(h.dir / "busy" / "notes.txt") << "x"; }
    const Status busy = h.ed->start_export(h.dir / "busy", "Game");
    REQUIRE_FALSE(busy.ok());
    CHECK(busy.error().message.find("other files") != std::string::npos);
    CHECK_FALSE(h.ed->start_export({}, "Game").ok());
    CHECK_FALSE(h.ed->exporting());

    const sfs::path out = h.dir / "export";
    REQUIRE(h.ed->start_export(out, "Tiny").ok());
    CHECK(h.ed->exporting());
    CHECK_FALSE(h.ed->start_export(out, "Tiny").ok()); // one at a time
    h.frames(1);                                       // the editor keeps drawing meanwhile
    h.ed->wait_for_export();
    CHECK_FALSE(h.ed->exporting());
    REQUIRE_MESSAGE(h.ed->last_export().has_value(), h.ed->export_error());
    const auto& r = *h.ed->last_export();
    CHECK(r.files > 0);
    CHECK(sfs::exists(out / "game" / "game.vpak"));
    CHECK(sfs::exists(out / "export.json"));
    CHECK(sfs::exists(r.executable));
    CHECK(r.executable.filename().string().starts_with("Tiny"));
    h.frames(2); // the dialog shows the result

    // Exporting again into a previous export is allowed (it replaces it).
    REQUIRE(h.ed->start_export(out, "Tiny").ok());
    h.ed->wait_for_export();
    CHECK(h.ed->last_export().has_value());
    CHECK(h.ed->export_error().empty());
    h.ed->close_project();
    CHECK_FALSE(h.ed->last_export().has_value());
}

TEST_CASE("studio diagnostics: Report a Problem bundles crash reports, logs and Studio facts") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Studio h;
    REQUIRE(h.init({}, true));
    ImGui::SetCurrentContext(h.ui->imgui());
    // The previous session crashed: the notice shows on the start screen without blocking it.
    CHECK(h.ed->crash_notice_visible());
    h.frames(2);
    CHECK(ImGui::FindWindowByName("##crash_notice") != nullptr);
    h.ed->set_crash_notice(false);

    const sfs::path diag = h.dir / "diagnostics";
    CHECK(h.ed->diagnostics_dir() == diag);
    CHECK(h.ed->default_bundle_path().extension() == ".zip");
    CHECK(h.ed->default_bundle_path().filename().string().starts_with("victus-diagnostics-"));
    sfs::create_directories(diag / "crashes");
    sfs::create_directories(diag / "logs");
    { std::ofstream(diag / "crashes" / "crash-1700000000-42-victus_editor.txt") << "reason: SIGSEGV\n"; }
    {
        std::ofstream(diag / "logs" / "victus_editor-1700000000-42.log")
            << "[info] opened " << (h.dir / "Bug Game").string() << "\n";
    }

    CHECK_FALSE(h.ed->start_diagnostics_bundle({}).ok());
    CHECK_FALSE(h.ed->start_diagnostics_bundle(h.dir / "bundle.txt").ok());
    CHECK_FALSE(h.ed->start_diagnostics_bundle(h.dir).ok()); // a folder

    REQUIRE(h.ed->create_project(h.dir / "Bug Game", "Bug Game", "2d").ok());
    h.ed->wait_for_project_check();
    h.ed->open_diagnostics_dialog();
    h.frames(2);
    CHECK(ImGui::FindWindowByName("Create Diagnostics Bundle") != nullptr);

    const sfs::path out = h.dir / "reports" / "bug.zip";
    REQUIRE(h.ed->start_diagnostics_bundle(out).ok());
    CHECK(h.ed->creating_bundle());
    CHECK_FALSE(h.ed->start_diagnostics_bundle(out).ok()); // one at a time
    h.frames(1);                                           // the editor keeps drawing meanwhile
    h.ed->wait_for_bundle();
    CHECK_FALSE(h.ed->creating_bundle());
    REQUIRE_MESSAGE(h.ed->last_bundle().has_value(), h.ed->bundle_error());
    const auto& r = *h.ed->last_bundle();
    CHECK(r.crash_reports == 1);
    CHECK(r.logs == 1);
    REQUIRE(sfs::exists(out));
    const auto bytes = fs::read_binary(out);
    REQUIRE(bytes.ok());
    const auto entries = zip::read(bytes.value());
    REQUIRE(entries.ok());
    std::string studio, gpu_txt, all;
    for (const auto& e : entries.value()) {
        const std::string text(e.data.begin(), e.data.end());
        all += text;
        if (e.name == "extra/studio.txt") studio = text;
        if (e.name == "extra/gpu.txt") gpu_txt = text;
    }
    CHECK(studio.find("project: Bug Game") != std::string::npos);
    CHECK(studio.find("open panels:") != std::string::npos);
    CHECK(gpu_txt.find("backend:") != std::string::npos);
    // Redacted by default: the project folder never appears, not even in the log.
    CHECK(all.find((h.dir / "Bug Game").string()) == std::string::npos);
    h.frames(2); // the dialog shows the result

    // About renders.
    h.ed->open_about_dialog();
    h.frames(2);
    CHECK(ImGui::FindWindowByName("About VICTUS Studio") != nullptr);
}

TEST_CASE("studio hierarchy: deep trees start collapsed, search keeps parents, selection is revealed") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Studio h;
    REQUIRE(h.init());
    ImGui::SetCurrentContext(h.ui->imgui());
    REQUIRE(h.ed->create_project(h.dir / "Tree Game", "Tree Game", "3d").ok());
    h.frames(2);
    const std::size_t total = h.ed->edit_scene().size();
    const std::size_t collapsed = h.ed->hierarchy_rows();
    CHECK(collapsed > 0);
    CHECK(collapsed < total / 2); // the characters' bones are not listed until opened

    // Search: matches plus the parents that lead to them, nothing else.
    h.ed->set_hierarchy_filter("hand.l");
    h.frames(2);
    CHECK(h.ed->hierarchy_matches() >= 2); // Hand.L and Socket.Hand.L on each character
    CHECK(h.ed->hierarchy_rows() > h.ed->hierarchy_matches());
    CHECK(h.ed->hierarchy_rows() < collapsed + 40);
    h.ed->set_hierarchy_filter("HAND.L"); // case does not matter
    h.frames(1);
    const std::size_t upper = h.ed->hierarchy_matches();
    CHECK(upper >= 2);
    h.ed->set_hierarchy_filter("no such entity");
    h.frames(2);
    CHECK(h.ed->hierarchy_matches() == 0);
    CHECK(h.ed->hierarchy_rows() == 0);
    h.ed->set_hierarchy_filter("");
    h.frames(2);
    CHECK(h.ed->hierarchy_rows() == collapsed);

    // Selecting a bone (as a viewport click does) opens its parents.
    const auto hand = h.ed->edit_scene().find_by_name("Hand.L");
    REQUIRE_FALSE(hand.is_null());
    h.ed->select(hand);
    h.frames(3);
    CHECK(h.ed->hierarchy_rows() > collapsed);

    // The create menu adds with the new entity selected, also as a child.
    const std::size_t before = h.ed->edit_scene().size();
    const auto cube = h.ed->create_entity(editor::Primitive::Cube, hand);
    REQUIRE_FALSE(cube.is_null());
    CHECK(h.ed->edit_scene().parent(cube) == hand);
    CHECK(h.ed->edit_scene().size() == before + 1);
    h.frames(2);
}

TEST_CASE("studio inspector: Add Component lists plain names by category and adds in one undo step") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Studio h;
    REQUIRE(h.init());
    ImGui::SetCurrentContext(h.ui->imgui());
    REQUIRE(h.ed->create_project(h.dir / "Inspect Game", "Inspect Game", "3d").ok());
    h.frames(2);

    // Nothing selected: the Inspector explains what to do instead of showing a blank panel.
    h.ed->clear_selection();
    h.frames(2);

    const auto cube = h.ed->create_entity(editor::Primitive::Cube, {});
    REQUIRE_FALSE(cube.is_null());
    const auto find = [&](std::string_view id) -> std::optional<editor::ComponentChoice> {
        for (const auto& c : h.ed->component_choices(cube))
            if (c.id == id) return c;
        return std::nullopt;
    };
    const auto body = find("RigidBody");
    REQUIRE(body.has_value());
    CHECK(body->category == "Physics 3D");
    CHECK(body->name == "Rigid Body");
    CHECK_FALSE(body->description.empty());
    CHECK_FALSE(body->present);
    const auto body2d = find("RigidBody2D");
    REQUIRE(body2d.has_value());
    CHECK(body2d->name == "Rigid Body 2D");
    // No-code behaviours come straight after rendering, each explained in plain words.
    const auto platformer = find("PlatformerMovement");
    REQUIRE(platformer.has_value());
    CHECK(platformer->category == "Behaviours");
    CHECK(platformer->name == "Platformer Movement");
    std::string previous_category;
    std::vector<std::string> categories;
    for (const auto& c : h.ed->component_choices(cube)) {
        if (c.category != previous_category) categories.push_back(previous_category = c.category);
        if (c.category == "Behaviours") CHECK_MESSAGE(!c.description.empty(), c.id);
    }
    REQUIRE(categories.size() > 2);
    CHECK(categories[0] == "Rendering");
    CHECK(categories[1] == "Behaviours");
    for (const auto& c : h.ed->component_choices(cube)) {
        CHECK_FALSE(c.name.empty());
        CHECK_FALSE(c.category.empty());
    }

    CHECK(h.ed->add_component(cube, "RigidBody"));
    CHECK(find("RigidBody")->present);
    CHECK_FALSE(h.ed->add_component(cube, "RigidBody")); // already there
    CHECK_FALSE(h.ed->add_component(cube, "NoSuchComponent"));
    h.ed->undo();
    CHECK_FALSE(find("RigidBody")->present);

    // The picker opens and draws with the cube selected; readable labels everywhere.
    h.ed->select(cube);
    h.ed->open_add_component();
    h.frames(8);
}

TEST_CASE("studio console: severity toggles, search, merged repeats and copy text") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Studio h;
    REQUIRE(h.init());
    ImGui::SetCurrentContext(h.ui->imgui());
    REQUIRE(h.ed->create_project(h.dir / "Console Game", "Console Game", "3d").ok());
    h.ed->focus_console();
    h.frames(2);
    h.ed->clear_console();
    h.frames(2);
    // Other systems may log at any time; every line here carries "zq" and the checks search for it.
    h.ed->set_console_filter("zq");

    VICTUS_LOG_INFO("zqgame", "coin collected");
    VICTUS_LOG_INFO("zqgame", "coin collected");
    VICTUS_LOG_INFO("zqgame", "coin collected");
    VICTUS_LOG_WARN("zqaudio", "sound missing: jump.wav");
    VICTUS_LOG_ERROR("zqgame", "player.luau:12: attempt to index nil");
    h.frames(2);
    CHECK(h.ed->console_rows() == 3); // the three repeats are one row
    const std::string all = h.ed->console_text();
    CHECK(all.find("coin collected (x3)") != std::string::npos);
    CHECK(all.find("sound missing") != std::string::npos);

    h.ed->set_console_collapse(false);
    CHECK(h.ed->console_rows() == 5);
    h.ed->set_console_collapse(true);

    h.ed->set_console_levels(false, false, true); // errors only
    h.frames(1);
    CHECK(h.ed->console_rows() == 1);
    CHECK(h.ed->console_text().find("attempt to index nil") != std::string::npos);
    h.ed->set_console_levels(true, true, true);

    h.ed->set_console_filter("ZQAUDIO"); // category, any case
    h.frames(1);
    CHECK(h.ed->console_rows() == 1);
    h.ed->set_console_filter("nothing like this");
    h.frames(2); // the "Nothing to show" state draws
    CHECK(h.ed->console_rows() == 0);
    h.ed->set_console_filter("zq");
    CHECK(h.ed->console_rows() == 3);
    h.ed->set_console_filter("");
    CHECK(h.ed->console_rows() >= 3);
    h.ed->clear_console();
    h.frames(1);
    h.ed->set_console_filter("zq");
    CHECK(h.ed->console_rows() == 0);
}

TEST_CASE("studio assets: search, type and folder filters, plain type names, empty states") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Studio h;
    REQUIRE(h.init());
    ImGui::SetCurrentContext(h.ui->imgui());
    CHECK(std::string(editor::Editor::asset_kind_label(assets::AssetKind::Texture)) == "Image");
    CHECK(std::string(editor::Editor::asset_kind_label(assets::AssetKind::Model)) == "3D Model");
    CHECK(std::string(editor::Editor::asset_kind_label(assets::AssetKind::SpriteSheet)) == "Sprite Sheet");

    REQUIRE(h.ed->create_project(h.dir / "Asset Game", "Asset Game", "3d").ok());
    h.frames(2);
    const std::size_t total = h.ed->asset_rows();
    REQUIRE(total > 0);
    const auto folders = h.ed->asset_folders();
    REQUIRE_FALSE(folders.empty());
    CHECK(std::is_sorted(folders.begin(), folders.end()));

    // A folder narrows the list (to it and below) and every folder holds something.
    std::size_t in_folders = 0;
    for (const auto& f : folders) {
        h.ed->set_asset_folder(f);
        CHECK(h.ed->asset_rows() > 0);
        CHECK(h.ed->asset_rows() <= total);
        if (f.find('/') == std::string::npos) in_folders += h.ed->asset_rows();
    }
    h.ed->set_asset_folder("");
    CHECK(in_folders <= total);

    // One type, or a search by name or by type name.
    h.ed->set_asset_kind_filter(assets::AssetKind::Model);
    const std::size_t models = h.ed->asset_rows();
    CHECK(models > 0);
    CHECK(models < total);
    h.ed->set_asset_kind_filter(std::nullopt);
    h.ed->set_asset_filter("3D MODEL");
    CHECK(h.ed->asset_rows() == models);
    h.ed->set_asset_filter(".GLB");
    const std::size_t glb = h.ed->asset_rows();
    h.ed->set_asset_filter(".glb");
    CHECK(h.ed->asset_rows() == glb);
    h.frames(2); // list view with results

    h.ed->set_asset_filter("no asset is called this");
    CHECK(h.ed->asset_rows() == 0);
    h.frames(2); // "Nothing matches" state
    h.ed->set_asset_view(true, 64);
    h.frames(2);
    h.ed->set_asset_filter("");
    CHECK(h.ed->asset_rows() == total);
    h.frames(2); // grid view with the folder list
}

TEST_CASE("studio import: dropped files are copied into the right folders and imported") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Studio h;
    REQUIRE(h.init());
    ImGui::SetCurrentContext(h.ui->imgui());
    REQUIRE(h.ed->create_project(h.dir / "Drop Game", "Drop Game", "2d").ok());
    h.frames(2);
    const sfs::path outside = h.dir / "Downloads";
    sfs::create_directories(outside / "pack" / "more");
    std::ofstream(outside / "hello.luau") << "print('hello')\n";
    std::ofstream(outside / "hero.fbx") << "not really";
    std::ofstream(outside / "pack" / "a.luau") << "return 1\n";
    std::ofstream(outside / "pack" / "more" / "b.luau") << "return 2\n";
    // A .gltf with an outside buffer and texture, plus a data: URI and an escaping path that are ignored.
    std::ofstream(outside / "ship.gltf")
        << R"({"asset":{"version":"2.0"},"buffers":[{"uri":"ship%20data.bin","byteLength":4},)"
        << R"({"uri":"data:application/octet-stream;base64,AAAA","byteLength":3}],)"
        << R"("images":[{"uri":"tex/hull.png"},{"uri":"../secret.png"}]})";
    std::ofstream(outside / "ship data.bin") << "abcd";
    sfs::create_directories(outside / "tex");
    std::ofstream(outside / "tex" / "hull.png") << "png?";

    const auto r = h.ed->import_files({outside / "hello.luau", outside / "hero.fbx", outside / "pack",
                                       outside / "ship.gltf", outside / "missing.png"});
    const sfs::path assets = h.dir / "Drop Game" / "assets";
    CHECK(sfs::exists(assets / "scripts" / "hello.luau"));
    CHECK(sfs::exists(assets / "pack" / "more" / "b.luau"));
    CHECK(sfs::exists(assets / "models" / "ship" / "ship.gltf"));
    CHECK(sfs::exists(assets / "models" / "ship" / "ship data.bin"));
    CHECK(sfs::exists(assets / "models" / "ship" / "tex" / "hull.png"));
    CHECK_FALSE(sfs::exists(assets / "models" / "secret.png"));
    const auto added = [&](std::string_view p) {
        return std::find(r.added.begin(), r.added.end(), p) != r.added.end();
    };
    CHECK(added("scripts/hello.luau"));
    CHECK(added("pack/a.luau"));
    CHECK(added("pack/more/b.luau"));
    CHECK(added("models/ship/ship.gltf"));
    // Unsupported and missing files are explained, not silently dropped.
    const auto said = [&](std::string_view needle) {
        return std::any_of(r.skipped.begin(), r.skipped.end(),
                           [&](const std::string& s) { return s.find(needle) != std::string::npos; });
    };
    CHECK(said("hero.fbx"));
    CHECK(said("glTF"));
    CHECK(said("missing.png"));

    // Same name again: a second copy, not an overwrite. Files already in the project are left alone.
    const auto again = h.ed->import_files({outside / "hello.luau", assets / "scripts" / "hello.luau"});
    CHECK(again.added == std::vector<std::string>{"scripts/hello 2.luau"});
    REQUIRE(again.skipped.size() == 1);
    CHECK(again.skipped[0].find("already in this project") != std::string::npos);

    // With a folder picked in the Assets panel, files go there.
    h.ed->set_asset_folder("pack");
    const auto into = h.ed->import_files({outside / "pack" / "a.luau"});
    CHECK(into.added == std::vector<std::string>{"pack/a 2.luau"});
    h.frames(2);
}

TEST_CASE("studio guide: interface size, Getting Started steps, help search, undo history") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Studio h;
    REQUIRE(h.init());
    CHECK(h.ed->ui_scale() == doctest::Approx(1.0f));
    REQUIRE(h.ed->create_project(h.dir / "Guide", "Guide", "3d").ok());
    h.frames(2);
    ImGui::SetCurrentContext(h.ui->imgui());
    const float pad = ImGui::GetStyle().FramePadding.y;

    // Interface size: rescales fonts and spacing from the unscaled theme, steps through the
    // presets, clamps, and is saved for the next session.
    h.ed->set_ui_scale(1.3f);
    h.frames(2);
    ImGui::SetCurrentContext(h.ui->imgui());
    CHECK(ImGui::GetStyle().FontScaleMain == doctest::Approx(1.3f));
    CHECK(ImGui::GetStyle().FramePadding.y > pad); // ImGui rounds scaled sizes down to whole pixels
    h.ed->step_ui_scale(1);
    CHECK(h.ed->ui_scale() == doctest::Approx(1.5f));
    h.ed->step_ui_scale(-1);
    CHECK(h.ed->ui_scale() == doctest::Approx(1.3f));
    h.ed->set_ui_scale(10.0f);
    CHECK(h.ed->ui_scale() == doctest::Approx(editor::kMaxUiScale));
    h.ed->set_ui_scale(1.15f);
    {
        auto text = fs::read_text(h.ed->preferences_file());
        REQUIRE(text);
        auto doc = json::parse(text.value());
        REQUIRE(doc);
        CHECK(doc.value()["ui_scale"].as_float() == doctest::Approx(1.15f));
    }
    h.ed->step_ui_scale(0);
    CHECK(h.ed->ui_scale() == doctest::Approx(1.0f));
    h.frames(2);
    ImGui::SetCurrentContext(h.ui->imgui());
    CHECK(ImGui::GetStyle().FramePadding.y == doctest::Approx(pad)); // no compounding

    // Getting Started opens with a new user's first project; steps tick off from real actions.
    CHECK(h.ed->panel_open(editor::Panel::GettingStarted));
    CHECK(h.ed->guide_done_count() == 0);
    h.ed->run_guide_step(editor::GuideStep::AddObject);
    h.frames(1);
    CHECK(h.ed->guide_done(editor::GuideStep::AddObject));
    CHECK_FALSE(h.ed->guide_done(editor::GuideStep::MoveObject));
    h.ed->run_guide_step(editor::GuideStep::MoveObject);
    h.frames(1);
    CHECK(h.ed->guide_done(editor::GuideStep::MoveObject));
    h.ed->run_guide_step(editor::GuideStep::EditSetting);
    h.frames(1);
    CHECK(h.ed->guide_done(editor::GuideStep::EditSetting));
    const scene::Entity cube = h.ed->selected();
    REQUIRE_FALSE(cube.is_null());
    const Uuid cube_id = h.ed->edit_scene().uuid(cube);
    CHECK(h.ed->edit_scene().world().get<physics::RigidBodyComponent>(cube) != nullptr);
    CHECK(editor::guide_step_order()[3] == editor::GuideStep::AddBehaviour);
    h.ed->run_guide_step(editor::GuideStep::AddBehaviour);
    h.frames(1);
    CHECK(h.ed->guide_done(editor::GuideStep::AddBehaviour));
    CHECK(h.ed->edit_scene().world().get<script::PatrolComponent>(cube) != nullptr);
    h.ed->run_guide_step(editor::GuideStep::Play);
    h.frames(2);
    CHECK(h.ed->guide_done(editor::GuideStep::Play));
    h.ed->stop();
    h.ed->run_guide_step(editor::GuideStep::Save);
    CHECK(h.ed->guide_done(editor::GuideStep::Save));
    CHECK_FALSE(h.ed->guide_done(editor::GuideStep::Export));
    CHECK(h.ed->guide_done_count() == 6);
    h.frames(2); // the checklist draws with progress

    // Undo History: jump back to before every edit and forward again.
    const std::size_t edits = h.ed->undo_stack().size();
    REQUIRE(edits >= 3);
    CHECK(h.ed->undo_to(-1));
    CHECK(h.ed->undo_stack().position() == 0);
    CHECK(h.ed->edit_scene().find(cube_id).is_null());
    CHECK(h.ed->undo_to(static_cast<std::ptrdiff_t>(edits) - 1));
    CHECK(h.ed->undo_stack().position() == edits);
    CHECK_FALSE(h.ed->undo_to(static_cast<std::ptrdiff_t>(edits)));
    h.ed->show_panel(editor::Panel::History);
    h.frames(2);
    ImGui::SetCurrentContext(h.ui->imgui());
    CHECK(ImGui::FindWindowByName("Undo History") != nullptr);

    // Hiding keeps progress; restarting clears it and shows the checklist again.
    h.ed->dismiss_guide();
    CHECK_FALSE(h.ed->panel_open(editor::Panel::GettingStarted));
    CHECK(h.ed->guide_dismissed());
    h.ed->reset_guide();
    CHECK(h.ed->guide_done_count() == 0);
    CHECK(h.ed->panel_open(editor::Panel::GettingStarted));

    // Help: every topic for an empty search, word search over titles, bodies and keywords.
    CHECK(editor::find_help_topics("").size() == editor::help_topics().size());
    const auto ragdoll = editor::find_help_topics("ragdoll");
    REQUIRE_FALSE(ragdoll.empty());
    CHECK(std::string_view(ragdoll.front()->title) == "Physics and ragdolls");
    CHECK(std::string_view(editor::find_help_topics("bigger text")[0]->title) == "Make the Studio bigger or smaller");
    CHECK(editor::find_help_topics("qqqzzz").empty());
    h.ed->open_help("Keyboard shortcuts");
    h.ed->show_panel(editor::Panel::ProjectSettings);
    h.frames(3);
    ImGui::SetCurrentContext(h.ui->imgui());
    CHECK(h.ed->panel_open(editor::Panel::Help));
    CHECK(ImGui::FindWindowByName("Help") != nullptr);
    CHECK(ImGui::FindWindowByName("Project Settings") != nullptr);
}

TEST_CASE("studio guide: in a 2D scene the steps make a sprite with 2D physics") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Studio h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Guide2D", "Guide2D", "2d").ok());
    h.frames(2);
    h.ed->run_guide_step(editor::GuideStep::AddObject);
    h.frames(2); // the checklist draws its 2D wording
    CHECK(h.ed->guide_done(editor::GuideStep::AddObject));
    const scene::Entity e = h.ed->selected();
    REQUIRE_FALSE(e.is_null());
    CHECK(h.ed->edit_scene().world().get<scene::SpriteComponent>(e) != nullptr);
    CHECK(h.ed->edit_scene().world().get<scene::MeshRendererComponent>(e) == nullptr);
    h.ed->run_guide_step(editor::GuideStep::EditSetting);
    h.frames(1);
    CHECK(h.ed->edit_scene().world().get<physics::RigidBody2DComponent>(e) != nullptr);
}

TEST_CASE("studio roles: Make it a Player, Enemy, Coin or Goal in one undoable click") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Studio h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Roles", "Roles", "3d").ok());
    h.frames(2);
    auto& s = h.ed->edit_scene();
    auto& w = s.world();
    // The 3D template has no behaviours yet: making a player adds movement, health, Game Rules
    // and a camera that follows it.
    CHECK(w.count<script::GameRulesComponent>() == 0);
    const scene::Entity cube = h.ed->create_entity(editor::Primitive::Cube);
    h.ed->select(cube);
    const std::size_t edits = h.ed->undo_stack().size();
    CHECK(h.ed->make_role(cube, editor::Role::Player));
    CHECK(h.ed->undo_stack().size() == edits + 1);
    CHECK(w.get<scene::Name>(cube)->value == "Player");
    CHECK(w.get<script::TopDownMovementComponent>(cube));
    CHECK(w.get<script::HealthComponent>(cube)->on_death == script::DeathAction::Respawn);
    CHECK(w.count<script::GameRulesComponent>() == 1);
    CHECK(w.count<script::CameraFollowComponent>() == 1);
    CHECK(h.ed->guide_done(editor::GuideStep::AddBehaviour));
    CHECK_FALSE(h.ed->make_role(cube, editor::Role::Player)); // nothing left to add
    h.frames(2); // the Inspector draws the buttons
    // One undo takes it all back.
    h.ed->undo();
    CHECK_FALSE(w.get<script::TopDownMovementComponent>(cube));
    CHECK(w.count<script::GameRulesComponent>() == 0);
    CHECK(w.count<script::CameraFollowComponent>() == 0);
    CHECK(w.get<scene::Name>(cube)->value == "Cube");
    h.ed->redo();
    CHECK(w.get<script::TopDownMovementComponent>(cube));
    // The other roles.
    const scene::Entity enemy = h.ed->create_entity(editor::Primitive::Cube);
    CHECK(h.ed->make_role(enemy, editor::Role::Enemy));
    CHECK(w.get<script::HazardComponent>(enemy)->stompable);
    CHECK(w.get<script::PatrolComponent>(enemy));
    CHECK(w.get<script::HealthComponent>(enemy)->max == doctest::Approx(1));
    const scene::Entity coin = h.ed->create_entity(editor::Primitive::Sphere);
    CHECK(h.ed->make_role(coin, editor::Role::Coin));
    CHECK(w.get<script::CollectibleComponent>(coin));
    CHECK(w.get<gameplay::RotatorComponent>(coin));
    const scene::Entity goal = h.ed->create_entity(editor::Primitive::Cube);
    CHECK(h.ed->make_role(goal, editor::Role::Goal));
    CHECK(w.get<script::GoalComponent>(goal));
    CHECK(w.get<scene::Name>(goal)->value == "Goal");
    CHECK(w.count<script::GameRulesComponent>() == 1); // reused, not duplicated
    const scene::Entity lift = h.ed->create_entity(editor::Primitive::Cube);
    CHECK(h.ed->make_role(lift, editor::Role::MovingPlatform));
    CHECK(w.get<script::MovingPlatformComponent>(lift));
    CHECK(w.get<scene::Name>(lift)->value == "Platform");
    const scene::Entity door = h.ed->create_entity(editor::Primitive::Cube);
    CHECK(h.ed->make_role(door, editor::Role::Door));
    CHECK(w.get<script::TriggerComponent>(door)->required_item == "Key");
    CHECK(w.get<physics::ColliderComponent>(door));
    const scene::Entity key = h.ed->create_entity(editor::Primitive::Sphere);
    CHECK(h.ed->make_role(key, editor::Role::Key));
    CHECK(w.get<script::CollectibleComponent>(key)->item == "Key");
    const scene::Entity sign = h.ed->create_entity(editor::Primitive::Cube);
    CHECK(h.ed->make_role(sign, editor::Role::Sign));
    CHECK_FALSE(w.get<script::TriggerComponent>(sign)->message.empty());
    const scene::Entity pad = h.ed->create_entity(editor::Primitive::Cube);
    CHECK(h.ed->make_role(pad, editor::Role::JumpPad));
    CHECK(w.get<script::JumpPadComponent>(pad));
    const scene::Entity lever = h.ed->create_entity(editor::Primitive::Cube);
    CHECK(h.ed->make_role(lever, editor::Role::Switch));
    CHECK(w.get<script::TriggerComponent>(lever)->press_to_use);
    CHECK(w.get<script::TriggerComponent>(lever)->target == "Platform");
    const scene::Entity npc = h.ed->create_entity(editor::Primitive::Cube);
    CHECK(h.ed->make_role(npc, editor::Role::Talker));
    CHECK(w.get<script::TriggerComponent>(npc)->press_to_use);
    CHECK(w.get<script::TriggerComponent>(npc)->message.find('|') != std::string::npos);
    h.ed->select(goal);
    h.frames(2);
}

TEST_CASE("studio lessons: a first 2D game built step by step, played to the goal and saved") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Studio h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Lesson", "Lesson", "2d").ok());
    h.frames(2);
    REQUIRE(h.ed->start_lesson(editor::Lesson::First2D));
    CHECK(h.ed->lesson() == editor::Lesson::First2D);
    CHECK(h.ed->scene_path() == "scenes/lesson_2d.vscene");
    CHECK(runtime::scene_view_mode(h.ed->edit_scene()) == runtime::ViewMode::TwoD);
    CHECK(h.ed->lesson_step() == 0);
    h.frames(2); // the lesson draws in the Getting Started panel
    // Steps 1-5 with their "do it for me" buttons.
    for (std::size_t i = 0; i < 5; ++i) {
        CAPTURE(i);
        h.ed->run_lesson_step();
        h.frames(1);
        CHECK(h.ed->lesson_step() == i + 1);
    }
    auto& w = h.ed->edit_scene().world();
    CHECK(w.count<script::PlatformerMovementComponent>() == 1);
    CHECK(w.count<script::CollectibleComponent>() == 1);
    CHECK(w.count<script::GoalComponent>() == 1);
    CHECK(w.count<script::GameRulesComponent>() == 1);
    // Play: walk the player to the goal.
    h.ed->run_lesson_step();
    REQUIRE(h.ed->play_state() == editor::PlayState::Playing);
    h.frames(30); // the player lands on the floor
    auto& ps = h.ed->active_scene();
    const scene::Entity player = ps.find_by_name("Player");
    REQUIRE_FALSE(player.is_null());
    CHECK(ps.world().get<scene::Transform>(player)->position.y > -3.0f); // standing on the floor
    ps.world().get<scene::Transform>(player)->position = {7.2f, -2.5f, 0};
    for (int i = 0; i < 120 && h.ed->lesson_step() < 6; ++i) h.frames(1);
    CHECK(h.ed->lesson_step() == 6);
    h.ed->stop();
    h.ed->run_lesson_step(); // save
    h.frames(1);
    CHECK(h.ed->lesson_step() == editor::lesson_step_count());
    CHECK(std::filesystem::exists(h.dir / "Lesson" / "scenes" / "lesson_2d.vscene"));
    h.frames(2);
    h.ed->stop_lesson();
    CHECK(h.ed->lesson() == editor::Lesson::None);

    // A second lesson gets its own scene file; steps also tick off from the user's own edits.
    REQUIRE(h.ed->start_lesson(editor::Lesson::First3D));
    CHECK(h.ed->scene_path() == "scenes/lesson_3d.vscene");
    const scene::Entity floor = h.ed->create_entity(editor::Primitive::Cube);
    REQUIRE(h.ed->add_component(floor, "Collider"));
    h.ed->edit_scene().world().get<scene::Transform>(floor)->position = {0, -0.5f, 0};
    h.ed->edit_scene().world().get<scene::Transform>(floor)->scale = {22, 1, 6};
    h.frames(1);
    CHECK(h.ed->lesson_step() == 1);
    const scene::Entity hero = h.ed->create_entity(editor::Primitive::Cube);
    REQUIRE(h.ed->make_role(hero, editor::Role::Player));
    h.ed->edit_scene().world().get<scene::Transform>(hero)->position = {-8, 1, 0};
    h.frames(1);
    CHECK(h.ed->lesson_step() == 2);
    for (int i = 0; i < 3; ++i) h.ed->run_lesson_step(); // coin, enemy, goal
    h.frames(1);
    CHECK(h.ed->lesson_step() == 5);
    h.ed->run_lesson_step();
    REQUIRE(h.ed->play_state() == editor::PlayState::Playing);
    h.frames(40);
    {
        auto& s3 = h.ed->active_scene();
        const scene::Entity p = s3.find_by_name("Player");
        REQUIRE_FALSE(p.is_null());
        CHECK(s3.world().get<scene::Transform>(p)->position.y > -0.2f); // stands on the floor
        s3.world().get<scene::Transform>(p)->position = {8.2f, 0.6f, 0};
    }
    for (int i = 0; i < 120 && h.ed->lesson_step() < 6; ++i) h.frames(1);
    CHECK(h.ed->lesson_step() == 6);
    h.ed->stop();
    // An unsaved, never-saved scene is not thrown away for a lesson.
    h.ed->stop_lesson();
    h.ed->new_scene();
    (void)h.ed->create_entity(editor::Primitive::Cube);
    CHECK(h.ed->dirty());
    CHECK_FALSE(h.ed->start_lesson(editor::Lesson::First2D));
}

TEST_CASE("studio ragdolls: Make Ragdoll on Orion, behaviour buttons, undo, and the Ragdoll Lab") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Studio h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Rag", "Rag", "3d").ok());
    h.frames(2);
    scene::Scene& s = h.ed->edit_scene();
    const scene::Entity robot = s.find_by_name("Robot");
    REQUIRE_FALSE(robot.is_null());
    CHECK(h.ed->can_make_ragdoll(robot));
    CHECK_FALSE(h.ed->can_make_ragdoll(s.find_by_name("Sentry")));
    h.ed->select(robot);
    h.frames(2); // Inspector offers the Make Ragdoll card

    // The 21-joint character rig maps to 16 ragdoll body parts (Neck, Shoulders and Toes ride along).
    const std::size_t undo_before = h.ed->undo_stack().size();
    CHECK(h.ed->make_ragdoll(robot) == 16);
    CHECK(h.ed->ragdoll_bone_count(robot) == 16);
    CHECK(h.ed->undo_stack().size() == undo_before + 1);
    for (const char* bone : {"Hips", "Spine", "Chest", "Head", "UpperArm.L", "LowerArm.R", "Hand.L", "UpperLeg.R",
                             "LowerLeg.L", "Foot.R"}) {
        const scene::Entity b = s.find_by_name(bone);
        REQUIRE_FALSE(b.is_null());
        CHECK_MESSAGE(s.world().get<physics::RagdollBoneComponent>(b) != nullptr, bone);
    }
    CHECK(s.world().get<physics::RagdollBoneComponent>(s.find_by_name("Toe.L")) == nullptr);
    CHECK(h.ed->set_ragdoll_mode(robot, physics::RagdollComponent::Mode::Passive));
    CHECK(s.world().get<physics::RagdollComponent>(robot)->mode == physics::RagdollComponent::Mode::Passive);
    h.frames(2); // the behaviour buttons draw

    // Undo the behaviour change, then the ragdoll itself; redo brings both back.
    h.ed->undo();
    CHECK(s.world().get<physics::RagdollComponent>(robot)->mode == physics::RagdollComponent::Mode::Off);
    h.ed->undo();
    CHECK(s.world().get<physics::RagdollComponent>(robot) == nullptr);
    CHECK(h.ed->ragdoll_bone_count(robot) == 0);
    h.ed->redo();
    h.ed->redo();
    CHECK(h.ed->ragdoll_bone_count(robot) == 16);
    CHECK(s.world().get<physics::RagdollComponent>(robot)->mode == physics::RagdollComponent::Mode::Passive);
    CHECK(h.ed->remove_ragdoll(robot) == 17);
    CHECK(h.ed->ragdoll_bone_count(robot) == 0);
    h.ed->undo();
    CHECK(h.ed->ragdoll_bone_count(robot) == 16);

    // Ragdoll Lab: Orion and Nova on a floor, one undo step; Orion falls when played.
    const std::size_t entities = s.size();
    const scene::Entity lab = h.ed->add_ragdoll_lab();
    REQUIRE_FALSE(lab.is_null());
    const scene::Entity v01 = s.find_by_name("Orion");
    const scene::Entity v02 = s.find_by_name("Nova");
    REQUIRE_FALSE(v01.is_null());
    REQUIRE_FALSE(v02.is_null());
    CHECK(s.parent(v01) == lab);
    CHECK(h.ed->ragdoll_bone_count(v01) == 16);
    CHECK(h.ed->ragdoll_bone_count(v02) == 16);
    CHECK(s.world().get<physics::RagdollComponent>(v01)->mode == physics::RagdollComponent::Mode::Passive);
    CHECK(s.world().get<physics::RagdollComponent>(v02)->mode == physics::RagdollComponent::Mode::Powered);
    REQUIRE_FALSE(s.find_by_name("Lab Floor").is_null());
    h.ed->undo();
    CHECK(s.size() == entities);
    h.ed->redo();
    CHECK(s.size() > entities);

    // In play the limp Orion's head drops towards the floor.
    const scene::Entity lab_v01 = h.ed->edit_scene().find_by_name("Orion");
    REQUIRE_FALSE(lab_v01.is_null());
    h.ed->play();
    h.frames(2);
    scene::Scene& play = h.ed->active_scene();
    scene::Entity head;
    for (const scene::Entity c : [&] {
             std::vector<scene::Entity> all, stack{play.find_by_name("Orion")};
             while (!stack.empty()) {
                 const scene::Entity e = stack.back();
                 stack.pop_back();
                 all.push_back(e);
                 for (const scene::Entity k : play.children(e)) stack.push_back(k);
             }
             return all;
         }())
        if (const auto* n = play.world().get<scene::Name>(c); n && n->value == "Head") head = c;
    REQUIRE_FALSE(head.is_null());
    const float y0 = play.world_matrix(head).transform_point({}).y;
    h.frames(90);
    const float y1 = play.world_matrix(head).transform_point({}).y;
    CHECK(y1 < y0 - 0.3f);
    h.ed->stop();
}

TEST_CASE("studio safe mode: unfinished openings switch it on, it skips project content and restarts normally") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Studio h;
    REQUIRE(h.init({}, false, [](editor::EditorOptions& o) { o.startup_frames = 4; }));
    ImGui::SetCurrentContext(h.ui->imgui());
    const sfs::path root = h.dir / "game";
    REQUIRE(h.ed->create_project(root, "Safe", "3d").ok());
    const std::string startup = h.ed->scene_path();
    CHECK_FALSE(startup.empty());
    CHECK_FALSE(h.ed->safe_mode());
    // The opening is recorded until startup_frames frames have run.
    CHECK(h.ed->startup_pending());
    CHECK(editor::unfinished_startups(root) == 1);
    h.frames(4);
    CHECK_FALSE(h.ed->startup_pending());
    CHECK_FALSE(sfs::exists(editor::startup_marker_path(root)));
    CHECK(editor::unfinished_startups(root) == 0);

    // Two openings that never finished (crashed, hung or killed) and an autosave they left behind.
    h.ed->close_project();
    REQUIRE(fs::write_text_atomic(editor::startup_marker_path(root), "{\"unfinished\": 2}").ok());
    const sfs::path autosave = root / ".victus" / "autosave" / "untitled.vscene";
    sfs::create_directories(autosave.parent_path());
    REQUIRE(fs::write_text_atomic(autosave, R"({"format":"victus.scene","version":1,"entities":[]})").ok());
    REQUIRE(h.ed->open_project(root).ok());
    CHECK(h.ed->safe_mode());
    CHECK(h.ed->safe_mode_reason().find("last 2 attempts") != std::string::npos);
    CHECK(editor::unfinished_startups(root) == 3); // this opening is recorded too
    CHECK(h.ed->scene_path().empty());            // the startup scene was not opened
    CHECK_FALSE(h.ed->recovery_available());      // autosaves are not offered...
    h.ed->play();
    CHECK(h.ed->play_state() == editor::PlayState::Editing); // play mode is off
    h.frames(2);
    CHECK(sfs::exists(autosave)); // ...and not deleted or overwritten
    CHECK(h.ed->safe_mode_card_visible());
    CHECK(ImGui::FindWindowByName("##safe_mode") != nullptr);
    CHECK(editor::safe_mode_skipped().size() == 6);
    // Opening a scene by hand works in safe mode.
    REQUIRE(h.ed->open_scene(startup).ok());
    CHECK_FALSE(h.ed->recovery_available());
    h.frames(2); // startup finished: the marker is gone, so the next start is normal
    CHECK(editor::unfinished_startups(root) == 0);

    // Restart Normally: everything is back on, including the startup scene.
    REQUIRE(h.ed->reopen_normally().ok());
    CHECK_FALSE(h.ed->safe_mode());
    CHECK(h.ed->scene_path() == startup);
    CHECK_FALSE(h.ed->safe_mode_card_visible());
    h.ed->play();
    CHECK(h.ed->play_state() == editor::PlayState::Playing);
    h.ed->stop();

    // Asking for it (launcher "Open in Safe Mode"); closing ends it.
    REQUIRE(h.ed->open_project_safe(root).ok());
    CHECK(h.ed->safe_mode());
    CHECK(h.ed->safe_mode_reason().find("you asked") != std::string::npos);
    h.ed->close_project();
    CHECK_FALSE(h.ed->safe_mode());
    CHECK_FALSE(sfs::exists(editor::startup_marker_path(root))); // closing is a finished start
    REQUIRE(h.ed->open_project(root).ok());
    CHECK_FALSE(h.ed->safe_mode());

    // A torn marker still counts as one unfinished opening; a missing project is not touched.
    h.ed->close_project();
    REQUIRE(fs::write_text_atomic(editor::startup_marker_path(root), "{").ok());
    CHECK(editor::unfinished_startups(root) == 1);
    CHECK(editor::unfinished_startups(h.dir / "missing") == 0);
    CHECK_FALSE(h.ed->open_project_safe(h.dir / "missing").ok());
    REQUIRE(h.ed->open_project(root).ok());
    CHECK_FALSE(h.ed->safe_mode()); // one unfinished opening is below the default of 2
}

TEST_CASE("studio safe mode: --safe-mode covers every project; safe_mode_after 0 never switches it on") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    {
        Studio h;
        REQUIRE(h.init({}, false, [](editor::EditorOptions& o) { o.safe_mode = true; }));
        const sfs::path root = h.dir / "game";
        REQUIRE(h.ed->create_project(root, "Forced", "2d").ok());
        CHECK(h.ed->safe_mode());
        CHECK(h.ed->safe_mode_reason().find("you asked") != std::string::npos);
        REQUIRE(h.ed->reopen_normally().ok());
        CHECK_FALSE(h.ed->safe_mode());
        h.ed.reset(); // a normal quit removes the record
        CHECK_FALSE(sfs::exists(editor::startup_marker_path(root)));
    }
    Studio h;
    REQUIRE(h.init({}, false, [](editor::EditorOptions& o) { o.safe_mode_after = 0; }));
    const sfs::path root = h.dir / "game";
    REQUIRE(project::Project::create(root, "Never", "3d").ok());
    REQUIRE(fs::write_text_atomic(editor::startup_marker_path(root), "{\"unfinished\": 9}").ok());
    REQUIRE(h.ed->open_project(root).ok());
    CHECK_FALSE(h.ed->safe_mode());
}

TEST_CASE("studio: the Script Debugger stops play mode at a breakpoint, inspects, steps and stops") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Studio h;
    REQUIRE(h.init());
    const sfs::path root = h.dir / "Dbg";
    REQUIRE(h.ed->create_project(root, "Dbg", "3d").ok());
    h.frames(2);
    const std::string script = "scripts/crate.luau";
    // Line 14 is `self.speed = vector.magnitude(self.entity.velocity)` in Crate:update.
    REQUIRE(h.ed->toggle_breakpoint(script, 14));
    REQUIRE(h.ed->breakpoints().count(script) == 1);
    auto saved = fs::read_text(root / ".victus" / "breakpoints.json");
    REQUIRE(saved);
    CHECK(saved->find("scripts/crate.luau") != std::string::npos);
    CHECK(h.ed->toggle_breakpoint(script, 99)); // a second line adds a breakpoint
    CHECK(h.ed->breakpoints().at(script).size() == 2);
    CHECK_FALSE(h.ed->toggle_breakpoint(script, 99)); // toggling again removes it
    CHECK(h.ed->breakpoints().at(script).size() == 1);

    struct Seen {
        script::StopReason reason;
        int line;
        std::string source, min_speed;
        bool has_self_local;
    };
    std::vector<Seen> seen;
    std::vector<script::StepAction> plan{script::StepAction::StepOver, script::StepAction::Continue};
    bool stop_game_on_next = false;
    int nested = 0;
    h.ed->set_frame_pump({.suspend = [] { ImGui::EndFrame(); },
                          .frame =
                              [&] {
                                  ++nested;
                                  h.frames(1); // a whole Studio frame while the script is stopped
                                  script::DebugStop* s = h.ed->debug_stop();
                                  REQUIRE(s != nullptr);
                                  REQUIRE_FALSE(s->frames().empty());
                                  Seen v{s->reason(), s->frames().front().line, s->frames().front().source, {}, false};
                                  if (auto r = s->evaluate("self.min_speed", 0)) v.min_speed = r->value;
                                  for (const auto& l : s->locals(0))
                                      v.has_self_local = v.has_self_local || l.name == "self";
                                  seen.push_back(v);
                                  if (stop_game_on_next) {
                                      h.ed->stop(); // deferred: the script finishes, then play stops
                                  } else {
                                      h.ed->debug_resume(plan.empty() ? script::StepAction::Continue : plan.front());
                                      if (!plan.empty()) plan.erase(plan.begin());
                                  }
                                  return true;
                              },
                          .resume = [&] { h.ui->new_frame(1.0f / 60.0f, {static_cast<float>(Studio::W), static_cast<float>(Studio::H)}); }});
    h.ed->play();
    REQUIRE(h.ed->play_state() == editor::PlayState::Playing);
    h.frames(1);
    REQUIRE(seen.size() >= 2);
    CHECK(seen[0].reason == script::StopReason::Breakpoint);
    CHECK(seen[0].line == 14);
    CHECK(seen[0].source == script);
    CHECK(seen[0].min_speed == "1.5");
    CHECK(seen[0].has_self_local);
    CHECK(seen[1].reason == script::StopReason::Step);
    CHECK(seen[1].line > 14);
    CHECK(h.ed->debug_stops() == seen.size());
    CHECK_FALSE(h.ed->debug_stopped());
    CHECK(nested == static_cast<int>(seen.size()));
    // The source view follows the stop and the breakpoint verifies in the running game.
    const auto states = h.ed->breakpoint_states(script);
    REQUIRE(states.size() == 1);
    CHECK(states[0].verified);
    CHECK(states[0].line == 14);

    // Stop while stopped: the call finishes without stopping again and play mode ends.
    stop_game_on_next = true;
    const std::size_t before = seen.size();
    h.frames(1);
    CHECK(seen.size() == before + 1);
    CHECK(h.ed->play_state() == editor::PlayState::Editing);
    CHECK_FALSE(h.ed->debug_stopped());

    // Breakpoints and the break-on-error choice persist with the project.
    h.ed->set_break_on_error(false);
    h.ed->set_frame_pump({});
    REQUIRE(h.ed->open_project(root).ok());
    CHECK(h.ed->breakpoints().count(script) == 1);
    CHECK_FALSE(h.ed->break_on_error());
    // Without a frame pump (headless) a breakpoint is counted but never blocks.
    h.ed->play();
    h.frames(2);
    CHECK(h.ed->debug_stops() > 0);
    CHECK(h.ed->play_state() == editor::PlayState::Playing);
    h.ed->stop();
}

TEST_CASE("studio workspace: open panels and the layout preset are remembered between sessions") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Studio h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Remember", "Remember", "2d").ok());
    h.frames(2);
    h.ed->apply_layout(editor::Layout::Animation);
    h.frames(1);
    h.ed->set_panel_open(editor::Panel::Console, false);
    h.ed->set_panel_open(editor::Panel::Profiler, true);
    h.frames(2);
    const auto prefs = h.ed->preferences_file();
    {
        auto text = fs::read_text(prefs);
        REQUIRE(text);
        auto doc = json::parse(text.value());
        REQUIRE(doc);
        CHECK(doc.value()["workspace"]["layout"].as_string_or("") == "Animation");
    }

    // A second session reading the same preferences restores both.
    editor::EditorOptions o;
    o.recent_file = h.dir / "recent.json";
    o.default_project_dir = h.dir;
    o.diagnostics_dir = h.dir / "diagnostics";
    h.ed.reset();
    auto e = editor::Editor::create(*gpu().ctx, *h.ui, nullptr, nullptr, o);
    REQUIRE(e);
    h.ed = std::move(e).value();
    h.ed->set_viewport_size_override(320, 180);
    CHECK(h.ed->layout() == editor::Layout::Animation);
    CHECK_FALSE(h.ed->panel_open(editor::Panel::Console));
    CHECK(h.ed->panel_open(editor::Panel::Profiler));
    CHECK(h.ed->panel_open(editor::Panel::Sequencer));
    CHECK(h.ed->panel_open(editor::Panel::Viewport));
    REQUIRE(h.ed->open_project(h.dir / "Remember").ok());
    h.frames(2);
    CHECK_FALSE(h.ed->panel_open(editor::Panel::Console));
    CHECK(h.ed->panel_open(editor::Panel::Profiler));
}

TEST_CASE("studio workspace: named layouts keep panels and docking and survive restarts") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Studio h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Named", "Named", "2d").ok());
    h.frames(2);
    h.ed->apply_layout(editor::Layout::Debugging);
    h.frames(3);
    CHECK_FALSE(h.ed->save_custom_layout("   ")); // needs a name
    REQUIRE(h.ed->save_custom_layout(" Bug Hunt "));
    REQUIRE(h.ed->custom_layouts().size() == 1);
    CHECK(h.ed->custom_layouts()[0].name == "Bug Hunt");
    CHECK(h.ed->custom_layouts()[0].ini.find("[Docking]") != std::string::npos);
    h.ed->apply_layout(editor::Layout::Default);
    h.frames(2);
    CHECK_FALSE(h.ed->panel_open(editor::Panel::Profiler));
    REQUIRE(h.ed->apply_custom_layout("Bug Hunt"));
    h.frames(3); // docking is restored at the start of the next frame
    CHECK(h.ed->panel_open(editor::Panel::Profiler));
    CHECK(h.ed->panel_open(editor::Panel::Network));
    CHECK_FALSE(h.ed->apply_custom_layout("Missing"));
    REQUIRE(h.ed->save_custom_layout("Bug Hunt")); // same name replaces
    CHECK(h.ed->custom_layouts().size() == 1);

    editor::EditorOptions o;
    o.recent_file = h.dir / "recent.json";
    o.default_project_dir = h.dir;
    o.diagnostics_dir = h.dir / "diagnostics";
    h.ed.reset();
    auto e = editor::Editor::create(*gpu().ctx, *h.ui, nullptr, nullptr, o);
    REQUIRE(e);
    h.ed = std::move(e).value();
    h.ed->set_viewport_size_override(320, 180);
    REQUIRE(h.ed->custom_layouts().size() == 1);
    CHECK(((h.ed->custom_layouts()[0].panels >> static_cast<int>(editor::Panel::Profiler)) & 1u) == 1u);
    CHECK(h.ed->delete_custom_layout("Bug Hunt"));
    CHECK(h.ed->custom_layouts().empty());
}

TEST_CASE("studio behaviour guides: patrol and platform routes, chase ranges and jump arcs show for the selection") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Studio h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Guides", "Guides", "2d").ok());
    h.frames(2);
    auto& w = h.ed->edit_scene().world();
    const scene::Entity lift = h.ed->create_entity(editor::Primitive::Sprite);
    w.get<scene::Transform>(lift)->position = {1, 2, 0};
    w.add<script::MovingPlatformComponent>(lift).offset = {4, 0, 0};
    const auto lines = h.ed->behaviour_guides(lift, true);
    REQUIRE(lines.size() >= 3);
    CHECK(lines[0].a.x == doctest::Approx(1));
    CHECK(lines[0].b.x == doctest::Approx(5)); // the route
    CHECK(lines[0].b.y == doctest::Approx(2));
    const scene::Entity chaser = h.ed->create_entity(editor::Primitive::Sprite);
    w.add<script::ChaseComponent>(chaser);
    CHECK(h.ed->behaviour_guides(chaser, true).size() == 96); // detect and lose circles
    const scene::Entity pad = h.ed->create_entity(editor::Primitive::Sprite);
    w.add<script::JumpPadComponent>(pad).strength = 9.81f;
    const auto arc = h.ed->behaviour_guides(pad, true);
    REQUIRE(arc.size() == 34); // 32 arc segments and the peak cross
    CHECK(arc[15].b.y - arc[0].a.y == doctest::Approx(4.905f).epsilon(0.01)); // v^2 / 2g with g = 9.81
    CHECK(arc[31].b.y == doctest::Approx(arc[0].a.y).epsilon(0.01));
    const scene::Entity plain = h.ed->create_entity(editor::Primitive::Sprite);
    CHECK(h.ed->behaviour_guides(plain, true).empty());
    h.ed->select(lift);
    h.frames(2); // drawn without errors
}

TEST_CASE("studio inspector: inventories, timers and attributes are shown and items can be added") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Studio h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Inv", "Inv", "2d").ok());
    h.frames(2);
    auto& w = h.ed->edit_scene().world();
    const scene::Entity e = h.ed->create_entity(editor::Primitive::Sprite);
    bool listed = false;
    for (const auto& c : h.ed->component_choices(e))
        if (c.id == "Inventory") listed = true;
    CHECK(listed);
    REQUIRE(h.ed->add_component(e, "Inventory"));
    CHECK_FALSE(h.ed->add_component(e, "Inventory")); // only once
    REQUIRE(w.get<gameplay::InventoryComponent>(e));
    CHECK(gameplay::add_item(h.ed->edit_scene(), e, "Key", 2) == 2);
    gameplay::start_timer(h.ed->edit_scene(), e, "tick", 3, true);
    h.ed->select(e);
    h.frames(3); // all three sections draw
    CHECK(w.get<gameplay::TimersComponent>(e));
    CHECK(gameplay::item_count(h.ed->edit_scene(), e, "Key") == 2);
}
