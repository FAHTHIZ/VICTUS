// SPDX-License-Identifier: Apache-2.0
#include "victus/runtime/hot_reload.hpp"

#include "victus/core/log.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>

namespace victus::runtime {
namespace sfs = std::filesystem;

namespace {
std::string lower_extension(const std::string& path) {
    std::string ext = sfs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}
} // namespace

std::vector<std::string> HotReloadReport::lines() const {
    std::vector<std::string> out;
    for (const auto& p : reimported_paths) out.push_back("reloaded " + p);
    for (const auto& f : failed) out.push_back("reload failed, keeping the previous version: " + f);
    for (const auto& a : audio) out.push_back("reloaded audio " + a);
    for (const auto& p : prefabs) out.push_back("reloaded prefab " + p);
    for (const auto& m : materials) out.push_back("reloaded material " + m);
    for (const auto& r : restart_needed) out.push_back(r + " changed; restart to see it");
    return out;
}

AssetHotReloader::AssetHotReloader(assets::AssetDatabase& db, double interval_seconds)
    : db_(db), interval_(interval_seconds) {
    if (!db_.packaged()) stamps_ = snapshot();
}

std::unordered_map<std::string, AssetHotReloader::Stamp> AssetHotReloader::snapshot() const {
    std::unordered_map<std::string, Stamp> out;
    const sfs::path root = db_.assets_dir();
    std::error_code ec;
    for (auto it = sfs::recursive_directory_iterator(root, sfs::directory_options::skip_permission_denied, ec);
         !ec && it != sfs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code fe;
        if (!it->is_regular_file(fe)) continue;
        Stamp s;
        s.size = it->file_size(fe);
        if (fe) continue;
        s.time = static_cast<std::int64_t>(it->last_write_time(fe).time_since_epoch().count());
        if (fe) continue;
        out.emplace(sfs::relative(it->path(), root, fe).generic_string(), s);
    }
    return out;
}

HotReloadReport AssetHotReloader::poll(double now) {
    if (now < next_) return {};
    next_ = now + interval_;
    return check();
}

HotReloadReport AssetHotReloader::check() {
    HotReloadReport report;
    if (db_.packaged()) return report;
    ++scans_;
    auto now = snapshot();
    std::vector<std::string> changed;
    for (const auto& [path, stamp] : now) {
        const auto it = stamps_.find(path);
        if (it == stamps_.end() || !(it->second == stamp)) changed.push_back(path);
    }
    for (const auto& [path, stamp] : stamps_)
        if (!now.contains(path)) changed.push_back(path);
    stamps_ = std::move(now);
    // Scripts are reloaded by the script host; they are not asset-database work.
    std::erase_if(changed, [](const std::string& p) { return lower_extension(p) == ".luau"; });
    if (changed.empty()) return report;
    std::sort(changed.begin(), changed.end());
    report.changed = changed;

    const auto refresh = db_.refresh();
    for (const auto& e : refresh.errors) report.failed.push_back(e);
    for (const auto& rec : db_.records()) {
        if (rec.importer != "texture" && rec.importer != "gltf") continue;
        auto fresh = db_.is_up_to_date(rec.uuid);
        if (fresh && *fresh) continue;
        std::vector<std::string> messages;
        auto imported = db_.import(rec.uuid, &messages);
        if (!imported) {
            report.failed.push_back(imported.error().message);
            continue;
        }
        if (*imported) {
            report.reimported.push_back(rec.uuid);
            report.reimported_paths.push_back(rec.path);
        }
    }
    for (const auto& path : changed) {
        const std::string ext = lower_extension(path);
        if (ext == ".meta") continue;
        const assets::AssetKind kind = assets::kind_for_extension(ext);
        if (kind == assets::AssetKind::Audio) {
            report.audio.push_back(path);
        } else if (kind == assets::AssetKind::Prefab) {
            report.prefabs.push_back(path);
        } else if (kind == assets::AssetKind::Material) {
            report.materials.push_back(path);
        } else if (kind == assets::AssetKind::Texture || kind == assets::AssetKind::Model) {
            continue; // handled above (or a dependency of a model that was re-imported)
        } else if (ext == ".bin") {
            continue; // glTF buffers: their model re-imports through its dependency hashes
        } else {
            report.restart_needed.push_back(path);
        }
    }
    for (const auto& line : report.lines()) VICTUS_LOG_INFO("hotreload", "{}", line);
    return report;
}

void AssetHotReloader::apply(const HotReloadReport& report, GpuAssetCache* cache, animation::AnimationSystem* animations) {
    if (cache && !report.materials.empty()) cache->evict_materials();
    for (const auto& uuid : report.reimported) {
        if (cache) cache->evict(uuid);
        if (animations) animations->invalidate(uuid);
    }
}

} // namespace victus::runtime
