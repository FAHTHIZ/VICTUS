// SPDX-License-Identifier: Apache-2.0
// Diagnostics bundles (ADR-0072). See victus/player/diagnostics.hpp.
#include "victus/player/diagnostics.hpp"
#include "victus/core/build_info.hpp"
#include "victus/core/crash.hpp"
#include "victus/core/json.hpp"
#include "victus/core/zip.hpp"
#include "victus/player/validate.hpp"
#include "victus/project/project.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace victus::player {

namespace {

namespace sfs = std::filesystem;

bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.'; }

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::string system_text() {
    std::string s;
    s += "engine_version: " + std::string(build::version()) + "\n";
    s += "engine_revision: " + std::string(build::git_revision()) + "\n";
    s += "build_type: " + std::string(build::build_type()) + "\n";
    s += "compiler: " + std::string(build::compiler()) + "\n";
    s += "platform: " + std::string(build::platform()) + "\n";
    s += "architecture: " + std::string(build::architecture()) + "\n";
    s += "hardware_threads: " + std::to_string(std::thread::hardware_concurrency()) + "\n";
#if defined(_WIN32)
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof mem;
    if (GlobalMemoryStatusEx(&mem)) s += "physical_memory_mb: " + std::to_string(mem.ullTotalPhys >> 20) + "\n";
#else
    utsname u{};
    if (uname(&u) == 0) // nodename (the host name) is deliberately not collected
        s += "os: " + std::string(u.sysname) + " " + u.release + " (" + u.version + ") " + u.machine + "\n";
#if defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
    const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page > 0)
        s += "physical_memory_mb: " + std::to_string((static_cast<unsigned long long>(pages) * static_cast<unsigned long long>(page)) >> 20) + "\n";
#endif
#endif
    s += "gpu: not collected by this tool (the editor adds its GPU report when it makes the bundle)\n";
    return s;
}

// Last `max` bytes of a file, starting at a line boundary, with a note when cut.
Result<std::string> read_tail(const fs::Path& path, std::size_t max) {
    std::error_code ec;
    const auto size = sfs::file_size(path, ec);
    if (ec) return make_error(ErrorCode::IoError, "cannot read " + path.string() + ": " + ec.message());
    auto bytes = fs::read_binary(path);
    if (!bytes) return std::move(bytes).error();
    std::string text(bytes->begin(), bytes->end());
    if (text.size() <= max) return text;
    std::size_t start = text.size() - max;
    if (const auto nl = text.find('\n', start); nl != std::string::npos && nl + 1 < text.size()) start = nl + 1;
    return "[victus diagnose: first " + std::to_string(start) + " of " + std::to_string(size) + " bytes omitted]\n" + text.substr(start);
}

std::vector<fs::Path> newest_files(const fs::Path& dir, std::string_view ext, std::size_t max) {
    std::vector<std::pair<sfs::file_time_type, fs::Path>> files;
    std::error_code ec;
    for (const auto& e : sfs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ext) continue;
        files.emplace_back(e.last_write_time(ec), e.path());
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second > b.second); });
    std::vector<fs::Path> out;
    for (std::size_t i = 0; i < files.size() && i < max; ++i) out.push_back(files[i].second);
    return out;
}

void add_path_rules(std::vector<Redaction>& rules, const fs::Path& p, const std::string& to) {
    if (p.empty()) return;
    std::error_code ec;
    for (const fs::Path& v : {p, sfs::weakly_canonical(p, ec)}) {
        std::string native = v.string(), generic = v.generic_string();
        while (native.size() > 1 && (native.back() == '/' || native.back() == '\\')) native.pop_back();
        while (generic.size() > 1 && generic.back() == '/') generic.pop_back();
        for (const std::string& s : {native, generic})
            if (s.size() >= 3 && std::none_of(rules.begin(), rules.end(), [&](const Redaction& r) { return r.from == s; }))
                rules.push_back({s, to, false});
    }
}

} // namespace

std::vector<Redaction> default_redactions(const fs::Path& project_root) {
    std::vector<Redaction> rules;
    add_path_rules(rules, project_root, "<project>");
#if defined(_WIN32)
    const std::string home = env("USERPROFILE");
    const std::string user = env("USERNAME");
#else
    const std::string home = env("HOME");
    const std::string user = env("USER").empty() ? env("LOGNAME") : env("USER");
#endif
    if (!home.empty() && home != "/") add_path_rules(rules, fs::Path(home), "<home>");
    if (user.size() >= 3 && user != "root") rules.push_back({user, "<user>", true});
    std::stable_sort(rules.begin(), rules.end(), [](const Redaction& a, const Redaction& b) { return a.from.size() > b.from.size(); });
    return rules;
}

std::string redact(std::string_view text, const std::vector<Redaction>& rules, std::size_t* count) {
    std::string out;
    out.reserve(text.size());
    std::size_t n = 0;
    for (std::size_t i = 0; i < text.size();) {
        bool hit = false;
        for (const Redaction& r : rules) {
            if (r.from.empty() || text.compare(i, r.from.size(), r.from) != 0) continue;
            if (r.whole_word && ((i > 0 && word_char(text[i - 1])) ||
                                 (i + r.from.size() < text.size() && word_char(text[i + r.from.size()]))))
                continue;
            out += r.to;
            i += r.from.size();
            ++n;
            hit = true;
            break;
        }
        if (!hit) out.push_back(text[i++]);
    }
    if (count) *count += n;
    return out;
}

Result<BundleResult> create_diagnostics_bundle(const BundleOptions& o) {
    if (o.output.empty()) return make_error(ErrorCode::InvalidArgument, "diagnostics bundle needs an output file");
    const fs::Path dir = o.diagnostics_dir.empty() ? crash::default_directory() : o.diagnostics_dir;
    const auto now = static_cast<std::int64_t>(std::time(nullptr));

    fs::Path project_root;
    std::optional<project::Project> proj;
    std::string project_error;
    if (!o.project.empty()) {
        project::register_components();
        auto p = project::Project::open(o.project);
        if (p) {
            project_root = p->root();
            proj = std::move(*p);
        } else {
            project_error = p.error().message;
            project_root = sfs::is_directory(o.project) ? o.project : o.project.parent_path();
        }
    }
    const std::vector<Redaction> rules = o.redact_paths ? default_redactions(project_root) : std::vector<Redaction>{};

    BundleResult result;
    zip::Writer zw;
    json::Value files = json::Value::array();
    const auto add = [&](const std::string& name, const std::string& text, const char* section) -> Status {
        const std::string body = o.redact_paths ? redact(text, rules, &result.redactions) : text;
        if (auto st = zw.add(name, body, now); !st) return st;
        result.files.push_back(name);
        json::Value f = json::Value::object();
        f.set("name", name);
        f.set("section", section);
        f.set("bytes", static_cast<std::uint64_t>(body.size()));
        files.as_array().push_back(std::move(f));
        return {};
    };

    if (o.include_system)
        if (auto st = add("system.txt", system_text(), "system"); !st) return std::move(st).error();
    if (o.include_crash_reports)
        for (const crash::ReportInfo& r : crash::list_reports(dir)) {
            if (result.crash_reports >= o.max_crash_reports) break;
            auto text = read_tail(r.path, o.max_file_bytes);
            if (!text) continue;
            if (auto st = add("crashes/" + r.path.filename().string(), *text, "crash_reports"); !st) return std::move(st).error();
            ++result.crash_reports;
        }
    if (o.include_logs)
        for (const fs::Path& log : newest_files(dir / "logs", ".log", o.max_logs)) {
            auto text = read_tail(log, o.max_file_bytes);
            if (!text) continue;
            if (auto st = add("logs/" + log.filename().string(), *text, "logs"); !st) return std::move(st).error();
            ++result.logs;
        }
    if (!o.project.empty() && !proj) {
        if (auto st = add("project/open_error.txt", project_error + "\n", "project"); !st) return std::move(st).error();
    }
    if (proj && o.include_project_settings) {
        auto text = fs::read_text(proj->file());
        if (text)
            if (auto st = add("project/victus.project", *text, "project_settings"); !st) return std::move(st).error();
    }
    if (proj && o.include_validation) {
        auto report = validate_project(*proj);
        std::string txt;
        json::Value vj = json::Value::object();
        if (!report) {
            txt = "validation could not run: " + report.error().message + "\n";
            vj.set("error", report.error().message);
            result.validation_ok = false;
        } else {
            json::Value list = json::Value::array();
            for (const Diagnostic& d : report->diagnostics) {
                txt += d.line() + "\n";
                json::Value dj = json::Value::object();
                dj.set("severity", to_string(d.severity));
                dj.set("category", to_string(d.category));
                dj.set("message", d.message);
                dj.set("path", d.path);
                if (!d.entity_name.empty()) dj.set("entity_name", d.entity_name);
                if (!d.entity_uuid.is_nil()) dj.set("entity_uuid", d.entity_uuid.to_string());
                if (!d.asset.is_nil()) dj.set("asset", d.asset.to_string());
                if (!d.suggested_fix.empty()) dj.set("suggested_fix", d.suggested_fix);
                list.as_array().push_back(std::move(dj));
            }
            txt += report->summary() + "\n";
            vj.set("summary", report->summary());
            vj.set("errors", static_cast<std::uint64_t>(report->errors()));
            vj.set("warnings", static_cast<std::uint64_t>(report->warnings()));
            vj.set("diagnostics", std::move(list));
            result.validation_ok = report->ok();
        }
        if (auto st = add("project/validation.txt", txt, "validation"); !st) return std::move(st).error();
        if (auto st = add("project/validation.json", json::stringify(vj) + "\n", "validation"); !st) return std::move(st).error();
    }
    for (const auto& [name, text] : o.extra) {
        if (name.empty() || name.find_first_of("/\\") != std::string::npos)
            return make_error(ErrorCode::InvalidArgument, "diagnostics extra file name must not contain folders: '" + name + "'");
        if (auto st = add("extra/" + name, text, "extra"); !st) return std::move(st).error();
    }

    // README and manifest last, so they describe exactly what is in the archive.
    std::string readme =
        "VICTUS diagnostics bundle\n"
        "=========================\n\n"
        "Made by " + o.requested_by + " with VICTUS " + build::version() + " (" + build::git_revision() + ").\n"
        "Nothing was uploaded. Review the files before attaching this bundle to a bug report.\n\n"
        "Contents:\n";
    for (const std::string& f : result.files) readme += "  " + f + "\n";
    readme += "\nPrivacy:\n";
    readme += o.redact_paths ? "  Home folder, user name and project path were replaced by <home>, <user> and <project> (" +
                                   std::to_string(result.redactions) + " replacements).\n"
                             : "  Paths were NOT redacted (redaction turned off).\n";
    readme += "  The host name, hardware serials and network addresses are never collected.\n";
    readme += "  Scenes, scripts and assets are never included; project files: " +
              std::string(proj && o.include_project_settings ? "victus.project only" : "none") + ".\n";
    readme += std::string("  Sections left out: ") + (o.include_system ? "" : "system ") + (o.include_crash_reports ? "" : "crash-reports ") +
              (o.include_logs ? "" : "logs ") + (o.include_project_settings ? "" : "project-settings ") +
              (o.include_validation ? "" : "validation ") + "\n";
    if (auto st = zw.add("README.txt", readme, now); !st) return std::move(st).error();
    result.files.push_back("README.txt");

    json::Value m = json::Value::object();
    m.set("victus_diagnostics", 1);
    m.set("created_unix", now);
    m.set("requested_by", o.requested_by);
    json::Value eng = json::Value::object();
    eng.set("version", build::version());
    eng.set("revision", build::git_revision());
    eng.set("build_type", build::build_type());
    eng.set("platform", build::platform());
    eng.set("architecture", build::architecture());
    m.set("engine", std::move(eng));
    json::Value inc = json::Value::object();
    inc.set("system", o.include_system);
    inc.set("crash_reports", o.include_crash_reports);
    inc.set("logs", o.include_logs);
    inc.set("project_settings", o.include_project_settings && proj.has_value());
    inc.set("validation", o.include_validation && proj.has_value());
    m.set("included", std::move(inc));
    json::Value priv = json::Value::object();
    priv.set("paths_redacted", o.redact_paths);
    priv.set("redactions", static_cast<std::uint64_t>(result.redactions));
    priv.set("uploaded", false);
    m.set("privacy", std::move(priv));
    m.set("files", std::move(files));
    if (auto st = zw.add("manifest.json", json::stringify(m) + "\n", now); !st) return std::move(st).error();
    result.files.push_back("manifest.json");

    const std::vector<std::uint8_t> bytes = zw.finish();
    std::error_code ec;
    if (o.output.has_parent_path()) sfs::create_directories(o.output.parent_path(), ec);
    if (auto st = fs::write_atomic(o.output, bytes); !st) return std::move(st).error();
    result.path = o.output;
    result.bytes = bytes.size();
    return result;
}

} // namespace victus::player
