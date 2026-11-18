// SPDX-License-Identifier: Apache-2.0
#include "victus/gameui/localization.hpp"

#include "victus/core/json.hpp"

#include <algorithm>
#include <format>

namespace victus::gameui {

namespace {

constexpr std::size_t kMaxDepth = 16;
constexpr std::uint64_t kMaxTableBytes = 16ull << 20;

Status flatten(const json::Value& node, const std::string& prefix, std::size_t depth, StringTable& out) {
    if (depth > kMaxDepth) return make_error(ErrorCode::LimitExceeded, std::format("'{}' is nested too deeply", prefix));
    for (const auto& [key, value] : node.as_object()) {
        if (key.empty()) return make_error(ErrorCode::ParseError, std::format("empty key under '{}'", prefix));
        std::string full = prefix.empty() ? key : prefix + "." + key;
        if (value.is_string()) {
            out.set(std::move(full), value.as_string());
        } else if (value.is_object()) {
            VICTUS_TRY_STATUS(flatten(value, full, depth + 1, out));
        } else {
            return make_error(ErrorCode::ParseError, std::format("'{}' must be a string or an object of strings", full));
        }
    }
    return {};
}

struct LocalizationBinding {
    std::shared_ptr<Localization> loc;
};

} // namespace

Result<StringTable> StringTable::parse(std::string_view text) {
    auto doc = json::parse(text);
    if (!doc) return std::move(doc).error();
    if (!doc->is_object()) return make_error(ErrorCode::ParseError, "a string table must be a JSON object");
    StringTable t;
    if (const json::Value* name = doc->find("name")) {
        if (!name->is_string()) return make_error(ErrorCode::ParseError, "'name' must be a string");
        t.name_ = name->as_string();
    }
    const json::Value* strings = doc->find("strings");
    if (!strings || !strings->is_object()) return make_error(ErrorCode::ParseError, "a string table needs a 'strings' object");
    VICTUS_TRY_STATUS(flatten(*strings, {}, 0, t));
    return t;
}

const std::string* StringTable::find(std::string_view key) const {
    const auto it = strings_.find(std::string(key));
    return it == strings_.end() ? nullptr : &it->second;
}

std::string format_text(std::string_view pattern, const std::function<std::optional<std::string>(std::string_view)>& param) {
    std::string out;
    out.reserve(pattern.size());
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if ((c == '{' || c == '}') && i + 1 < pattern.size() && pattern[i + 1] == c) {
            out.push_back(c);
            ++i;
            continue;
        }
        if (c == '{') {
            const std::size_t close = pattern.find('}', i + 1);
            if (close != std::string_view::npos) {
                const std::string_view name = pattern.substr(i + 1, close - i - 1);
                std::optional<std::string> value = param ? param(name) : std::nullopt;
                if (value) {
                    out += *value;
                    i = close;
                    continue;
                }
            }
        }
        out.push_back(c);
    }
    return out;
}

Localization::Localization(std::string fallback_locale) : fallback_(std::move(fallback_locale)), current_(fallback_) {}

std::shared_ptr<Localization> Localization::load(const fs::Path& assets_dir, std::string fallback_locale) {
    auto loc = std::make_shared<Localization>(std::move(fallback_locale));
    const fs::Path dir = assets_dir / std::string(kLocalizationFolder);
    for (const std::string& rel : fs::list_files(dir)) {
        const fs::Path p = dir / rel;
        if (p.extension().string() != kStringTableExtension || rel.find('/') != std::string::npos) continue;
        const std::string code = p.stem().string();
        auto text = fs::read_text(p, kMaxTableBytes);
        if (!text) {
            loc->errors_.push_back(std::format("localization/{}: {}", rel, text.error().message));
            continue;
        }
        auto table = StringTable::parse(*text);
        if (!table) {
            loc->errors_.push_back(std::format("localization/{}: {}", rel, table.error().message));
            continue;
        }
        loc->add(code, std::move(*table));
    }
    if (!loc->tables_.empty() && !loc->tables_.contains(loc->fallback_)) loc->current_ = loc->tables_.begin()->first;
    return loc;
}

void Localization::add(std::string locale, StringTable table) {
    tables_.insert_or_assign(std::move(locale), std::move(table));
    ++revision_;
}

bool Localization::set_locale(std::string_view locale) {
    if (!tables_.contains(locale)) return false;
    if (current_ != locale) {
        current_ = std::string(locale);
        ++revision_;
    }
    return true;
}

std::vector<std::string> Localization::locales() const {
    std::vector<std::string> out;
    out.reserve(tables_.size());
    for (const auto& [code, table] : tables_) out.push_back(code);
    return out;
}

const StringTable* Localization::table(std::string_view locale) const {
    const auto it = tables_.find(locale);
    return it == tables_.end() ? nullptr : &it->second;
}

const std::string* Localization::find(std::string_view key) const {
    if (const StringTable* t = table(current_))
        if (const std::string* s = t->find(key)) return s;
    if (current_ != fallback_)
        if (const StringTable* t = table(fallback_))
            if (const std::string* s = t->find(key)) return s;
    return nullptr;
}

void bind_localization(scene::Scene& scene, std::shared_ptr<Localization> loc) {
    auto* b = scene.world().resource<LocalizationBinding>();
    if (!b) b = &scene.world().set_resource<LocalizationBinding>();
    b->loc = std::move(loc);
}

Localization* localization_of(scene::Scene& scene) {
    auto* b = scene.world().resource<LocalizationBinding>();
    return b ? b->loc.get() : nullptr;
}

} // namespace victus::gameui
