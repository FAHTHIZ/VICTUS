// SPDX-License-Identifier: Apache-2.0
// VICTUS localization: per-locale string tables for UI text and scripts.
// - A table is `assets/localization/<locale>.vstrings`, JSON:
//     { "name": "Français", "strings": { "menu": { "start": "Commencer" }, "hud.score": "Score : {score}" } }
//   Nested objects flatten to dotted keys ("menu.start"). The file name (without extension) is the
//   locale code, e.g. "en", "fr", "pt-BR".
// - Lookups try the current locale, then the fallback locale (the project's "default_locale").
// - `{name}` in a string is a placeholder filled by format_text (scripts: loc.text(key, params));
//   `{{` and `}}` are literal braces.
// - Text is UTF-8. The runtime shapes and lays it out with victus::text (HarfBuzz, Unicode bidi;
//   ADR-0224), so Bengali, Arabic and other complex or right-to-left scripts render correctly when
//   a font covers them (built in: Latin, Cyrillic, Greek, Bengali, Arabic).
#pragma once
#include "victus/core/fs.hpp"
#include "victus/core/result.hpp"
#include "victus/scene/scene.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace victus::gameui {

inline constexpr std::string_view kStringTableExtension = ".vstrings";
inline constexpr std::string_view kLocalizationFolder = "localization"; // under assets/

class StringTable {
public:
    // Parses a .vstrings document. Errors name the offending key.
    [[nodiscard]] static Result<StringTable> parse(std::string_view text);

    [[nodiscard]] const std::string* find(std::string_view key) const;
    [[nodiscard]] std::size_t size() const noexcept { return strings_.size(); }
    [[nodiscard]] const std::string& display_name() const noexcept { return name_; }
    void set(std::string key, std::string value) { strings_[std::move(key)] = std::move(value); }

private:
    std::string name_;
    std::unordered_map<std::string, std::string> strings_;
};

// Replaces `{name}` placeholders with param(name); placeholders param() returns nullopt for are
// kept verbatim, so a missing parameter is visible instead of silently dropped.
[[nodiscard]] std::string format_text(std::string_view pattern,
                                      const std::function<std::optional<std::string>(std::string_view)>& param);

class Localization {
public:
    explicit Localization(std::string fallback_locale = "en");

    // Loads every <assets_dir>/localization/*.vstrings (loose files or a mounted pack). Unreadable
    // or invalid files are skipped and listed in errors(). The current locale starts at the
    // fallback when it has a table, else at the first locale in name order.
    [[nodiscard]] static std::shared_ptr<Localization> load(const fs::Path& assets_dir, std::string fallback_locale);

    void add(std::string locale, StringTable table);
    // False (and no change) when there is no table for `locale`.
    bool set_locale(std::string_view locale);
    [[nodiscard]] const std::string& locale() const noexcept { return current_; }
    [[nodiscard]] const std::string& fallback_locale() const noexcept { return fallback_; }
    [[nodiscard]] std::vector<std::string> locales() const;
    [[nodiscard]] const StringTable* table(std::string_view locale) const;
    // Current locale, then the fallback; nullptr when neither has the key.
    [[nodiscard]] const std::string* find(std::string_view key) const;
    // Increases whenever the current locale or a table changes.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] const std::vector<std::string>& errors() const noexcept { return errors_; }

private:
    std::string fallback_;
    std::string current_;
    std::map<std::string, StringTable, std::less<>> tables_;
    std::vector<std::string> errors_;
    std::uint64_t revision_{0};
};

// Makes `loc` the scene's localization: UiText components with a `key` show its translation.
// Hosts (the script host in the player, editor play mode and `victus simulate`) bind it to every
// scene they run. Passing nullptr unbinds.
void bind_localization(scene::Scene& scene, std::shared_ptr<Localization> loc);
[[nodiscard]] Localization* localization_of(scene::Scene& scene);

} // namespace victus::gameui
