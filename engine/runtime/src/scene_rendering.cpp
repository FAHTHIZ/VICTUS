// SPDX-License-Identifier: Apache-2.0
#include "victus/runtime/scene_rendering.hpp"
#include "victus/render/skinning.hpp"
#include "victus/assets/correctives.hpp"
#include "victus/render/reflection_capture.hpp"
#include "victus/tilemap/tilemap.hpp"
#include "victus/runtime/scene_lightmap.hpp"
#include "victus/runtime/surface_shader.hpp"
#include "victus/terrain/terrain.hpp"
#include "victus/particles/particles.hpp"

#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/gameplay/water.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <chrono>
#include <cmath>
#include <span>
#include <format>
#include <limits>

namespace victus::runtime {


using namespace victus::scene;

GpuAssetCache::~GpuAssetCache() { clear(); }

void GpuAssetCache::clear() {
    if (loader_) loader_->forget_all();
    // The embedded-texture registry stays: its keys are deterministic and late results must be recognised.
    resident_.clear();
    stream_.clear();
    model_hints_.clear();
    material_textures_.clear();
    residency_stats_.resident_bytes = 0;
    for (auto& [id, tex] : textures_) ctx_->device().destroy(tex);
    textures_.clear();
    nearest_.clear();
    environments_.clear();
    environment_errors_.clear();
    cookies_.clear();
    atmosphere_.reset();
    probes_.clear();
    irradiance_.clear();
    probe_renderer_.reset();
    drop_morphed(nullptr);
    drop_crowd(nullptr);
    for (auto& [id, model] : models_) {
        for (auto& m : model->meshes) ctx_->destroy(m);
        for (auto& t : model->textures) ctx_->device().destroy(t);
    }
    models_.clear();
    for (auto& [name, mesh] : primitives_) ctx_->destroy(mesh);
    primitives_.clear();
    evict_lightmaps();
    for (auto& [id, entry] : procedural_)
        if (entry.mesh.valid()) ctx_->destroy(entry.mesh);
    procedural_.clear();
    evict_shaders();
    if (terrain_shader_ && *terrain_shader_) ctx_->device().destroy(**terrain_shader_);
    terrain_shader_.reset();
}

void GpuAssetCache::rebind(render::GpuContext& ctx) {
    wait_loads(); // decodes in flight finish before their records are forgotten
    clear();
    materials_.clear();
    ctx_ = &ctx;
}

void GpuAssetCache::evict(const Uuid& uuid) {
    // Materials hold texture handles: any eviction may invalidate them, and they are cheap to rebuild.
    materials_.clear();
    nearest_.erase(uuid);
    if (const auto it = resident_.find(uuid); it != resident_.end()) {
        residency_stats_.resident_bytes -= it->second.bytes;
        resident_.erase(it);
    }
    if (const auto it = stream_.find(uuid); it != stream_.end()) {
        it->second.levels = 0; // footprints stay: the next upload starts at the level they need
        it->second.level = 0;
        it->second.pending = -1;
        it->second.tail_bytes.clear();
    }
    if (loader_) loader_->forget(uuid); // a reload in flight is discarded; the next request re-reads
    environments_.erase(uuid);
    environment_errors_.erase(uuid);
    cookies_.erase(uuid); // the next request reads it again under a new version
    if (const auto it = textures_.find(uuid); it != textures_.end()) {
        ctx_->device().destroy(it->second);
        textures_.erase(it);
    }
    drop_morphed(&uuid);
    drop_crowd(&uuid);
    if (const auto it = models_.find(uuid); it != models_.end()) {
        forget_model_streams(*it->second, uuid);
        for (auto& m : it->second->meshes) ctx_->destroy(m);
        for (auto& t : it->second->textures) ctx_->device().destroy(t);
        models_.erase(it);
    }
}

void GpuAssetCache::drop_crowd(const Uuid* model) {
    for (auto it = crowd_.begin(); it != crowd_.end();) {
        if (model && it->first.model != *model) {
            ++it;
            continue;
        }
        if (it->second->buffer) ctx_->device().destroy(it->second->buffer);
        it = crowd_.erase(it);
    }
}

std::size_t GpuAssetCache::crowd_clip_count() const noexcept {
    std::size_t n = 0;
    for (const auto& [key, c] : crowd_) n += c->failed ? 0 : 1;
    return n;
}

std::size_t GpuAssetCache::crowd_bytes() const noexcept {
    std::size_t n = 0;
    for (const auto& [key, c] : crowd_) n += c->failed ? 0 : c->bake.bytes();
    return n;
}

const GpuAssetCache::CrowdClip* GpuAssetCache::crowd_clip(const Uuid& model_id, const GpuModel& model, std::uint32_t skin,
                                                          std::string_view clip) {
    CrowdKey key{model_id, skin, std::string(clip)};
    auto& slot = crowd_[key];
    if (slot && slot->model != &model) { // the model was reloaded under the same id
        if (slot->buffer) ctx_->device().destroy(slot->buffer);
        slot.reset();
    }
    if (slot) return slot->failed ? nullptr : slot.get();
    slot = std::make_unique<CrowdClip>();
    CrowdClip& c = *slot;
    c.model = &model;
    const auto fail = [&](std::string_view why) -> const CrowdClip* {
        c.failed = true;
        VICTUS_LOG_WARN("runtime", "crowd: model {} skin {} clip '{}': {}", model_id.to_string(), skin, clip, why);
        return nullptr;
    };
    const assets::CookedModel& cpu = model.cpu;
    if (skin >= cpu.skins.size()) return fail("the model has no such skin");
    const assets::ModelSkin& ms = cpu.skins[skin];
    // Meshes the skin deforms, and the union of their per-joint bounds.
    std::vector<math::Aabb> joint_bounds(ms.joints.size());
    for (const assets::ModelNode& n : cpu.nodes) {
        if (n.skin != static_cast<std::int32_t>(skin)) continue;
        for (const std::uint32_t mi : n.meshes) {
            if (mi >= model.meshes.size() || model.max_joint[mi] < 0) continue;
            if (std::find(c.meshes.begin(), c.meshes.end(), mi) == c.meshes.end()) c.meshes.push_back(mi);
            const auto& jb = model.joint_bounds[mi];
            for (std::size_t j = 0; j < jb.size() && j < joint_bounds.size(); ++j)
                if (jb[j].valid()) {
                    joint_bounds[j].expand(jb[j].min);
                    joint_bounds[j].expand(jb[j].max);
                }
        }
    }
    if (c.meshes.empty()) return fail("no mesh is skinned by the skin");
    std::span<const animation::Clip> clips;
    for (const animation::Clip& a : cpu.animations)
        if (!clip.empty() && a.name == clip) {
            clips = std::span<const animation::Clip>(&a, 1);
            break;
        }
    if (!clip.empty() && clips.empty()) VICTUS_LOG_WARN("runtime", "crowd: model {} has no clip '{}'; drawing the bind pose", model_id.to_string(), clip);
    animation::CrowdBakeInput in;
    in.nodes = model.skeleton;
    in.clips = clips;
    in.joints = ms.joints;
    in.inverse_bind = ms.inverse_bind;
    in.joint_bounds = joint_bounds;
    auto bake = animation::bake_crowd(in);
    if (!bake) return fail(bake.error().message);
    c.bake = std::move(*bake);
    const std::size_t bytes = c.bake.bytes();
    if (bytes > std::numeric_limits<std::uint32_t>::max()) return fail("bake too large");
    auto buf = ctx_->device().create_buffer({std::format("crowd_{}_{}", model_id.to_string(), clip), static_cast<std::uint32_t>(bytes),
                                             rhi::BufferUsageStorageRead});
    if (!buf) return fail(buf.error().message);
    c.buffer = *buf;
    auto cmd = ctx_->device().begin_commands();
    cmd.upload_buffer(c.buffer, 0, {reinterpret_cast<const std::uint8_t*>(c.bake.matrices.data()), bytes});
    cmd.submit();
    return &c;
}

Result<render::Image> load_cookie_image(assets::AssetDatabase& db, const Uuid& texture) {
    auto cooked = db.load_texture(texture);
    if (!cooked) return std::move(cooked).error();
    if (cooked->mips.empty()) return make_error(ErrorCode::ParseError, "texture has no levels");
    // The smallest level that still has at least SceneRenderer::kCookieSize pixels on its long side.
    std::size_t level = 0;
    while (level + 1 < cooked->mips.size() &&
           std::max(cooked->mips[level + 1].width, cooked->mips[level + 1].height) >= render::SceneRenderer::kCookieSize)
        ++level;
    auto img = assets::rgba8_level(*cooked, level);
    if (!img) return std::move(img).error();
    if (!cooked->srgb) { // linear data textures (masks) store the value itself: encode it as sRGB
        for (std::size_t i = 0; i < img->pixels.size(); ++i) {
            if (i % 4 == 3) continue;
            const float v = math::linear_to_srgb(static_cast<float>(img->pixels[i]) / 255.0f);
            img->pixels[i] = static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        }
    }
    return img;
}

const GpuAssetCache::CookieImage* GpuAssetCache::cookie(const Uuid& texture) {
    if (texture.is_nil()) return nullptr;
    if (const auto it = cookies_.find(texture); it != cookies_.end()) return it->second ? &*it->second : nullptr;
    auto& slot = cookies_[texture];
    if (!db_) return nullptr;
    auto img = load_cookie_image(*db_, texture);
    if (!img) {
        VICTUS_LOG_WARN("runtime", "light cookie {}: {}", texture.to_string(), img.error().message);
        return nullptr;
    }
    CookieImage c;
    c.image = std::move(*img);
    c.key = UuidHash{}(texture) | 1u; // never 0 (0 = no cookie)
    c.version = ++cookie_versions_;
    slot = std::move(c);
    return &*slot;
}

assets::VariantFilter GpuAssetCache::variant_filter() const {
    // Snapshot of what this device samples, so loader threads never query the device. Only the
    // variant upload_cooked_texture would pick is decoded; with compressed uploads off nothing
    // matches and every variant is kept (they are decoded on the CPU).
    if (!ctx_ || !allow_compressed_) return {};
    std::array<bool, 2 * 8> ok{};
    for (std::uint8_t c = 1; c < 8; ++c) {
        const auto codec = static_cast<assets::TextureCodec>(c);
        if (!assets::parse_texture_codec(assets::to_string(codec))) continue;
        for (const bool srgb : {false, true})
            ok[std::size_t{c} * 2 + (srgb ? 1 : 0)] = ctx_->device().supports_format(assets::codec_format(codec, srgb), rhi::TextureUsageSampled);
    }
    return [ok](assets::TextureCodec codec, bool srgb) {
        const std::size_t i = std::size_t{static_cast<std::uint8_t>(codec)} * 2 + (srgb ? 1 : 0);
        return i < ok.size() && ok[i];
    };
}

Result<rhi::TextureHandle> upload_cooked_texture(render::GpuContext& ctx, const assets::CookedTexture& texture, bool allow_compressed,
                                                 std::optional<assets::TextureCodec>* chosen, std::uint32_t base_level) {
    if (chosen) chosen->reset();
    render::TextureOptions opt;
    opt.srgb = texture.srgb;
    opt.name = texture.name;
    const std::size_t base = texture.mips.empty() ? 0 : std::min<std::size_t>(base_level, texture.mips.size() - 1);
    if (allow_compressed && !texture.mips.empty()) {
        for (const auto& variant : texture.compressed) {
            const rhi::Format format = assets::codec_format(variant.codec, texture.srgb);
            if (variant.levels.size() != texture.mips.size() || !ctx.device().supports_format(format, rhi::TextureUsageSampled)) continue;
            const std::span<const std::vector<std::uint8_t>> levels = std::span(variant.levels).subspan(base);
            auto tex = ctx.create_compressed_texture(format, texture.mips[base].width, texture.mips[base].height, levels, opt);
            if (!tex) continue; // fall through to the next variant / RGBA8
            if (chosen) *chosen = variant.codec;
            return tex;
        }
    }
    if (texture.has_rgba8() || texture.mips.empty()) return ctx.create_texture_mips(std::span(texture.mips).subspan(base), opt);
    // A mobile export keeps the large levels only as ASTC; a device that cannot sample them
    // (or allow_compressed off) gets a CPU decode, so the texture still renders, just slower to load.
    std::vector<render::Image> decoded;
    decoded.reserve(texture.mips.size() - base);
    for (std::size_t i = base; i < texture.mips.size(); ++i) {
        auto img = assets::rgba8_level(texture, i);
        if (!img) return std::move(img).error();
        decoded.push_back(std::move(*img));
    }
    return ctx.create_texture_mips(decoded, opt);
}

namespace {
// Bytes uploaded for each starting level (index levels = 0), matching assets::upload_bytes().
std::vector<std::size_t> texture_tail_bytes(const assets::CookedTexture& t) {
    const std::size_t n = t.mips.size();
    std::vector<std::size_t> tail(n + 1, 0);
    const bool compressed = !t.compressed.empty() && t.compressed.front().levels.size() == n;
    for (std::size_t i = n; i-- > 0;)
        tail[i] = tail[i + 1] + (compressed ? t.compressed.front().levels[i].size() : t.mips[i].pixels.size());
    return tail;
}
} // namespace

float projected_pixels(const render::Camera3D& camera, const math::Aabb& world_bounds, std::uint32_t screen_height) noexcept {
    if (!world_bounds.valid()) return 0.0f;
    const float radius = math::length(world_bounds.extents());
    const float distance = std::max(math::length(world_bounds.center() - camera.position) - radius, std::max(camera.z_near, 1e-3f));
    const float tan_half = std::tan(std::clamp(camera.fov_y, 1e-3f, 3.1f) * 0.5f);
    return 2.0f * radius * static_cast<float>(screen_height) / (2.0f * distance * tan_half);
}

void GpuAssetCache::note_footprint(const Uuid& texture, float pixels) {
    if (!residency_settings_.mip_streaming || texture.is_nil() || !(pixels > 0.0f)) return;
    Streamed& s = stream_[texture];
    if (s.seen_frame != residency_stats_.frame) {
        s.seen_frame = residency_stats_.frame;
        s.seen_pixels = pixels;
    } else {
        s.seen_pixels = std::max(s.seen_pixels, pixels);
    }
}

void GpuAssetCache::note_material_footprint(const Uuid& material, float pixels) {
    if (!residency_settings_.mip_streaming) return;
    const auto refs = material_textures_.find(material);
    if (refs == material_textures_.end()) return;
    float tiling = 1.0f; // a texture repeated n times across the surface needs n times the texels
    if (const auto m = materials_.find(material); m != materials_.end() && m->second)
        tiling = std::max({std::abs(m->second->uv_scale.x), std::abs(m->second->uv_scale.y), 1.0f});
    for (const Uuid& t : refs->second) note_footprint(t, pixels * tiling);
}

Uuid GpuAssetCache::embedded_texture_key(const Uuid& model, std::uint32_t index) noexcept {
    return {model.hi ^ 0x6d6f64656c746578ull, model.lo ^ ((static_cast<std::uint64_t>(index) + 1) * 0x9e3779b97f4a7c15ull)};
}

std::optional<GpuAssetCache::EmbeddedRef> GpuAssetCache::EmbeddedRegistry::find(const Uuid& key) const {
    std::lock_guard lock(mutex);
    const auto it = refs.find(key);
    if (it == refs.end()) return std::nullopt;
    return it->second;
}

std::vector<std::uint32_t> GpuAssetCache::mesh_textures(const assets::CookedModel& model, std::uint32_t mesh) {
    std::vector<std::uint32_t> out;
    if (mesh >= model.meshes.size() || model.meshes[mesh].material >= model.materials.size()) return out;
    const assets::ModelMaterial& mat = model.materials[model.meshes[mesh].material];
    for (const std::int32_t t : {mat.base_color_texture, mat.metallic_roughness_texture, mat.normal_texture, mat.emissive_texture})
        if (t >= 0) out.push_back(static_cast<std::uint32_t>(t));
    return out;
}

void GpuAssetCache::note_model_footprint(const Uuid& model, std::uint32_t mesh, float pixels) {
    if (!residency_settings_.mip_streaming) return;
    const auto it = models_.find(model);
    if (it == models_.end()) return;
    for (const std::uint32_t t : mesh_textures(it->second->cpu, mesh)) note_footprint(embedded_texture_key(model, t), pixels);
}

void GpuAssetCache::note_reloading_model(const Uuid& model, std::uint32_t mesh, const math::Mat4& world, const render::Camera3D& camera) {
    if (!residency_settings_.mip_streaming) return;
    const auto it = model_hints_.find(model);
    if (it == model_hints_.end() || mesh >= it->second.bounds.size()) return;
    const float px = projected_pixels(camera, it->second.bounds[mesh].transformed(world), residency_settings_.screen_height);
    for (const std::uint32_t t : it->second.textures[mesh]) note_footprint(embedded_texture_key(model, t), px);
}

std::optional<std::uint32_t> GpuAssetCache::model_texture_level(const Uuid& model, std::uint32_t index) const {
    const auto m = models_.find(model);
    if (m == models_.end() || index >= m->second->textures.size()) return std::nullopt;
    const auto it = stream_.find(embedded_texture_key(model, index));
    return it == stream_.end() || it->second.levels == 0 ? 0u : it->second.level;
}

rhi::TextureHandle* GpuAssetCache::streamed_handle(const Uuid& key, Uuid* owner) {
    if (const auto it = textures_.find(key); it != textures_.end()) {
        if (owner) *owner = key;
        return &it->second;
    }
    const auto ref = embedded_->find(key);
    if (!ref) return nullptr;
    const auto m = models_.find(ref->model);
    if (m == models_.end() || ref->index >= m->second->textures.size()) return nullptr;
    if (owner) *owner = ref->model;
    return &m->second->textures[ref->index];
}

void GpuAssetCache::forget_model_streams(const GpuModel& model, const Uuid& uuid) {
    if (!model.textures.empty()) {
        ModelHint& hint = model_hints_[uuid];
        hint.bounds.clear();
        hint.textures.clear();
        for (std::uint32_t m = 0; m < model.meshes.size(); ++m) {
            hint.bounds.push_back(model.meshes[m].bounds);
            hint.textures.push_back(mesh_textures(model.cpu, m));
        }
    }
    for (std::uint32_t i = 0; i < model.textures.size(); ++i) {
        const Uuid key = embedded_texture_key(uuid, i);
        if (const auto it = stream_.find(key); it != stream_.end()) {
            it->second.levels = 0; // footprints stay, like texture assets
            it->second.level = 0;
            it->second.pending = -1;
            it->second.tail_bytes.clear();
        }
        if (loader_) loader_->forget(key);
    }
}

std::optional<std::uint32_t> GpuAssetCache::texture_level(const Uuid& uuid) const {
    if (!textures_.contains(uuid)) return std::nullopt;
    const auto it = stream_.find(uuid);
    return it == stream_.end() || it->second.levels == 0 ? 0u : it->second.level;
}

std::uint32_t GpuAssetCache::max_level(const Streamed& s) const noexcept {
    // The lowest detail allowed: the top level stays at least min_streamed_size on its longer edge.
    const std::uint32_t edge = std::max(s.width, s.height);
    const std::uint32_t floor_edge = std::max(residency_settings_.min_streamed_size, 1u);
    std::uint32_t level = 0;
    while (level + 1 < s.levels && (edge >> (level + 1)) >= floor_edge) ++level;
    return level;
}

std::uint32_t GpuAssetCache::wanted_level(const Streamed& s) const noexcept {
    // Level L has edge/2^L texels: keep the first level whose resolution still covers the footprint.
    const float edge = static_cast<float>(std::max(s.width, s.height));
    std::uint32_t level = 0;
    if (s.seen_pixels > 0.0f && edge > s.seen_pixels)
        level = static_cast<std::uint32_t>(std::floor(std::log2(edge / s.seen_pixels)));
    return std::min(level, max_level(s));
}

Result<rhi::TextureHandle> GpuAssetCache::upload_texture(const Uuid& uuid, const assets::CookedTexture& cooked) {
    // Mip streaming: a texture already seen on screen uploads only the levels it needs.
    std::uint32_t level = 0;
    Streamed* s = nullptr;
    if (residency_settings_.mip_streaming && !cooked.hdr && cooked.mips.size() > 1) {
        s = &stream_[uuid];
        s->width = cooked.mips[0].width;
        s->height = cooked.mips[0].height;
        s->levels = static_cast<std::uint32_t>(cooked.mips.size());
        s->tail_bytes = texture_tail_bytes(cooked);
        s->pending = -1;
        s->lower_since = 0;
        if (residency_stats_.frame - s->seen_frame <= 1) level = wanted_level(*s);
    }
    std::optional<assets::TextureCodec> codec;
    auto tex = upload_cooked_texture(*ctx_, cooked, allow_compressed_, &codec, level);
    if (!tex) {
        if (s) s->levels = 0;
        return tex;
    }
    if (codec) ++compressed_uploads_;
    textures_.emplace(uuid, *tex);
    std::size_t bytes = assets::upload_bytes(cooked);
    if (s) {
        s->level = level;
        bytes += s->tail_bytes[level] - s->tail_bytes[0]; // the levels above `level` were not uploaded
    }
    make_resident(uuid, bytes);
    return tex;
}

void GpuAssetCache::replace_texture(const Uuid& uuid, const assets::CookedTexture& cooked, std::uint32_t level) {
    Uuid owner;
    rhi::TextureHandle* handle = streamed_handle(uuid, &owner);
    const auto st = stream_.find(uuid);
    if (!handle || st == stream_.end() || st->second.levels != cooked.mips.size()) return;
    Streamed& s = st->second;
    s.pending = -1;
    level = std::min(level, max_level(s));
    if (level == s.level) return;
    std::optional<assets::TextureCodec> codec;
    auto tex = upload_cooked_texture(*ctx_, cooked, allow_compressed_, &codec, level);
    if (!tex) {
        VICTUS_LOG_WARN("assets", "mip streaming: re-upload of {} failed: {}", uuid.to_string(), tex.error().message);
        return; // the old levels stay
    }
    const rhi::TextureHandle old = *handle;
    // Safe while frames that sample the old texture are in flight: the device frees it once the GPU is done.
    ctx_->device().destroy(old);
    *handle = *tex;
    const Resident before = resident_[owner];
    if (owner == uuid) {
        make_resident(uuid, assets::upload_bytes(cooked) + s.tail_bytes[level] - s.tail_bytes[0]);
    } else {
        // An embedded model texture: the model's account changes by the levels added or dropped,
        // and its meshes' resolved materials point at the new handle.
        make_resident(owner, before.bytes + s.tail_bytes[level] - s.tail_bytes[s.level]);
        for (render::Material& m : models_[owner]->mesh_materials)
            for (rhi::TextureHandle* t : {&m.base_color_texture, &m.metallic_roughness_texture, &m.normal_texture, &m.emissive_texture})
                if (*t == old) *t = *tex;
    }
    resident_[owner].last_used = before.last_used;
    s.level = level;
    s.lower_since = 0;
    ++residency_stats_.restreams;
    materials_.clear(); // they hold the old handle
}

void GpuAssetCache::restream(const Uuid& uuid, std::uint32_t level) {
    Streamed& s = stream_[uuid];
    if (loader_) {
        s.pending = static_cast<std::int32_t>(level);
        loader_->forget(uuid); // the slot was taken: request a fresh decode
        (void)loader_->request(uuid, assets::LoadType::Texture, assets::LoadPriority::Normal);
        return;
    }
    if (!db_) return;
    const auto ref = embedded_->find(uuid);
    auto cooked = ref ? db_->load_model_texture(ref->model, ref->index, variant_filter()) : db_->load_texture(uuid);
    if (!cooked) {
        VICTUS_LOG_WARN("assets", "mip streaming: reload of {} failed: {}", uuid.to_string(), cooked.error().message);
        return;
    }
    replace_texture(uuid, *cooked, level);
}

void GpuAssetCache::stream_mips() {
    if (!residency_settings_.mip_streaming) return;
    const std::uint64_t now = residency_stats_.frame;
    const std::size_t budget = residency_settings_.budget_bytes;
    struct Change {
        Uuid id;
        std::uint32_t level;
        float priority;
    };
    std::vector<Change> drops, raises;
    std::vector<std::pair<Uuid, std::size_t>> in_use; // candidates for budget drops: (id, bytes)
    for (auto& [id, s] : stream_) {
        if (s.levels == 0 || s.pending >= 0 || !streamed_handle(id)) continue;
        if (now - s.seen_frame > 1) { // not on screen last frame: idle eviction handles it
            s.lower_since = 0;
            continue;
        }
        const std::uint32_t want = wanted_level(s);
        if (want < s.level) {
            raises.push_back({id, want, s.seen_pixels});
            in_use.emplace_back(id, s.tail_bytes[s.level]);
            s.lower_since = 0;
        } else if (want > s.level) {
            if (s.lower_since == 0) s.lower_since = now;
            if (now - s.lower_since >= residency_settings_.mip_drop_frames) drops.push_back({id, want, 0.0f});
            else in_use.emplace_back(id, s.tail_bytes[s.level]);
        } else {
            s.lower_since = 0;
            in_use.emplace_back(id, s.tail_bytes[s.level]);
        }
    }
    std::uint32_t left = residency_settings_.restreams_per_pump;
    std::size_t projected = residency_stats_.resident_bytes;
    for (const Change& c : drops) {
        if (left == 0) break;
        const Streamed& s = stream_[c.id];
        projected -= s.tail_bytes[s.level] - s.tail_bytes[c.level];
        restream(c.id, c.level);
        --left;
    }
    // Over the budget with nothing idle left: the largest textures in use give up their top level.
    if (budget > 0 && projected > budget) {
        std::sort(in_use.begin(), in_use.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (const auto& [id, bytes] : in_use) {
            if (left == 0 || projected <= budget) break;
            const Streamed& s = stream_[id];
            if (s.level >= max_level(s)) continue;
            projected -= s.tail_bytes[s.level] - s.tail_bytes[s.level + 1];
            restream(id, s.level + 1);
            ++residency_stats_.budget_drops;
            --left;
        }
        return; // no raises while over the budget
    }
    // Raises: the largest footprints first, and only while they fit the budget.
    std::sort(raises.begin(), raises.end(), [](const Change& a, const Change& b) { return a.priority > b.priority; });
    for (const Change& c : raises) {
        if (left == 0) break;
        const Streamed& s = stream_[c.id];
        std::uint32_t level = c.level; // the most detail that still fits the budget
        while (level < s.level && budget > 0 && projected + (s.tail_bytes[level] - s.tail_bytes[s.level]) > budget) ++level;
        if (level == s.level) continue;
        projected += s.tail_bytes[level] - s.tail_bytes[s.level];
        restream(c.id, level);
        --left;
    }
}

void GpuAssetCache::touch(const Uuid& uuid) noexcept {
    if (const auto it = resident_.find(uuid); it != resident_.end()) it->second.last_used = residency_stats_.frame;
}

void GpuAssetCache::make_resident(const Uuid& uuid, std::size_t bytes) {
    Resident& r = resident_[uuid];
    residency_stats_.resident_bytes += bytes - r.bytes;
    r.bytes = bytes;
    r.last_used = residency_stats_.frame;
}

void GpuAssetCache::enforce_residency() {
    const std::size_t budget = residency_settings_.budget_bytes;
    residency_stats_.over_budget = false;
    if (budget == 0 || residency_stats_.resident_bytes <= budget) return;
    const std::uint64_t now = residency_stats_.frame;
    std::vector<std::pair<Uuid, Resident>> idle;
    for (const auto& [id, r] : resident_)
        if (now - r.last_used >= residency_settings_.min_idle_frames) idle.emplace_back(id, r);
    std::sort(idle.begin(), idle.end(), [](const auto& a, const auto& b) {
        if (a.second.last_used != b.second.last_used) return a.second.last_used < b.second.last_used;
        return a.second.bytes > b.second.bytes; // equally stale: free more per eviction
    });
    for (const auto& [id, r] : idle) {
        if (residency_stats_.resident_bytes <= budget) break;
        evict(id);
        ++residency_stats_.evicted;
        residency_stats_.evicted_bytes += r.bytes;
    }
    residency_stats_.over_budget = residency_stats_.resident_bytes > budget;
}

ResidencyStats GpuAssetCache::residency() const {
    ResidencyStats s = residency_stats_;
    s.resident_assets = static_cast<std::uint32_t>(resident_.size());
    for (const auto& [id, st] : stream_)
        if (st.levels > 0 && st.level > 0 && textures_.contains(id)) ++s.streamed_textures;
    return s;
}

namespace {
// The answer for an asset the loader knows about but the cache has not uploaded.
Error not_resident(const assets::AsyncLoader& loader, const Uuid& uuid, assets::LoadType type) {
    switch (loader.state(uuid, type)) {
    case assets::LoadState::Failed: return *loader.error(uuid, type);
    case assets::LoadState::Taken: // decoded and handed over, but the upload failed (logged)
        return make_error(ErrorCode::DeviceError, "upload of " + uuid.to_string() + " failed");
    default: return make_error(ErrorCode::Pending, uuid.to_string() + " is loading");
    }
}
} // namespace

Result<rhi::TextureHandle> GpuAssetCache::texture(const Uuid& uuid) {
    if (const auto it = textures_.find(uuid); it != textures_.end()) {
        touch(uuid);
        return it->second;
    }
    if (!db_) return make_error(ErrorCode::NotFound, "no asset database");
    if (loader_) {
        (void)loader_->request(uuid, assets::LoadType::Texture);
        if (const auto it = textures_.find(uuid); it != textures_.end()) return it->second; // never: uploads happen in pump()
        return not_resident(*loader_, uuid, assets::LoadType::Texture);
    }
    auto cooked = db_->load_texture(uuid);
    if (!cooked) return std::move(cooked).error();
    return upload_texture(uuid, *cooked);
}

void GpuAssetCache::set_async_loading(jobs::JobSystem* jobs, const AsyncLoadingSettings& settings) {
    if (loader_) {
        loader_->wait_idle();
        loader_.reset();
    }
    async_settings_ = settings;
    if (!jobs || !db_) return;
    assets::AssetDatabase* db = db_;
    loader_ = std::make_unique<assets::AsyncLoader>(
        jobs,
        [db, embedded = embedded_, keep = variant_filter()](const Uuid& uuid, assets::LoadType type) -> Result<assets::LoadPayload> {
            if (type == assets::LoadType::Texture) {
                const auto ref = embedded->find(uuid); // a model texture re-streaming its mips
                auto t = ref ? db->load_model_texture(ref->model, ref->index, keep) : db->load_texture(uuid);
                if (!t) return std::move(t).error();
                return assets::LoadPayload{std::move(t).value()};
            }
            auto m = db->load_model(uuid, keep);
            if (!m) return std::move(m).error();
            return assets::LoadPayload{std::move(m).value()};
        },
        assets::LoaderSettings{.max_in_flight = settings.max_in_flight});
}

bool GpuAssetCache::settled(const Uuid& uuid, assets::LoadType type) const {
    if (type == assets::LoadType::Texture ? textures_.contains(uuid) : models_.contains(uuid)) return true;
    if (!loader_) return true;
    const assets::LoadState s = loader_->state(uuid, type);
    return s == assets::LoadState::None || s == assets::LoadState::Failed || s == assets::LoadState::Taken;
}

void GpuAssetCache::prefetch(const Uuid& uuid, assets::LoadType type, assets::LoadPriority priority) {
    if (!loader_ || uuid.is_nil()) return;
    if (type == assets::LoadType::Texture ? textures_.contains(uuid) : models_.contains(uuid)) return;
    (void)loader_->request(uuid, type, priority);
}

std::uint32_t GpuAssetCache::pump() {
    VICTUS_PROFILE_SCOPE("GpuAssetCache::pump");
    ++residency_stats_.frame;
    // Eviction first: this frame's references have not touched anything yet, so "idle" means
    // unused during the last min_idle_frames frames, and new uploads below are never candidates.
    enforce_residency();
    stream_mips();
    if (!loader_) return 0;
    async_stats_.uploaded_last_pump = 0;
    async_stats_.uploaded_bytes_last_pump = 0;
    auto ready = loader_->take(async_settings_.upload_budget_bytes);
    bool any_texture = false;
    for (auto& c : ready) {
        Status st;
        if (auto* tex = std::get_if<assets::CookedTexture>(&c.payload)) {
            if (embedded_->find(c.uuid)) { // model textures only ever re-stream (or arrive late)
                if (const auto s = stream_.find(c.uuid); s != stream_.end() && s->second.pending >= 0)
                    replace_texture(c.uuid, *tex, static_cast<std::uint32_t>(s->second.pending));
                continue;
            }
            if (textures_.contains(c.uuid)) {
                // A mip-streaming re-decode: swap in the requested levels.
                if (const auto s = stream_.find(c.uuid); s != stream_.end() && s->second.pending >= 0)
                    replace_texture(c.uuid, *tex, static_cast<std::uint32_t>(s->second.pending));
                continue;
            }
            auto r = upload_texture(c.uuid, *tex);
            if (!r) st = std::move(r).error();
            any_texture = true;
        } else {
            if (models_.contains(c.uuid)) continue;
            const Uuid id = c.uuid;
            auto r = add_model(id, std::move(std::get<assets::CookedModel>(c.payload)));
            if (!r) st = std::move(r).error();
            // add_model() evicted the id, which forgot the loader slot; the model is resident now.
        }
        if (!st.ok()) {
            ++async_stats_.failed;
            VICTUS_LOG_ERROR("assets", "upload of {} failed: {}", c.uuid.to_string(), st.error().message);
            continue;
        }
        ++async_stats_.uploaded;
        ++async_stats_.uploaded_last_pump;
        async_stats_.uploaded_bytes_last_pump += c.bytes;
    }
    // Materials resolved while a texture was loading hold an empty slot: rebuild them.
    if (any_texture) materials_.clear();
    return async_stats_.uploaded_last_pump;
}

void GpuAssetCache::finish_loads() {
    if (!loader_) return;
    for (;;) {
        loader_->wait_idle();
        (void)pump();
        const auto s = loader_->stats();
        if (s.queued == 0 && s.loading == 0 && s.decoded == 0) return;
    }
}

void GpuAssetCache::wait_loads() {
    if (loader_) loader_->wait_idle();
}

AsyncLoadingStats GpuAssetCache::async_stats() const {
    AsyncLoadingStats s = async_stats_;
    if (loader_) {
        const auto l = loader_->stats();
        s.pending = l.queued + l.loading + l.decoded;
        s.failed += l.failed;
    }
    return s;
}

Result<const render::EnvironmentMap*> GpuAssetCache::environment(const Uuid& uuid) {
    if (const auto it = environments_.find(uuid); it != environments_.end())
        return static_cast<const render::EnvironmentMap*>(it->second.get());
    if (const auto it = environment_errors_.find(uuid); it != environment_errors_.end()) return it->second;
    auto build = [&]() -> Result<std::unique_ptr<render::EnvironmentMap>> {
        if (!db_) return make_error(ErrorCode::NotFound, "no asset database");
        auto cooked = db_->load_texture(uuid);
        if (!cooked) return std::move(cooked).error();
        if (!cooked->hdr)
            return make_error(ErrorCode::InvalidArgument,
                              std::format("{}: environment maps need an HDR (.hdr) texture", cooked->name));
        render::SphericalHarmonics9 sh;
        sh.coefficients = cooked->sh;
        return render::EnvironmentMap::create(*ctx_, cooked->hdr_image(), sh, env_bake_);
    };
    auto env = build();
    if (!env) {
        VICTUS_LOG_ERROR("runtime", "environment map {}: {}", uuid.to_string(), env.error().message);
        environment_errors_.emplace(uuid, env.error());
        return std::move(env).error();
    }
    const render::EnvironmentMap* ptr = env->get();
    environments_.emplace(uuid, std::move(*env));
    return ptr;
}

Result<const render::EnvironmentMap*> GpuAssetCache::atmosphere(const render::AtmosphereSettings& s) {
    if (atmosphere_ && atmosphere_key_.same_as(s)) return static_cast<const render::EnvironmentMap*>(atmosphere_.get());
    VICTUS_PROFILE_SCOPE("GpuAssetCache::atmosphere");
    // Bake settings scale the cube; the sky itself is smooth, so a 64-128 px cube is plenty.
    render::EnvironmentBakeSettings bake = env_bake_;
    bake.cube_size = std::min(bake.cube_size, 128u);
    auto env = render::EnvironmentMap::create(*ctx_, render::render_atmosphere(s), bake);
    if (!env) return std::move(env).error();
    atmosphere_ = std::move(*env); // the previous cube is released once in-flight frames finish
    atmosphere_key_ = s;
    ++atmosphere_bakes_;
    return static_cast<const render::EnvironmentMap*>(atmosphere_.get());
}

Result<render::SceneRenderer*> GpuAssetCache::probe_renderer() {
    if (!probe_renderer_) {
        auto r = render::SceneRenderer::create(*ctx_);
        if (!r) return std::move(r).error();
        probe_renderer_ = std::move(*r);
        // Captures are small and viewed through rough reflections: skip SSAO, keep shadows.
        render::RenderSettings s = render::RenderSettings::preset(render::QualityTier::Medium);
        s.ssao = false;
        s.contact_shadows = false;
        s.shadow_cascades = 1;
        s.shadow_resolution = 1024;
        probe_renderer_->set_settings(s);
    }
    return probe_renderer_.get();
}

bool GpuAssetCache::texture_nearest(const Uuid& uuid) {
    if (uuid.is_nil() || !db_) return false;
    if (const auto it = nearest_.find(uuid); it != nearest_.end()) return it->second;
    const assets::AssetRecord* rec = db_->find(uuid);
    const bool nearest = rec && rec->kind == assets::AssetKind::Texture &&
                         assets::TextureImportSettings::from_json(rec->settings).nearest;
    nearest_.emplace(uuid, nearest);
    return nearest;
}

Result<render::Material> GpuAssetCache::material(const Uuid& uuid) {
    if (const auto it = materials_.find(uuid); it != materials_.end()) {
        // The material holds its textures' handles: keep them resident while it is in use.
        if (const auto refs = material_textures_.find(uuid); refs != material_textures_.end())
            for (const Uuid& t : refs->second) touch(t);
        return it->second;
    }
    auto build = [&]() -> Result<render::Material> {
        if (!db_) return make_error(ErrorCode::NotFound, "no asset database");
        auto src = db_->resolve_material(uuid); // instances inherit from their parents
        if (!src) return std::move(src).error();
        render::Material m;
        m.base_color = src->base_color;
        m.emissive = src->emissive;
        m.emissive_strength = src->emissive_strength;
        m.metallic = src->metallic;
        m.roughness = src->roughness;
        m.normal_scale = src->normal_scale;
        m.alpha_cutoff = src->alpha_cutoff;
        m.double_sided = src->double_sided;
        m.unlit = src->unlit;
        m.uv_scale = src->uv_scale;
        m.uv_offset = src->uv_offset;
        m.triplanar = src->triplanar;
        m.triplanar_sharpness = src->triplanar_sharpness;
        m.clearcoat = src->clearcoat;
        m.clearcoat_roughness = src->clearcoat_roughness;
        // Nearest when the material asks for it or its base colour texture is imported as pixel art.
        m.nearest_filter = src->filter == assets::TextureFilter::Nearest || texture_nearest(src->base_color_texture);
        m.shader_params = src->shader_params;
        std::vector<Uuid>& used = material_textures_[uuid];
        used.clear();
        if (!src->shader.is_nil()) {
            if (auto sh = surface_shader(src->shader); sh) {
                m.surface_shader = *sh;
                m.vertex_stage = surface_vertex_stage(src->shader);
                if (const render::SurfaceDeclarations* decl = surface_declarations(src->shader)) {
                    // Slots the material does not set take the shader's declared defaults.
                    for (const render::SurfaceParamDecl& p : decl->params)
                        if (!src->shader_param_set(p.slot)) m.shader_params[p.slot] = p.value;
                    m.shader_texture_count = static_cast<std::uint8_t>(decl->textures.size());
                    for (const render::SurfaceTextureDecl& t : decl->textures) {
                        rhi::TextureHandle& out = m.shader_textures[t.slot];
                        out = t.fallback == render::SurfaceTextureDefault::Black    ? ctx_->black()
                              : t.fallback == render::SurfaceTextureDefault::Normal ? ctx_->flat_normal()
                                                                                    : ctx_->white();
                        const Uuid& ref = src->shader_textures[t.slot];
                        if (ref.is_nil()) continue;
                        const assets::AssetRecord* rec = db_->find(ref);
                        if (!rec || rec->kind != assets::AssetKind::Texture) {
                            VICTUS_LOG_WARN("assets", "material {}: shader texture '{}' {} is missing", uuid.to_string(), t.name,
                                            ref.to_string());
                            continue;
                        }
                        used.push_back(ref);
                        if (auto tex = texture(ref); tex) out = *tex;
                        else if (tex.error().code != ErrorCode::Pending)
                            VICTUS_LOG_WARN("assets", "material {}: {}", uuid.to_string(), tex.error().message);
                    }
                }
            }
            // else: already logged by surface_shader(); the material renders with built-in PBR
        }
        m.vertex_alpha_coverage_mask = src->vertex_alpha_coverage_mask;
        m.coverage_response = src->coverage_response;
        m.occlusion_strength = src->occlusion_strength;
        m.blend = static_cast<render::MaterialBlend>(src->blend);
        m.responsive = src->responsive;
        rhi::TextureHandle* slots[4] = {&m.base_color_texture, &m.metallic_roughness_texture, &m.normal_texture, &m.emissive_texture};
        const auto refs = src->textures();
        for (std::size_t i = 0; i < 4; ++i) {
            if (refs[i]->is_nil()) continue;
            used.push_back(*refs[i]);
            const assets::AssetRecord* rec = db_->find(*refs[i]);
            if (!rec || rec->kind != assets::AssetKind::Texture) {
                VICTUS_LOG_WARN("assets", "material {}: {} texture {} is missing", uuid.to_string(),
                                assets::kMaterialTextureSlotNames[i], refs[i]->to_string());
                continue;
            }
            if (auto t = texture(*refs[i]); t) *slots[i] = *t;
            else if (t.error().code != ErrorCode::Pending) // pump() rebuilds materials once it arrives
                VICTUS_LOG_WARN("assets", "material {}: {}", uuid.to_string(), t.error().message);
        }
        return m;
    };
    auto result = build();
    if (!result) VICTUS_LOG_WARN("assets", "{}", result.error().message);
    return materials_.emplace(uuid, std::move(result)).first->second;
}

namespace {
Result<rhi::ShaderHandle> create_surface_fragment(rhi::Device& device, std::string_view code, const std::string& name) {
    return create_surface_shader(device, code, name);
}
} // namespace

Result<rhi::ShaderHandle> GpuAssetCache::surface_shader(const Uuid& uuid) {
    if (const auto it = shaders_.find(uuid); it != shaders_.end()) return it->second;
    auto build = [&]() -> Result<rhi::ShaderHandle> {
        if (!db_) return make_error(ErrorCode::NotFound, "no asset database");
        const assets::AssetRecord* rec = db_->find(uuid);
        if (!rec || rec->kind != assets::AssetKind::Shader)
            return make_error(ErrorCode::NotFound, "surface shader " + uuid.to_string() + " is missing");
        const std::string path = rec->path; // records may move on refresh
        auto code = load_surface_shader_code(*db_, uuid);
        if (!code) return std::move(code).error();
        auto fragment = create_surface_fragment(ctx_->device(), *code, path);
        if (fragment) shader_declarations_[uuid] = render::parse_surface_declarations(*code);
        if (!fragment || !surface_shader_has_vertex(*code)) return fragment;
        auto stage = create_surface_vertex_stage(ctx_->device(), *code, path);
        if (!stage) {
            shader_declarations_.erase(uuid);
            ctx_->device().destroy(*fragment);
            return std::move(stage).error();
        }
        vertex_stages_[uuid] = std::make_unique<render::SurfaceVertexStage>(*stage);
        return fragment;
    };
    auto result = build();
    if (!result) VICTUS_LOG_ERROR("assets", "{}", result.error().message);
    return shaders_.emplace(uuid, std::move(result)).first->second;
}

Result<rhi::ShaderHandle> GpuAssetCache::terrain_shader() {
    if (!terrain_shader_) {
        terrain_shader_ = create_surface_fragment(ctx_->device(), terrain::terrain_surface_code(), "terrain");
        if (!*terrain_shader_) VICTUS_LOG_ERROR("assets", "built-in terrain shader: {}", terrain_shader_->error().message);
    }
    return *terrain_shader_;
}

const render::SurfaceVertexStage* GpuAssetCache::surface_vertex_stage(const Uuid& uuid) {
    if (!shaders_.contains(uuid)) (void)surface_shader(uuid);
    const auto it = vertex_stages_.find(uuid);
    return it == vertex_stages_.end() ? nullptr : it->second.get();
}

const render::SurfaceDeclarations* GpuAssetCache::surface_declarations(const Uuid& uuid) {
    if (!shaders_.contains(uuid)) (void)surface_shader(uuid);
    const auto it = shader_declarations_.find(uuid);
    return it == shader_declarations_.end() ? nullptr : &it->second;
}

void GpuAssetCache::evict_shaders() {
    materials_.clear(); // they hold the shader handles
    for (auto& [id, sh] : shaders_)
        if (sh) ctx_->device().destroy(*sh);
    shaders_.clear();
    for (auto& [id, stage] : vertex_stages_)
        for (auto sh : stage->shaders)
            if (sh) ctx_->device().destroy(sh);
    vertex_stages_.clear();
    shader_declarations_.clear();
}

Result<const GpuModel*> GpuAssetCache::model(const Uuid& uuid) {
    if (const auto it = models_.find(uuid); it != models_.end()) {
        touch(uuid);
        return static_cast<const GpuModel*>(it->second.get());
    }
    if (!db_) return make_error(ErrorCode::NotFound, "no asset database");
    if (loader_) {
        (void)loader_->request(uuid, assets::LoadType::Model);
        return not_resident(*loader_, uuid, assets::LoadType::Model);
    }
    auto cooked = db_->load_model(uuid, variant_filter());
    if (!cooked) return std::move(cooked).error();
    return add_model(uuid, std::move(cooked).value());
}

Result<const GpuModel*> GpuAssetCache::add_model(const Uuid& uuid, assets::CookedModel model) {
    VICTUS_PROFILE_SCOPE("GpuAssetCache::add_model");
    evict(uuid);
    const std::size_t bytes = assets::upload_bytes(model);
    auto gpu = std::make_unique<GpuModel>();
    auto fail = [&](Error e) -> Result<const GpuModel*> {
        for (auto& m : gpu->meshes) ctx_->destroy(m);
        for (auto& t : gpu->textures) ctx_->device().destroy(t);
        return e;
    };
    std::size_t skipped_bytes = 0; // top levels of streamed textures that were not uploaded
    for (std::uint32_t i = 0; i < model.textures.size(); ++i) {
        const assets::CookedTexture& t = model.textures[i];
        std::uint32_t level = 0;
        Streamed* s = nullptr;
        if (residency_settings_.mip_streaming && !t.hdr && t.mips.size() > 1) {
            const Uuid key = embedded_texture_key(uuid, i);
            {
                std::lock_guard lock(embedded_->mutex);
                embedded_->refs[key] = {uuid, i};
            }
            s = &stream_[key];
            s->width = t.mips[0].width;
            s->height = t.mips[0].height;
            s->levels = static_cast<std::uint32_t>(t.mips.size());
            s->tail_bytes = texture_tail_bytes(t);
            s->pending = -1;
            s->lower_since = 0;
            // Re-uploads after an eviction start at the level the last footprint needed.
            if (residency_stats_.frame - s->seen_frame <= 1) level = wanted_level(*s);
        }
        std::optional<assets::TextureCodec> codec;
        auto tex = upload_cooked_texture(*ctx_, t, allow_compressed_, &codec, level);
        if (!tex) {
            if (s) s->levels = 0;
            return fail(std::move(tex).error());
        }
        if (codec) ++compressed_uploads_;
        if (s) {
            s->level = level;
            skipped_bytes += s->tail_bytes[0] - s->tail_bytes[level];
        }
        gpu->textures.push_back(*tex);
    }
    auto tex_or = [&](std::int32_t i) { return i >= 0 ? gpu->textures[static_cast<std::size_t>(i)] : rhi::TextureHandle{}; };
    for (auto& mesh : model.meshes) {
        std::vector<math::Aabb> bounds;
        std::int32_t max_joint = -1;
        if (mesh.data.skinned()) {
            for (std::size_t v = 0; v < mesh.data.skin.size(); ++v) {
                const render::SkinVertex& sv = mesh.data.skin[v];
                for (int k = 0; k < 4; ++k) {
                    if (sv.weights[k] <= 0.0f) continue;
                    const std::uint16_t j = sv.joints[k];
                    if (j >= bounds.size()) bounds.resize(j + 1u);
                    bounds[j].expand(mesh.data.vertices[v].position);
                    max_joint = std::max<std::int32_t>(max_joint, j);
                }
            }
        }
        gpu->joint_bounds.push_back(std::move(bounds));
        gpu->max_joint.push_back(max_joint);
        auto g = ctx_->create_mesh(mesh.data, mesh.name);
        if (!g) return fail(std::move(g).error());
        gpu->meshes.push_back(*g);
        const assets::ModelMaterial& src = model.materials[mesh.material];
        render::Material m;
        m.base_color = src.base_color;
        m.emissive = src.emissive;
        m.emissive_strength = src.emissive_strength;
        m.metallic = src.metallic;
        m.roughness = src.roughness;
        m.normal_scale = src.normal_scale;
        m.alpha_cutoff = src.alpha_cutoff;
        m.clearcoat = src.clearcoat;
        m.clearcoat_roughness = src.clearcoat_roughness;
        m.double_sided = src.double_sided;
        m.unlit = src.unlit;
        m.base_color_texture = tex_or(src.base_color_texture);
        m.metallic_roughness_texture = tex_or(src.metallic_roughness_texture);
        m.occlusion_strength = m.metallic_roughness_texture ? src.occlusion_strength : 0.0f;
        m.normal_texture = tex_or(src.normal_texture);
        m.emissive_texture = tex_or(src.emissive_texture);
        gpu->mesh_materials.push_back(m);
        // GPU owns the geometry now; keep only counts/bounds-relevant metadata on the CPU side
        // (and base vertices of meshes with morph targets, which are morphed on the CPU).
        if (mesh.data.morphs.empty()) {
            mesh.data.vertices.clear();
            mesh.data.vertices.shrink_to_fit();
        }
        mesh.data.indices.clear();
        mesh.data.indices.shrink_to_fit();
    }
    model.textures.clear();
    for (const auto& n : model.nodes)
        if (!n.morph_weights.empty()) gpu->node_morph_weights.emplace_back(n.name, n.morph_weights);
    for (const auto& n : model.nodes) gpu->skeleton.push_back({.name = n.name, .parent = n.parent, .rest = {n.translation, n.rotation, n.scale}});
    gpu->cpu = std::move(model);
    const GpuModel* raw = gpu.get();
    models_[uuid] = std::move(gpu);
    make_resident(uuid, bytes - std::min(bytes, skipped_bytes));
    return raw;
}

const render::GpuMesh* GpuAssetCache::procedural(const scene::ProceduralMesh& mesh) {
    auto [it, inserted] = procedural_.try_emplace(mesh.id);
    ProceduralEntry& entry = it->second;
    entry.last_used = procedural_clock_;
    if (!inserted) return entry.failed ? nullptr : &entry.mesh;
    if (mesh.positions.empty() || mesh.indices.empty()) {
        entry.failed = true;
        return nullptr;
    }
    const render::MeshData data = procedural_mesh_data(mesh); // shared with lightmap baking
    auto g = ctx_->create_mesh(data, "procedural");
    if (!g) {
        VICTUS_LOG_ERROR("runtime", "procedural mesh {}: {}", mesh.id, g.error().message);
        entry.failed = true;
        return nullptr;
    }
    entry.mesh = *g;
    return &entry.mesh;
}

void GpuAssetCache::drop_morphed(const Uuid* model) {
    for (auto it = morphed_.begin(); it != morphed_.end();) {
        if (model && it->second.model != *model) {
            ++it;
            continue;
        }
        if (it->second.gpu.valid()) ctx_->destroy_vertex_variant(it->second.gpu);
        it = morphed_.erase(it);
    }
}

const render::GpuMesh* GpuAssetCache::morphed(std::uint64_t key, const Uuid& model_id, const GpuModel& model, std::uint32_t mesh,
                                              std::span<const float> weights) {
    if (mesh >= model.meshes.size() || mesh >= model.cpu.meshes.size()) return nullptr;
    const render::MeshData& base = model.cpu.meshes[mesh].data;
    if (base.morphs.empty() || base.vertices.size() != model.meshes[mesh].vertex_count) return nullptr;
    // Nothing weighted moves a vertex of this primitive (all zero, or only targets other primitives
    // of the glTF mesh use): draw the base mesh, without copying it (ADR-0222).
    if (!render::morphs_active(base, weights)) {
        if (auto old = morphed_.find(key); old != morphed_.end()) {
            if (old->second.gpu.valid()) ctx_->destroy_vertex_variant(old->second.gpu);
            morphed_.erase(old);
        }
        return nullptr;
    }
    auto [it, inserted] = morphed_.try_emplace(key);
    MorphEntry& e = it->second;
    e.last_used = procedural_clock_;
    if (!inserted && (e.model != model_id || e.mesh != mesh)) {
        if (e.gpu.valid()) ctx_->destroy_vertex_variant(e.gpu);
        e = MorphEntry{.last_used = procedural_clock_};
    }
    const std::size_t n = std::min(weights.size(), base.morphs.size());
    const bool same = e.gpu.valid() && e.weights.size() == n && std::equal(e.weights.begin(), e.weights.end(), weights.begin());
    if (same) return &e.gpu;
    if (!render::apply_morphs(base, weights.first(n), morph_scratch_)) {
        if (e.gpu.valid()) ctx_->destroy_vertex_variant(e.gpu); // all zero: the base mesh is exact
        morphed_.erase(it);
        return nullptr;
    }
    math::Aabb bounds;
    for (const render::Vertex3D& v : morph_scratch_) bounds.expand(v.position);
    if (e.gpu.valid()) {
        if (auto st = ctx_->update_vertex_variant(e.gpu, morph_scratch_); !st) {
            VICTUS_LOG_ERROR("runtime", "morph upload: {}", st.error().message);
            return nullptr;
        }
    } else {
        auto g = ctx_->create_vertex_variant(model.meshes[mesh], morph_scratch_, "morphed");
        if (!g) {
            VICTUS_LOG_ERROR("runtime", "morph upload: {}", g.error().message);
            morphed_.erase(it);
            return nullptr;
        }
        e.gpu = *g;
        e.model = model_id;
        e.mesh = mesh;
    }
    e.gpu.bounds = bounds;
    e.weights.assign(weights.begin(), weights.begin() + static_cast<std::ptrdiff_t>(n));
    ++morph_uploads_;
    return &e.gpu;
}

void GpuAssetCache::collect_procedural(std::uint32_t keep_frames) {
    ++procedural_clock_;
    for (auto it = morphed_.begin(); it != morphed_.end();) {
        if (it->second.last_used + keep_frames < procedural_clock_) {
            if (it->second.gpu.valid()) ctx_->destroy_vertex_variant(it->second.gpu);
            it = morphed_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = procedural_.begin(); it != procedural_.end();) {
        if (it->second.last_used + keep_frames < procedural_clock_) {
            if (it->second.mesh.valid()) ctx_->destroy(it->second.mesh);
            it = procedural_.erase(it);
        } else {
            ++it;
        }
    }
}

const render::GpuMesh* GpuAssetCache::primitive(std::string_view name) {
    const std::string key(name);
    if (const auto it = primitives_.find(key); it != primitives_.end()) return &it->second;
    render::MeshData data = primitive_mesh_data(key); // shared with lightmap baking (same fingerprint)
    if (data.indices.empty()) return nullptr;
    auto g = ctx_->create_mesh(data, key);
    if (!g) {
        VICTUS_LOG_ERROR("runtime", "primitive '{}': {}", key, g.error().message);
        return nullptr;
    }
    return &primitives_.emplace(key, *g).first->second;
}

rhi::TextureHandle GpuAssetCache::lightmap(std::string_view path) {
    const std::string key(path);
    if (const auto it = lightmaps_.find(key); it != lightmaps_.end()) return it->second.texture;
    LightmapEntry& entry = lightmaps_[key]; // cached even when it fails, so a bad file is read once
    if (!db_) {
        VICTUS_LOG_ERROR("runtime", "lightmap '{}': no asset database to resolve it", key);
        return {};
    }
    // Scene data names the file: keep it inside the project (no absolute paths, no "..").
    const fs::Path rel(key);
    if (rel.is_absolute() || rel.has_root_name() || std::any_of(rel.begin(), rel.end(), [](const fs::Path& c) { return c == ".."; })) {
        VICTUS_LOG_ERROR("runtime", "lightmap '{}': must be a project-relative path", key);
        return {};
    }
    auto bytes = fs::read_binary(db_->project_root() / key, 1ull << 30);
    if (!bytes) {
        VICTUS_LOG_WARN("runtime", "lightmap '{}': {} (bake lighting to create it)", key, bytes.error().message);
        return {};
    }
    auto file = render::decode_lightmap_file(*bytes);
    if (!file) {
        VICTUS_LOG_ERROR("runtime", "lightmap '{}': {}", key, file.error().message);
        return {};
    }
    auto tex = ctx_->create_lightmap_texture(file->atlas, key);
    if (!tex) {
        VICTUS_LOG_ERROR("runtime", "lightmap '{}': {}", key, tex.error().message);
        return {};
    }
    entry.texture = *tex;
    if (!file->direction.empty()) {
        auto dir = ctx_->create_lightmap_direction_texture(file->direction, key + "#direction");
        if (dir) entry.direction = *dir;
        else VICTUS_LOG_WARN("runtime", "lightmap '{}': {} (normal maps will not shade baked light)", key, dir.error().message);
    }
    entry.shadowmask_direction = file->shadowmask_direction;
    if (!file->probes.empty()) {
        render::IrradianceGrid grid;
        grid.bounds = file->probes.bounds;
        grid.counts = file->probes.counts;
        auto vol = render::IrradianceVolume::create(*ctx_, grid);
        if (vol && (*vol)->grid().counts == grid.counts) {
            for (std::uint32_t i = 0; i < file->probes.probe_count(); ++i) {
                if (!file->probes.valid[i]) continue;
                render::SphericalHarmonics9 sh;
                for (std::size_t k = 0; k < 9; ++k) sh.coefficients[k] = file->probes.sh[i][k];
                (*vol)->set_probe(i, sh);
                // Probe sun occlusion (ADR-0136); only meaningful with the shadowmask direction.
                if (file->probes.sun.size() == file->probes.sh.size() && math::length(file->shadowmask_direction) > 0.5f)
                    (*vol)->set_probe_sun(i, file->probes.sun[i]);
            }
            if (const Status st = (*vol)->upload(); st.ok()) entry.probes = std::move(*vol);
            else VICTUS_LOG_WARN("runtime", "lightmap '{}' probes: {}", key, st.error().message);
        } else {
            VICTUS_LOG_WARN("runtime", "lightmap '{}': {} (dynamic objects keep sky ambient)", key,
                            vol ? std::string("probe grid rejected") : vol.error().message);
        }
    }
    entry.entries = std::move(file->entries);
    return entry.texture;
}

const render::IrradianceVolume* GpuAssetCache::lightmap_probes(std::string_view path) {
    if (!lightmap(path).valid()) return nullptr;
    const auto& probes = lightmaps_.find(std::string(path))->second.probes;
    return probes && probes->valid_count() > 0 ? probes.get() : nullptr;
}

math::Vec3 GpuAssetCache::lightmap_shadowmask_direction(std::string_view path) {
    if (!lightmap(path).valid()) return {0, 0, 0};
    return lightmaps_.find(std::string(path))->second.shadowmask_direction;
}

rhi::TextureHandle GpuAssetCache::lightmap_direction(std::string_view path) {
    if (!lightmap(path).valid()) return {};
    return lightmaps_.find(std::string(path))->second.direction;
}

GpuAssetCache::LightmappedMesh GpuAssetCache::lightmapped_mesh(std::string_view path, scene::Scene& scene, scene::Entity entity) {
    if (!lightmap(path).valid()) return {};
    LightmapEntry& lm = lightmaps_.find(std::string(path))->second;
    const Uuid id = scene.uuid(entity);
    if (id.is_nil()) return {};
    auto [it, inserted] = lm.meshes.try_emplace(id);
    if (!inserted) return it->second ? *it->second : LightmappedMesh{};
    const auto e = std::find_if(lm.entries.begin(), lm.entries.end(), [&](const render::LightmapFileEntry& x) { return x.entity == id; });
    if (e == lm.entries.end()) {
        VICTUS_LOG_WARN("runtime", "lightmap '{}' has no bake for entity {}: bake lighting again", path, id.to_string());
        return {};
    }
    auto data = static_mesh_data(scene, entity, db_);
    if (!data) {
        VICTUS_LOG_WARN("runtime", "lightmap '{}', entity {}: {}", path, id.to_string(), data.error().message);
        return {};
    }
    const auto* wt = scene.world().get<scene::WorldTransform>(entity);
    if (!wt || scene_lightmap_fingerprint(*data, wt->matrix) != e->mesh_fingerprint) {
        VICTUS_LOG_WARN("runtime", "lightmap '{}': entity {} moved or its mesh changed since it was baked; bake lighting again", path,
                        id.to_string());
        return {};
    }
    // A congruent copy already uploaded (same geometry, uvs shifted by a constant): draw that mesh
    // with this instance's offset so the copies batch (ADR-0135).
    // Every vertex attribute counts (texture uvs, tangents, colours), not only the geometry.
    std::uint64_t geometry = 1469598103934665603ull;
    const auto hash_bytes = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) geometry = (geometry ^ b[i]) * 1099511628211ull;
    };
    hash_bytes(data->vertices.data(), data->vertices.size() * sizeof(render::Vertex3D));
    hash_bytes(data->indices.data(), data->indices.size() * sizeof(std::uint32_t));
    for (const auto& sh : lm.shared) {
        if (sh->geometry != geometry) continue;
        if (const auto off = render::lightmap_uv_offset(lm.entries[sh->entry].unwrap, e->unwrap)) {
            it->second = LightmappedMesh{&sh->gpu, *off};
            return *it->second;
        }
    }
    auto g = ctx_->create_lightmapped_mesh(*data, e->unwrap, "lightmapped");
    if (!g) {
        VICTUS_LOG_ERROR("runtime", "lightmap '{}', entity {}: {}", path, id.to_string(), g.error().message);
        return {};
    }
    auto sh = std::make_unique<LightmapEntry::Shared>();
    sh->gpu = *g;
    sh->geometry = geometry;
    sh->entry = static_cast<std::size_t>(e - lm.entries.begin());
    it->second = LightmappedMesh{&sh->gpu, {}};
    lm.shared.push_back(std::move(sh));
    return *it->second;
}

std::size_t GpuAssetCache::lightmapped_mesh_count() const noexcept {
    std::size_t n = 0;
    for (const auto& [path, lm] : lightmaps_) n += lm.shared.size();
    return n;
}

void GpuAssetCache::evict_lightmaps() {
    for (auto& [path, lm] : lightmaps_) {
        if (lm.texture.valid()) ctx_->device().destroy(lm.texture);
        if (lm.direction.valid()) ctx_->device().destroy(lm.direction);
        lm.probes.reset();
        for (auto& sh : lm.shared) ctx_->destroy(sh->gpu);
    }
    lightmaps_.clear();
}

static_assert(static_cast<int>(gameplay::EnvironmentComponent::Tonemapper::Reinhard) ==
              static_cast<int>(render::Tonemapper::Reinhard));
static_assert(static_cast<int>(gameplay::EnvironmentComponent::Tonemapper::Neutral) ==
              static_cast<int>(render::Tonemapper::Neutral));

void apply_environment(const gameplay::EnvironmentComponent& e, render::Environment& env, render::DirectionalLight& sun) {
    env.sky_zenith = e.sky_zenith;
    env.sky_horizon = e.sky_horizon;
    env.ground = e.ground;
    env.ambient_intensity = std::max(e.ambient_intensity, 0.0f);
    env.draw_sky = e.draw_sky;
    env.clear_color = e.clear_color;
    env.tonemap = true;
    env.tonemapper = static_cast<render::Tonemapper>(e.tonemapper);
    env.exposure = std::max(e.exposure, 0.0f);
    env.exposure_compensation = e.exposure_compensation;
    env.auto_exposure = e.auto_exposure;
    env.auto_exposure_min_ev = e.auto_exposure_min_ev;
    env.auto_exposure_max_ev = std::max(e.auto_exposure_min_ev, e.auto_exposure_max_ev);
    env.saturation = std::max(e.saturation, 0.0f);
    env.contrast = std::max(e.contrast, 0.0f);
    env.vignette = std::clamp(e.vignette, 0.0f, 1.0f);
    env.lens_distortion = std::clamp(e.lens_distortion, -1.0f, 1.0f);
    env.chromatic_aberration = std::clamp(e.chromatic_aberration, 0.0f, 1.0f);
    env.film_grain = std::clamp(e.film_grain, 0.0f, 1.0f);
    env.bloom_intensity = std::max(e.bloom_intensity, 0.0f);
    env.bloom_threshold = std::max(e.bloom_threshold, 0.0f);
    env.ao_intensity = std::max(e.ao_intensity, 0.0f);
    env.ao_radius = std::max(e.ao_radius, 0.0f);
    env.contact_shadow_length = std::isfinite(e.contact_shadow_length) ? std::clamp(e.contact_shadow_length, 0.0f, 10.0f) : 0.0f;
    env.contact_shadow_thickness =
        std::isfinite(e.contact_shadow_thickness) ? std::clamp(e.contact_shadow_thickness, 0.01f, 10.0f) : 0.25f;
    sun.shadow_distance = std::max(e.shadow_distance, 1.0f);
    env.fog_mode = static_cast<render::FogMode>(e.fog);
    env.fog_color = e.fog_color;
    env.fog_start = std::max(e.fog_start, 0.0f);
    env.fog_end = std::max(e.fog_end, env.fog_start);
    env.fog_density = std::max(e.fog_density, 0.0f);
    env.fog_max_opacity = std::clamp(e.fog_max_opacity, 0.0f, 1.0f);
    env.coverage_amount = std::clamp(e.coverage_amount, 0.0f, 1.0f);
    env.coverage_color = e.coverage_color;
    env.coverage_roughness = e.coverage_roughness;
    env.coverage_slope = e.coverage_slope;
    // environment_map (an asset) is resolved by extract_scene through the GpuAssetCache.
    env.environment_intensity = std::isfinite(e.environment_intensity) ? std::max(e.environment_intensity, 0.0f) : 1.0f;
    env.environment_rotation = std::isfinite(e.environment_rotation) ? math::radians(e.environment_rotation) : 0.0f;
    env.sky_blur = std::isfinite(e.sky_blur) ? std::clamp(e.sky_blur, 0.0f, 1.0f) : 0.0f;
    const auto fin = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
    render::VolumetricFog& vf = env.volumetric_fog;
    vf.enabled = e.volumetric_fog;
    vf.density = std::max(fin(e.volumetric_density, 0.0f), 0.0f);
    vf.height = fin(e.volumetric_height, 0.0f);
    vf.height_falloff = std::max(fin(e.volumetric_falloff, 0.0f), 0.0f);
    vf.albedo = e.volumetric_albedo;
    vf.anisotropy = std::clamp(fin(e.volumetric_anisotropy, 0.0f), -0.95f, 0.95f);
    vf.distance = std::clamp(fin(e.volumetric_distance, 80.0f), 8.0f, 1000.0f);
    vf.sun_intensity = std::max(fin(e.volumetric_sun, 1.0f), 0.0f);
    vf.ambient_intensity = std::max(fin(e.volumetric_ambient, 1.0f), 0.0f);
    vf.local_light_intensity = std::max(fin(e.volumetric_local_lights, 1.0f), 0.0f);
    vf.temporal = std::clamp(fin(e.volumetric_temporal, 0.9f), 0.0f, 0.98f);
    vf.noise = std::clamp(fin(e.volumetric_noise, 0.0f), 0.0f, 1.0f);
    vf.noise_scale = std::clamp(fin(e.volumetric_noise_scale, 6.0f), 0.25f, 1000.0f);
    vf.wind = {fin(e.volumetric_wind.x, 0.0f), fin(e.volumetric_wind.y, 0.0f), fin(e.volumetric_wind.z, 0.0f)};
    vf.self_shadow = std::clamp(fin(e.volumetric_self_shadow, 0.0f), 0.0f, 4.0f);
    render::VolumetricClouds& vc = env.clouds;
    vc.enabled = e.clouds;
    vc.coverage = std::clamp(fin(e.cloud_coverage, 0.5f), 0.0f, 1.0f);
    vc.density = std::max(fin(e.cloud_density, 0.03f), 0.0f);
    vc.bottom = fin(e.cloud_bottom, 1500.0f);
    vc.top = vc.bottom + std::max(fin(e.cloud_thickness, 2000.0f), 10.0f);
    vc.shape_size = std::max(fin(e.cloud_size, 4000.0f), 10.0f);
    vc.detail_size = vc.shape_size * 0.15f;
    vc.detail = std::clamp(fin(e.cloud_detail, 0.35f), 0.0f, 1.0f);
    vc.wind = {fin(e.cloud_wind.x, 0.0f), fin(e.cloud_wind.y, 0.0f), fin(e.cloud_wind.z, 0.0f)};
    vc.anisotropy = std::clamp(fin(e.cloud_anisotropy, 0.6f), 0.0f, 0.95f);
    vc.sun_intensity = std::max(fin(e.cloud_sun, 1.0f), 0.0f);
    vc.ambient_intensity = std::max(fin(e.cloud_ambient, 1.0f), 0.0f);
    vc.distance = std::clamp(fin(e.cloud_distance, 30000.0f), 100.0f, 200000.0f);
    vc.shadow_strength = std::clamp(fin(e.cloud_shadows, 1.0f), 0.0f, 1.0f);
    render::Precipitation& pr = env.precipitation;
    pr.type = static_cast<render::PrecipitationType>(e.precipitation);
    pr.intensity = std::clamp(fin(e.precipitation_intensity, 1.0f), 0.0f, 1.0f);
    pr.wind = {fin(e.precipitation_wind.x, 0.0f), fin(e.precipitation_wind.y, 0.0f), fin(e.precipitation_wind.z, 0.0f)};
    pr.radius = std::clamp(fin(e.precipitation_radius, 15.0f), 2.0f, 100.0f);
    pr.size = std::clamp(fin(e.precipitation_size, 0.0f), 0.0f, 1.0f);
    pr.fall_speed = std::clamp(fin(e.precipitation_speed, 0.0f), 0.0f, 100.0f);
    pr.color = e.precipitation_color;
    pr.opacity = std::clamp(fin(e.precipitation_opacity, 0.0f), 0.0f, 1.0f);
    pr.brightness = std::max(fin(e.precipitation_brightness, 1.0f), 0.0f);
    env.wetness = std::clamp(fin(e.wetness, 0.0f), 0.0f, 1.0f);
    env.puddles = std::clamp(fin(e.puddles, 0.0f), 0.0f, 1.0f);
    env.puddle_size = std::clamp(fin(e.puddle_size, 3.0f), 0.25f, 100.0f);
    pr.ripples = std::clamp(fin(e.precipitation_ripples, 1.0f), 0.0f, 1.0f);
    pr.splashes = std::clamp(fin(e.precipitation_splashes, 1.0f), 0.0f, 1.0f);
}
static_assert(static_cast<int>(gameplay::EnvironmentComponent::Precipitation::Snow) == static_cast<int>(render::PrecipitationType::Snow));
static_assert(static_cast<int>(gameplay::EnvironmentComponent::Fog::ExponentialSquared) ==
              static_cast<int>(render::FogMode::ExponentialSquared));

namespace {

// Joint entities of skeletons, cached per (skeleton root, model) in a scene resource and
// re-resolved when an entity dies or is renamed.
struct SkinBindings {
    struct Entry {
        Entity root;
        Uuid model;
        std::vector<Entity> nodes;
    };
    std::unordered_map<std::uint64_t, Entry> entries;
};

const std::vector<Entity>& skeleton_nodes(Scene& scene, Entity root, const Uuid& model_uuid, const GpuModel& model) {
    auto* res = scene.world().resource<SkinBindings>();
    if (!res) res = &scene.world().set_resource<SkinBindings>();
    if (res->entries.size() > 4096) res->entries.clear(); // bound memory for scenes that churn characters
    const std::uint64_t key = root.bits() * 0x9e3779b97f4a7c15ull ^ UuidHash{}(model_uuid);
    auto& entry = res->entries[key];
    bool valid = entry.root == root && entry.model == model_uuid && entry.nodes.size() == model.skeleton.size();
    for (std::size_t i = 0; valid && i < entry.nodes.size(); ++i) {
        const Entity e = entry.nodes[i];
        if (e == Entity{}) continue;
        const auto* n = scene.world().alive(e) ? scene.world().get<Name>(e) : nullptr;
        valid = n && n->value == model.skeleton[i].name && scene.is_descendant(e, root);
    }
    if (!valid) {
        entry.root = root;
        entry.model = model_uuid;
        entry.nodes = animation::bind_nodes(scene, root, model.skeleton);
    }
    return entry.nodes;
}

} // namespace

namespace {
// Captures stale probes (within the per-frame budget) and hands every captured probe to the
// renderer. Captures see the scene without probes (one reflection bounce).
void apply_reflection_probes(Scene& scene, GpuAssetCache& assets, render::SceneRenderer& renderer,
                             const SceneViewSettings& settings, ExtractStats& stats) {
    auto& w = scene.world();
    struct Found {
        std::uint64_t key;
        gameplay::ReflectionProbeComponent probe;
        math::Vec3 center, capture;
    };
    std::vector<Found> found;
    w.each<gameplay::ReflectionProbeComponent, WorldTransform>(
        [&](Entity e, const gameplay::ReflectionProbeComponent& p, const WorldTransform& wt) {
            const math::Vec3 c = wt.matrix.transform_point({});
            found.push_back({e.bits(), p, c, c + p.capture_offset});
        });
    auto& captures = assets.probe_captures();
    for (auto& [key, entry] : captures) entry.seen = false;
    std::uint32_t budget = assets.probe_captures_per_frame();
    for (const Found& f : found) {
        auto& entry = captures[f.key];
        entry.seen = true;
        const std::uint32_t res = std::clamp(f.probe.resolution, 16u, 512u);
        const bool stale = !entry.map || entry.revision != f.probe.revision || entry.resolution != res ||
                           math::length(entry.position - f.capture) > 0.01f;
        if (stale && budget > 0) {
            --budget;
            auto r = assets.probe_renderer();
            if (!r) {
                VICTUS_LOG_ERROR("runtime", "reflection probe renderer: {}", r.error().message);
                continue;
            }
            assets.set_capturing_probes(true);
            auto img = render::capture_equirect(assets.context(), **r, f.capture, res,
                                                [&](render::SceneRenderer& pr, const render::Camera3D& cam) {
                                                    (void)extract_scene(scene, assets, pr, settings, &cam);
                                                });
            assets.set_capturing_probes(false);
            if (!img) {
                VICTUS_LOG_ERROR("runtime", "reflection probe capture: {}", img.error().message);
                continue;
            }
            render::EnvironmentBakeSettings bake = assets.environment_bake();
            bake.cube_size = res;
            auto map = render::EnvironmentMap::create(assets.context(), *img, bake);
            if (!map) {
                VICTUS_LOG_ERROR("runtime", "reflection probe bake: {}", map.error().message);
                continue;
            }
            entry.map = std::move(*map);
            entry.position = f.capture;
            entry.revision = f.probe.revision;
            entry.resolution = res;
            assets.note_probe_capture();
        }
        if (!entry.map) continue;
        render::ReflectionProbe rp;
        rp.map = entry.map.get();
        const math::Vec3 half = math::Vec3{std::abs(f.probe.size.x), std::abs(f.probe.size.y), std::abs(f.probe.size.z)} * 0.5f;
        rp.box.min = f.center - half;
        rp.box.max = f.center + half;
        rp.position = entry.position;
        rp.intensity = std::isfinite(f.probe.intensity) ? std::max(f.probe.intensity, 0.0f) : 1.0f;
        renderer.add_reflection_probe(rp);
        ++stats.reflection_probes;
    }
    std::erase_if(captures, [](const auto& kv) { return !kv.second.seen; });
}

// Advances stale irradiance bakes (within the per-frame probe budget) and binds the volume that
// contains the camera (the smallest one when several do), else the nearest. Bake captures see the
// scene without probes or volumes; bounce passes add the volume being baked (ADR-0056).
void apply_irradiance_volumes(Scene& scene, GpuAssetCache& assets, render::SceneRenderer& renderer,
                              const SceneViewSettings& settings, math::Vec3 eye, ExtractStats& stats,
                              const render::IrradianceVolume* baked) {
    auto& w = scene.world();
    auto& bakes = assets.irradiance_bakes();
    for (auto& [key, entry] : bakes) entry.seen = false;
    // Tiers without irradiance volumes (Low) never draw hand-placed ones, so they are not captured
    // either; the lightmap's baked probes below still light dynamic objects (ADR-0138).
    std::uint32_t budget = renderer.settings().irradiance_volumes ? assets.irradiance_probes_per_frame() : 0u;
    const render::IrradianceVolume* best = nullptr;
    float best_score = 0.0f, best_intensity = 1.0f, best_offset = 0.0f;
    bool best_inside = false;
    w.each<gameplay::IrradianceVolumeComponent, WorldTransform>(
        [&](Entity e, const gameplay::IrradianceVolumeComponent& v, const WorldTransform& wt) {
            auto& entry = bakes[e.bits()];
            entry.seen = true;
            const math::Vec3 c = wt.matrix.transform_point({});
            const auto finite = [](float f, float d) { return std::isfinite(f) ? std::abs(f) : d; };
            const math::Vec3 size{std::max(finite(v.size.x, 1.0f), 0.1f), std::max(finite(v.size.y, 1.0f), 0.1f),
                                  std::max(finite(v.size.z, 1.0f), 0.1f)};
            render::IrradianceGrid grid;
            grid.bounds = {c - size * 0.5f, c + size * 0.5f};
            grid.counts = render::irradiance_counts_for(size, finite(v.spacing, 2.5f));
            grid = grid.sanitized();
            const std::uint32_t res = std::clamp(v.resolution, 16u, 128u), bounces = std::clamp(v.bounces, 1u, 4u);
            const float offset = std::clamp(finite(v.normal_offset, 0.0f), 0.0f, 4.0f);
            const float intensity = std::isfinite(v.intensity) ? std::max(v.intensity, 0.0f) : 1.0f;
            const bool moved = math::length(entry.grid.bounds.min - grid.bounds.min) > 0.01f ||
                               math::length(entry.grid.bounds.max - grid.bounds.max) > 0.01f || entry.grid.counts != grid.counts;
            const bool stale = moved || entry.revision != v.revision || entry.resolution != res || entry.bounces != bounces ||
                               std::abs(entry.normal_offset - offset) > 1e-4f;
            if (stale || (!entry.active && !entry.pending)) {
                if (moved) entry.active.reset(); // old probes would light the wrong place
                entry.grid = grid;
                entry.revision = v.revision;
                entry.resolution = res;
                entry.bounces = bounces;
                entry.normal_offset = offset;
                entry.baker.reset();
                entry.refresher.reset();
                auto vol = render::IrradianceVolume::create(assets.context(), grid);
                if (!vol) {
                    VICTUS_LOG_ERROR("runtime", "irradiance volume: {}", vol.error().message);
                    entry.pending.reset();
                    return;
                }
                entry.pending = std::move(*vol);
            }
            if (entry.pending && budget > 0) {
                auto r = assets.probe_renderer();
                if (!r) {
                    VICTUS_LOG_ERROR("runtime", "irradiance probe renderer: {}", r.error().message);
                } else {
                    render::IrradianceBakeSettings bs;
                    bs.face_size = res;
                    bs.bounces = bounces;
                    bs.intensity = intensity;
                    bs.normal_offset = offset;
                    const std::uint32_t before = entry.baker.probes_baked();
                    assets.set_capturing_probes(true);
                    const Status st = entry.baker.step(assets.context(), **r, *entry.pending,
                                                       [&](render::SceneRenderer& pr, const render::Camera3D& cam) {
                                                           (void)extract_scene(scene, assets, pr, settings, &cam);
                                                       },
                                                       bs, budget);
                    assets.set_capturing_probes(false);
                    const std::uint32_t done = entry.baker.probes_baked() - before;
                    budget -= std::min(budget, done);
                    stats.irradiance_probes_baked += done;
                    if (!st.ok()) {
                        VICTUS_LOG_ERROR("runtime", "irradiance bake: {}", st.error().message);
                        entry.pending.reset();
                    } else if (entry.baker.done()) {
                        entry.active = std::move(entry.pending);
                        assets.note_irradiance_bake();
                    }
                }
            }
            // Dynamic volumes keep refreshing the finished bake (ADR-0057).
            if (v.dynamic && entry.active && !entry.pending && budget > 0 && renderer.settings().dynamic_gi) {
                if (auto r = assets.probe_renderer()) {
                    render::IrradianceBakeSettings bs;
                    bs.face_size = res;
                    bs.intensity = intensity;
                    bs.normal_offset = offset;
                    const std::uint64_t before = entry.refresher.probes_refreshed();
                    assets.set_capturing_probes(true);
                    const Status st = entry.refresher.step(assets.context(), **r, *entry.active,
                                                           [&](render::SceneRenderer& pr, const render::Camera3D& cam) {
                                                               (void)extract_scene(scene, assets, pr, settings, &cam);
                                                           },
                                                           bs, budget, v.dynamic_blend);
                    assets.set_capturing_probes(false);
                    const auto done = static_cast<std::uint32_t>(entry.refresher.probes_refreshed() - before);
                    budget -= std::min(budget, done);
                    stats.irradiance_probes_refreshed += done;
                    if (!st.ok()) VICTUS_LOG_ERROR("runtime", "irradiance refresh: {}", st.error().message);
                }
            }
            // While nothing has finished yet, the partially baked first pass already helps.
            const render::IrradianceVolume* shown = entry.active ? entry.active.get() : entry.pending.get();
            if (!shown || shown->valid_count() == 0) return;
            ++stats.irradiance_volumes;
            const math::Vec3 lo = grid.bounds.min, hi = grid.bounds.max;
            const bool inside = eye.x >= lo.x && eye.y >= lo.y && eye.z >= lo.z && eye.x <= hi.x && eye.y <= hi.y && eye.z <= hi.z;
            const math::Vec3 d{std::max({lo.x - eye.x, 0.0f, eye.x - hi.x}), std::max({lo.y - eye.y, 0.0f, eye.y - hi.y}),
                               std::max({lo.z - eye.z, 0.0f, eye.z - hi.z})};
            // Inside volumes rank by size (smaller = more detailed), outside ones after them by distance.
            const float score = inside ? size.x * size.y * size.z : math::length(d);
            if (!best || (inside && !best_inside) || (inside == best_inside && score < best_score)) {
                best = shown;
                best_inside = inside;
                best_score = score;
                best_intensity = intensity;
                best_offset = offset;
            }
        });
    std::erase_if(bakes, [](const auto& kv) { return !kv.second.seen; });
    // Baked lightmap probes (ADR-0133) cover the whole baked scene: a hand-placed volume around
    // the camera is more specific and wins; otherwise the baked probes light dynamic objects.
    if (baked && (!best || !best_inside)) {
        const float offset = std::min(0.25f, 0.2f * std::min({baked->grid().spacing().x, baked->grid().spacing().y, baked->grid().spacing().z}));
        renderer.set_irradiance_volume(baked, 1.0f, offset, true);
        stats.lightmap_probes = true;
        return;
    }
    if (best) renderer.set_irradiance_volume(best, best_intensity, best_offset);
}
} // namespace


ExtractStats extract_scene(Scene& scene, GpuAssetCache& assets, render::SceneRenderer& renderer,
                           const SceneViewSettings& settings, const render::Camera3D* camera_override) {
    ExtractStats stats;
    assets.collect_procedural();
    (void)assets.pump();
    ecs::World& w = scene.world();
    render::Camera3D camera = settings.fallback_camera;
    if (camera_override) {
        camera = *camera_override;
        stats.camera_found = true;
    } else {
        w.each<CameraComponent, WorldTransform>([&](const CameraComponent& c, const WorldTransform& wt) {
            if (stats.camera_found || !c.primary) return;
            math::Vec3 pos, scale;
            math::Quat rot;
            math::decompose(wt.matrix, pos, rot, scale);
            camera = render::Camera3D::from_transform(pos, rot);
            camera.fov_y = math::radians(c.fov_y_degrees);
            camera.z_near = c.z_near;
            camera.z_far = c.z_far;
            stats.camera_found = true;
        });
    }

    render::DirectionalLight sun;
    bool has_sun = false;
    std::vector<render::PointLight> points;
    std::vector<render::SpotLight> spots;
    std::vector<render::AreaLight> areas;
    // Baked lights (LightComponent::baked) are dropped below when the scene's lightmap loads.
    std::vector<render::PointLight> baked_points;
    std::vector<render::SpotLight> baked_spots;
    std::vector<render::AreaLight> baked_areas;
    // Registers a light's cookie texture with the renderer (ADR-0195, ADR-0201); 0 = none drawn.
    const auto cookie_key = [&](const Uuid& texture) -> std::uint64_t {
        const GpuAssetCache::CookieImage* c = assets.cookie(texture);
        if (!c) {
            ++stats.missing_assets;
            return 0;
        }
        if (renderer.cookie_version(c->key) != c->version) {
            if (auto st = renderer.set_cookie(c->key, c->image, c->version); !st.ok())
                VICTUS_LOG_WARN("runtime", "light cookie {}: {}", texture.to_string(), st.error().message);
        }
        if (!renderer.has_cookie(c->key)) return 0;
        ++stats.light_cookies;
        return c->key;
    };
    w.each<LightComponent, WorldTransform>([&](const LightComponent& l, const WorldTransform& wt) {
        ++stats.lights;
        const bool baked = l.baked && l.type != LightComponent::Type::Directional;
        if (l.type == LightComponent::Type::Directional) {
            if (has_sun) return;
            has_sun = true;
            sun.direction = math::normalize(wt.matrix.transform_vector({0, 0, -1}));
            sun.color = l.color;
            sun.intensity = l.intensity;
            sun.shadows = l.shadows;
            if (!l.cookie.is_nil()) {
                const math::Vec3 x = wt.matrix.transform_vector({1, 0, 0});
                sun.cookie_right = math::length(x) > 1e-6f ? math::normalize(x) : math::Vec3{1, 0, 0};
                sun.cookie_size = std::isfinite(l.cookie_size) && l.cookie_size > 0.0f ? l.cookie_size : 10.0f;
                sun.cookie = cookie_key(l.cookie);
            }
        } else if (l.type == LightComponent::Type::Spot) {
            render::SpotLight sl;
            sl.position = wt.matrix.transform_point({});
            sl.direction = math::normalize(wt.matrix.transform_vector({0, 0, -1}));
            sl.color = l.color;
            sl.intensity = l.intensity;
            sl.range = l.range;
            sl.cast_shadows = l.shadows;
            const float outer = std::isfinite(l.outer_angle_degrees) ? std::clamp(l.outer_angle_degrees, 0.1f, 89.5f) : 30.0f;
            sl.outer_angle = math::radians(outer);
            sl.inner_angle = math::radians(std::isfinite(l.inner_angle_degrees) ? std::clamp(l.inner_angle_degrees, 0.0f, outer) : outer);
            if (!l.cookie.is_nil()) {
                const math::Vec3 x = wt.matrix.transform_vector({1, 0, 0});
                sl.right = math::length(x) > 1e-6f ? math::normalize(x) : math::Vec3{1, 0, 0};
                sl.cookie = cookie_key(l.cookie);
            }
            (baked ? baked_spots : spots).push_back(sl);
        } else if (l.type == LightComponent::Type::Area) {
            render::AreaLight al;
            al.position = wt.matrix.transform_point({});
            al.direction = math::normalize(wt.matrix.transform_vector({0, 0, -1}));
            const math::Vec3 x = wt.matrix.transform_vector({1, 0, 0}), y = wt.matrix.transform_vector({0, 1, 0});
            al.right = math::length(x) > 1e-6f ? math::normalize(x) : math::Vec3{1, 0, 0};
            al.color = l.color;
            al.intensity = l.intensity;
            al.range = l.range;
            // The entity's scale stretches the rectangle like any other child of the transform.
            al.width = l.width * math::length(x);
            al.height = l.height * math::length(y);
            (baked ? baked_areas : areas).push_back(al);
        } else {
            render::PointLight p;
            p.position = wt.matrix.transform_point({});
            p.color = l.color;
            p.intensity = l.intensity;
            p.range = l.range;
            p.cast_shadows = l.shadows;
            p.radius = std::isfinite(l.radius) ? std::max(l.radius, 0.0f) : 0.0f;
            if (!l.cookie.is_nil()) {
                const math::Vec3 x = wt.matrix.transform_vector({1, 0, 0}), f = wt.matrix.transform_vector({0, 0, -1});
                p.right = math::length(x) > 1e-6f ? math::normalize(x) : math::Vec3{1, 0, 0};
                p.forward = math::length(f) > 1e-6f ? math::normalize(f) : math::Vec3{0, 0, -1};
                p.cookie = cookie_key(l.cookie);
            }
            (baked ? baked_points : points).push_back(p);
        }
    });
    if (!has_sun) sun.intensity = 0.0f;
    render::Environment env = settings.environment;
    bool has_env = false;
    std::string lightmap_path; // the scene's loaded lightmap (ADR-0125); empty = none
    w.each<gameplay::EnvironmentComponent>([&](const gameplay::EnvironmentComponent& e) {
        if (has_env) return;
        has_env = true;
        apply_environment(e, env, sun);
        if (!e.lightmap.empty()) {
            env.lightmap = assets.lightmap(e.lightmap);
            if (env.lightmap.valid()) {
                lightmap_path = e.lightmap;
                env.lightmap_direction = assets.lightmap_direction(e.lightmap);
                env.lightmap_shadowmask_direction = assets.lightmap_shadowmask_direction(e.lightmap);
            }
        }
        if (!e.environment_map.is_nil()) {
            if (auto map = assets.environment(e.environment_map)) env.environment_map = *map;
            else ++stats.missing_assets;
        } else if (e.physical_sky) {
            render::AtmosphereSettings a;
            a.sun_direction = -sun.direction;
            const float scale = std::isfinite(e.atmosphere_intensity) ? std::max(e.atmosphere_intensity, 0.0f) : 1.0f;
            a.sun_illuminance = (has_sun ? sun.intensity : 3.0f) * scale;
            a.rayleigh_scale = e.atmosphere_density;
            a.mie_scale = e.atmosphere_haze;
            a.ground_albedo = {e.ground.r, e.ground.g, e.ground.b};
            a.width = assets.environment_bake().cube_size <= 128 ? 64 : 128; // Low tier: ~4x cheaper CPU bake
            if (auto map = assets.atmosphere(a)) {
                env.environment_map = *map;
                env.environment_sun_disc = has_sun && a.sun_direction.y > -0.02f; // no sun disc drawn on the ground at night
                env.environment_rotation = 0.0f; // the sky is baked in world space
                if (e.atmosphere_tints_sun && has_sun) {
                    const math::Vec3 t = render::atmosphere_sun_transmittance(a);
                    sun.color = {sun.color.r * t.x, sun.color.g * t.y, sun.color.b * t.z, sun.color.a};
                }
            } else {
                VICTUS_LOG_ERROR("runtime", "physical sky: {}", map.error().message);
            }
        }
    });
    if (lightmap_path.empty()) { // no lightmap holds them: light them in real time
        points.insert(points.end(), baked_points.begin(), baked_points.end());
        spots.insert(spots.end(), baked_spots.begin(), baked_spots.end());
        areas.insert(areas.end(), baked_areas.begin(), baked_areas.end());
    }
    renderer.begin(camera, sun, env);
    for (const auto& p : points) renderer.add_point_light(p);
    for (const auto& sl : spots) renderer.add_spot_light(sl);
    for (const auto& al : areas) renderer.add_area_light(al);
    if (!assets.capturing_probes()) {
        apply_reflection_probes(scene, assets, renderer, settings, stats);
        apply_irradiance_volumes(scene, assets, renderer, settings, camera.position, stats,
                                 lightmap_path.empty() ? nullptr : assets.lightmap_probes(lightmap_path));
    }
    static_assert(static_cast<int>(gameplay::DecalComponent::Blend::Additive) == static_cast<int>(render::DecalBlend::Additive));
    static_assert(static_cast<int>(gameplay::DecalComponent::Blend::Alpha) == static_cast<int>(render::DecalBlend::Alpha));
    const bool streaming = assets.residency_settings().mip_streaming; // footprints feed GpuAssetCache::stream_mips
    w.each<gameplay::DecalComponent, WorldTransform>([&](const gameplay::DecalComponent& d, const WorldTransform& wt) {
        if (!d.visible) return;
        render::Decal decal;
        const math::Vec3 size{std::max(d.size.x, 0.001f), std::max(d.size.y, 0.001f), std::max(d.size.z, 0.001f)};
        decal.transform = wt.matrix * math::Mat4::scale(size);
        if (!d.texture.is_nil()) {
            if (streaming)
                assets.note_footprint(d.texture, runtime::projected_pixels(camera, math::Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}}.transformed(decal.transform),
                                                                       assets.residency_settings().screen_height));
            auto tex = assets.texture(d.texture);
            if (tex) decal.texture = *tex;
            else if (tex.error().code == ErrorCode::Pending) {
                ++stats.loading_assets; // an untextured decal would flash: wait for it
                return;
            } else ++stats.missing_assets;
            decal.nearest_filter = assets.texture_nearest(d.texture);
        }
        decal.color = d.color;
        decal.blend = static_cast<render::DecalBlend>(std::min<int>(static_cast<int>(d.blend), 2));
        decal.uv = {d.uv_min.x, d.uv_min.y, d.uv_max.x, d.uv_max.y};
        decal.angle_fade = d.angle_fade;
        decal.depth_fade = d.depth_fade;
        decal.sort_order = d.sort_order;
        renderer.add_decal(decal);
        ++stats.decals;
    });

    // Fog volumes (ADR-0098): only while the environment has volumetric fog on.
    if (env.volumetric_fog.enabled)
        w.each<gameplay::FogVolumeComponent, WorldTransform>([&](const gameplay::FogVolumeComponent& f, const WorldTransform& wt) {
            if (!f.visible || !(f.density > 0.0f)) return;
            render::FogVolume fv;
            const math::Vec3 size{std::max(f.size.x, 0.001f), std::max(f.size.y, 0.001f), std::max(f.size.z, 0.001f)};
            fv.transform = wt.matrix * math::Mat4::scale(size);
            fv.density = f.density;
            fv.albedo = f.albedo;
            fv.edge_fade = f.edge_fade;
            renderer.add_fog_volume(fv);
            ++stats.fog_volumes;
        });

    // Water volumes (ADR-0095): the gameplay wave set at the simulated water time, so buoyancy
    // matches; outside play (water time 0) the editor preview animates on wall-clock time.
    {
        const double sim_time = gameplay::water_time(scene);
        double wt = sim_time;
        if (wt <= 0.0)
            wt = std::fmod(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(), 86400.0);
        const auto wave_time = static_cast<float>(std::fmod(wt, 3600.0));
        bool any = false;
        w.each<gameplay::WaterComponent>([&](Entity, const gameplay::WaterComponent& c) { any = any || c.visible; });
        if (any) {
            for (const gameplay::WaterVolume& v : gameplay::water_volumes(scene)) {
                if (!v.water->visible) continue;
                render::WaterSurface ws;
                ws.center = v.center;
                ws.half_extent = v.half_extent;
                const gameplay::WaterWaves waves = sim_time > 0.0 ? v.waves : gameplay::water_waves(*v.water, wt);
                static_assert(gameplay::kWaterWaves == std::tuple_size_v<decltype(ws.waves)>);
                for (std::size_t i = 0; i < waves.size(); ++i)
                    ws.waves[i] = {waves[i].direction, waves[i].k, waves[i].amplitude, waves[i].q, waves[i].phase};
                ws.shallow_color = v.water->shallow_color;
                ws.deep_color = v.water->deep_color;
                ws.clarity = v.water->clarity;
                ws.foam = v.water->foam;
                ws.refraction = v.water->refraction;
                ws.roughness = v.water->roughness;
                ws.resolution = static_cast<std::uint32_t>(std::clamp(v.water->resolution, 16, 512));
                ws.time = wave_time;
                renderer.add_water(ws);
                ++stats.water_surfaces;
            }
        }
    }

    std::vector<math::Mat4> palette;
    std::vector<float> corrective_weights; // morph weights with corrective shapes added (ADR-0220)
    const auto apply_overrides = [&](Entity entity, render::Material& m) {
        const auto* o = w.get<gameplay::MaterialOverridesComponent>(entity);
        if (!o) return;
        m.uv_offset = {m.uv_offset.x + o->uv_offset.x + o->scrolled.x, m.uv_offset.y + o->uv_offset.y + o->scrolled.y};
        if (o->override_emissive) {
            m.emissive = o->emissive;
            m.emissive_strength = o->emissive_strength;
        }
    };
    w.each<terrain::TerrainComponent, terrain::TerrainRuntime, WorldTransform>(
        [&](Entity entity, const terrain::TerrainComponent& t, const terrain::TerrainRuntime& rt, const WorldTransform& wt) {
            if (!rt.data || rt.chunks.empty()) return;
            render::Material m;
            m.base_color = {1, 1, 1, 1};
            m.metallic = 0.0f;
            m.roughness = std::clamp(t.roughness, 0.0f, 1.0f);
            m.cast_shadows = t.cast_shadows;
            if (auto sh = assets.terrain_shader(); sh) m.surface_shader = *sh;
            rhi::TextureHandle* slots[terrain::kLayerCount] = {&m.base_color_texture, &m.metallic_roughness_texture,
                                                              &m.normal_texture, &m.emissive_texture};
            for (std::uint32_t i = 0; i < terrain::kLayerCount; ++i) {
                const math::Color tint = t.layer_tint(i);
                float tiling = 0.0f;
                if (const Uuid& tex = t.layer_texture(i); !tex.is_nil()) {
                    if (auto h = assets.texture(tex); h) {
                        *slots[i] = *h;
                        tiling = std::max(t.layer_tiling(i), 1e-3f);
                    } else if (h.error().code == ErrorCode::Pending) {
                        ++stats.loading_assets;
                    } else {
                        ++stats.missing_assets;
                    }
                }
                m.shader_params[i] = {tint.r, tint.g, tint.b, tiling};
            }
            // Scale is ignored (the component's size sets the extent): normalized axes.
            const math::Mat4 model = terrain::rigid_matrix(wt.matrix);
            const std::uint64_t motion_base = (entity.bits() + 1) * 0x9E3779B97F4A7C15ull; // distinct per chunk
            for (std::size_t i = 0; i < rt.chunks.size(); ++i) {
                if (!rt.chunks[i]) continue;
                const render::GpuMesh* mesh = assets.procedural(*rt.chunks[i]);
                if (!mesh) continue;
                renderer.submit(*mesh, m, model, (motion_base + i) | 1u);
                ++stats.terrain_chunks;
            }
        });
    // Foliage entities draw their MeshRenderer once per scattered instance. The copies live in a
    // retained instance group per entity and mesh (ADR-0164), rebuilt only when the scatter, the
    // terrain transform, the mesh or the material change; each frame the renderer keeps the
    // clusters within the draw distance and drops shadows beyond the tier's shadow fraction
    // (ADR-0150), per cluster rather than per instance.
    // Small clusters keep the draw-distance edge close to the per-instance one (ADR-0164).
    constexpr std::uint32_t kFoliageCluster = 32;
    const float foliage_scale = renderer.settings().foliage_distance_scale;
    const float foliage_shadow = renderer.settings().foliage_shadow_fraction;
    struct FoliageView {
        const terrain::FoliageRuntime* runtime{nullptr};
        math::Mat4 model{math::Mat4::identity()};
        std::uint64_t revision{0};
        render::SceneRenderer::InstanceGroupDraw limits;
        bool counted{false};
    };
    FoliageView fv;
    const auto foliage_view = [&](Entity entity) -> bool {
        const auto* fr = w.get<terrain::FoliageRuntime>(entity);
        const auto* fc = fr ? w.get<terrain::FoliageComponent>(entity) : nullptr;
        if (!fc) return false;
        fv = FoliageView{};
        const auto* twt = fr->terrain.is_null() ? nullptr : w.get<WorldTransform>(fr->terrain);
        if (!twt || fr->instances == 0) return true;
        fv.runtime = fr;
        fv.model = terrain::rigid_matrix(twt->matrix);
        const float range = (std::isfinite(fc->draw_distance) ? std::max(fc->draw_distance, 0.0f) : 0.0f) * foliage_scale;
        fv.limits.max_distance = range;
        fv.limits.shadow_distance = range * foliage_shadow;
        std::uint64_t h = 1469598103934665603ull;
        const auto hash = [&h](const void* data, std::size_t n) {
            const auto* b = static_cast<const std::uint8_t*>(data);
            for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
        };
        hash(&fr->settings, sizeof(fr->settings));
        hash(&fr->instances, sizeof(fr->instances));
        hash(&fv.model, sizeof(fv.model));
        if (!fr->chunk_built.empty()) hash(fr->chunk_built.data(), fr->chunk_built.size() * sizeof(std::uint64_t));
        fv.revision = h;
        return true;
    };
    w.each<MeshRendererComponent, WorldTransform>([&](Entity entity, const MeshRendererComponent& mr, const WorldTransform& wt) {
        if (!mr.visible) return;
        const std::uint64_t motion_id = entity.bits() + 1; // stable per entity; never 0 for live entities
        const bool scattered = foliage_view(entity);
        // `retain`: the mesh is the asset's own (not a per-entity morphed copy), so an unchanged
        // renderer may be drawn from a retained static group (ADR-0230).
        const auto emit = [&](const render::GpuMesh& base, const render::Material& m, bool retain = true) {
            // Baked lighting (ADR-0125): static renderers draw their lightmapped copy when the
            // scene's lightmap has a current bake for them.
            const GpuAssetCache::LightmappedMesh lmm = mr.lightmapped && !scattered && !lightmap_path.empty()
                                                           ? assets.lightmapped_mesh(lightmap_path, scene, entity)
                                                           : GpuAssetCache::LightmappedMesh{};
            const render::GpuMesh* lm = lmm.mesh;
            const render::GpuMesh& mesh = lm ? *lm : base;
            if (lm) ++stats.lightmapped;
            if (streaming && !mr.material_asset.is_nil())
                assets.note_material_footprint(mr.material_asset,
                                               runtime::projected_pixels(camera, mesh.bounds.transformed(wt.matrix), assets.residency_settings().screen_height));
            if (!scattered) {
                if (retain) renderer.submit_static(motion_id, mesh, m, wt.matrix, lmm.uv_offset);
                else renderer.submit(mesh, m, wt.matrix, motion_id, lmm.uv_offset);
                return;
            }
            if (!fv.runtime) return;
            const terrain::FoliageRuntime& fr = *fv.runtime;
            const std::uint64_t key = (entity.bits() * 0x9E3779B97F4A7C15ull) ^ reinterpret_cast<std::uintptr_t>(&mesh);
            // A mesh replaced at the same address (hot reload) must not reuse stale cluster bounds.
            std::uint64_t revision = fv.revision;
            for (const float f : {mesh.bounds.min.x, mesh.bounds.min.y, mesh.bounds.min.z, mesh.bounds.max.x, mesh.bounds.max.y,
                                  mesh.bounds.max.z}) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &f, sizeof(bits));
                revision = (revision ^ bits) * 1099511628211ull;
            }
            const std::uint32_t group = renderer.cached_instance_group(key, revision, mesh, m, [&](std::vector<math::Mat4>& out) {
                out.reserve(fr.instances);
                for (const auto& list : fr.chunks)
                    for (const math::Mat4& local : list) out.push_back(fv.model * local);
            }, kFoliageCluster);
            const std::uint32_t drawn = renderer.submit_instance_group(group, fv.limits); // static: no motion id
            if (!fv.counted) {
                stats.foliage_instances += drawn;
                fv.counted = true;
            }
        };
        const auto* pm = w.get<scene::ProceduralMeshComponent>(entity);
        if (pm && pm->mesh) {
            const render::GpuMesh* mesh = assets.procedural(*pm->mesh);
            if (!mesh) return; // empty geometry draws nothing
            render::Material m;
            m.base_color = mr.base_color;
            m.metallic = mr.metallic;
            m.roughness = mr.roughness;
            if (!mr.material_asset.is_nil()) {
                if (auto mat = assets.material(mr.material_asset); mat) {
                    m = *mat;
                    m.base_color = {m.base_color.r * mr.base_color.r, m.base_color.g * mr.base_color.g,
                                    m.base_color.b * mr.base_color.b, m.base_color.a * mr.base_color.a};
                } else {
                    ++stats.missing_assets;
                }
            }
            m.cast_shadows = mr.cast_shadows;
            apply_overrides(entity, m);
            emit(*mesh, m);
            ++stats.meshes;
            return;
        }
        if (!mr.mesh_asset.is_nil()) {
            auto model = assets.model(mr.mesh_asset);
            if (!model && model.error().code == ErrorCode::Pending) {
                if (streaming && mr.material_asset.is_nil()) assets.note_reloading_model(mr.mesh_asset, mr.mesh_index, wt.matrix, camera);
                ++stats.loading_assets; // drawn once resident
                return;
            }
            if (!model || mr.mesh_index >= (*model)->meshes.size()) {
                ++stats.missing_assets;
                return;
            }
            render::Material m = (*model)->mesh_materials[mr.mesh_index];
            if (!mr.material_asset.is_nil()) {
                if (auto mat = assets.material(mr.material_asset); mat) m = *mat;
                else ++stats.missing_assets;
            }
            m.base_color = {m.base_color.r * mr.base_color.r, m.base_color.g * mr.base_color.g,
                            m.base_color.b * mr.base_color.b, m.base_color.a * mr.base_color.a};
            m.cast_shadows = mr.cast_shadows;
            apply_overrides(entity, m);
            const GpuModel& gm = **model;
            // Morph targets: the entity's own weights, else its node's (the Animator writes the
            // node entity; multi-primitive nodes draw from child entities), else the mesh defaults.
            const render::GpuMesh* drawn = &gm.meshes[mr.mesh_index];
            if (mr.mesh_index < gm.cpu.meshes.size() && !gm.cpu.meshes[mr.mesh_index].data.morphs.empty()) {
                const auto* h = w.get<scene::Hierarchy>(entity);
                const Entity parent = h ? h->parent : Entity{};
                const auto* mw = w.get<animation::MorphWeightsComponent>(entity);
                if (!mw && parent != Entity{}) mw = w.get<animation::MorphWeightsComponent>(parent);
                std::span<const float> weights = gm.cpu.meshes[mr.mesh_index].data.morph_weights;
                if (mw) {
                    weights = mw->weights;
                } else if (!gm.node_morph_weights.empty()) {
                    // Static glTF node.weights: the node entity is the mesh entity or its parent.
                    const auto* own = w.get<scene::Name>(entity);
                    const auto* up = parent != Entity{} ? w.get<scene::Name>(parent) : nullptr;
                    for (const auto& [node, nw] : gm.node_morph_weights)
                        if ((own && own->value == node) || (up && up->value == node)) {
                            weights = nw;
                            break;
                        }
                }
                // Corrective shapes (ADR-0220): weights driven by the posed joints' local rotations,
                // so clips, IK and edited poses all reach them; evaluated before skinning.
                if (const auto* sk = w.get<animation::SkinnedMeshComponent>(entity);
                    sk && sk->skin < gm.cpu.skins.size() && !gm.cpu.skins[sk->skin].correctives.empty()) {
                    const Entity root = scene.find(sk->skeleton);
                    if (root != Entity{}) {
                        const auto& nodes = skeleton_nodes(scene, root, mr.mesh_asset, gm);
                        const auto local = [&](std::uint32_t node) {
                            const Entity je = node < nodes.size() ? nodes[node] : Entity{};
                            const auto* t = je != Entity{} ? w.get<scene::Transform>(je) : nullptr;
                            return t ? t->rotation : gm.cpu.nodes[node].rotation;
                        };
                        if (assets::corrective_mesh_weights(gm.cpu, gm.cpu.skins[sk->skin], gm.cpu.meshes[mr.mesh_index].data, weights, local,
                                                            corrective_weights) > 0)
                            ++stats.correctives;
                        weights = corrective_weights;
                    }
                }
                if (const render::GpuMesh* mm = assets.morphed(entity.bits(), mr.mesh_asset, gm, mr.mesh_index, weights)) {
                    drawn = mm;
                    ++stats.morphed;
                }
            }
            const auto* skinned = w.get<animation::SkinnedMeshComponent>(entity);
            if (!scattered && skinned && gm.max_joint[mr.mesh_index] >= 0 && skinned->skin < gm.cpu.skins.size()) {
                const assets::ModelSkin& skin = gm.cpu.skins[skinned->skin];
                const Entity root = scene.find(skinned->skeleton);
                if (root != Entity{} && static_cast<std::size_t>(gm.max_joint[mr.mesh_index]) < skin.joints.size()) {
                    const auto& nodes = skeleton_nodes(scene, root, mr.mesh_asset, gm);
                    const math::Mat4 inv_model = math::inverse(wt.matrix);
                    const auto& jb = gm.joint_bounds[mr.mesh_index];
                    palette.resize(skin.joints.size());
                    math::Aabb bounds;
                    for (std::size_t j = 0; j < skin.joints.size(); ++j) {
                        const Entity je = skin.joints[j] < nodes.size() ? nodes[skin.joints[j]] : Entity{};
                        const auto* jw = je != Entity{} ? w.get<WorldTransform>(je) : nullptr;
                        // Unbound joints keep the bind pose relative to the mesh (identity skin matrix).
                        const math::Mat4 world_skin = jw ? jw->matrix * skin.inverse_bind[j] : wt.matrix;
                        palette[j] = jw ? inv_model * world_skin : math::Mat4{};
                        if (j < jb.size() && jb[j].valid()) {
                            const math::Aabb b = jb[j].transformed(world_skin);
                            bounds.expand(b.min);
                            bounds.expand(b.max);
                        }
                    }
                    if (skinned->method == animation::SkinningMode::DualQuaternion) render::encode_dq_palette(palette);
                    if (streaming) {
                        const float px = runtime::projected_pixels(camera, bounds, assets.residency_settings().screen_height);
                        if (!mr.material_asset.is_nil()) assets.note_material_footprint(mr.material_asset, px);
                        else assets.note_model_footprint(mr.mesh_asset, mr.mesh_index, px);
                    }
                    renderer.submit_skinned(*drawn, m, wt.matrix, palette, bounds, motion_id);
                    ++stats.meshes;
                    ++stats.skinned;
                    return;
                }
            }
            if (streaming && mr.material_asset.is_nil())
                assets.note_model_footprint(mr.mesh_asset, mr.mesh_index,
                                            runtime::projected_pixels(camera, drawn->bounds.transformed(wt.matrix),
                                                                      assets.residency_settings().screen_height));
            emit(*drawn, m, drawn == &gm.meshes[mr.mesh_index]);
            ++stats.meshes;
            return;
        }
        const render::GpuMesh* mesh = mr.primitive.empty() ? nullptr : assets.primitive(mr.primitive);
        if (!mesh) {
            ++stats.missing_assets;
            return;
        }
        render::Material m;
        m.base_color = mr.base_color;
        m.metallic = mr.metallic;
        m.roughness = mr.roughness;
        if (!mr.material_asset.is_nil()) {
            if (auto mat = assets.material(mr.material_asset); mat) {
                m = *mat;
                m.base_color = {m.base_color.r * mr.base_color.r, m.base_color.g * mr.base_color.g,
                                m.base_color.b * mr.base_color.b, m.base_color.a * mr.base_color.a};
            } else {
                ++stats.missing_assets;
            }
        }
        m.cast_shadows = mr.cast_shadows;
        apply_overrides(entity, m);
        emit(*mesh, m);
        ++stats.meshes;
    });

    // Crowd agents (ADR-0200): whole skinned models drawn from clips baked once per (model, skin,
    // clip); no joint entities and no per-agent palette upload, so agents of one clip batch.
    w.each<animation::CrowdAgentComponent, WorldTransform>([&](Entity entity, const animation::CrowdAgentComponent& ca,
                                                               const WorldTransform& wt) {
        if (ca.model.is_nil()) return;
        auto model = assets.model(ca.model);
        if (!model) {
            if (model.error().code == ErrorCode::Pending) ++stats.loading_assets;
            else ++stats.missing_assets;
            return;
        }
        const GpuModel& gm = **model;
        const GpuAssetCache::CrowdClip* cc = assets.crowd_clip(ca.model, gm, ca.skin, ca.clip);
        if (!cc) {
            ++stats.missing_assets;
            return;
        }
        const animation::CrowdBake& bake = cc->bake;
        const bool clip = !bake.clips.empty();
        const std::uint32_t frame = clip ? bake.frame_at(0, ca.time, ca.loop) : 0;
        const math::Aabb bounds = (clip && bake.clips[0].bounds.valid() ? bake.clips[0].bounds : bake.rest_bounds).transformed(wt.matrix);
        const float px = streaming ? runtime::projected_pixels(camera, bounds, assets.residency_settings().screen_height) : 0.0f;
        for (std::size_t k = 0; k < cc->meshes.size(); ++k) {
            const std::uint32_t mi = cc->meshes[k];
            render::Material m = gm.mesh_materials[mi];
            m.base_color = {m.base_color.r * ca.tint.r, m.base_color.g * ca.tint.g, m.base_color.b * ca.tint.b, m.base_color.a * ca.tint.a};
            m.cast_shadows = ca.cast_shadows;
            apply_overrides(entity, m);
            if (streaming) assets.note_model_footprint(ca.model, mi, px);
            // Stable and non-zero per (agent, primitive).
            const std::uint64_t motion_id = ((entity.bits() + 1) * 0x9E3779B97F4A7C15ull) ^ (k + 1);
            renderer.submit_skinned(gm.meshes[mi], m, wt.matrix, cc->buffer, frame * bake.joints, bake.joints, bounds,
                                    motion_id ? motion_id : 1);
            ++stats.meshes;
            ++stats.skinned;
        }
        ++stats.crowd_agents;
    });

    std::vector<particles::EmitterView> emitters;
    std::vector<particles::Billboard> stretched;
    particles::gather(scene, emitters);
    for (const particles::EmitterView& v : emitters) {
        render::ParticleMaterial pm;
        pm.blend = v.emitter->blend == particles::Blend::Additive ? render::ParticleBlend::Additive : render::ParticleBlend::Alpha;
        pm.softness = v.emitter->softness;
        pm.lit = v.emitter->lit;
        if (!v.emitter->texture.is_nil()) {
            auto tex = assets.texture(v.emitter->texture);
            if (tex) pm.texture = *tex;
            else if (tex.error().code == ErrorCode::Pending) {
                ++stats.loading_assets;
                continue;
            } else ++stats.missing_assets;
        }
        if (v.gpu != nullptr) {
            // GPU emitter (ADR-0097): the renderer simulates and draws it from the spawn records.
            static_assert(sizeof(particles::GpuSpawnRecord) == sizeof(render::GpuParticleSpawnRecord));
            static_assert(sizeof(particles::GpuParticle) == sizeof(render::GpuParticleSpawn));
            const particles::ParticleEmitterComponent& e = *v.emitter;
            const particles::GpuEmitterView& gv = *v.gpu;
            const auto finite = [](float x) { return std::isfinite(x) ? x : 0.0f; };
            render::GpuParticleEmitter g;
            g.key = gv.key;
            g.instance = gv.instance;
            g.generation = gv.generation;
            g.capacity = gv.capacity;
            g.alive = gv.alive;
            g.time = gv.time;
            g.spawned = gv.spawned;
            g.records = {reinterpret_cast<const render::GpuParticleSpawnRecord*>(gv.records.data()), gv.records.size()};
            render::GpuParticleParams& p = g.params;
            p.world = gv.world;
            p.gravity_drag = {e.gravity.x, e.gravity.y, e.gravity.z, std::max(0.0f, finite(e.drag))};
            p.turbulence = {finite(e.turbulence), 2.0f * math::kPi * std::max(0.0f, finite(e.turbulence_frequency)), finite(e.turbulence_speed),
                            particles::turbulence_phase(e, gv.time)};
            p.size = {e.size_start, e.size_end, std::max(0.0f, e.intensity), e.velocity_stretch > 0 ? finite(e.velocity_stretch) : 0.0f};
            p.color_start = {e.color_start.r, e.color_start.g, e.color_start.b, e.color_start.a};
            p.color_end = {e.color_end.r, e.color_end.g, e.color_end.b, e.color_end.a};
            p.flipbook = {static_cast<float>(std::clamp<std::uint32_t>(e.flipbook_columns, 1, 15)),
                          static_cast<float>(std::clamp<std::uint32_t>(e.flipbook_rows, 1, 15)),
                          static_cast<float>(particles::flipbook_frame_count(e)), e.flipbook_fps > 0 ? finite(e.flipbook_fps) : 0.0f};
            p.collision = {std::clamp(finite(e.bounce), 0.0f, 1.0f), std::clamp(finite(e.friction), 0.0f, 1.0f),
                           std::max(0.0f, finite(e.collision_thickness)), 0.0f};
            const std::size_t ck = std::min(e.color_over_life.size(), particles::kMaxCurveKeys);
            p.color_keys = static_cast<std::uint32_t>(ck);
            for (std::size_t k = 0; k < ck; ++k) {
                const auto& key = e.color_over_life[k];
                p.color_key[k] = {key.color.r, key.color.g, key.color.b, key.color.a};
                p.color_time[k] = key.time;
            }
            const std::size_t sk = std::min(e.size_over_life.size(), particles::kMaxCurveKeys);
            p.size_keys = static_cast<std::uint32_t>(sk);
            for (std::size_t k = 0; k < sk; ++k) {
                p.size_time[k] = e.size_over_life[k].x;
                p.size_value[k] = e.size_over_life[k].y;
            }
            const auto curve = [](const std::vector<math::Vec2>& c, std::uint32_t& n, std::array<float, 8>& times, std::array<float, 8>& values) {
                n = static_cast<std::uint32_t>(std::min(c.size(), particles::kMaxCurveKeys));
                for (std::uint32_t k = 0; k < n; ++k) {
                    times[k] = c[k].x;
                    values[k] = c[k].y;
                }
            };
            curve(e.speed_over_life, p.speed_keys, p.speed_time, p.speed_value); // ADR-0211
            curve(e.spin_over_life, p.spin_keys, p.spin_time, p.spin_value);
            p.world_space = e.world_space;
            p.collide = e.collision;
            if (e.trail_lifetime > 0 && std::isfinite(e.trail_lifetime)) { // trails (ADR-0110)
                p.trail_points = std::clamp<std::uint32_t>(e.trail_max_points, 2, 64);
                p.trail = {e.trail_lifetime, std::max(0.0f, finite(e.trail_min_distance)), std::max(0.0f, finite(e.trail_width)),
                           std::max(0.0f, finite(e.trail_width_end))};
                p.trail_alpha_end = std::clamp(finite(e.trail_alpha_end), 0.0f, 1.0f);
                p.trail_billboards = e.trail_billboards;
            }
            g.material = pm;
            g.bounds = v.bounds;
            renderer.submit_gpu_particles(g);
            if (gv.alive > 0) ++stats.particle_emitters;
            stats.particles += gv.alive;
            continue;
        }
        if (!v.mesh_transforms.empty()) {
            // Mesh particles (ADR-0215): instanced lit meshes through the opaque path (culled,
            // LOD-selected and shadowed per instance like submit_instances() props).
            const particles::ParticleEmitterComponent& e = *v.emitter;
            const render::GpuMesh* mesh = nullptr;
            render::Material m;
            m.base_color = {e.color_start.r, e.color_start.g, e.color_start.b, 1.0f};
            m.roughness = 0.6f;
            if (!e.mesh_asset.is_nil()) {
                auto model = assets.model(e.mesh_asset);
                if (!model && model.error().code == ErrorCode::Pending) {
                    ++stats.loading_assets;
                    continue;
                }
                if (!model || e.mesh_index >= (*model)->meshes.size()) {
                    ++stats.missing_assets;
                    continue;
                }
                mesh = &(*model)->meshes[e.mesh_index];
                m = (*model)->mesh_materials[e.mesh_index];
            } else {
                mesh = assets.primitive(e.mesh_primitive);
                if (!mesh) {
                    ++stats.missing_assets;
                    continue;
                }
            }
            if (!e.mesh_material.is_nil()) {
                if (auto mat = assets.material(e.mesh_material); mat) m = *mat;
                else ++stats.missing_assets;
            }
            m.cast_shadows = e.mesh_cast_shadows;
            renderer.submit_instances(*mesh, m, v.mesh_transforms);
            if (!v.ribbon_strips.empty()) {
                renderer.submit_ribbons({reinterpret_cast<const render::RibbonPoint*>(v.ribbon_points.data()), v.ribbon_points.size()},
                                        v.ribbon_strips, pm, v.bounds);
                stats.particle_trails += static_cast<std::uint32_t>(v.ribbon_strips.size());
            }
            ++stats.particle_emitters;
            stats.particles += static_cast<std::uint32_t>(v.mesh_transforms.size());
            stats.particle_meshes += static_cast<std::uint32_t>(v.mesh_transforms.size());
            continue;
        }
        // Billboard and ParticleInstance share one 48-byte layout (position+size, colour, rotation).
        static_assert(sizeof(particles::Billboard) == sizeof(render::ParticleInstance));
        std::span<const render::ParticleInstance> instances(reinterpret_cast<const render::ParticleInstance*>(v.billboards.data()),
                                                            v.billboards.size());
        if (!v.velocities.empty()) {
            // Stretched particles: aim each one along its on-screen motion for this camera.
            stretched.assign(v.billboards.begin(), v.billboards.end());
            for (std::size_t i = 0; i < stretched.size(); ++i) particles::aim_stretched(stretched[i], v.velocities[i], camera.view);
            instances = {reinterpret_cast<const render::ParticleInstance*>(stretched.data()), stretched.size()};
        }
        renderer.submit_particles(instances, pm, v.bounds);
        if (!v.ribbon_strips.empty()) {
            static_assert(sizeof(particles::RibbonPoint) == sizeof(render::RibbonPoint));
            renderer.submit_ribbons({reinterpret_cast<const render::RibbonPoint*>(v.ribbon_points.data()), v.ribbon_points.size()},
                                    v.ribbon_strips, pm, v.bounds);
            stats.particle_trails += static_cast<std::uint32_t>(v.ribbon_strips.size());
        }
        ++stats.particle_emitters;
        stats.particles += static_cast<std::uint32_t>(v.billboards.size());
    }
    return stats;
}

std::uint32_t extract_lights_2d(Scene& scene, render::SpriteBatch& batch) {
    bool any = false;
    math::Color ambient{0, 0, 0, 1};
    std::vector<render::Light2D> lights;
    scene.world().each<gameplay::Light2DComponent, WorldTransform>([&](const gameplay::Light2DComponent& l, const WorldTransform& wt) {
        if (!l.enabled) return;
        any = true;
        const float k = std::isfinite(l.intensity) ? std::max(l.intensity, 0.0f) : 0.0f;
        if (l.type == gameplay::Light2DComponent::Type::Global) {
            ambient = {ambient.r + l.color.r * k, ambient.g + l.color.g * k, ambient.b + l.color.b * k, 1};
            return;
        }
        render::Light2D out;
        out.type = l.type == gameplay::Light2DComponent::Type::Spot ? render::Light2D::Type::Spot : render::Light2D::Type::Point;
        const math::Vec3 p = wt.matrix.transform_point({});
        out.position = {p.x, p.y};
        const math::Vec3 ax = wt.matrix.transform_vector({1, 0, 0}), ay = wt.matrix.transform_vector({0, 1, 0});
        const float scale = std::max(math::length(math::Vec2{ax.x, ax.y}), math::length(math::Vec2{ay.x, ay.y}));
        out.radius = l.radius * (scale > 0.0f ? scale : 1.0f);
        out.color = l.color;
        out.intensity = k;
        out.direction = {-ay.x, -ay.y};
        const float outer = std::isfinite(l.outer_angle_degrees) ? std::clamp(l.outer_angle_degrees, 0.1f, 180.0f) : 40.0f;
        out.outer_angle = math::radians(outer);
        out.inner_angle = math::radians(std::isfinite(l.inner_angle_degrees) ? std::clamp(l.inner_angle_degrees, 0.0f, outer) : outer);
        out.falloff = l.falloff;
        lights.push_back(out);
    });
    if (!any) return 0;
    batch.set_lighting(ambient);
    for (const auto& l : lights) batch.add_light(l);
    return static_cast<std::uint32_t>(lights.size());
}

std::uint32_t extract_sprites(Scene& scene, GpuAssetCache& assets, render::SpriteBatch& batch) {
    std::uint32_t count = 0;
    (void)assets.pump();
    (void)extract_lights_2d(scene, batch);
    scene.world().each<SpriteComponent, WorldTransform>([&](const SpriteComponent& s, const WorldTransform& wt) {
        render::Sprite sp;
        if (!s.texture_asset.is_nil()) {
            auto tex = assets.texture(s.texture_asset);
            if (tex) sp.texture = *tex;
            else if (tex.error().code == ErrorCode::Pending) return; // a white quad would flash
            sp.nearest = assets.texture_nearest(s.texture_asset);
        }
        math::Vec3 pos, scale;
        math::Quat rot;
        math::decompose(wt.matrix, pos, rot, scale);
        sp.position = {pos.x, pos.y};
        sp.size = {s.size.x * scale.x, s.size.y * scale.y};
        sp.rotation = math::to_euler(rot).z;
        sp.origin = s.pivot;
        sp.color = s.tint;
        sp.layer = s.layer;
        sp.flip_x = s.flip_x;
        sp.flip_y = s.flip_y;
        sp.uv = s.uv;
        sp.lit = !s.unlit;
        batch.draw(sp);
        ++count;
    });
    // Tilemaps: one sprite per painted cell, through the map's transform.
    scene.world().each<tilemap::TilemapComponent, tilemap::TilemapRuntime, WorldTransform>(
        [&](const tilemap::TilemapComponent& map, const tilemap::TilemapRuntime& rt, const WorldTransform& wt) {
            if (!rt.tileset || map.cells.empty()) return;
            const tilemap::Tileset& ts = *rt.tileset;
            rhi::TextureHandle texture{};
            bool nearest = false;
            if (!ts.texture.is_nil()) {
                auto tex = assets.texture(ts.texture);
                if (tex) texture = *tex;
                nearest = assets.texture_nearest(ts.texture);
            }
            math::Vec3 pos, scale;
            math::Quat rot;
            math::decompose(wt.matrix, pos, rot, scale);
            const float rotation = math::to_euler(rot).z;
            const math::Vec2 size{map.cell_size.x * scale.x, map.cell_size.y * scale.y};
            const std::uint32_t tiles = ts.tile_count();
            for (const auto& [key, cell] : map.cells) {
                if (cell.tile < 0 || static_cast<std::uint32_t>(cell.tile) >= tiles) continue;
                const math::Vec3 local{(static_cast<float>(tilemap::key_x(key)) + 0.5f) * map.cell_size.x,
                                       (static_cast<float>(tilemap::key_y(key)) + 0.5f) * map.cell_size.y, 0.0f};
                const math::Vec3 p = wt.matrix.transform_point(local);
                render::Sprite sp;
                sp.texture = texture;
                sp.nearest = nearest;
                sp.position = {p.x, p.y};
                sp.size = size;
                sp.rotation = rotation;
                sp.origin = {0.5f, 0.5f};
                sp.color = map.tint;
                sp.layer = map.layer;
                sp.flip_x = cell.flip_x;
                sp.flip_y = cell.flip_y;
                sp.uv = ts.tile_uv(static_cast<std::uint32_t>(cell.tile));
                sp.lit = true;
                batch.draw(sp);
                ++count;
            }
        });
    // 2D particles: one sprite per billboard on the emitter's layer (XY plane, Z ignored).
    std::vector<particles::EmitterView> emitters;
    particles::gather(scene, emitters);
    for (const particles::EmitterView& v : emitters) {
        if (v.gpu != nullptr) continue; // GPU particles are 3D only (ADR-0097)
        rhi::TextureHandle texture = assets.context().soft_particle();
        bool nearest = false;
        if (!v.emitter->texture.is_nil()) {
            auto tex = assets.texture(v.emitter->texture);
            if (tex) texture = *tex;
            else if (tex.error().code == ErrorCode::Pending) continue;
            nearest = assets.texture_nearest(v.emitter->texture);
        }
        for (const particles::Billboard& b : v.billboards) {
            render::Sprite sp;
            sp.texture = texture;
            sp.nearest = nearest;
            sp.position = {b.position.x, b.position.y};
            sp.size = {b.size, b.aspect > 0.0f ? b.size * b.aspect : b.size};
            sp.rotation = b.rotation;
            sp.uv = particles::flipbook_uv(b);
            sp.color = {std::min(b.color.r, 1.0f), std::min(b.color.g, 1.0f), std::min(b.color.b, 1.0f), b.color.a};
            sp.layer = v.emitter->layer;
            sp.additive = v.emitter->blend == particles::Blend::Additive;
            sp.lit = true; // additive particles stay emissive (never lit)
            batch.draw(sp);
            ++count;
        }
        // Trails: one sprite per segment, its up axis along the segment (x across, y along the trail).
        const bool textured = !v.emitter->texture.is_nil();
        std::uint32_t at = 0;
        for (const std::uint32_t n : v.ribbon_strips) {
            for (std::uint32_t k = at; k + 1 < at + n; ++k) {
                const particles::RibbonPoint& a = v.ribbon_points[k];
                const particles::RibbonPoint& b = v.ribbon_points[k + 1];
                const math::Vec2 d{b.position.x - a.position.x, b.position.y - a.position.y};
                const float len = std::sqrt(d.x * d.x + d.y * d.y);
                const float w = 0.5f * (a.width + b.width);
                if (len <= 1e-6f || w <= 0.0f) continue;
                render::Sprite sp;
                sp.texture = texture;
                sp.nearest = nearest;
                sp.position = {0.5f * (a.position.x + b.position.x), 0.5f * (a.position.y + b.position.y)};
                // Segments meet end to end (no overlap, which would double up under additive
                // blending); sharp bends leave small wedge gaps.
                sp.size = {w, len};
                sp.rotation = std::atan2(-d.x, d.y);
                sp.uv = textured ? math::Vec4{0, b.v, 1, a.v} : math::Vec4{0, 0.5f, 1, 0.5f};
                const auto ch = [](float x, float y) { return std::min(0.5f * (x + y), 1.0f); };
                sp.color = {ch(a.color.r, b.color.r), ch(a.color.g, b.color.g), ch(a.color.b, b.color.b), 0.5f * (a.color.a + b.color.a)};
                sp.layer = v.emitter->layer;
                sp.additive = v.emitter->blend == particles::Blend::Additive;
                sp.lit = true;
                batch.draw(sp);
                ++count;
            }
            at += n;
        }
    }
    return count;
}

} // namespace victus::runtime
