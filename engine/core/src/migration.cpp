// SPDX-License-Identifier: Apache-2.0
#include "victus/core/migration.hpp"

#include "victus/core/assert.hpp"

#include <algorithm>
#include <format>

namespace victus::migration {

Format::Format(std::string name, std::uint32_t current, std::vector<Step> steps)
    : name_(std::move(name)), current_(current), steps_(std::move(steps)) {
    std::sort(steps_.begin(), steps_.end(), [](const Step& a, const Step& b) { return a.from < b.from; });
    valid_ = current_ >= 1 && steps_.size() == current_ - 1;
    for (std::size_t i = 0; valid_ && i < steps_.size(); ++i) valid_ = steps_[i].from == i + 1 && steps_[i].apply;
    VICTUS_ASSERT(valid_, "migration::Format needs exactly one step per version from 1 to current - 1");
}

Result<std::uint32_t> Format::version_of(const json::Value& doc) const {
    const json::Value* v = doc.is_object() ? doc.find("version") : nullptr;
    if (!v || !v->is_number()) return make_error(ErrorCode::ParseError, std::format("{} document has no version", name_));
    const std::int64_t version = v->as_int(0);
    if (version < 1 || static_cast<double>(version) != v->as_number())
        return make_error(ErrorCode::ParseError, std::format("{} document has an invalid version {}", name_, v->as_number()));
    if (version > static_cast<std::int64_t>(current_))
        return make_error(ErrorCode::VersionMismatch,
                          std::format("{} version {} was written by a newer VICTUS; this build reads versions 1 to {}", name_, version, current_));
    return static_cast<std::uint32_t>(version);
}

Result<Report> Format::upgrade(json::Value& doc) const {
    auto version = version_of(doc);
    if (!version) return version.error();
    if (!valid_ && *version != current_)
        return make_error(ErrorCode::Unsupported, std::format("{} has an incomplete migration chain", name_));
    Report report{name_, *version, current_, {}};
    if (*version == current_) return report;
    json::Value work = doc; // copy-on-write: the original stays intact if a step fails
    for (std::uint32_t v = *version; v < current_; ++v) {
        const Step& step = steps_[v - 1];
        if (auto st = step.apply(work); !st)
            return make_error(st.error().code, std::format("{} migration from version {} ({}) failed: {}", name_, v, step.summary,
                                                           st.error().message));
        if (!work.is_object()) return make_error(ErrorCode::ParseError, std::format("{} migration from version {} broke the document", name_, v));
        report.applied.push_back(std::format("{} -> {}: {}", v, v + 1, step.summary));
    }
    work.set("version", current_);
    doc = std::move(work);
    return report;
}

std::uint32_t component_version(const json::Value& component) noexcept {
    const json::Value* v = component.is_object() ? component.find(kComponentVersionKey) : nullptr;
    if (!v || !v->is_number()) return 1;
    const std::int64_t n = v->as_int(1);
    return n < 1 ? 1u : static_cast<std::uint32_t>(std::min<std::int64_t>(n, 0xFFFFFFFF));
}

} // namespace victus::migration
