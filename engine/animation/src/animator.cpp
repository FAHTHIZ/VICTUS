// SPDX-License-Identifier: Apache-2.0
#include "victus/animation/animation.hpp"
#include "victus/core/jobs.hpp"
#include "victus/core/log.hpp"
#include "victus/scene/reflect.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace victus::animation {

using namespace math;
using scene::Entity;
using scene::Scene;

void register_components() {
    static std::once_flag once;
    std::call_once(once, [] {
        scene::Schema<AnimatorComponent>("Animator", "Animation")
            .field(&AnimatorComponent::model, "model", {.tooltip = "Model asset providing clips", .hint = "model"})
            .field(&AnimatorComponent::clip, "clip", {.tooltip = "Clip to play; changing it cross-fades"})
            .field(&AnimatorComponent::playing, "playing")
            .field(&AnimatorComponent::loop, "loop")
            .field(&AnimatorComponent::speed, "speed", {.min = -10, .max = 10, .speed = 0.01f})
            .field(&AnimatorComponent::crossfade, "crossfade", {.min = 0, .max = 10, .speed = 0.01f, .tooltip = "seconds"})
            .field(&AnimatorComponent::time, "time", {.min = 0, .max = 1e6f, .speed = 0.01f, .tooltip = "seconds"})
            .commit();
        scene::Schema<SpriteAnimatorComponent>("SpriteAnimator", "Animation")
            .field(&SpriteAnimatorComponent::sheet, "sheet", {.tooltip = "Sprite sheet (.vsheet) asset", .hint = "spritesheet"})
            .field(&SpriteAnimatorComponent::clip, "clip", {.tooltip = "Clip to play; empty shows 'frame'"})
            .field(&SpriteAnimatorComponent::playing, "playing")
            .field(&SpriteAnimatorComponent::speed, "speed", {.min = -10, .max = 10, .speed = 0.01f})
            .field(&SpriteAnimatorComponent::time, "time", {.min = -1e6f, .max = 1e6f, .speed = 0.01f, .tooltip = "seconds"})
            .field(&SpriteAnimatorComponent::frame, "frame", {.min = 0, .max = 65535, .tooltip = "Sheet frame shown when no clip is set"})
            .commit();
        scene::Schema<SkinnedMeshComponent>("SkinnedMesh", "Animation")
            .field(&SkinnedMeshComponent::skeleton, "skeleton", {.tooltip = "Root entity of the skeleton", .hint = "entity"})
            .field(&SkinnedMeshComponent::skin, "skin", {.min = 0, .max = 1024})
            .commit();
    });
}

namespace {

// Runtime playback state, not serialized: rebuilt from AnimatorComponent after loading.
struct AnimatorState {
    Entity owner;
    Uuid model;
    std::shared_ptr<const AnimationSet> set;
    std::string clip_name;      // clip name last applied (compared with the component)
    std::int32_t current{-1};
    std::int32_t previous{-1};
    float previous_time{0};
    float fade_duration{0}, fade_elapsed{0};
    float pending_fade{-1};     // fade requested by play(); < 0 = component crossfade
    bool finished{false};
    std::vector<Entity> bindings; // node index -> entity
    std::vector<std::uint8_t> animated; // node index -> written by current or previous clip
    std::vector<NodePose> pose, pose_b;
    std::uint32_t bound{0};
    std::size_t scene_size{0}; // entity count when bound: retry missing nodes after it changes
};

struct SystemBinding {
    AnimationSystem* system{nullptr};
};

Entity find_child(Scene& scene, Entity parent, std::string_view name) {
    for (const Entity c : scene.children(parent)) {
        const auto* n = scene.world().get<scene::Name>(c);
        if (n && n->value == name) return c;
    }
    return {};
}

Entity find_in_subtree(Scene& scene, Entity root, std::string_view name) {
    std::vector<Entity> stack = scene.children(root);
    std::size_t head = 0;
    while (head < stack.size()) { // breadth-first so the shallowest match wins
        const Entity e = stack[head++];
        const auto* n = scene.world().get<scene::Name>(e);
        if (n && n->value == name) return e;
        for (const Entity c : scene.children(e)) stack.push_back(c);
    }
    return {};
}

bool bindings_valid(Scene& scene, const AnimatorState& s, Entity owner) {
    if (s.owner != owner || s.bindings.size() != (s.set ? s.set->nodes.size() : 0)) return false;
    if (s.bound < s.bindings.size() && s.scene_size != scene.size()) return false;
    for (const Entity e : s.bindings)
        if (e != Entity{} && !scene.world().alive(e)) return false;
    return true;
}

void bind_state(Scene& scene, AnimatorState& s, Entity owner) {
    s.owner = owner;
    s.scene_size = scene.size();
    s.bindings = s.set ? bind_nodes(scene, owner, s.set->nodes) : std::vector<Entity>{};
    s.bound = 0;
    for (const Entity e : s.bindings)
        if (e != Entity{}) ++s.bound;
}

void mark_animated(AnimatorState& s) {
    s.animated.assign(s.set ? s.set->nodes.size() : 0, 0);
    if (!s.set) return;
    for (const std::int32_t ci : {s.current, s.previous}) {
        if (ci < 0) continue;
        for (const Channel& ch : s.set->clips[static_cast<std::size_t>(ci)].channels)
            if (ch.node < s.animated.size()) s.animated[ch.node] = 1;
    }
}

// Applies a clip change on the component to the state (cross-fade from the current clip).
void sync_clip(AnimatorState& s, AnimatorComponent& a) {
    if (s.clip_name == a.clip && (s.current >= 0 || !s.set || s.set->find_clip(a.clip) < 0)) return;
    const std::int32_t next = s.set ? s.set->find_clip(a.clip) : -1;
    const float fade = s.pending_fade >= 0 ? s.pending_fade : a.crossfade;
    s.pending_fade = -1;
    if (s.current >= 0 && next >= 0 && next != s.current && fade > 0.0f) {
        s.previous = s.current;
        s.previous_time = a.time;
        s.fade_duration = fade;
        s.fade_elapsed = 0;
    } else {
        s.previous = -1;
    }
    if (next != s.current) a.time = a.speed < 0.0f && next >= 0 ? s.set->clips[static_cast<std::size_t>(next)].duration : 0.0f;
    s.current = next;
    s.clip_name = a.clip;
    s.finished = false;
    mark_animated(s);
}

struct Work {
    Entity entity;
    AnimatorComponent* a;
    AnimatorState* s;
    std::uint32_t channels{0}, written{0};
};

float advance(float time, float delta, float duration, bool loop, bool& finished) {
    const float t = time + delta;
    if (loop) return wrap_time(t, duration, true);
    if (duration <= 0.0f) {
        finished = true;
        return 0.0f;
    }
    if (t >= duration && delta > 0.0f) finished = true;
    if (t <= 0.0f && delta < 0.0f) finished = true;
    return std::clamp(t, 0.0f, duration);
}

void evaluate(Scene& scene, Work& w, float dt) {
    AnimatorState& s = *w.s;
    AnimatorComponent& a = *w.a;
    if (!s.set || s.current < 0) return;
    const Clip& clip = s.set->clips[static_cast<std::size_t>(s.current)];
    const float delta = a.playing ? dt * a.speed : 0.0f;
    if (a.playing) {
        s.finished = false;
        a.time = advance(a.time, delta, clip.duration, a.loop, s.finished);
    } else {
        a.time = wrap_time(a.time, clip.duration, a.loop);
    }

    const std::size_t n = s.set->nodes.size();
    s.pose.resize(n);
    for (std::size_t i = 0; i < n; ++i) s.pose[i] = s.set->nodes[i].rest;
    sample(clip, a.time, s.pose);
    w.channels += static_cast<std::uint32_t>(clip.channels.size());

    if (s.previous >= 0) {
        const Clip& prev = s.set->clips[static_cast<std::size_t>(s.previous)];
        bool prev_done = false;
        if (a.playing) s.previous_time = advance(s.previous_time, delta, prev.duration, a.loop, prev_done);
        s.pose_b.resize(n);
        for (std::size_t i = 0; i < n; ++i) s.pose_b[i] = s.set->nodes[i].rest;
        sample(prev, s.previous_time, s.pose_b);
        w.channels += static_cast<std::uint32_t>(prev.channels.size());
        s.fade_elapsed += std::abs(dt);
        const float weight = s.fade_duration > 0.0f ? std::clamp(s.fade_elapsed / s.fade_duration, 0.0f, 1.0f) : 1.0f;
        for (std::size_t i = 0; i < n; ++i)
            if (s.animated[i]) s.pose[i] = blend(s.pose_b[i], s.pose[i], weight);
        if (weight >= 1.0f) {
            s.previous = -1;
            mark_animated(s);
        }
    }

    for (std::size_t i = 0; i < n; ++i) {
        if (!s.animated[i] || s.bindings[i] == Entity{}) continue;
        auto* t = scene.world().get<scene::Transform>(s.bindings[i]);
        if (!t) continue;
        t->position = s.pose[i].translation;
        t->rotation = s.pose[i].rotation;
        t->scale = s.pose[i].scale;
        ++w.written;
    }
}

} // namespace

std::vector<Entity> bind_nodes(Scene& scene, Entity root, std::span<const AnimationSet::Node> nodes) {
    std::vector<Entity> out(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        Entity e;
        const bool parent_known = node.parent >= 0 && static_cast<std::size_t>(node.parent) < i;
        const Entity parent = node.parent < 0 ? root : (parent_known ? out[static_cast<std::size_t>(node.parent)] : Entity{});
        if (parent != Entity{}) e = find_child(scene, parent, node.name);
        if (e == Entity{}) e = find_in_subtree(scene, root, node.name);
        out[i] = e;
    }
    return out;
}

struct AnimationSystem::Impl {
    AnimationSettings settings;
    std::unordered_map<Uuid, std::shared_ptr<const AnimationSet>, UuidHash> sets;     // explicit
    std::unordered_map<Uuid, std::shared_ptr<const AnimationSet>, UuidHash> cache;    // from provider
    std::vector<Work> work;
    std::unordered_map<Uuid, std::shared_ptr<const SpriteSheet>, UuidHash> explicit_sheets;
    std::unordered_map<Uuid, std::shared_ptr<const SpriteSheet>, UuidHash> sheet_cache;

    std::shared_ptr<const SpriteSheet> resolve_sheet(const Uuid& id) {
        if (id.is_nil()) return nullptr;
        if (const auto it = explicit_sheets.find(id); it != explicit_sheets.end()) return it->second;
        if (const auto it = sheet_cache.find(id); it != sheet_cache.end()) return it->second;
        auto s = settings.sheets ? settings.sheets(id) : nullptr;
        sheet_cache.emplace(id, s);
        return s;
    }

    std::shared_ptr<const AnimationSet> resolve(const Uuid& model) {
        if (const auto it = sets.find(model); it != sets.end()) return it->second;
        if (const auto it = cache.find(model); it != cache.end()) return it->second;
        std::shared_ptr<const AnimationSet> set = settings.provider ? settings.provider(model) : nullptr;
        if (set) {
            for (const Clip& clip : set->clips) {
                if (auto st = validate(clip, set->nodes.size()); !st) {
                    VICTUS_LOG_WARN("animation", "model {}: {}", model.to_string(), st.error().message);
                    set = nullptr;
                    break;
                }
            }
        }
        cache.emplace(model, set);
        return set;
    }

    // Ensures `s` reflects the component (model, bindings, clip). Serial: may read the hierarchy.
    void prepare(Scene& scene, Entity e, AnimatorComponent& a, AnimatorState& s) {
        if (s.model != a.model || !s.set) {
            auto set = resolve(a.model);
            if (set != s.set || s.model != a.model) {
                s = AnimatorState{};
                s.model = a.model;
                s.set = std::move(set);
            }
        }
        if (!bindings_valid(scene, s, e)) bind_state(scene, s, e);
        sync_clip(s, a);
    }
};

AnimationSystem::AnimationSystem(AnimationSettings settings) : impl_(std::make_unique<Impl>()) {
    register_components();
    impl_->settings = std::move(settings);
}
AnimationSystem::~AnimationSystem() = default;

void AnimationSystem::add_set(const Uuid& model, std::shared_ptr<const AnimationSet> set) { impl_->sets[model] = std::move(set); }
void AnimationSystem::invalidate(const Uuid& model) {
    impl_->cache.erase(model);
}
void AnimationSystem::clear_cache() { impl_->cache.clear(); }
std::shared_ptr<const AnimationSet> AnimationSystem::set(const Uuid& model) { return impl_->resolve(model); }

void AnimationSystem::update(Scene& scene, float dt) {
    update_sprites(scene, dt);
    ecs::World& world = scene.world();
    // Structural changes first: add state to new animators, drop state of removed ones.
    std::vector<Entity> missing, orphans;
    world.each<AnimatorComponent>([&](Entity e, AnimatorComponent&) {
        if (!world.has<AnimatorState>(e)) missing.push_back(e);
    });
    world.each_excluding<AnimatorState>(ecs::World::Exclude<AnimatorComponent>{}, [&](Entity e, AnimatorState&) { orphans.push_back(e); });
    for (const Entity e : missing) world.add<AnimatorState>(e);
    for (const Entity e : orphans) world.remove<AnimatorState>(e);

    auto& work = impl_->work;
    work.clear();
    world.each<AnimatorComponent, AnimatorState>([&](Entity e, AnimatorComponent& a, AnimatorState& s) { work.push_back({e, &a, &s}); });

    stats_ = {};
    stats_.animators = static_cast<std::uint32_t>(work.size());
    for (Work& w : work) {
        impl_->prepare(scene, w.entity, *w.a, *w.s);
        if (!w.s->set) ++stats_.missing_models;
    }
    jobs::JobSystem* js = impl_->settings.jobs;
    if (js && work.size() >= impl_->settings.parallel_threshold) {
        js->parallel_for(0, work.size(), 8, [&](std::size_t b, std::size_t end) {
            for (std::size_t i = b; i < end; ++i) evaluate(scene, work[i], dt);
        });
    } else {
        for (Work& w : work) evaluate(scene, w, dt);
    }
    for (const Work& w : work) {
        stats_.channels_sampled += w.channels;
        stats_.nodes_written += w.written;
    }
}

bool AnimationSystem::play(Scene& scene, Entity e, std::string_view clip, float fade, bool restart) {
    auto* a = scene.world().get<AnimatorComponent>(e);
    if (!a) return false;
    auto set = impl_->resolve(a->model);
    if (!set || set->find_clip(clip) < 0) return false;
    auto* s = scene.world().get<AnimatorState>(e);
    if (!s) {
        scene.world().add<AnimatorState>(e);
        a = scene.world().get<AnimatorComponent>(e);
        s = scene.world().get<AnimatorState>(e);
    }
    a->playing = true;
    if (a->clip == clip) {
        if (restart) {
            a->time = a->speed < 0.0f ? set->clips[static_cast<std::size_t>(set->find_clip(clip))].duration : 0.0f;
            s->finished = false;
        }
        return true;
    }
    a->clip = std::string(clip);
    s->pending_fade = fade;
    impl_->prepare(scene, e, *a, *s);
    if (!restart && s->previous >= 0) a->time = wrap_time(s->previous_time, set->clips[static_cast<std::size_t>(s->current)].duration, a->loop);
    return true;
}

AnimatorStatus AnimationSystem::status(Scene& scene, Entity e) {
    AnimatorStatus st;
    auto* a = scene.world().get<AnimatorComponent>(e);
    if (!a) return st;
    st.clip = a->clip;
    st.time = a->time;
    st.playing = a->playing;
    st.looping = a->loop;
    auto set = impl_->resolve(a->model);
    st.valid = set != nullptr;
    if (set) {
        if (const std::int32_t ci = set->find_clip(a->clip); ci >= 0) st.duration = set->clips[static_cast<std::size_t>(ci)].duration;
    }
    if (const auto* s = scene.world().get<AnimatorState>(e)) {
        st.finished = s->finished;
        st.bound_nodes = s->bound;
        for (const auto v : s->animated) st.animated_nodes += v;
        if (s->previous >= 0 && s->fade_duration > 0.0f) st.blend = std::clamp(s->fade_elapsed / s->fade_duration, 0.0f, 1.0f);
    }
    return st;
}

std::vector<std::string> AnimationSystem::clips(Scene& scene, Entity e) {
    std::vector<std::string> out;
    const auto* a = scene.world().get<AnimatorComponent>(e);
    if (!a) return out;
    if (auto set = impl_->resolve(a->model))
        for (const Clip& c : set->clips) out.push_back(c.name);
    return out;
}

void bind(Scene& scene, AnimationSystem* system) {
    if (auto* b = scene.world().resource<SystemBinding>()) b->system = system;
    else scene.world().set_resource<SystemBinding>().system = system;
}

AnimationSystem* system_of(Scene& scene) {
    const auto* b = scene.world().resource<SystemBinding>();
    return b ? b->system : nullptr;
}

void add_animation_systems(gameplay::Simulation& sim, AnimationSystem& system) {
    AnimationSystem* sys = &system;
    sim.add_system("animation.update", gameplay::Phase::Update, [sys](Scene& scene, const gameplay::FrameTime& t) {
        if (system_of(scene) != sys) bind(scene, sys);
        sys->update(scene, static_cast<float>(t.dt));
    });
}

void skeleton_lines(Scene& scene, const std::function<void(Vec3, Vec3)>& emit) {
    scene.world().each<AnimatorState>([&](Entity, AnimatorState& s) {
        if (!s.set) return;
        for (std::size_t i = 0; i < s.bindings.size() && i < s.set->nodes.size(); ++i) {
            const std::int32_t p = s.set->nodes[i].parent;
            if (p < 0 || static_cast<std::size_t>(p) >= s.bindings.size()) continue;
            const Entity a = s.bindings[i], b = s.bindings[static_cast<std::size_t>(p)];
            if (a == Entity{} || b == Entity{}) continue;
            const auto* wa = scene.world().get<scene::WorldTransform>(a);
            const auto* wb = scene.world().get<scene::WorldTransform>(b);
            if (!wa || !wb) continue;
            emit(wb->matrix.transform_point({}), wa->matrix.transform_point({}));
        }
    });
}

} // namespace victus::animation

// ---- sprite animation ------------------------------------------------------------------------

namespace victus::animation {

namespace {
// Runtime state: the clip last applied, to restart time when the component's clip changes.
struct SpriteAnimatorState {
    std::string clip;
    bool started{false};
};
} // namespace

void AnimationSystem::add_sheet(const Uuid& id, std::shared_ptr<const SpriteSheet> data) { impl_->explicit_sheets[id] = std::move(data); }
void AnimationSystem::clear_sheets() { impl_->sheet_cache.clear(); }
std::shared_ptr<const SpriteSheet> AnimationSystem::sheet(const Uuid& id) { return impl_->resolve_sheet(id); }

void AnimationSystem::update_sprites(Scene& scene, float dt) {
    auto& w = scene.world();
    std::vector<Entity> fresh;
    w.each<SpriteAnimatorComponent>([&](Entity e, SpriteAnimatorComponent& a) {
        auto* sp = w.get<scene::SpriteComponent>(e);
        if (!sp) return;
        const auto sheet = impl_->resolve_sheet(a.sheet);
        if (!sheet) return;
        auto* st = w.get<SpriteAnimatorState>(e);
        if (!st) {
            fresh.push_back(e);
        } else if (st->clip != a.clip) {
            if (st->started) a.time = 0; // a clip change restarts; a loaded time is kept
            st->clip = a.clip;
        }
        if (st) st->started = true;
        if (!sheet->texture.is_nil()) sp->texture_asset = sheet->texture;
        const SpriteClip* clip = a.clip.empty() ? nullptr : sheet->find(a.clip);
        if (!clip) {
            sp->uv = sheet->frame_uv(a.frame);
            return;
        }
        if (a.playing && dt != 0) {
            a.time += dt * a.speed;
            // Keep looping clocks small so float precision never degrades.
            const float dur = clip->duration();
            if (clip->loop && dur > 0 && std::fabs(a.time) > dur * 1024) a.time = std::fmod(a.time, dur);
            if (!clip->loop && dur > 0) a.time = std::clamp(a.time, 0.0f, dur);
        }
        sp->uv = sheet->frame_uv(clip->frames[clip_frame_at(*clip, a.time)]);
    });
    for (const Entity e : fresh) {
        auto& st = w.add<SpriteAnimatorState>(e);
        st.clip = w.get<SpriteAnimatorComponent>(e)->clip;
        st.started = true;
    }
}

bool AnimationSystem::play_sprite(Scene& scene, Entity e, std::string_view clip, bool restart) {
    auto& w = scene.world();
    auto* a = w.alive(e) ? w.get<SpriteAnimatorComponent>(e) : nullptr;
    if (!a) return false;
    const auto sheet = impl_->resolve_sheet(a->sheet);
    if (!sheet || !sheet->find(clip)) return false;
    if (a->clip != clip || restart) a->time = 0;
    a->clip = std::string(clip);
    a->playing = true;
    if (auto* st = w.get<SpriteAnimatorState>(e)) st->clip = a->clip; // time already reset
    return true;
}

SpriteAnimatorStatus AnimationSystem::sprite_status(Scene& scene, Entity e) {
    SpriteAnimatorStatus out;
    auto& w = scene.world();
    const auto* a = w.alive(e) ? w.get<SpriteAnimatorComponent>(e) : nullptr;
    if (!a) return out;
    const auto sheet = impl_->resolve_sheet(a->sheet);
    if (!sheet) return out;
    out.valid = true;
    out.playing = a->playing;
    out.time = a->time;
    const SpriteClip* clip = a->clip.empty() ? nullptr : sheet->find(a->clip);
    if (!clip) {
        out.frame = std::min(a->frame, sheet->frame_count() - 1);
        return out;
    }
    out.clip = clip->name;
    out.duration = clip->duration();
    out.looping = clip->loop;
    out.clip_frame = clip_frame_at(*clip, a->time);
    out.frame = clip->frames[out.clip_frame];
    out.finished = !clip->loop && (a->speed >= 0 ? a->time >= out.duration : a->time <= 0);
    return out;
}

std::vector<std::string> AnimationSystem::sprite_clips(Scene& scene, Entity e) {
    std::vector<std::string> out;
    auto& w = scene.world();
    const auto* a = w.alive(e) ? w.get<SpriteAnimatorComponent>(e) : nullptr;
    if (!a) return out;
    if (const auto sheet = impl_->resolve_sheet(a->sheet))
        for (const auto& c : sheet->clips) out.push_back(c.name);
    return out;
}

} // namespace victus::animation
