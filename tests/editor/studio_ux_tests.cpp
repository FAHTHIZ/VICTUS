// SPDX-License-Identifier: Apache-2.0
// VICTUS Studio UX: design tokens (WCAG contrast), typography, and the workspace chrome (toolbar,
// status bar, console counts) driven headlessly on the GPU.
#include "../render/gpu_fixture.hpp"
#include "victus/core/log.hpp"
#include "victus/editor/editor.hpp"
#include "victus/core/fs.hpp"
#include "victus/project/project.hpp"
#include "victus/ui/style.hpp"
#include "victus/ui/ui.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstring>
#include <fstream>
#include <filesystem>

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

    bool init(const sfs::path& samples_dir = {}) {
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
