// SPDX-License-Identifier: Apache-2.0
// Diagnostics bundles (victus/player/diagnostics.hpp): what `victus diagnose` and the editor's
// "Create diagnostics bundle" write, its privacy defaults and its opt-outs.
#include "doctest.h"
#include "victus/core/json.hpp"
#include "victus/core/zip.hpp"
#include "victus/player/diagnostics.hpp"
#include "victus/project/project.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

using namespace victus;
namespace sfs = std::filesystem;

namespace {

struct TempDir {
    fs::Path path;
    explicit TempDir(const char* name) {
        path = sfs::temp_directory_path() / (std::string(name) + "_" + Uuid::generate().to_string().substr(0, 8));
        sfs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        sfs::remove_all(path, ec);
    }
};

void write(const fs::Path& p, std::string_view body) {
    sfs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << body;
}

std::map<std::string, std::string> unzip(const fs::Path& p) {
    auto bytes = fs::read_binary(p);
    REQUIRE(bytes.ok());
    auto entries = zip::read(*bytes);
    REQUIRE(entries.ok());
    std::map<std::string, std::string> out;
    for (auto& e : *entries) out[e.name] = std::string(e.data.begin(), e.data.end());
    return out;
}

std::string home() {
#if defined(_WIN32)
    const char* h = std::getenv("USERPROFILE");
#else
    const char* h = std::getenv("HOME");
#endif
    return h ? h : "";
}

} // namespace

TEST_CASE("diagnostics: redact replaces longest match first and respects word boundaries") {
    const std::vector<player::Redaction> rules = {
        {"/home/ann/games/space", "<project>", false},
        {"/home/ann", "<home>", false},
        {"ann", "<user>", true},
    };
    std::size_t n = 0;
    CHECK(player::redact("open /home/ann/games/space/assets/a.lua", rules, &n) == "open <project>/assets/a.lua");
    CHECK(player::redact("cfg /home/ann/.config", rules, &n) == "cfg <home>/.config");
    CHECK(player::redact("by ann, not annual or joanna", rules, &n) == "by <user>, not annual or joanna");
    CHECK(n == 3);
    CHECK(player::redact("nothing here", rules, nullptr) == "nothing here");

    const fs::Path project = sfs::temp_directory_path() / "victus_some_project";
    const auto defaults = player::default_redactions(project);
    REQUIRE_FALSE(defaults.empty());
    CHECK(std::is_sorted(defaults.begin(), defaults.end(),
                         [](const player::Redaction& a, const player::Redaction& b) { return a.from.size() > b.from.size(); }));
    CHECK(std::any_of(defaults.begin(), defaults.end(), [&](const player::Redaction& r) { return r.from == project.string() && r.to == "<project>"; }));
}

TEST_CASE("diagnostics: bundle has crash reports, logs, system, project and validation, redacted") {
    TempDir tmp("victus_diag_bundle");
    const fs::Path diag = tmp.path / "diag";
    const fs::Path proj = tmp.path / "My Game";
    REQUIRE(project::Project::create(proj, "My Game", "2d").ok());
    const std::string h = home();
    for (int i = 0; i < 12; ++i)
        write(diag / "crashes" / ("crash-" + std::to_string(1700000000 + i) + "-42-victus_player.txt"),
              "VICTUS crash report\nformat: 1\nreason: SIGSEGV " + std::to_string(i) + "\nscript " + proj.string() +
                  "/assets/scripts/player.lua\nconfig " + h + "/victus.cfg\n");
    for (int i = 0; i < 4; ++i) write(diag / "logs" / ("victus_editor-" + std::to_string(1700000000 + i) + "-7.log"), "log " + std::to_string(i) + "\n");
    write(diag / "logs" / "victus_editor-1700000003-7.log", std::string(300, 'a') + "\n" + std::string(300, 'b') + "\nlast line\n");

    player::BundleOptions o;
    o.output = tmp.path / "out" / "bundle.zip";
    o.diagnostics_dir = diag;
    o.project = proj;
    o.requested_by = "diagnostics test";
    o.max_file_bytes = 400;
    o.extra.emplace_back("gpu.txt", "adapter: test gpu\n");
    auto r = player::create_diagnostics_bundle(o);
    REQUIRE(r.ok());
    CHECK(r->path == o.output);
    CHECK(r->crash_reports == 10); // max_crash_reports default
    CHECK(r->logs == 3);           // max_logs default
    CHECK(r->validation_ok);
    CHECK(r->redactions > 0);
    CHECK(r->bytes == sfs::file_size(o.output));

    const auto files = unzip(o.output);
    CHECK(files.size() == r->files.size());
    for (const char* f : {"README.txt", "manifest.json", "system.txt", "project/victus.project", "project/validation.txt",
                          "project/validation.json", "extra/gpu.txt", "crashes/crash-1700000011-42-victus_player.txt"}) {
        CAPTURE(f);
        CHECK(files.count(f) == 1);
    }
    CHECK(files.count("crashes/crash-1700000001-42-victus_player.txt") == 0); // the two oldest are left out

    const std::string& crash = files.at("crashes/crash-1700000011-42-victus_player.txt");
    CHECK(crash.find("reason: SIGSEGV 11") != std::string::npos);
    CHECK(crash.find("<project>/assets/scripts/player.lua") != std::string::npos);
    for (const auto& [name, body] : files) {
        CAPTURE(name);
        CHECK(body.find(proj.string()) == std::string::npos);
        if (h.size() >= 3) CHECK(body.find(h) == std::string::npos);
    }
    // Long files keep their tail, cut at a line boundary.
    for (const auto& [name, body] : files)
        if (name.starts_with("logs/") && body.find("last line") != std::string::npos) {
            CHECK(body.starts_with("[victus diagnose: first "));
            CHECK(body.find(std::string(300, 'a')) == std::string::npos);
        }
    CHECK(files.at("system.txt").find("engine_version: ") != std::string::npos);
    CHECK(files.at("project/validation.txt").find(": 0 problems") != std::string::npos);
    CHECK(files.at("README.txt").find("Nothing was uploaded") != std::string::npos);

    auto manifest = json::parse(files.at("manifest.json"));
    REQUIRE(manifest.ok());
    CHECK((*manifest)["victus_diagnostics"].as_int() == 1);
    CHECK((*manifest)["requested_by"].as_string() == "diagnostics test");
    CHECK((*manifest)["privacy"]["paths_redacted"].as_bool());
    CHECK_FALSE((*manifest)["privacy"]["uploaded"].as_bool(true));
    CHECK((*manifest)["files"].as_array().size() + 2 == files.size()); // README and manifest describe the rest
}

TEST_CASE("diagnostics: opt-outs leave sections out, keep-paths keeps paths") {
    TempDir tmp("victus_diag_optout");
    const fs::Path diag = tmp.path / "diag";
    write(diag / "crashes" / "crash-1700000000-1-victus.txt", "reason: x\npath " + tmp.path.string() + "\n");
    write(diag / "logs" / "victus-1700000000-1.log", "hello\n");
    player::BundleOptions o;
    o.output = tmp.path / "b.zip";
    o.diagnostics_dir = diag;
    o.include_system = false;
    o.include_logs = false;
    o.redact_paths = false;
    auto r = player::create_diagnostics_bundle(o);
    REQUIRE(r.ok());
    CHECK(r->logs == 0);
    CHECK(r->crash_reports == 1);
    CHECK(r->redactions == 0);
    const auto files = unzip(o.output);
    CHECK(files.count("system.txt") == 0);
    CHECK(std::none_of(files.begin(), files.end(), [](const auto& f) { return f.first.starts_with("logs/") || f.first.starts_with("project/"); }));
    CHECK(files.at("crashes/crash-1700000000-1-victus.txt").find(tmp.path.string()) != std::string::npos);
    CHECK(files.at("README.txt").find("NOT redacted") != std::string::npos);

    o.include_crash_reports = false;
    o.extra.emplace_back("../escape.txt", "x");
    CHECK_FALSE(player::create_diagnostics_bundle(o).ok());
    o.extra.clear();
    o.output.clear();
    CHECK_FALSE(player::create_diagnostics_bundle(o).ok());
}

TEST_CASE("diagnostics: an unopenable project is reported inside the bundle") {
    TempDir tmp("victus_diag_badproject");
    player::BundleOptions o;
    o.output = tmp.path / "b.zip";
    o.diagnostics_dir = tmp.path / "empty";
    o.project = tmp.path / "not_a_project";
    sfs::create_directories(o.project);
    auto r = player::create_diagnostics_bundle(o);
    REQUIRE(r.ok());
    const auto files = unzip(o.output);
    CHECK(files.count("project/open_error.txt") == 1);
    CHECK(files.count("project/validation.txt") == 0);
}
