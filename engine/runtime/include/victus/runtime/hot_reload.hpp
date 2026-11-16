// SPDX-License-Identifier: Apache-2.0
// Asset hot reload for development runs (`victus run`, the editor): watches the project's
// assets/ folder, re-imports textures and models whose sources (or .meta settings, or glTF
// dependencies) changed, and tells the caller what to refresh. What cannot be swapped safely
// while a game runs is reported instead of reloaded:
//   - textures, models (mesh data and materials)   reloaded in place (GpuAssetCache::evict)
//   - audio clips                                  reported in `audio`; AudioEngine::reload_clip
//   - scripts (.luau)                              ignored here; ScriptHost reloads them itself
//   - prefabs (.vprefab)                           reported in `prefabs`; new instances use the
//                                                  new version, the editor updates existing ones
//   - materials (.vmat)                            reported in `materials`; apply() drops cached
//                                                  materials so the next frame uses the new file
//   - scenes, fonts, other files                   `restart_needed`: picked up on the next start
//   - shaders and engine code                      built into the engine; never hot-reloaded
// A failed re-import keeps the previous cooked data (and GPU resources) in use and is reported
// in `failed`, so a half-saved file never breaks the running game.
#pragma once
#include "victus/animation/animation.hpp"
#include "victus/assets/database.hpp"
#include "victus/runtime/scene_rendering.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace victus::runtime {

struct HotReloadReport {
    std::vector<std::string> changed;        // changed/added/removed files (relative to assets/)
    std::vector<Uuid> reimported;            // assets re-imported successfully
    std::vector<std::string> reimported_paths;
    std::vector<std::string> failed;         // "path: error" (previous version kept)
    std::vector<std::string> audio;          // audio clips to reload (paths relative to assets/)
    std::vector<std::string> prefabs;        // changed prefabs (paths relative to assets/)
    std::vector<std::string> materials;      // changed materials (paths relative to assets/)
    std::vector<std::string> restart_needed; // files whose changes appear on the next start
    [[nodiscard]] bool empty() const noexcept { return changed.empty(); }
    // One human-readable line per event, for logs and editor notifications.
    [[nodiscard]] std::vector<std::string> lines() const;
};

class AssetHotReloader {
public:
    // Takes a baseline snapshot of assets/ immediately. Packaged databases never report changes.
    explicit AssetHotReloader(assets::AssetDatabase& db, double interval_seconds = 0.5);

    // Scans at most every `interval_seconds` (pass a monotonic clock); see check().
    [[nodiscard]] HotReloadReport poll(double now_seconds);
    // Scans now: compares file sizes and modification times with the last scan; when anything
    // changed, refreshes metadata and re-imports every stale texture/model.
    [[nodiscard]] HotReloadReport check();

    // Evicts re-imported assets from the GPU cache and animation caches (either may be null).
    static void apply(const HotReloadReport& report, GpuAssetCache* cache, animation::AnimationSystem* animations);

    [[nodiscard]] std::uint64_t scans() const noexcept { return scans_; }

private:
    struct Stamp {
        std::int64_t time{0};
        std::uintmax_t size{0};
        friend bool operator==(const Stamp&, const Stamp&) = default;
    };
    [[nodiscard]] std::unordered_map<std::string, Stamp> snapshot() const;

    assets::AssetDatabase& db_;
    double interval_;
    double next_{0};
    std::uint64_t scans_{0};
    std::unordered_map<std::string, Stamp> stamps_;
};

} // namespace victus::runtime
