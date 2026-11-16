// SPDX-License-Identifier: Apache-2.0
#include "victus/project/project.hpp"

#include "victus/core/build_info.hpp"
#include "victus/gameplay/gameplay.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace victus::project {

using json::Value;
namespace sfs = std::filesystem;

namespace {
Status bad(std::string message) { return make_error(ErrorCode::ParseError, std::move(message)); }

Value settings_to_json(const ProjectSettings& s) {
    Value v = s.extra.is_object() ? s.extra : Value::object();
    v.set("format", "victus.project");
    v.set("version", kProjectFormatVersion);
    v.set("name", s.name);
    v.set("startup_scene", s.startup_scene);
    v.set("template", s.template_id);
    v.set("engine_version", s.engine_version);
    Value window = Value::object();
    window.set("width", s.window_width);
    window.set("height", s.window_height);
    window.set("vsync", s.vsync);
    v.set("window", std::move(window));
    Value rendering = Value::object();
    rendering.set("quality", s.render_quality);
    if (s.render_scale > 0.0f) rendering.set("render_scale", s.render_scale);
    v.set("rendering", std::move(rendering));
    return v;
}

Status settings_from_json(const Value& v, ProjectSettings& s) {
    if (v["format"].as_string_or("") != "victus.project") return bad("not a victus.project file");
    const auto version = v["version"].as_int(0);
    if (version < 1 || version > kProjectFormatVersion)
        return make_error(ErrorCode::VersionMismatch, "unsupported project version " + std::to_string(version));
    s.name = v["name"].as_string_or("Untitled");
    s.startup_scene = v["startup_scene"].as_string_or("scenes/main.vscene");
    s.template_id = v["template"].as_string_or("");
    s.engine_version = v["engine_version"].as_string_or("");
    const Value& w = v["window"];
    s.window_width = static_cast<int>(std::clamp<std::int64_t>(w["width"].as_int(1280), 64, 16384));
    s.window_height = static_cast<int>(std::clamp<std::int64_t>(w["height"].as_int(720), 64, 16384));
    s.vsync = w["vsync"].as_bool(true);
    const Value& r = v["rendering"];
    s.render_quality = r["quality"].as_string_or("high");
    if (s.render_quality != "low" && s.render_quality != "medium" && s.render_quality != "high" &&
        s.render_quality != "ultra")
        return bad("rendering.quality must be low, medium, high or ultra");
    s.render_scale = r["render_scale"].as_float(0.0f);
    if (!(s.render_scale == 0.0f || (s.render_scale >= 0.25f && s.render_scale <= 2.0f)))
        return bad("rendering.render_scale must be 0 (tier default) or between 0.25 and 2");
    s.extra = Value::object();
    for (const auto& [key, value] : v.as_object()) {
        if (key == "format" || key == "version" || key == "name" || key == "startup_scene" || key == "template" ||
            key == "engine_version" || key == "window" || key == "rendering")
            continue;
        s.extra.set(key, value);
    }
    if (!fs::sanitize_relative(s.startup_scene)) return bad("startup_scene must be a project-relative path");
    return {};
}

fs::Path path_from_utf8(std::string_view s) {
    std::u8string u;
    u.reserve(s.size());
    for (const char c : s) u.push_back(static_cast<char8_t>(c));
    return fs::Path(u);
}

std::string path_to_utf8(const fs::Path& p) {
    const std::u8string u = p.u8string();
    std::string out;
    out.reserve(u.size());
    for (const char8_t c : u) out.push_back(static_cast<char>(c));
    return out;
}

constexpr std::string_view kGitIgnore = "# VICTUS: derived data, safe to delete\n/.victus/\n/build/\n";

std::string readme(std::string_view name, std::string_view template_name) {
    return std::string("# ") + std::string(name) +
           "\n\nCreated with VICTUS from the " + std::string(template_name) +
           " template.\n\n- `victus.project` - project settings\n- `assets/` - source assets (each with a `.meta` file; "
           "commit both)\n- `assets/scripts/` - Luau gameplay scripts, hot reloaded while playing in the editor\n"
           "- `scenes/` - scenes (`.vscene`, JSON)\n- `.victus/` - import cache, safe to delete\n\n"
           "Open the folder in the VICTUS editor, or run `victus run <this folder>`.\n";
}
} // namespace

bool Project::is_project_dir(const fs::Path& directory) {
    return fs::exists(directory / kProjectFileName);
}

Result<Project> Project::create(const fs::Path& directory, std::string_view name, std::string_view template_id) {
    const auto& list = templates();
    const auto it = std::find_if(list.begin(), list.end(), [&](const TemplateInfo& t) { return t.id == template_id; });
    if (it == list.end()) return make_error(ErrorCode::InvalidArgument, "unknown template '" + std::string(template_id) + "'");
    if (name.empty()) return make_error(ErrorCode::InvalidArgument, "project name must not be empty");
    std::error_code ec;
    if (sfs::exists(directory, ec)) {
        if (!sfs::is_directory(directory, ec) || !sfs::is_empty(directory, ec))
            return make_error(ErrorCode::AlreadyExists, "target folder exists and is not empty: " + directory.string());
    }
    sfs::create_directories(directory / "assets", ec);
    if (!ec) sfs::create_directories(directory / "scenes", ec);
    if (ec) return make_error(ErrorCode::IoError, "cannot create project folders: " + ec.message());

    Project p;
    p.root_ = sfs::absolute(directory, ec).lexically_normal();
    p.settings_.name = std::string(name);
    p.settings_.template_id = std::string(template_id);
    p.settings_.engine_version = build::version();

    register_components();
    scene::Scene starter;
    VICTUS_TRY_STATUS(build_template_scene(template_id, starter));
    for (const TemplateFile& f : template_files(template_id)) {
        auto path = p.resolve(f.path);
        if (!path) return std::move(path).error();
        sfs::create_directories(path->parent_path(), ec);
        if (ec) return make_error(ErrorCode::IoError, "cannot create " + path->parent_path().string() + ": " + ec.message());
        VICTUS_TRY_STATUS(fs::write_text_atomic(*path, f.content));
    }
    starter.name = "Main";
    auto scene_path = p.resolve(p.settings_.startup_scene);
    if (!scene_path) return std::move(scene_path).error();
    VICTUS_TRY_STATUS(starter.save_file(*scene_path));
    VICTUS_TRY_STATUS(fs::write_text_atomic(p.root_ / ".gitignore", kGitIgnore));
    VICTUS_TRY_STATUS(fs::write_text_atomic(p.root_ / "README.md", readme(name, it->display_name)));
    VICTUS_TRY_STATUS(p.save());
    return p;
}

Result<Project> Project::open(const fs::Path& path) {
    std::error_code ec;
    fs::Path root = path;
    if (path.filename() == kProjectFileName && fs::exists(path)) root = path.parent_path();
    if (!is_project_dir(root)) return make_error(ErrorCode::NotFound, "no victus.project in " + root.string());
    auto text = fs::read_text(root / kProjectFileName, 1u << 20);
    if (!text) return std::move(text).error();
    auto doc = json::parse(*text);
    if (!doc) return make_error(ErrorCode::ParseError, "victus.project: " + doc.error().message);
    Project p;
    p.root_ = sfs::absolute(root, ec).lexically_normal();
    VICTUS_TRY_STATUS(settings_from_json(*doc, p.settings_));
    return p;
}

Status Project::save() const { return fs::write_text_atomic(file(), json::stringify(settings_to_json(settings_)) + "\n"); }

Result<fs::Path> Project::resolve(std::string_view rel) const { return fs::resolve_under(root_, rel); }

std::string Project::relative(const fs::Path& path) const {
    std::error_code ec;
    const fs::Path abs = sfs::absolute(path, ec).lexically_normal();
    const fs::Path rel = abs.lexically_relative(root_);
    if (rel.empty()) return {};
    const std::string s = rel.generic_string();
    if (s == ".." || s.starts_with("../")) return {};
    return s;
}

std::vector<std::string> Project::scenes() const {
    std::vector<std::string> out;
    const fs::Path dir = scenes_dir();
    for (const std::string& rel : fs::list_files(dir))
        if (fs::Path(rel).extension() == kSceneExtension) out.push_back(relative(dir / fs::Path(rel)));
    std::sort(out.begin(), out.end());
    return out;
}

// ---- recent projects ---------------------------------------------------------------------------

fs::Path RecentProjects::default_file() { return fs::user_data_dir() / "recent_projects.json"; }

RecentProjects::RecentProjects(fs::Path file) : file_(std::move(file)) {
    auto text = fs::read_text(file_, 1u << 20);
    if (!text) return;
    auto doc = json::parse(*text);
    if (!doc || !(*doc)["projects"].is_array()) return;
    const Value& arr = (*doc)["projects"];
    for (std::size_t i = 0; i < arr.size() && entries_.size() < kMax; ++i) {
        if (arr[i].is_string()) entries_.push_back(path_from_utf8(arr[i].as_string_or("")));
    }
}

void RecentProjects::add(const fs::Path& root) {
    remove(root);
    entries_.insert(entries_.begin(), root);
    if (entries_.size() > kMax) entries_.resize(kMax);
}

void RecentProjects::remove(const fs::Path& root) {
    std::erase_if(entries_, [&](const fs::Path& p) { return p.lexically_normal() == root.lexically_normal(); });
}

Status RecentProjects::save() const {
    std::error_code ec;
    sfs::create_directories(file_.parent_path(), ec);
    Value doc = Value::object();
    Value arr = Value::array();
    for (const auto& p : entries_) {
        arr.push(path_to_utf8(p));
    }
    doc.set("projects", std::move(arr));
    return fs::write_text_atomic(file_, json::stringify(doc) + "\n");
}

} // namespace victus::project
