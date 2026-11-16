// SPDX-License-Identifier: Apache-2.0
// `victus` command-line tool: project creation, asset import, running games and inspection
// from a terminal or CI. Shares the project, asset and player modules with the editor.
#include "victus/assets/database.hpp"
#include "victus/core/build_info.hpp"
#include "victus/core/cli.hpp"
#include "victus/core/log.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/player/player.hpp"
#include "victus/project/project.hpp"
#include "victus/scene/scene.hpp"
#include "victus/script/script.hpp"

#include <filesystem>

#include <cstdio>
#include <string>
#include <vector>

using namespace victus;

namespace {

constexpr const char* kUsage =
    "usage: victus <command> [options]\n"
    "\n"
    "commands:\n"
    "  new <folder> [--template 3d|2d|simulation] [--name NAME]   create a project\n"
    "  templates                                                  list project templates\n"
    "  info <project>                                             show project settings, scenes, assets\n"
    "  import <project>                                           import changed assets (exit 1 on failure)\n"
    "  run <project> [--scene S] [--frames N] [--screenshot PNG]  play the startup (or given) scene\n"
    "  simulate <project> [--frames N] [--hold KEYS] [--save F]   run gameplay and scripts headless (no GPU)\n"
    "  check <project>                                            validate scenes and script syntax\n"
    "  version                                                    print the engine version\n"
    "\n"
    "Run `victus <command> --help` for command options. The editor is `victus_editor`.\n";

int error(const std::string& message) {
    std::fprintf(stderr, "victus: %s\n", message.c_str());
    return 1;
}

std::vector<std::string> rest(int argc, char** argv) {
    std::vector<std::string> v;
    for (int i = 2; i < argc; ++i) v.emplace_back(argv[i]);
    return v;
}

bool wants_help(const std::vector<std::string>& a) {
    for (const auto& s : a)
        if (s == "--help" || s == "-h") return true;
    return false;
}

int cmd_new(const std::vector<std::string>& a) {
    cli::Parser p("victus new", "Create a project from a template");
    p.positional("folder", "new project folder", true)
        .option("template", "3d, 2d or simulation", "3d")
        .option("name", "project name (default: folder name)");
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message + "\n" + p.help());
    const fs::Path dir = p.get("folder");
    const std::string name = p.has("name") ? p.get("name") : dir.filename().string();
    auto proj = project::Project::create(dir, name, p.get("template"));
    if (!proj) return error(proj.error().message);
    std::printf("Created %s project '%s' in %s\n", p.get("template").c_str(), name.c_str(), proj->root().string().c_str());
    std::printf("Open it with: victus_editor \"%s\"   or run it with: victus run \"%s\"\n", proj->root().string().c_str(),
                proj->root().string().c_str());
    return 0;
}

int cmd_templates() {
    for (const auto& t : project::templates()) std::printf("%-12s %-12s %s\n", t.id.c_str(), t.display_name.c_str(), t.description.c_str());
    return 0;
}

int cmd_info(const std::vector<std::string>& a) {
    cli::Parser p("victus info", "Show a project");
    p.positional("project", "project folder", true);
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);
    auto proj = project::Project::open(p.get("project"));
    if (!proj) return error(proj.error().message);
    const auto& s = proj->settings();
    std::printf("name:          %s\nroot:          %s\ntemplate:      %s\nengine:        %s\nstartup scene: %s\nwindow:        %dx%d vsync=%s\n",
                s.name.c_str(), proj->root().string().c_str(), s.template_id.c_str(), s.engine_version.c_str(),
                s.startup_scene.c_str(), s.window_width, s.window_height, s.vsync ? "on" : "off");
    std::printf("scenes:\n");
    for (const auto& sc : proj->scenes()) std::printf("  %s\n", sc.c_str());
    auto db = assets::AssetDatabase::open(proj->root());
    if (!db) return error(db.error().message);
    const auto recs = (*db)->records();
    std::printf("assets: %zu\n", recs.size());
    for (const auto& r : recs) {
        auto up = (*db)->is_up_to_date(r.uuid);
        std::printf("  %-8s %-40s %s\n", assets::to_string(r.kind), r.path.c_str(),
                    r.importer.empty() ? "" : (up && *up) ? "imported" : "needs import");
    }
    return 0;
}

int cmd_import(const std::vector<std::string>& a) {
    cli::Parser p("victus import", "Import changed assets");
    p.positional("project", "project folder", true);
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);
    auto proj = project::Project::open(p.get("project"));
    if (!proj) return error(proj.error().message);
    assets::RefreshReport refresh;
    auto db = assets::AssetDatabase::open(proj->root(), &refresh);
    if (!db) return error(db.error().message);
    for (const auto& s : refresh.added) std::printf("new asset: %s\n", s.c_str());
    for (const auto& s : refresh.errors) std::printf("metadata error: %s\n", s.c_str());
    jobs::JobSystem jobs;
    const auto r = (*db)->import_all(&jobs);
    for (const auto& m : r.messages) std::printf("%s\n", m.c_str());
    std::printf("%zu imported, %zu up to date, %zu failed\n", r.imported, r.up_to_date, r.failed);
    return r.failed || !refresh.errors.empty() ? 1 : 0;
}

int cmd_run(const std::vector<std::string>& a) {
    cli::Parser p("victus run", "Play a project");
    p.positional("project", "project folder", true)
        .option("scene", "project-relative scene (default: startup scene)")
        .option("frames", "exit after N frames (deterministic 1/60 s steps)", "0")
        .option("screenshot", "write the last frame to this PNG (with --frames)")
        .option("width", "window width (default: project setting)", "0")
        .option("height", "window height (default: project setting)", "0")
        .flag("fullscreen", "run fullscreen")
        .flag("no-hot-reload", "do not reload scripts when their files change")
        .flag("strict", "exit with code 3 if any script raised an error");
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);
    player::PlayerOptions o;
    o.project = p.get("project");
    o.scene = p.get("scene");
    o.max_frames = static_cast<std::uint64_t>(p.get_number("frames", 0));
    o.screenshot = p.get("screenshot");
    o.width = static_cast<int>(p.get_number("width", 0));
    o.height = static_cast<int>(p.get_number("height", 0));
    o.fullscreen = p.has("fullscreen");
    o.hot_reload = !p.has("no-hot-reload");
    auto r = player::run(o);
    if (!r) return error(r.error().message);
    std::printf("ran %llu frames in %.2f s on %s (%llu script errors)\n", static_cast<unsigned long long>(r->frames),
                r->seconds, r->backend.c_str(), static_cast<unsigned long long>(r->script_errors));
    return p.has("strict") && r->script_errors ? 3 : 0;
}

std::string entity_name(const scene::Scene& s, scene::Entity e) {
    const auto* n = s.world().get<scene::Name>(e);
    return n ? n->value : std::string("<unnamed>");
}

std::vector<std::string> split_list(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t c = s.find(',', start);
        const std::string item = s.substr(start, c == std::string::npos ? std::string::npos : c - start);
        if (!item.empty()) out.push_back(item);
        if (c == std::string::npos) break;
        start = c + 1;
    }
    return out;
}

// Runs a scene's gameplay and scripts at a fixed 1/60 s step without a window or GPU. Used for
// CI checks of game logic, dedicated-server style runs and simulation batch jobs.
int cmd_simulate(const std::vector<std::string>& a) {
    cli::Parser p("victus simulate", "Run a scene's gameplay and scripts headless");
    p.positional("project", "project folder", true)
        .option("scene", "project-relative scene (default: startup scene)")
        .option("frames", "number of 1/60 s frames to run", "600")
        .option("hold", "comma-separated keys held for the whole run, e.g. D,Space")
        .option("print", "comma-separated entity names whose final position is printed")
        .option("save", "write the final scene state to this .vscene file");
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);
    project::register_components();
    auto proj = project::Project::open(p.get("project"));
    if (!proj) return error(proj.error().message);
    auto path = p.has("scene") ? proj->resolve(p.get("scene")) : proj->startup_scene_path();
    if (!path) return error(path.error().message);
    scene::Scene scene;
    if (auto st = scene.load_file(*path); !st) return error(st.error().message);
    script::HostOptions so;
    so.assets_root = proj->root() / "assets";
    auto host = script::ScriptHost::create(so);
    if (!host) return error(host.error().message);
    gameplay::ManualInput input;
    for (const auto& k : split_list(p.get("hold"))) input.set_key(k, true);
    gameplay::Simulation sim;
    gameplay::add_builtin_systems(sim);
    script::add_script_systems(sim, **host);
    sim.input = &input;
    const auto frames = static_cast<std::uint64_t>(p.get_number("frames", 600));
    for (std::uint64_t f = 0; f < frames; ++f) {
        sim.tick(scene, 1.0f / 60.0f);
        input.end_frame();
    }
    const auto& st = (*host)->stats();
    std::printf("simulated %llu frames (%.2f s game time): %zu entities, %zu script instances, %llu script errors\n",
                static_cast<unsigned long long>(frames), sim.time().time, scene.world().alive_count(), st.instances,
                static_cast<unsigned long long>(st.errors));
    for (const auto& e : (*host)->recent_errors()) std::fprintf(stderr, "script error: %s\n", e.c_str());
    for (const auto& name : split_list(p.get("print"))) {
        const auto e = scene.find_by_name(name);
        if (const auto* t = e != scene::kNullEntity ? scene.world().get<scene::Transform>(e) : nullptr)
            std::printf("%s: %.3f %.3f %.3f\n", name.c_str(), t->position.x, t->position.y, t->position.z);
        else
            std::printf("%s: (not found)\n", name.c_str());
    }
    if (p.has("save"))
        if (auto s2 = scene.save_file(p.get("save")); !s2) return error(s2.error().message);
    return st.errors ? 3 : 0;
}

// Validates every scene (loads, no unknown components) and every script (syntax, module loads,
// referenced files exist). Exit 1 if anything is wrong.
int cmd_check(const std::vector<std::string>& a) {
    cli::Parser p("victus check", "Validate scenes and scripts");
    p.positional("project", "project folder", true);
    if (wants_help(a)) return std::printf("%s", p.help().c_str()), 0;
    if (auto st = p.parse(a); !st) return error(st.error().message);
    project::register_components();
    auto proj = project::Project::open(p.get("project"));
    if (!proj) return error(proj.error().message);
    const fs::Path assets = proj->root() / "assets";
    std::size_t problems = 0, scripts = 0, scenes = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(assets, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file() || it->path().extension() != ".luau") continue;
        ++scripts;
        const std::string rel = std::filesystem::relative(it->path(), assets).generic_string();
        auto text = fs::read_text(it->path());
        if (!text) {
            std::printf("error: %s: %s\n", rel.c_str(), text.error().message.c_str());
            ++problems;
        } else if (auto st = script::check_syntax(*text, rel); !st) {
            std::printf("error: %s\n", st.error().message.c_str());
            ++problems;
        }
    }
    script::HostOptions so;
    so.assets_root = assets;
    auto host = script::ScriptHost::create(so);
    if (!host) return error(host.error().message);
    for (const auto& rel : proj->scenes()) {
        ++scenes;
        auto path = proj->resolve(rel);
        scene::Scene scene;
        if (!path) {
            std::printf("error: %s: %s\n", rel.c_str(), path.error().message.c_str());
            ++problems;
            continue;
        }
        if (auto st = scene.load_file(*path); !st) {
            std::printf("error: %s: %s\n", rel.c_str(), st.error().message.c_str());
            ++problems;
            continue;
        }
        scene.world().each<scene::UnknownComponents>([&](scene::Entity e, scene::UnknownComponents& u) {
            for (const auto& [name, value] : u.components) {
                std::printf("warning: %s: entity '%s' has unknown component '%s'\n", rel.c_str(),
                            entity_name(scene, e).c_str(), name.c_str());
                (void)value;
            }
        });
        scene.world().each<script::ScriptComponent>([&](scene::Entity e, script::ScriptComponent& c) {
            if (c.script.empty()) return;
            if (auto d = (*host)->describe(c.script); !d) {
                std::printf("error: %s: entity '%s': %s\n", rel.c_str(), entity_name(scene, e).c_str(),
                            d.error().message.c_str());
                ++problems;
            }
        });
    }
    std::printf("checked %zu scenes and %zu scripts: %zu problems\n", scenes, scripts, problems);
    return problems ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return std::fprintf(stderr, "%s", kUsage), 2;
    const std::string cmd = argv[1];
    const auto args = rest(argc, argv);
    if (cmd == "new") return cmd_new(args);
    if (cmd == "templates") return cmd_templates();
    if (cmd == "info") return cmd_info(args);
    if (cmd == "import") return cmd_import(args);
    if (cmd == "run") return cmd_run(args);
    if (cmd == "simulate") return cmd_simulate(args);
    if (cmd == "check") return cmd_check(args);
    if (cmd == "version" || cmd == "--version") return std::printf("VICTUS %s\n", build::version()), 0;
    if (cmd == "help" || cmd == "--help" || cmd == "-h") return std::printf("%s", kUsage), 0;
    std::fprintf(stderr, "victus: unknown command '%s'\n\n%s", cmd.c_str(), kUsage);
    return 2;
}
