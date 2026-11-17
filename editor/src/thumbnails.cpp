// SPDX-License-Identifier: Apache-2.0
#include "victus/editor/thumbnails.hpp"

#include "victus/core/log.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/scene/prefab.hpp"

#include <algorithm>
#include <cmath>

namespace victus::editor {

using assets::AssetKind;
using scene::Entity;
using scene::MeshRendererComponent;
using scene::Scene;
using scene::SpriteComponent;
using scene::WorldTransform;

namespace {

math::Quat look_rotation(math::Vec3 dir) {
    dir = math::normalize(dir);
    const float yaw = std::atan2(-dir.x, -dir.z);
    const float pitch = std::asin(std::clamp(dir.y, -1.0f, 1.0f));
    return math::Quat::euler(pitch, yaw, 0.0f);
}

// Neutral studio look: soft grey sky, darker ground, key light from the upper left.
void add_studio(Scene& s) {
    const Entity sun = s.create("Thumbnail Key Light");
    s.world().get<scene::Transform>(sun)->rotation = look_rotation({-0.25f, -0.8f, -0.55f});
    auto& l = s.world().add<scene::LightComponent>(sun);
    l.type = scene::LightComponent::Type::Directional;
    l.color = {1.0f, 0.97f, 0.92f, 1};
    l.intensity = 3.0f;
    l.shadows = false;
    auto& env = s.world().add<gameplay::EnvironmentComponent>(s.create("Thumbnail Environment"));
    env.sky_zenith = {0.32f, 0.34f, 0.38f, 1};
    env.sky_horizon = {0.5f, 0.52f, 0.56f, 1};
    env.ground = {0.16f, 0.16f, 0.17f, 1};
    env.ambient_intensity = 0.8f;
    env.bloom_intensity = 0.0f;
    env.vignette = 0.0f;
    env.ao_intensity = 0.0f;
    env.auto_exposure = false;
}

// World bounds of everything drawable in the preview scene. `sprites_only` reports whether no
// mesh was found but sprites were (the preview then renders in 2D).
math::Aabb scene_bounds(Scene& s, runtime::GpuAssetCache& cache, bool& any, bool& sprites_only) {
    math::Aabb box;
    bool meshes = false, sprites = false;
    s.world().each<MeshRendererComponent, WorldTransform>([&](Entity, const MeshRendererComponent& mr, const WorldTransform& wt) {
        if (!mr.visible) return;
        math::Aabb local{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
        if (!mr.mesh_asset.is_nil()) {
            auto m = cache.model(mr.mesh_asset);
            if (!m || mr.mesh_index >= (*m)->meshes.size()) return;
            local = (*m)->meshes[mr.mesh_index].bounds;
        } else if (const auto* g = cache.primitive(mr.primitive)) {
            local = g->bounds;
        }
        const math::Aabb w = local.transformed(wt.matrix);
        box.expand(w.min);
        box.expand(w.max);
        meshes = true;
    });
    if (!meshes) {
        s.world().each<SpriteComponent, WorldTransform>([&](Entity, const SpriteComponent& sp, const WorldTransform& wt) {
            const math::Aabb local{{-sp.pivot.x * sp.size.x, -sp.pivot.y * sp.size.y, 0.0f},
                                   {(1 - sp.pivot.x) * sp.size.x, (1 - sp.pivot.y) * sp.size.y, 0.0f}};
            const math::Aabb w = local.transformed(wt.matrix);
            box.expand(w.min);
            box.expand(w.max);
            sprites = true;
        });
    }
    any = meshes || sprites;
    sprites_only = !meshes && sprites;
    return box;
}

} // namespace

render::Camera3D thumbnail_camera(const math::Aabb& bounds, float fov_y, bool sphere) {
    const math::Vec3 c = bounds.valid() ? bounds.center() : math::Vec3{};
    math::Vec3 ext = bounds.valid() ? bounds.extents() : math::Vec3{0.5f, 0.5f, 0.5f};
    ext = math::max(ext, math::Vec3{1e-4f, 1e-4f, 1e-4f});
    const float yaw = math::radians(35.0f), pitch = math::radians(-25.0f);
    // Unit vector from the target towards the eye, and the camera basis.
    const math::Vec3 back{std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    const math::Vec3 right = math::normalize(math::cross(math::Vec3{0, 1, 0}, back));
    const math::Vec3 up = math::cross(back, right);
    // Closest distance at which every corner fits the (square) frustum with a small margin:
    // a corner at lateral offset `a` and depth offset `z` towards the eye needs a/(d - z) <= t.
    const float t = std::tan(fov_y * 0.5f) * 0.92f;
    float dist = 0.0f, nearest = 0.0f;
    for (int i = 0; i < 8; ++i) {
        const math::Vec3 o{i & 1 ? ext.x : -ext.x, i & 2 ? ext.y : -ext.y, i & 4 ? ext.z : -ext.z};
        const float z = math::dot(o, back);
        dist = std::max({dist, std::abs(math::dot(o, right)) / t + z, std::abs(math::dot(o, up)) / t + z});
        nearest = std::max(nearest, z);
    }
    const float bound = math::length(ext);
    dist = std::max(dist, nearest + bound * 0.05f);
    // A sphere inscribed in the box needs less room: its silhouette half-angle is asin(r/d).
    if (sphere) dist = std::min(ext.x, std::min(ext.y, ext.z)) / std::sin(std::atan(t));
    render::Camera3D cam = render::Camera3D::look_at(c + back * dist, c);
    cam.fov_y = fov_y;
    cam.z_near = std::max((dist - bound) * 0.5f, dist * 0.01f);
    cam.z_far = dist + bound * 2.0f;
    return cam;
}

Result<std::unique_ptr<ThumbnailCache>> ThumbnailCache::create(render::GpuContext& ctx, std::uint32_t size) {
    std::unique_ptr<ThumbnailCache> t(new ThumbnailCache());
    t->gpu_ = &ctx;
    t->size_ = std::clamp(size, 16u, 1024u);
    auto view = runtime::SceneView::create(ctx, rhi::Format::RGBA8_UNORM);
    if (!view) return std::move(view).error();
    t->view_ = std::move(view).value();
    // One-shot renders: no temporal effects (TAA, eye adaptation) that need several frames to
    // converge, and no screen-space effects that are noise at thumbnail size.
    render::RenderSettings rs = render::RenderSettings::preset(render::QualityTier::Medium);
    rs.taa = false;
    rs.auto_exposure = false;
    rs.ssao = false;
    rs.bloom = false;
    rs.fxaa = true;
    rs.shadow_cascades = 1;
    rs.shadow_resolution = 512;
    t->view_->renderer().set_settings(rs);
    return t;
}

ThumbnailCache::~ThumbnailCache() { clear(); }

bool ThumbnailCache::supports(AssetKind kind) noexcept {
    switch (kind) {
    case AssetKind::Texture:
    case AssetKind::SpriteSheet:
    case AssetKind::Tileset:
    case AssetKind::Model:
    case AssetKind::Material:
    case AssetKind::Prefab: return true;
    default: return false;
    }
}

bool ThumbnailCache::renders(AssetKind kind) noexcept {
    return kind == AssetKind::Model || kind == AssetKind::Material || kind == AssetKind::Prefab;
}

void ThumbnailCache::release(Entry& e) {
    if (e.target && gpu_) gpu_->device().destroy(e.target);
    e.target = {};
}

void ThumbnailCache::invalidate(const Uuid& uuid) {
    auto it = entries_.find(uuid);
    if (it == entries_.end()) return;
    release(it->second);
    entries_.erase(it);
    std::erase(queue_, uuid);
}

void ThumbnailCache::invalidate_kind(AssetKind kind) {
    std::vector<Uuid> drop;
    for (const auto& [id, e] : entries_)
        if (e.kind == kind) drop.push_back(id);
    for (const auto& id : drop) invalidate(id);
}

void ThumbnailCache::clear() {
    for (auto& [id, e] : entries_) release(e);
    entries_.clear();
    queue_.clear();
}

std::optional<Thumbnail> ThumbnailCache::get(const assets::AssetRecord& rec, runtime::GpuAssetCache& cache, assets::AssetDatabase& db) {
    if (!supports(rec.kind)) return std::nullopt;
    if (!renders(rec.kind)) {
        // Shown straight from the asset cache (which owns the GPU texture).
        Uuid tex = rec.uuid;
        math::Vec4 uv{0, 0, 1, 1};
        if (rec.kind == AssetKind::SpriteSheet) {
            auto sheet = db.load_sprite_sheet(rec.uuid);
            if (!sheet) return std::nullopt;
            tex = sheet->texture;
            uv = sheet->frame_uv(0);
        } else if (rec.kind == AssetKind::Tileset) {
            auto ts = db.load_tileset(rec.uuid);
            if (!ts) return std::nullopt;
            tex = ts->texture;
        }
        auto t = cache.texture(tex);
        if (!t) return std::nullopt;
        Thumbnail out;
        out.texture = *t;
        out.uv = uv;
        if (const rhi::TextureDesc* d = gpu_->device().describe(*t); d && d->height > 0) {
            const float w = static_cast<float>(d->width) * std::abs(uv.z - uv.x);
            const float h = static_cast<float>(d->height) * std::abs(uv.w - uv.y);
            out.aspect = h > 0 ? w / h : 1.0f;
        }
        return out;
    }
    auto it = entries_.find(rec.uuid);
    if (it == entries_.end()) {
        Entry e;
        e.kind = rec.kind;
        entries_.emplace(rec.uuid, e);
        queue_.push_back(rec.uuid);
        return std::nullopt;
    }
    if (it->second.state != State::Ready) return std::nullopt;
    Thumbnail out;
    out.texture = it->second.target;
    out.rendered = true;
    return out;
}

Result<std::uint32_t> ThumbnailCache::render_pending(rhi::CommandBuffer& cmd, runtime::GpuAssetCache& cache, assets::AssetDatabase& db,
                                                     std::uint32_t budget) {
    std::uint32_t done = 0;
    while (done < budget && !queue_.empty()) {
        const Uuid id = queue_.front();
        queue_.erase(queue_.begin());
        auto it = entries_.find(id);
        if (it == entries_.end()) continue;
        Entry& e = it->second;
        if (auto st = render_one(cmd, cache, db, id, e); !st) {
            VICTUS_LOG_WARN("editor", "thumbnail for {} failed: {}", id.to_string(), st.error().message);
            release(e);
            e.state = State::Failed;
        } else {
            e.state = State::Ready;
            ++rendered_total_;
        }
        ++done;
    }
    return done;
}

Status ThumbnailCache::render_one(rhi::CommandBuffer& cmd, runtime::GpuAssetCache& cache, assets::AssetDatabase& db, const Uuid& uuid,
                                  Entry& entry) {
    const assets::AssetRecord* rec = db.find(uuid);
    if (!rec) return make_error(ErrorCode::NotFound, "asset no longer exists");
    Scene s;
    switch (rec->kind) {
    case AssetKind::Model: {
        auto model = cache.model(uuid);
        if (!model) return std::move(model).error();
        (void)runtime::instantiate_model(s, uuid, (*model)->cpu, "Model");
        break;
    }
    case AssetKind::Material: {
        auto mat = cache.material(uuid);
        if (!mat) return std::move(mat).error();
        const Entity e = s.create("Material Sphere");
        auto& mr = s.world().add<MeshRendererComponent>(e);
        mr.primitive = "sphere";
        mr.material_asset = uuid;
        break;
    }
    case AssetKind::Prefab: {
        auto prefab = scene::load_prefab_file(db.source_path(*rec));
        if (!prefab) return std::move(prefab).error();
        auto root = scene::instantiate(s, *prefab, uuid, {});
        if (!root) return std::move(root).error();
        break;
    }
    default: return make_error(ErrorCode::InvalidArgument, "asset kind has no rendered thumbnail");
    }
    s.update_transforms();
    bool any = false, sprites_only = false;
    const math::Aabb box = scene_bounds(s, cache, any, sprites_only);
    if (!any) return make_error(ErrorCode::InvalidArgument, "nothing visible to preview");
    add_studio(s);
    s.update_transforms();

    if (!entry.target) {
        rhi::TextureDesc d;
        d.name = "editor_thumbnail";
        d.format = rhi::Format::RGBA8_UNORM;
        d.width = size_;
        d.height = size_;
        d.usage = rhi::TextureUsageSampled | rhi::TextureUsageColorTarget;
        auto t = gpu_->device().create_texture(d);
        if (!t) return std::move(t).error();
        entry.target = *t;
    }

    runtime::SceneViewOptions opt;
    opt.draw_ui = false;
    const render::Camera3D cam3 = thumbnail_camera(box, math::radians(40.0f), rec->kind == AssetKind::Material);
    render::Camera2D cam2;
    if (sprites_only) {
        opt.mode = runtime::ViewMode::TwoD;
        const math::Vec3 c = box.center();
        const float extent = std::max({box.max.x - box.min.x, box.max.y - box.min.y, 1e-3f});
        cam2.position = {c.x, c.y};
        cam2.pixels_per_unit = 1.0f;
        cam2.zoom = static_cast<float>(size_) / extent * 0.9f;
        opt.camera_2d = &cam2;
        opt.clear_2d = {0.2f, 0.21f, 0.23f, 1};
    } else {
        opt.mode = runtime::ViewMode::ThreeD;
        opt.camera_3d = &cam3;
        opt.settings.fallback_camera = cam3;
    }
    view_->renderer().reset_adaptation();
    view_->renderer().reset_history();
    // Reflection probe captures in the cache are keyed by entity and belong to the edited scene;
    // the preview neither needs nor may disturb them.
    const bool was_capturing = cache.capturing_probes();
    cache.set_capturing_probes(true);
    auto stats = view_->render(cmd, s, cache, entry.target, size_, size_, opt);
    cache.set_capturing_probes(was_capturing);
    if (!stats) return std::move(stats).error();
    return {};
}

} // namespace victus::editor
