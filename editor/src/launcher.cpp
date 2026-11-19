// SPDX-License-Identifier: Apache-2.0
// Editor launcher: the first screen. Four pages (New Project, Recent, Samples, Learn) in a
// sidebar layout, written for someone opening a game engine for the first time: friendly
// template cards, live validation of the project name and folder, samples copied before they
// open, and a short "first ten minutes" guide.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include "victus/core/build_info.hpp"
#include "victus/core/json.hpp"
#include "victus/platform/platform.hpp"
#include "victus/project/characters.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <cmath>
#include <format>
#include <system_error>

namespace victus::editor {

namespace sfs = std::filesystem;
namespace tk = ui::tokens;

namespace {

struct TemplateLook {
    kit::Icon icon;
    const char* tagline;
    const char* tags;
};

TemplateLook template_look(std::string_view id) {
    if (id == "3d")
        return {kit::Icon::Cube, "Walk a robot through a lit 3D level with physics, a companion and a follow camera.",
                "3D  -  Character  -  Physics"};
    if (id == "2d")
        return {kit::Icon::Sprite, "Run and jump through a side-scrolling platformer with coins, music and sound.",
                "2D  -  Platformer  -  Physics"};
    if (id == "nocode")
        return {kit::Icon::Sprite, "Build a whole platformer level from ready-made behaviours, with no code at all.",
                "2D  -  No code  -  Beginner"};
    if (id == "nocode3d")
        return {kit::Icon::Cube, "Jump across lava, collect coins and beat a robot in 3D, all from behaviours and no code.",
                "3D  -  No code  -  Beginner"};
    if (id == "simulation")
        return {kit::Icon::Particles,
                "Hundreds of animated objects and an orbiting camera: a base for visual or data-heavy work.",
                "3D  -  Systems  -  Camera"};
    if (id == "multiplayer")
        return {kit::Icon::Network,
                "Collect coins in an arena you can host and join over the network, or play offline.",
                "3D  -  Online  -  Character"};
    return {kit::Icon::File, nullptr, ""};
}

bool reserved_windows_name(std::string_view name) {
    std::string stem(name.substr(0, name.find('.')));
    for (char& c : stem)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    static constexpr const char* kReserved[] = {"CON", "PRN", "AUX", "NUL"};
    for (const char* r : kReserved)
        if (stem == r) return true;
    return stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '1' && stem[3] <= '9';
}

std::string readme_summary(const fs::Path& readme) {
    auto text = fs::read_text(readme, 1u << 20);
    if (!text) return {};
    std::string out;
    std::size_t pos = 0;
    const std::string& t = *text;
    while (pos < t.size()) {
        std::size_t nl = t.find('\n', pos);
        if (nl == std::string::npos) nl = t.size();
        std::string_view line(t.data() + pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.remove_suffix(1);
        if (line.starts_with('#')) continue;
        if (line.empty()) {
            if (!out.empty()) break;
            continue;
        }
        if (!out.empty()) out += ' ';
        out += line;
    }
    if (out.size() > 280) {
        out.resize(277);
        out += "...";
    }
    return out;
}

// A clickable card: background, icon tile, title, wrapped body. Returns true when clicked.
bool card(const char* id, kit::Icon icon, const char* title, const char* body, const char* footer, bool selected,
          ImVec2 size, ImVec4 icon_tint = tk::kAccent) {
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##card", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + size.x, p.y + size.y);
    dl->AddRectFilled(p, q,
                      ImGui::GetColorU32(selected  ? tk::kAccentSubtle
                                         : hovered ? tk::kSurfaceHover
                                                   : tk::kSurfaceRaised),
                      tk::kRadiusLarge);
    dl->AddRect(p, q, ImGui::GetColorU32(selected ? tk::kAccent : tk::kBorder), tk::kRadiusLarge,
                selected ? 2.0f : 1.0f);
    const float pad = tk::kSpace5;
    const float tile = 40.0f;
    const ImVec2 t0(p.x + pad, p.y + pad);
    dl->AddRectFilled(t0, ImVec2(t0.x + tile, t0.y + tile), ImGui::GetColorU32(ui::with_alpha(icon_tint, 0.14f)),
                      tk::kRadius);
    kit::draw_icon(dl, icon, ImVec2(t0.x + 9, t0.y + 9), tile - 18, ImGui::GetColorU32(icon_tint));
    const float x = t0.x + tile + tk::kSpace4;
    const float wrap = q.x - pad - x;
    ImFont* bold = kit::bold_font() ? kit::bold_font() : ImGui::GetFont();
    const float fs = ImGui::GetStyle().FontSizeBase;
    const ImVec4 clip(p.x, p.y, q.x - pad * 0.5f, q.y - tk::kSpace2);
    dl->AddText(bold, fs * tk::kTypeHeading, ImVec2(x, t0.y), ImGui::GetColorU32(tk::kText), title, nullptr, 0.0f,
                &clip);
    const float y = t0.y + fs * tk::kTypeHeading + tk::kSpace2;
    if (body && *body)
        dl->AddText(ImGui::GetFont(), fs, ImVec2(x, y), ImGui::GetColorU32(tk::kTextMuted), body, nullptr, wrap, &clip);
    if (footer && *footer)
        dl->AddText(ImGui::GetFont(), fs * tk::kTypeCaption, ImVec2(x, q.y - pad - fs * tk::kTypeCaption),
                    ImGui::GetColorU32(selected ? tk::kAccent : tk::kTextDisabled), footer, nullptr, 0.0f, &clip);
    ImGui::PopID();
    return clicked;
}

bool nav_item(const char* label, kit::Icon icon, bool active, const char* badge = nullptr) {
    const float h = ImGui::GetFrameHeight() + tk::kSpace3;
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::PushID(label);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##nav", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active || hovered)
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                          ImGui::GetColorU32(active ? tk::kAccentSubtle : tk::kSurfaceHover), tk::kRadius);
    if (active) dl->AddRectFilled(p, ImVec2(p.x + 3, p.y + h), ImGui::GetColorU32(tk::kAccent), 1.5f);
    const float ih = ImGui::GetTextLineHeight();
    const float y = p.y + (h - ih) * 0.5f;
    kit::draw_icon(dl, icon, ImVec2(p.x + tk::kSpace4, y), ih,
                   ImGui::GetColorU32(active ? tk::kAccent : tk::kTextMuted));
    dl->AddText(ImVec2(p.x + tk::kSpace4 + ih + tk::kSpace3, y),
                ImGui::GetColorU32(active ? tk::kText : tk::kTextMuted), label);
    if (badge) {
        const float bw = ImGui::CalcTextSize(badge).x;
        dl->AddText(ImVec2(p.x + w - bw - tk::kSpace4, y), ImGui::GetColorU32(tk::kTextDisabled), badge);
    }
    ImGui::PopID();
    return clicked;
}

void step(int n, const char* title, const char* body) {
    const float r = ImGui::GetTextLineHeight() * 0.75f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(ImVec2(p.x + r, p.y + r), r, ImGui::GetColorU32(tk::kAccentSubtle));
    const std::string num = std::to_string(n);
    const ImVec2 ns = ImGui::CalcTextSize(num.c_str());
    dl->AddText(ImVec2(p.x + r - ns.x * 0.5f, p.y + r - ns.y * 0.5f), ImGui::GetColorU32(tk::kAccent), num.c_str());
    ImGui::SetCursorScreenPos(ImVec2(p.x + r * 2 + tk::kSpace4, p.y));
    ImGui::BeginGroup();
    kit::heading(title, tk::kTypeBody);
    kit::subtitle(body);
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, tk::kSpace3));
}

} // namespace

// ---- API ----------------------------------------------------------------------------------------

std::string Editor::new_project_problem(const fs::Path& location, std::string_view name) {
    std::string_view trimmed = name;
    if (trimmed.empty() || trimmed.find_first_not_of(' ') == std::string_view::npos) return "Give your project a name.";
    if (trimmed.front() == ' ' || trimmed.back() == ' ' || trimmed.back() == '.')
        return "The name cannot start or end with a space, or end with a dot.";
    for (const char c : trimmed) {
        if (static_cast<unsigned char>(c) < 32 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos)
            return std::format("The name cannot contain '{}'. Letters, numbers, spaces, - and _ work everywhere.",
                               static_cast<unsigned char>(c) < 32 ? std::string("control characters")
                                                                  : std::string(1, c));
    }
    if (trimmed.size() > 100) return "The name is too long (100 characters at most).";
    if (reserved_windows_name(trimmed))
        return std::format("'{}' is a reserved name on Windows. Pick another name.", trimmed);
    if (location.empty()) return "Choose where to save the project.";
    std::error_code ec;
    if (sfs::exists(location, ec) && !sfs::is_directory(location, ec)) return "The location is a file, not a folder.";
    const fs::Path target = location / fs::Path(std::string(trimmed));
    if (sfs::exists(target, ec)) {
        if (project::Project::is_project_dir(target))
            return "A project with this name already exists here. Open it from Recent or Open, or pick another name.";
        if (!sfs::is_directory(target, ec)) return "A file with this name already exists here. Pick another name.";
        if (!sfs::is_empty(target, ec))
            return "A folder with this name already exists here and is not empty. Pick another name.";
    }
    return {};
}

const std::vector<SampleInfo>& Editor::samples() const {
    if (samples_) return *samples_;
    samples_.emplace();
    std::error_code ec;
    if (options_.samples_dir.empty() || !sfs::is_directory(options_.samples_dir, ec)) return *samples_;
    for (const auto& entry : sfs::directory_iterator(options_.samples_dir, ec)) {
        if (!entry.is_directory(ec) || !project::Project::is_project_dir(entry.path())) continue;
        SampleInfo info;
        info.dir = entry.path();
        info.name = entry.path().filename().string();
        if (auto text = fs::read_text(entry.path() / "victus.project", 1u << 20))
            if (auto v = json::parse(*text))
                if (const json::Value* n = v->find("name")) info.name = n->as_string_or(info.name);
        info.summary = readme_summary(entry.path() / "README.md");
        samples_->push_back(std::move(info));
    }
    std::sort(samples_->begin(), samples_->end(),
              [](const SampleInfo& a, const SampleInfo& b) { return a.name < b.name; });
    return *samples_;
}

Status Editor::open_sample_copy(const fs::Path& sample, const fs::Path& location) {
    if (!project::Project::is_project_dir(sample))
        return make_error(ErrorCode::NotFound, "not a sample project: " + sample.string());
    std::error_code ec;
    sfs::create_directories(location, ec);
    if (ec) return make_error(ErrorCode::IoError, std::format("cannot create {}: {}", location.string(), ec.message()));
    const std::string base = sample.filename().string();
    fs::Path target = location / base;
    for (int i = 2; sfs::exists(target, ec); ++i)
        target = location / std::format("{} {}", base, i);
    sfs::create_directories(target, ec);
    for (const auto& entry : sfs::directory_iterator(sample, ec)) {
        const std::string name = entry.path().filename().string();
        if (name == ".victus" || name == "build") continue; // caches and local build output
        sfs::copy(entry.path(), target / name, sfs::copy_options::recursive, ec);
        if (ec) return make_error(ErrorCode::IoError, std::format("copying {} failed: {}", name, ec.message()));
    }
    return open_project(target);
}

LauncherPage Editor::launcher_page() const {
    if (launcher_page_set_) return launcher_page_;
    return recent_ && !recent_->entries().empty() ? LauncherPage::Recent : LauncherPage::New;
}

// ---- UI -----------------------------------------------------------------------------------------

void Editor::draw_launcher() {
    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Equal)) step_ui_scale(1);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Minus)) step_ui_scale(-1);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_0)) step_ui_scale(0);
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, tk::kSurfacePanel);
    ImGui::Begin("##launcher", nullptr, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    // Sidebar.
    const float side_w = 250.0f * ui_scale_;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, tk::kSurfaceBase);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(tk::kSpace5, tk::kSpace6));
    ImGui::BeginChild("##side", ImVec2(side_w, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushFont(kit::bold_font(), ImGui::GetStyle().FontSizeBase * tk::kTypeTitle);
    ImGui::TextColored(tk::kAccent, "VICTUS");
    ImGui::PopFont();
    ImGui::TextColored(tk::kTextMuted, "Studio %s", build::version());
    ImGui::Dummy(ImVec2(0, tk::kSpace6));
    const LauncherPage page = launcher_page();
    const std::string recent_count =
        recent_ && !recent_->entries().empty() ? std::to_string(recent_->entries().size()) : "";
    const std::string sample_count = samples().empty() ? "" : std::to_string(samples().size());
    if (nav_item("New Project", kit::Icon::Plus, page == LauncherPage::New)) set_launcher_page(LauncherPage::New);
    if (nav_item("Recent", kit::Icon::FolderOpen, page == LauncherPage::Recent,
                 recent_count.empty() ? nullptr : recent_count.c_str()))
        set_launcher_page(LauncherPage::Recent);
    if (nav_item("Samples", kit::Icon::Rocket, page == LauncherPage::Samples,
                 sample_count.empty() ? nullptr : sample_count.c_str()))
        set_launcher_page(LauncherPage::Samples);
    if (nav_item("Learn", kit::Icon::Help, page == LauncherPage::Learn)) set_launcher_page(LauncherPage::Learn);
    const float foot = ImGui::GetTextLineHeightWithSpacing() * 3.0f + ImGui::GetFrameHeightWithSpacing() * 2.0f + tk::kSpace3 * 2.0f;
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - foot - tk::kSpace6));
    // Interface size is on the start screen so it can be fixed before anything else is read.
    ImGui::TextColored(tk::kTextMuted, "Interface size");
    ImGui::SetNextItemWidth(-1);
    {
        const char* current = "Custom";
        for (const UiScaleChoice& c : kUiScales)
            if (std::abs(c.scale - ui_scale_) < 1e-3f) current = c.name;
        if (ImGui::BeginCombo("##uiscale", current)) {
            for (const UiScaleChoice& c : kUiScales)
                if (ImGui::Selectable(std::format("{}  ({:.0f} %)", c.name, static_cast<double>(c.scale) * 100.0).c_str(),
                                      std::abs(c.scale - ui_scale_) < 1e-3f))
                    set_ui_scale(c.scale);
            ImGui::EndCombo();
        }
        kit::tooltip("Bigger text and buttons (Ctrl+= / Ctrl+-)");
    }
    ImGui::Dummy(ImVec2(0, tk::kSpace3));
    if (nav_item("Report a problem", kit::Icon::Help, false)) open_diagnostics_dialog();
    ImGui::Dummy(ImVec2(0, tk::kSpace3));
    ImGui::PushStyleColor(ImGuiCol_Text, tk::kTextDisabled);
    ImGui::TextUnformatted("Free and offline.");
    ImGui::TextUnformatted("No account needed.");
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // Page.
    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(tk::kSpace7 + tk::kSpace3, tk::kSpace6));
    ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    // Keep line lengths readable on wide screens.
    const float max_w = 980.0f * ui_scale_;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > max_w) ImGui::Indent((avail - max_w) * 0.5f);
    ImGui::PushItemWidth(std::min(avail, max_w) * 0.5f);
    switch (page) {
    case LauncherPage::New:
        draw_launcher_new();
        break;
    case LauncherPage::Recent:
        draw_launcher_recent();
        break;
    case LauncherPage::Samples:
        draw_launcher_samples();
        break;
    case LauncherPage::Learn:
        draw_launcher_learn();
        break;
    }
    ImGui::PopItemWidth();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::End();
}

void Editor::draw_launcher_new() {
    kit::heading("Create a new project", tk::kTypeTitle);
    kit::subtitle("Pick a starting point. Each one is a small game that runs the moment it opens, and you can change "
                  "everything.");
    ImGui::Dummy(ImVec2(0, tk::kSpace4));

    const float width = std::min(ImGui::GetContentRegionAvail().x, 980.0f);
    const float gap = tk::kSpace4;
    const int columns = width > 620.0f ? 2 : 1;
    const float cw = (width - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    const ImVec2 size(cw, 118.0f);
    int col = 0;
    for (const auto& t : project::templates()) {
        const TemplateLook look = template_look(t.id);
        if (col > 0) ImGui::SameLine(0, gap);
        const bool sel = new_template_ == t.id;
        if (card(t.id.c_str(), look.icon, t.display_name.c_str(), look.tagline ? look.tagline : t.description.c_str(),
                 look.tags, sel, size))
            new_template_ = t.id;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) new_template_ = t.id;
        if (look.tagline) kit::tooltip(t.description.c_str());
        col = (col + 1) % columns;
    }

    ImGui::Dummy(ImVec2(0, tk::kSpace5));
    ImGui::SeparatorText("Project");
    ImGui::Dummy(ImVec2(0, tk::kSpace2));
    const auto& tpls = project::templates();
    const auto tpl =
        std::find_if(tpls.begin(), tpls.end(), [&](const project::TemplateInfo& t) { return t.id == new_template_; });
    const bool has_character = tpl != tpls.end() && tpl->characters;
    const auto label = [](const char* text, const char* hint) {
        ImGui::TextUnformatted(text);
        if (hint) {
            ImGui::SameLine();
            ImGui::TextColored(tk::kTextDisabled, "%s", hint);
        }
    };
    label("Name", nullptr);
    ImGui::InputTextWithHint("##name", "My Game", &new_name_);
    ImGui::Dummy(ImVec2(0, tk::kSpace2));
    label("Location", "where the project folder is created");
    ImGui::InputText("##location", &new_location_);
    ImGui::SameLine();
    if (kit::icon_text_button("Browse...", kit::Icon::Folder, false, platform_ != nullptr)) {
        platform_->show_folder_dialog(window_, new_location_, [this](std::string path) {
            if (!path.empty()) new_location_ = std::move(path);
        });
    }
    if (has_character) {
        ImGui::Dummy(ImVec2(0, tk::kSpace2));
        label("Player character", "you can swap it later");
        const project::StarterCharacter* current = project::find_starter_character(new_character_);
        const std::string name = current ? std::string(current->display_name) : std::string("None (capsule body)");
        if (ImGui::BeginCombo("##character", name.c_str())) {
            for (const auto& c : project::starter_characters()) {
                const std::string n(c.display_name);
                if (ImGui::Selectable(n.c_str(), current == &c)) new_character_ = std::string(c.id);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%.*s", static_cast<int>(c.description.size()), c.description.data());
            }
            if (ImGui::Selectable("None (capsule body)", current == nullptr))
                new_character_ = std::string(project::kNoCharacter);
            ImGui::EndCombo();
        }
    }
    ImGui::Dummy(ImVec2(0, tk::kSpace3));
    const std::string problem = new_project_problem(new_location_, new_name_);
    const fs::Path target = fs::Path(new_location_) / new_name_;
    if (problem.empty()) {
        kit::icon(kit::Icon::Check, tk::kSuccess);
        ImGui::SameLine();
        ImGui::TextColored(tk::kTextMuted, "Will be created in %s", target.string().c_str());
    } else {
        kit::icon(kit::Icon::Info, tk::kWarning);
        ImGui::SameLine();
        ImGui::TextColored(tk::kWarning, "%s", problem.c_str());
    }
    ImGui::Dummy(ImVec2(0, tk::kSpace3));
    if (kit::icon_text_button("Create Project", kit::Icon::Plus, true, problem.empty(),
                              ImVec2(200, ImGui::GetFrameHeight() + tk::kSpace3))) {
        launcher_error_.clear();
        project::TemplateOptions options;
        options.character = has_character ? new_character_ : std::string(project::kDefaultCharacter);
        if (auto st = create_project(target, new_name_, new_template_, options); !st.ok())
            launcher_error_ = st.error().message;
    }
    if (!launcher_error_.empty()) {
        ImGui::Dummy(ImVec2(0, tk::kSpace2));
        ImGui::TextColored(tk::kDanger, "%s", launcher_error_.c_str());
    }
}

void Editor::draw_launcher_recent() {
    kit::heading("Recent projects", tk::kTypeTitle);
    kit::subtitle("Pick up where you left off, or open any project folder.");
    ImGui::Dummy(ImVec2(0, tk::kSpace4));
    const auto open = [this](const fs::Path& p) {
        launcher_error_.clear();
        if (auto st = open_project(p); !st.ok()) launcher_error_ = st.error().message;
    };
    ImGui::InputTextWithHint("##openpath", "Project folder path", &open_path_);
    ImGui::SameLine();
    if (kit::icon_text_button("Open", kit::Icon::FolderOpen, !open_path_.empty(), !open_path_.empty()))
        open(open_path_);
    ImGui::SameLine();
    if (kit::icon_text_button("Browse...", kit::Icon::Folder, false, platform_ != nullptr)) {
        platform_->show_folder_dialog(window_, open_path_.empty() ? new_location_ : open_path_,
                                      [this](std::string path) {
                                          if (path.empty()) return;
                                          open_path_ = path;
                                          if (auto st = open_project(path); !st.ok())
                                              launcher_error_ = st.error().message;
                                      });
    }
    if (!launcher_error_.empty()) ImGui::TextColored(tk::kDanger, "%s", launcher_error_.c_str());
    ImGui::Dummy(ImVec2(0, tk::kSpace4));

    if (!recent_ || recent_->entries().empty()) {
        if (kit::empty_state(kit::Icon::FolderOpen, "No recent projects yet",
                             "Projects you create or open appear here. Start with a template, or open a sample to see "
                             "what VICTUS can do.",
                             "Create a project"))
            set_launcher_page(LauncherPage::New);
        return;
    }
    fs::Path to_open, to_remove, to_locate;
    bool open_safe = false;
    const float width = std::min(ImGui::GetContentRegionAvail().x, 980.0f);
    const float h = ImGui::GetTextLineHeight() * 2.0f + tk::kSpace4 * 2.0f;
    for (const fs::Path& p : recent_->entries()) {
        const bool exists = project::Project::is_project_dir(p);
        ImGui::PushID(p.string().c_str());
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("##row", ImVec2(width, h));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Open", nullptr, false, exists)) to_open = p;
            if (ImGui::MenuItem("Open in Safe Mode", nullptr, false, exists)) {
                to_open = p;
                open_safe = true;
            }
            if (ImGui::MenuItem("Copy Path") && platform_) platform_->set_clipboard(p.string());
            if (ImGui::MenuItem("Remove from List")) to_remove = p;
            ImGui::EndPopup();
        }
        if (clicked && exists) to_open = p;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, ImVec2(a.x + width, a.y + h),
                          ImGui::GetColorU32(hovered && exists ? tk::kSurfaceHover : tk::kSurfaceRaised), tk::kRadius);
        const float ih = ImGui::GetTextLineHeight() * 1.4f;
        kit::draw_icon(dl, kit::Icon::Folder, ImVec2(a.x + tk::kSpace5, a.y + (h - ih) * 0.5f), ih,
                       ImGui::GetColorU32(exists ? tk::kAccent : tk::kTextDisabled));
        const float tx = a.x + tk::kSpace5 + ih + tk::kSpace4;
        ImFont* bold = kit::bold_font() ? kit::bold_font() : ImGui::GetFont();
        dl->AddText(bold, ImGui::GetStyle().FontSizeBase, ImVec2(tx, a.y + tk::kSpace4),
                    ImGui::GetColorU32(exists ? tk::kText : tk::kTextMuted), p.filename().string().c_str());
        dl->AddText(ImVec2(tx, a.y + tk::kSpace4 + ImGui::GetTextLineHeight()), ImGui::GetColorU32(tk::kTextMuted),
                    p.string().c_str());
        // Actions on the right.
        const ImVec2 after = ImGui::GetCursorScreenPos();
        const float bh = ImGui::GetFrameHeight();
        if (!exists) {
            ImGui::SetCursorScreenPos(ImVec2(a.x + width - 300, a.y + (h - bh) * 0.5f));
            kit::pill("Missing", ui::Tone::Warning);
            ImGui::SameLine();
            if (kit::icon_text_button("Locate...", kit::Icon::Search, false, platform_ != nullptr)) to_locate = p;
            ImGui::SameLine();
        } else {
            ImGui::SetCursorScreenPos(ImVec2(a.x + width - tk::kSpace4 - bh, a.y + (h - bh) * 0.5f));
        }
        if (kit::icon_button("remove", kit::Icon::Close, "Remove from the list (the folder is not deleted)"))
            to_remove = p;
        ImGui::SetCursorScreenPos(after);
        ImGui::Dummy(ImVec2(0, tk::kSpace1));
        ImGui::PopID();
    }
    if (!to_remove.empty()) {
        recent_->remove(to_remove);
        (void)recent_->save();
    }
    if (!to_locate.empty() && platform_) {
        platform_->show_folder_dialog(window_, to_locate.parent_path().string(), [this, to_locate](std::string path) {
            if (path.empty()) return;
            if (auto st = open_project(path); !st.ok()) {
                launcher_error_ = st.error().message;
                return;
            }
            recent_->remove(to_locate);
            (void)recent_->save();
        });
    }
    if (!to_open.empty() && open_safe) {
        launcher_error_.clear();
        if (auto st = open_project_safe(to_open); !st.ok()) launcher_error_ = st.error().message;
    } else if (!to_open.empty()) {
        open(to_open);
    }
}

void Editor::draw_launcher_samples() {
    kit::heading("Samples", tk::kTypeTitle);
    kit::subtitle("Complete small games built only with engine features. Opening one makes a copy in your projects "
                  "folder, so the original stays as it is.");
    ImGui::Dummy(ImVec2(0, tk::kSpace4));
    const auto& list = samples();
    if (list.empty()) {
        (void)kit::empty_state(kit::Icon::Rocket, "No samples found",
                               "Samples ship in the samples folder of the VICTUS source and release packages. Set "
                               "VICTUS_SAMPLES_DIR to point the launcher at them.");
        return;
    }
    const float width = std::min(ImGui::GetContentRegionAvail().x, 980.0f);
    const ImVec2 size(width, 112.0f);
    for (const SampleInfo& s : list) {
        const std::string footer = std::format("Opens a copy in {}", new_location_);
        if (card(s.dir.string().c_str(), kit::Icon::Rocket, s.name.c_str(), s.summary.c_str(), footer.c_str(), false,
                 size, tk::kInfo)) {
            launcher_error_.clear();
            if (auto st = open_sample_copy(s.dir, new_location_); !st.ok()) launcher_error_ = st.error().message;
        }
        ImGui::Dummy(ImVec2(0, tk::kSpace2));
    }
    if (!launcher_error_.empty()) ImGui::TextColored(tk::kDanger, "%s", launcher_error_.c_str());
}

void Editor::draw_launcher_learn() {
    kit::heading("Your first ten minutes", tk::kTypeTitle);
    kit::subtitle(
        "Everything below works offline. Hover any button in the editor for a short explanation and its shortcut.");
    ImGui::Dummy(ImVec2(0, tk::kSpace5));
    step(1, "Create a project from a template",
         "New Project > pick 3D Scene or 2D Game > Create Project. It opens with a playable scene.");
    step(2, "Press Play",
         "The green Play button (Ctrl+P) runs the game inside the editor. Changes made while playing are undone when "
         "you stop.");
    step(3, "Look around the scene",
         "Right-drag in the viewport to orbit, middle-drag to pan, scroll to zoom. Click an object to select it; F "
         "frames it.");
    step(4, "Change something",
         "Move, rotate or scale with W, E, R. Edit values in the Inspector on the right. Ctrl+Z undoes anything.");
    step(5, "Add your own things",
         "Use + Add in the Hierarchy for cubes, lights and cameras. Drop images, models and sounds into the project's "
         "assets folder.");
    step(6, "Save and share",
         "Ctrl+S saves the scene. File > Export Game builds a folder with your game that you can give to anyone.");
    ImGui::Dummy(ImVec2(0, tk::kSpace4));
    ImGui::SeparatorText("Handy shortcuts");
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_SizingFixedFit)) {
        static constexpr std::pair<const char*, const char*> kKeys[] = {
            {"Ctrl+Shift+P or F1", "Search every command"},
            {"Ctrl+P", "Play / Stop"},
            {"Ctrl+S", "Save scene"},
            {"Ctrl+Z / Ctrl+Y", "Undo / Redo"},
            {"W  E  R", "Move, rotate, scale tool"},
            {"T", "World or local axes"},
            {"F", "Frame the selection"},
            {"Ctrl+D", "Duplicate"},
            {"Delete", "Delete the selection"},
        };
        for (const auto& [k, v] : kKeys) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(tk::kAccent, "%s", k);
            ImGui::TableNextColumn();
            ImGui::TextColored(tk::kTextMuted, "%s", v);
        }
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0, tk::kSpace4));
    ImGui::TextColored(tk::kTextMuted, "The full guide is docs/USER_GUIDE.md in the VICTUS folder.");
    ImGui::Dummy(ImVec2(0, tk::kSpace4));
    if (kit::icon_text_button("Create my first project", kit::Icon::Plus, true)) set_launcher_page(LauncherPage::New);
}

} // namespace victus::editor
