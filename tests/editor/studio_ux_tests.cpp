// SPDX-License-Identifier: Apache-2.0
// VICTUS Studio UX: design tokens (WCAG contrast), typography, and the workspace chrome (toolbar,
// status bar, console counts) driven headlessly on the GPU.
#include "../render/gpu_fixture.hpp"
#include "victus/core/log.hpp"
#include "victus/editor/editor.hpp"
#include "victus/ui/style.hpp"
#include "victus/ui/ui.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstring>
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

    bool init() {
        auto& ctx = *gpu().ctx;
        dir = sfs::temp_directory_path() / ("victus_ux_" + Uuid::generate().to_string().substr(0, 8));
        sfs::create_directories(dir);
        auto u = ui::UiContext::create(ctx, rhi::Format::RGBA8_UNORM, nullptr, nullptr);
        if (!u) return false;
        ui = std::move(u).value();
        editor::EditorOptions o;
        o.recent_file = dir / "recent.json";
        o.default_project_dir = dir;
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
