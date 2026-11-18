// SPDX-License-Identifier: Apache-2.0
// Diagnostics bundles (ADR-0072, docs/DIAGNOSTICS.md): one .zip a user can attach to a bug
// report. `victus diagnose` and the editor build it with create_diagnostics_bundle().
//
//   manifest.json            format, time, engine build, what was included and redacted
//   README.txt               plain-language summary of the contents and the privacy choices
//   system.txt               OS name/release, CPU architecture and thread count, memory, build
//   crashes/crash-*.txt      newest crash reports (victus/core/crash.hpp)
//   logs/*.log               newest session logs (tails if large)
//   project/victus.project   project manifest (no scenes, scripts or assets)
//   project/validation.txt   `victus check` output; project/validation.json the same structured
//   extra/<name>             caller-provided text (e.g. the editor's GPU info or profiler capture)
//
// Privacy: nothing is uploaded; every section can be turned off; by default the home directory,
// the user name and the project path are replaced by <home>, <user> and <project> in every
// file, and the host name is never collected. Review the bundle before sharing it.
#pragma once
#include "victus/core/fs.hpp"
#include "victus/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace victus::player {

struct BundleOptions {
    fs::Path output;          // .zip to write; parent folders are created; overwritten
    fs::Path diagnostics_dir; // empty = crash::default_directory()
    fs::Path project;         // optional project folder (or victus.project)
    std::string requested_by{"victus"}; // tool that made the bundle, recorded in the manifest

    bool include_system{true};
    bool include_crash_reports{true};
    std::size_t max_crash_reports{10};
    bool include_logs{true};
    std::size_t max_logs{3};
    bool include_project_settings{true}; // victus.project
    bool include_validation{true};       // runs validate_project on `project`
    bool redact_paths{true};
    std::size_t max_file_bytes{4u << 20}; // larger logs/reports keep their last bytes

    std::vector<std::pair<std::string, std::string>> extra; // file name (no folders), text
};

struct BundleResult {
    fs::Path path;
    std::vector<std::string> files; // archive entry names in order
    std::size_t bytes{0};
    std::size_t redactions{0};    // replaced occurrences across all files
    std::size_t crash_reports{0};
    std::size_t logs{0};
    bool validation_ok{true};     // false if validation ran and found errors
};

[[nodiscard]] Result<BundleResult> create_diagnostics_bundle(const BundleOptions& options);

// What the bundle redacts: each `from` (longest first) becomes `to`; the user name only as a whole word. Built from the home
// directory, user name and project root of this machine. Exposed for tests and previews.
struct Redaction {
    std::string from, to;
    bool whole_word{false}; // only where not touching letters, digits, '_', '-' or '.'
};
[[nodiscard]] std::vector<Redaction> default_redactions(const fs::Path& project_root);
// Applies the rules; `count` (optional) receives the number of replacements.
[[nodiscard]] std::string redact(std::string_view text, const std::vector<Redaction>& rules, std::size_t* count = nullptr);

} // namespace victus::player
