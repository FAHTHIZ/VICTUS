// SPDX-License-Identifier: Apache-2.0
// Asset thumbnails for the editor's Assets panel.
//
// Textures, sprite sheets and tile sets show their (already uploaded) GPU texture directly,
// sprite sheets cropped to their first frame. Models, materials and prefabs are rendered once
// into a small offscreen colour target: a private preview scene (the model's node tree, a
// sphere wearing the material, or an instance of the prefab) framed from its bounds, lit by a
// key light and a neutral studio environment. Rendering is spread over frames by a budget, and
// a thumbnail is re-rendered only after invalidate() (re-import, hot reload, material save).
#pragma once
#include "victus/assets/database.hpp"
#include "victus/core/math.hpp"
#include "victus/runtime/scene_view.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace victus::editor {

struct Thumbnail {
    rhi::TextureHandle texture;
    math::Vec4 uv{0, 0, 1, 1}; // u0, v0 (top), u1, v1 (bottom)
    float aspect{1.0f};        // width / height of the shown region
    bool rendered{false};      // true: an offscreen preview render (model, material, prefab)
};

class ThumbnailCache {
public:
    // `size` is the edge length of rendered previews in pixels (clamped to 16..1024).
    [[nodiscard]] static Result<std::unique_ptr<ThumbnailCache>> create(render::GpuContext& ctx, std::uint32_t size = 128);
    ~ThumbnailCache();
    ThumbnailCache(const ThumbnailCache&) = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    // Kinds that get a picture (the rest keep their text icon).
    [[nodiscard]] static bool supports(assets::AssetKind kind) noexcept;
    [[nodiscard]] static bool renders(assets::AssetKind kind) noexcept; // needs an offscreen render

    // The thumbnail of an asset, or nullopt when it has none, is still queued, or failed.
    // Rendered kinds that were never requested are queued for render_pending().
    [[nodiscard]] std::optional<Thumbnail> get(const assets::AssetRecord& record, runtime::GpuAssetCache& cache,
                                               assets::AssetDatabase& db);
    // Renders up to `budget` queued previews (oldest request first) into their targets. Must be
    // recorded outside any render pass. Returns the number rendered this call.
    [[nodiscard]] Result<std::uint32_t> render_pending(rhi::CommandBuffer& cmd, runtime::GpuAssetCache& cache,
                                                       assets::AssetDatabase& db, std::uint32_t budget = 2);

    // Forget one asset's thumbnail (it re-renders / reloads on the next get()).
    void invalidate(const Uuid& uuid);
    // Forget every thumbnail of a kind (e.g. all materials after a shader changed).
    void invalidate_kind(assets::AssetKind kind);
    void clear();

    [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t pending() const noexcept { return queue_.size(); }
    [[nodiscard]] std::uint64_t rendered_total() const noexcept { return rendered_total_; }
    [[nodiscard]] std::size_t entries() const noexcept { return entries_.size(); }

private:
    ThumbnailCache() = default;
    enum class State : std::uint8_t { Queued, Ready, Failed };
    struct Entry {
        State state{State::Queued};
        assets::AssetKind kind{assets::AssetKind::Unknown};
        rhi::TextureHandle target; // owned (rendered kinds only)
    };
    [[nodiscard]] Status render_one(rhi::CommandBuffer& cmd, runtime::GpuAssetCache& cache, assets::AssetDatabase& db,
                                    const Uuid& uuid, Entry& entry);
    void release(Entry& entry);

    render::GpuContext* gpu_{nullptr};
    std::uint32_t size_{128};
    std::unique_ptr<runtime::SceneView> view_;
    std::unordered_map<Uuid, Entry, UuidHash> entries_;
    std::vector<Uuid> queue_;
    std::uint64_t rendered_total_{0};
};

// Camera that frames a world-space box from a three-quarter view (yaw 35 deg, pitch -25 deg)
// so the whole box fits a square viewport with field of view `fov_y` (radians). `sphere`
// frames the sphere inscribed in the box instead (tighter; material previews).
[[nodiscard]] render::Camera3D thumbnail_camera(const math::Aabb& bounds, float fov_y = math::radians(40.0f), bool sphere = false);

} // namespace victus::editor
