// SPDX-License-Identifier: Apache-2.0
#include "victus/gameplay/save.hpp"

#include "victus/core/fs.hpp"
#include "victus/core/hash.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <system_error>

namespace victus::gameplay {
namespace {

constexpr std::string_view kFormat = "victus.save";

std::string checksum(const json::Value& data) {
    return std::format("{:016x}", fnv1a64(std::string_view(json::stringify(data, false))));
}

Error bad(std::string what) { return make_error(ErrorCode::ParseError, std::move(what)); }

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Result<SaveFile> read_file(const std::filesystem::path& path) {
    auto text = fs::read_text(path, 256ull << 20);
    if (!text) return std::move(text).error();
    auto file = parse_save_document(*text);
    if (!file) return bad(std::format("{}: {}", path.filename().string(), file.error().message));
    return file;
}

} // namespace

bool valid_slot_name(std::string_view slot) noexcept {
    if (slot.empty() || slot.size() > 64 || slot.front() == '.') return false;
    return std::all_of(slot.begin(), slot.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
               c == '.';
    });
}

json::Value make_save_document(const json::Value& data, std::uint32_t data_version, std::int64_t saved_at,
                               std::string_view label) {
    json::Value doc = json::Value::object();
    doc.set("format", kFormat);
    doc.set("version", kSaveFormatVersion);
    doc.set("data_version", data_version);
    doc.set("saved_at", saved_at);
    if (!label.empty()) doc.set("label", label);
    doc.set("checksum", checksum(data));
    doc.set("data", data);
    return doc;
}

Result<SaveFile> parse_save_document(std::string_view text) {
    auto doc = json::parse(text);
    if (!doc) return bad(doc.error().message);
    if (!doc->is_object() || (*doc)["format"].as_string_or("") != kFormat) return bad("not a VICTUS save file");
    const auto version = (*doc)["version"].as_int(0);
    if (version < 1) return bad("missing save format version");
    if (version > kSaveFormatVersion)
        return make_error(ErrorCode::VersionMismatch,
                          std::format("save format {} is newer than this engine ({})", version, kSaveFormatVersion));
    const json::Value* data = doc->find("data");
    if (!data) return bad("missing data");
    if ((*doc)["checksum"].as_string_or("") != checksum(*data)) return bad("checksum mismatch (file damaged)");
    SaveFile f;
    f.data = *data;
    f.data_version = static_cast<std::uint32_t>(std::max<std::int64_t>(1, (*doc)["data_version"].as_int(1)));
    f.saved_at = (*doc)["saved_at"].as_int(0);
    f.label = (*doc)["label"].as_string_or("");
    return f;
}

SaveStore::SaveStore(std::filesystem::path directory, std::uint32_t data_version)
    : dir_(std::move(directory)), data_version_(data_version < 1 ? 1 : data_version) {}

void SaveStore::add_migration(std::uint32_t from, SaveMigration migration) { migrations_[from] = std::move(migration); }

std::filesystem::path SaveStore::slot_path(std::string_view slot) const {
    return dir_ / (std::string(slot) + std::string(kSaveExtension));
}

Status SaveStore::write(std::string_view slot, const json::Value& data, std::string_view label) {
    if (!valid_slot_name(slot))
        return make_error(ErrorCode::InvalidArgument, std::format("invalid save slot name '{}'", slot));
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) return make_error(ErrorCode::IoError, std::format("cannot create {}: {}", dir_.string(), ec.message()));
    const auto path = slot_path(slot);
    auto backup = path;
    backup += ".bak";
    // Keep the previous file as the backup, but only when it is itself a good save: a damaged
    // main file must never replace the last good backup.
    if (std::filesystem::exists(path, ec) && read_file(path).ok()) {
        std::filesystem::copy_file(path, backup, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) return make_error(ErrorCode::IoError, std::format("cannot back up {}: {}", path.string(), ec.message()));
    }
    return fs::write_text_atomic(path, json::stringify(make_save_document(data, data_version_, now_seconds(), label)) + "\n");
}

Result<SaveFile> SaveStore::migrate(SaveFile file) const {
    if (file.data_version > data_version_)
        return make_error(ErrorCode::VersionMismatch,
                          std::format("save data version {} is newer than the game ({})", file.data_version, data_version_));
    for (std::uint32_t v = file.data_version; v < data_version_; ++v) {
        const auto it = migrations_.find(v);
        if (it == migrations_.end())
            return make_error(ErrorCode::VersionMismatch,
                              std::format("no migration from save data version {} to {}", v, v + 1));
        auto next = it->second(std::move(file.data));
        if (!next) return make_error(next.error().code, std::format("migration {} -> {}: {}", v, v + 1, next.error().message));
        file.data = std::move(next).value();
        file.migrated = true;
    }
    return file;
}

Result<SaveFile> SaveStore::read(std::string_view slot, bool run_migrations) const {
    auto finish = [&](SaveFile f) -> Result<SaveFile> { return run_migrations ? migrate(std::move(f)) : Result<SaveFile>(std::move(f)); };
    if (!valid_slot_name(slot))
        return make_error(ErrorCode::InvalidArgument, std::format("invalid save slot name '{}'", slot));
    const auto path = slot_path(slot);
    auto backup = path;
    backup += ".bak";
    std::error_code ec;
    const bool has_main = std::filesystem::exists(path, ec);
    if (has_main) {
        auto main = read_file(path);
        if (main) return finish(std::move(main).value());
        if (main.error().code == ErrorCode::VersionMismatch) return std::move(main).error();
        auto old = read_file(backup);
        if (!old) return std::move(main).error();
        old->recovered = true;
        return finish(std::move(old).value());
    }
    if (std::filesystem::exists(backup, ec)) {
        auto old = read_file(backup);
        if (!old) return std::move(old).error();
        old->recovered = true;
        return finish(std::move(old).value());
    }
    return make_error(ErrorCode::NotFound, std::format("no save in slot '{}'", slot));
}

bool SaveStore::exists(std::string_view slot) const {
    if (!valid_slot_name(slot)) return false;
    std::error_code ec;
    auto backup = slot_path(slot);
    backup += ".bak";
    return std::filesystem::exists(slot_path(slot), ec) || std::filesystem::exists(backup, ec);
}

Status SaveStore::remove(std::string_view slot) {
    if (!valid_slot_name(slot))
        return make_error(ErrorCode::InvalidArgument, std::format("invalid save slot name '{}'", slot));
    std::error_code ec;
    auto backup = slot_path(slot);
    backup += ".bak";
    std::filesystem::remove(slot_path(slot), ec);
    if (ec) return make_error(ErrorCode::IoError, ec.message());
    std::filesystem::remove(backup, ec);
    if (ec) return make_error(ErrorCode::IoError, ec.message());
    return {};
}

std::vector<SlotInfo> SaveStore::list() const {
    std::vector<SlotInfo> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir_, ec)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const auto name = entry.path().filename().string();
        std::string slot;
        if (name.ends_with(kSaveExtension)) {
            slot = name.substr(0, name.size() - kSaveExtension.size());
        } else if (name.ends_with(std::string(kSaveExtension) + ".bak")) {
            slot = name.substr(0, name.size() - kSaveExtension.size() - 4);
            if (std::filesystem::exists(slot_path(slot), ec)) continue; // listed via the main file
        } else {
            continue;
        }
        if (!valid_slot_name(slot)) continue;
        SlotInfo info;
        info.slot = slot;
        info.bytes = entry.file_size(ec);
        auto file = read_file(entry.path());
        if (file && !name.ends_with(".bak")) {
            info.label = file->label;
            info.saved_at = file->saved_at;
            info.data_version = file->data_version;
        } else {
            info.damaged = true;
            if (file) { // only the backup is left
                info.label = file->label;
                info.saved_at = file->saved_at;
                info.data_version = file->data_version;
            }
        }
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const SlotInfo& a, const SlotInfo& b) {
        return a.saved_at != b.saved_at ? a.saved_at > b.saved_at : a.slot < b.slot;
    });
    return out;
}

std::filesystem::path default_save_directory(std::string_view game_name) {
    std::string safe;
    for (const char c : game_name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        safe += ok ? c : '_';
    }
    if (safe.empty()) safe = "game";
    return fs::user_data_dir() / "saves" / safe;
}

} // namespace victus::gameplay
