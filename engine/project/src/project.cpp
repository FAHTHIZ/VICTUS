// SPDX-License-Identifier: Apache-2.0
#include "victus/project/project.hpp"

#include "victus/core/build_info.hpp"
#include "victus/gameplay/gameplay.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
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
    if (s.target_fps > 0) {
        rendering.set("target_fps", s.target_fps);
        rendering.set("dynamic_resolution_min", s.dynamic_resolution_min);
    }
    if (!s.temporal_upscale) rendering.set("temporal_upscale", false);
    if (s.mobile_quality != "medium") rendering.set("mobile_quality", s.mobile_quality);
    if (s.mobile_target_fps != 30) rendering.set("mobile_target_fps", s.mobile_target_fps);
    v.set("rendering", std::move(rendering));
    v.set("input", s.input.to_json());
    if (s.save_version != 1) v.set("save_version", s.save_version);
    if (s.default_locale != "en") v.set("default_locale", s.default_locale);
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
    s.mobile_quality = r["mobile_quality"].as_string_or("medium");
    if (s.mobile_quality != "low" && s.mobile_quality != "medium" && s.mobile_quality != "high" &&
        s.mobile_quality != "ultra")
        return bad("rendering.mobile_quality must be low, medium, high or ultra");
    const auto mfps = r["mobile_target_fps"].as_int(30);
    if (mfps < 0 || mfps > 1000) return bad("rendering.mobile_target_fps must be 0 (off) or between 1 and 1000");
    s.mobile_target_fps = static_cast<int>(mfps);
    s.render_scale = r["render_scale"].as_float(0.0f);
    if (!(s.render_scale == 0.0f || (s.render_scale >= 0.25f && s.render_scale <= 2.0f)))
        return bad("rendering.render_scale must be 0 (tier default) or between 0.25 and 2");
    const auto fps = r["target_fps"].as_int(0);
    if (fps < 0 || fps > 1000) return bad("rendering.target_fps must be 0 (off) or between 1 and 1000");
    s.target_fps = static_cast<int>(fps);
    s.temporal_upscale = r["temporal_upscale"].as_bool(true);
    s.dynamic_resolution_min = r["dynamic_resolution_min"].as_float(0.5f);
    if (!(s.dynamic_resolution_min >= 0.25f && s.dynamic_resolution_min <= 1.0f))
        return bad("rendering.dynamic_resolution_min must be between 0.25 and 1");
    if (const Value* in = v.find("input")) {
        auto map = gameplay::InputMap::from_json(*in);
        if (!map) return bad(map.error().message);
        s.input = std::move(*map);
    } else {
        s.input = gameplay::default_input_map();
    }
    const auto save_version = v["save_version"].as_int(1);
    if (save_version < 1 || save_version > 1000000) return bad("save_version must be between 1 and 1000000");
    s.save_version = static_cast<std::uint32_t>(save_version);
    s.default_locale = v["default_locale"].as_string_or("en");
    if (s.default_locale.empty() || s.default_locale.size() > 32 ||
        !std::ranges::all_of(s.default_locale, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; }))
        return bad("default_locale must be a locale code such as \"en\" or \"pt-BR\"");
    s.extra = Value::object();
    for (const auto& [key, value] : v.as_object()) {
        if (key == "format" || key == "version" || key == "name" || key == "startup_scene" || key == "template" ||
            key == "engine_version" || key == "window" || key == "rendering" || key == "input" ||
            key == "save_version" || key == "default_locale")
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
           "- `scenes/` - scenes (`.vscene`, JSON)\n- `.victus/` - import cache and development save games, safe to delete\n\n"
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

Result<std::string> expand_bridge_variables(std::string_view text, const fs::Path& project_root,
                                            const fs::Path& save_directory) {
    std::string out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t open = text.find("${", pos);
        if (open == std::string_view::npos) {
            out.append(text.substr(pos));
            break;
        }
        out.append(text.substr(pos, open - pos));
        const std::size_t close = text.find('}', open);
        if (close == std::string_view::npos) return make_error(ErrorCode::ParseError, std::format("unclosed '${{' in '{}'", text));
        const std::string_view var = text.substr(open + 2, close - open - 2);
        if (var == "project") out += project_root.generic_string();
        else if (var == "saves") out += save_directory.generic_string();
        else if (var.starts_with("env:")) {
            std::string_view env = var.substr(4), fallback;
            bool has_fallback = false;
            if (const std::size_t d = env.find(":-"); d != std::string_view::npos) {
                fallback = env.substr(d + 2);
                env = env.substr(0, d);
                has_fallback = true;
            }
            const char* value = std::getenv(std::string(env).c_str());
            if (value && *value) out += value;
            else if (has_fallback) out += fallback;
            else return make_error(ErrorCode::NotFound, std::format("environment variable {} is not set", env));
        } else {
            return make_error(ErrorCode::InvalidArgument, std::format("unknown variable '${{{}}}' (project, saves, env:NAME)", var));
        }
        pos = close + 1;
    }
    return out;
}

Result<process::LaunchOptions> bridge_launch(const Project& project, std::string_view name, const fs::Path& save_directory) {
    const Value* bridges = project.settings().extra.find("bridges");
    const Value* entry = bridges && bridges->is_object() ? bridges->find(name) : nullptr;
    if (!entry || !entry->is_object())
        return make_error(ErrorCode::NotFound, std::format("the project has no bridge named '{}' (\"bridges\" in victus.project)", name));
    const Value* command = entry->find("command");
    if (!command || !command->is_array() || command->as_array().empty())
        return make_error(ErrorCode::InvalidArgument, std::format("bridge '{}': \"command\" must be a non-empty array of strings", name));
    process::LaunchOptions o;
    for (const Value& part : command->as_array()) {
        if (!part.is_string()) return make_error(ErrorCode::InvalidArgument, std::format("bridge '{}': command parts must be strings", name));
        auto expanded = expand_bridge_variables(part.as_string(), project.root(), save_directory);
        if (!expanded) return make_error(expanded.error().code, std::format("bridge '{}': {}", name, expanded.error().message));
        o.args.push_back(std::move(*expanded));
    }
    // Relative program paths with a directory are project-relative; bare names use PATH.
    if (fs::Path program(o.args.front()); program.is_relative() && o.args.front().find('/') != std::string::npos)
        o.args.front() = (project.root() / program).lexically_normal().string();
    o.working_directory = project.root().string();
    if (const Value* wd = entry->find("working_directory"); wd && wd->is_string()) {
        auto expanded = expand_bridge_variables(wd->as_string(), project.root(), save_directory);
        if (!expanded) return make_error(expanded.error().code, std::format("bridge '{}': {}", name, expanded.error().message));
        fs::Path dir(*expanded);
        o.working_directory = (dir.is_relative() ? project.root() / dir : dir).lexically_normal().string();
    }
    if (const Value* d = entry->find("discard_stderr"); d && d->is_bool()) o.discard_stderr = d->as_bool();
    std::error_code ec;
    if (!save_directory.empty()) std::filesystem::create_directories(save_directory, ec);
    return o;
}

} // namespace victus::project
