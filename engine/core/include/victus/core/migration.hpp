// SPDX-License-Identifier: Apache-2.0
// Versioned document migration (ADR-0100). A Format names a JSON document kind ("victus.scene")
// and its current version; each Step upgrades a document from `from` to `from + 1`. Loading runs
// the steps in order (like Unreal's per-module custom versions, never decreasing) and refuses
// documents written by a newer build instead of guessing. Upgrades are transactional: on any
// failure the document is left exactly as it was.
#pragma once
#include "victus/core/json.hpp"
#include "victus/core/result.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace victus::migration {

struct Step {
    std::uint32_t from{1};                         // upgrades version `from` to `from + 1`
    std::string summary;                           // shown in reports ("rename Light.range to radius")
    std::function<Status(json::Value& doc)> apply; // edits the whole document in place
};

struct Report {
    std::string format;
    std::uint32_t from{0};
    std::uint32_t to{0};
    std::vector<std::string> applied; // summaries of the steps that ran, oldest first
    [[nodiscard]] bool migrated() const noexcept { return from != to; }
};

class Format {
public:
    // `steps` must cover every version from 1 to current - 1 exactly once.
    Format(std::string name, std::uint32_t current, std::vector<Step> steps = {});

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] std::uint32_t current() const noexcept { return current_; }
    [[nodiscard]] const std::vector<Step>& steps() const noexcept { return steps_; }

    // Version stored in `doc` ("version"), checked against what this build reads.
    [[nodiscard]] Result<std::uint32_t> version_of(const json::Value& doc) const;
    // Upgrades `doc` to the current version (no-op when already current) and sets its "version".
    [[nodiscard]] Result<Report> upgrade(json::Value& doc) const;

private:
    std::string name_;
    std::uint32_t current_;
    std::vector<Step> steps_; // sorted by `from`, contiguous
    bool valid_{false};
};

// Component-level versions inside documents: a component object may carry "$version" (absent
// means 1). Helpers for serializers that version their data independently of the document.
inline constexpr std::string_view kComponentVersionKey = "$version";
[[nodiscard]] std::uint32_t component_version(const json::Value& component) noexcept;

} // namespace victus::migration
