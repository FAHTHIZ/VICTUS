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
        .flag("fullscreen", "run fullscreen");
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
    auto r = player::run(o);
    if (!r) return error(r.error().message);
    std::printf("ran %llu frames in %.2f s on %s\n", static_cast<unsigned long long>(r->frames), r->seconds, r->backend.c_str());
    return 0;
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
    if (cmd == "version" || cmd == "--version") return std::printf("VICTUS %s\n", build::version()), 0;
    if (cmd == "help" || cmd == "--help" || cmd == "-h") return std::printf("%s", kUsage), 0;
    std::fprintf(stderr, "victus: unknown command '%s'\n\n%s", cmd.c_str(), kUsage);
    return 2;
}
