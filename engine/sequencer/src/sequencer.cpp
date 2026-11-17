// SPDX-License-Identifier: Apache-2.0
// SequencerSystem: plays sequences on entities with a SequencePlayer component.
#include "victus/sequencer/sequencer.hpp"

#include "victus/animation/animation.hpp"
#include "victus/audio/audio.hpp"
#include "victus/core/fs.hpp"
#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"
#include "victus/scene/reflect.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>
#include <unordered_map>

namespace victus::sequencer {

using scene::Scene;

namespace {

struct EventQueue {
    std::vector<SequenceEvent> events;
    std::uint64_t next_id{1};
    std::uint64_t delivered{0};  // highest id delivered to scripts
    std::uint64_t last_start{1}; // first id fired by the previous update
};

struct Binding {
    SequencerSystem* system{nullptr};
};

EventQueue& queue_of(Scene& s) {
    if (auto* q = s.world().resource<EventQueue>()) return *q;
    return s.world().set_resource<EventQueue>();
}

const std::string* name_of(Scene& s, Entity e) {
    const auto* n = s.world().get<scene::Name>(e);
    return n ? &n->value : nullptr;
}

// A fired discrete key, ordered by time in the direction of travel.
struct Fired {
    float time;
    std::size_t track;
    std::size_t key;
};

} // namespace

Entity resolve_target(Scene& scene, Entity root, std::string_view target) {
    auto& w = scene.world();
    if (target.empty() || target == ".") return w.alive(root) ? root : Entity{};
    if (target.find('/') != std::string_view::npos) {
        Entity cur = root;
        std::size_t pos = 0;
        while (pos <= target.size()) {
            const std::size_t slash = target.find('/', pos);
            const std::string_view part =
                target.substr(pos, slash == std::string_view::npos ? std::string_view::npos : slash - pos);
            pos = slash == std::string_view::npos ? target.size() + 1 : slash + 1;
            if (part.empty() || part == ".") continue;
            if (!w.alive(cur)) return {};
            if (part == "..") {
                cur = scene.parent(cur);
                continue;
            }
            Entity next;
            for (Entity c : scene.children(cur)) {
                const std::string* n = name_of(scene, c);
                if (n && *n == part) {
                    next = c;
                    break;
                }
            }
            if (next.is_null()) return {};
            cur = next;
        }
        return w.alive(cur) ? cur : Entity{};
    }
    // Breadth-first below the root, then anywhere in the scene.
    if (w.alive(root)) {
        std::vector<Entity> frontier = scene.children(root);
        while (!frontier.empty()) {
            std::vector<Entity> next;
            for (Entity c : frontier) {
                const std::string* n = name_of(scene, c);
                if (n && *n == target) return c;
                for (Entity g : scene.children(c))
                    next.push_back(g);
            }
            frontier = std::move(next);
        }
    }
    return scene.find_by_name(target);
}

struct SequencerSystem::Impl {
    SequenceLoader loader;
    EventFn handler;

    struct Cached {
        std::shared_ptr<const Sequence> seq;
        std::string error;
    };
    std::unordered_map<std::string, Cached> cache;

    struct TrackState {
        Entity target;
        bool resolved{false};
        bool warned{false};
        bool has_base{false};
        scene::Transform base;
    };
    struct Instance {
        std::string path; // sequence path the instance was loaded from
        std::shared_ptr<const Sequence> seq;
        bool override_seq{false};
        bool needs_reload{false};
        std::string error;
        float time{0};
        bool playing{false}, paused{false}, finished{false};
        bool started{false}; // play_on_start handled
        bool fresh{false};   // the next advance fires keys at the start time too
        std::uint32_t loops{0};
        std::vector<TrackState> tracks;
        // Camera cuts: primary flags before the first cut, restored afterwards.
        std::vector<std::pair<Entity, bool>> saved_primary;
        bool camera_applied{false};
        std::string camera;
    };
    std::unordered_map<std::uint64_t, Instance> instances;
    Scene* scene{nullptr};

    std::shared_ptr<const Sequence> load(const std::string& path, std::string& error, SequencerStats& stats) {
        error.clear();
        if (path.empty()) return nullptr;
        auto it = cache.find(path);
        if (it == cache.end()) {
            Cached c;
            if (!loader) {
                c.error = std::format("{}: no sequence loader", path);
            } else if (auto text = loader(path); !text) {
                c.error = std::format("{}: {}", path, text.error().message);
            } else if (auto seq = Sequence::parse(*text, path); !seq) {
                c.error = std::format("{}: {}", path, seq.error().message);
            } else {
                c.seq = std::move(*seq);
            }
            if (!c.error.empty()) {
                ++stats.load_errors;
                VICTUS_LOG_WARN("sequencer", "{}", c.error);
            }
            it = cache.emplace(path, std::move(c)).first;
        }
        error = it->second.error;
        return it->second.seq;
    }

    Entity target_of(Instance& in, std::size_t ti, Scene& s, Entity player, SequencerStats& stats) {
        if (ti >= in.tracks.size()) in.tracks.resize(in.seq ? in.seq->tracks.size() : ti + 1);
        TrackState& ts = in.tracks[ti];
        if (!ts.resolved || !s.world().alive(ts.target)) {
            ts.target = resolve_target(s, player, in.seq->tracks[ti].target);
            ts.resolved = true;
            if (ts.target.is_null() && !ts.warned) {
                ts.warned = true;
                ++stats.missing_targets;
                VICTUS_LOG_WARN("sequencer", "{}: track {} target '{}' not found", in.seq->name, ti,
                                in.seq->tracks[ti].target);
            }
        }
        return ts.target;
    }

    void capture_bases(Instance& in, Scene& s, Entity player, SequencerStats& stats) {
        if (!in.seq) return;
        in.tracks.resize(in.seq->tracks.size());
        for (std::size_t i = 0; i < in.seq->tracks.size(); ++i) {
            const Track& t = in.seq->tracks[i];
            TrackState& ts = in.tracks[i];
            if (t.type != TrackType::Transform || !t.relative || ts.has_base) continue;
            const Entity e = target_of(in, i, s, player, stats);
            if (const auto* tr = e.is_null() ? nullptr : s.world().get<scene::Transform>(e)) {
                ts.base = *tr;
                ts.has_base = true;
            }
        }
    }

    void restore_cameras(Instance& in, Scene& s) {
        if (!in.camera_applied) return;
        for (const auto& [e, primary] : in.saved_primary)
            if (s.world().alive(e))
                if (auto* c = s.world().get<scene::CameraComponent>(e)) c->primary = primary;
        in.saved_primary.clear();
        in.camera_applied = false;
        in.camera.clear();
    }

    void apply_camera(Instance& in, Scene& s, Entity player, float t, SequencerStats& stats) {
        // The latest cut at or before t of the last camera track that has one.
        const CameraKey* active = nullptr;
        for (const Track& tr : in.seq->tracks) {
            if (tr.type != TrackType::Camera || tr.muted) continue;
            for (const auto& c : tr.cameras)
                if (c.time <= t) active = &c;
        }
        if (!active) {
            restore_cameras(in, s);
            return;
        }
        const Entity cam = resolve_target(s, player, active->camera);
        if (cam.is_null() || !s.world().get<scene::CameraComponent>(cam)) {
            if (in.camera != active->camera) {
                ++stats.missing_targets;
                VICTUS_LOG_WARN("sequencer", "{}: camera '{}' not found", in.seq->name, active->camera);
                in.camera = active->camera;
            }
            return;
        }
        if (!in.camera_applied) {
            in.saved_primary.clear();
            s.world().each<scene::CameraComponent>(
                [&](Entity e, scene::CameraComponent& c) { in.saved_primary.emplace_back(e, c.primary); });
            in.camera_applied = true;
        }
        s.world().each<scene::CameraComponent>([&](Entity e, scene::CameraComponent& c) { c.primary = e == cam; });
        in.camera = active->camera;
    }

    // Continuous state at time t: transform channels and camera cuts.
    void apply_state(Instance& in, Scene& s, Entity player, float t, SequencerStats& stats) {
        if (!in.seq) return;
        for (std::size_t i = 0; i < in.seq->tracks.size(); ++i) {
            const Track& tr = in.seq->tracks[i];
            if (tr.type != TrackType::Transform || tr.muted) continue;
            const Entity e = target_of(in, i, s, player, stats);
            auto* xf = e.is_null() ? nullptr : s.world().get<scene::Transform>(e);
            if (!xf) continue;
            const TrackState& ts = in.tracks[i];
            const bool rel = tr.relative && ts.has_base;
            if (auto p = sample(tr.position, t)) xf->position = rel ? ts.base.position + *p : *p;
            if (auto r = sample(tr.rotation, t)) xf->rotation = rel ? math::normalize(ts.base.rotation * *r) : *r;
            if (auto sc = sample(tr.scale, t))
                xf->scale =
                    rel ? math::Vec3{ts.base.scale.x * sc->x, ts.base.scale.y * sc->y, ts.base.scale.z * sc->z} : *sc;
        }
        apply_camera(in, s, player, t, stats);
    }

    // Plays the animation clip each animation track has active at time t (seeks, starts).
    void apply_animation_at(Instance& in, Scene& s, Entity player, float t, SequencerStats& stats) {
        auto* anim = animation::system_of(s);
        if (!anim || !in.seq) return;
        for (std::size_t i = 0; i < in.seq->tracks.size(); ++i) {
            const Track& tr = in.seq->tracks[i];
            if (tr.type != TrackType::Animation || tr.muted) continue;
            const AnimationKey* k = nullptr;
            for (const auto& a : tr.animation)
                if (a.time <= t) k = &a;
            if (!k) continue;
            const Entity e = target_of(in, i, s, player, stats);
            auto* ac = e.is_null() ? nullptr : s.world().get<animation::AnimatorComponent>(e);
            if (!ac) continue;
            ac->loop = k->loop;
            ac->speed = k->speed;
            if (anim->play(s, e, k->clip, 0.0f, true))
                if (auto* a2 = s.world().get<animation::AnimatorComponent>(e)) a2->time = (t - k->time) * k->speed;
        }
    }

    void fire(Instance& in, Scene& s, Entity player, float from, float to, bool inclusive, SequencerStats& stats) {
        if (!in.seq) return;
        const bool forward = to >= from;
        std::vector<Fired> fired;
        auto crosses = [&](float kt) {
            if (forward) return (kt > from || (inclusive && kt >= from)) && kt <= to;
            return (kt < from || (inclusive && kt <= from)) && kt >= to;
        };
        for (std::size_t i = 0; i < in.seq->tracks.size(); ++i) {
            const Track& tr = in.seq->tracks[i];
            if (tr.muted) continue;
            auto scan = [&](const auto& keys) {
                for (std::size_t k = 0; k < keys.size(); ++k)
                    if (crosses(keys[k].time)) fired.push_back({keys[k].time, i, k});
            };
            switch (tr.type) {
            case TrackType::Animation:
                scan(tr.animation);
                break;
            case TrackType::Audio:
                scan(tr.audio);
                break;
            case TrackType::Event:
                scan(tr.events);
                break;
            default:
                break;
            }
        }
        if (fired.empty()) return;
        std::stable_sort(fired.begin(), fired.end(), [forward](const Fired& a, const Fired& b) {
            return forward ? a.time < b.time : a.time > b.time;
        });
        // Keep the sequence alive while handlers run (they may reload or replace it).
        const std::shared_ptr<const Sequence> seq = in.seq;
        for (const Fired& f : fired) {
            const Track& tr = seq->tracks[f.track];
            const Entity target = target_of(in, f.track, s, player, stats);
            switch (tr.type) {
            case TrackType::Animation: {
                const AnimationKey& k = tr.animation[f.key];
                auto* anim = animation::system_of(s);
                auto* ac = target.is_null() ? nullptr : s.world().get<animation::AnimatorComponent>(target);
                if (!anim || !ac) break;
                ac->loop = k.loop;
                ac->speed = k.speed;
                if (anim->play(s, target, k.clip, k.fade, true))
                    ++stats.clips;
                else
                    VICTUS_LOG_WARN("sequencer", "{}: clip '{}' not found on the target of track {}", seq->name, k.clip,
                                    f.track);
                break;
            }
            case TrackType::Audio: {
                const AudioKey& k = tr.audio[f.key];
                auto* eng = audio::engine_of(s);
                if (!eng) break;
                audio::OneShot o;
                o.bus = audio::parse_bus(k.bus).value_or(audio::Bus::Sfx);
                o.volume = k.volume;
                o.pitch = k.pitch;
                if (k.spatial && !target.is_null()) o.position = s.world_matrix(target).transform_point({});
                if (auto st = eng->play_one_shot(k.clip, o); st)
                    ++stats.one_shots;
                else
                    VICTUS_LOG_WARN("sequencer", "{}: {}", seq->name, st.error().message);
                break;
            }
            case TrackType::Event: {
                const EventKey& k = tr.events[f.key];
                EventQueue& q = queue_of(s);
                SequenceEvent ev{q.next_id++, player, target, k.name, k.payload, k.time};
                ++stats.events;
                if (handler) handler(s, ev);
                q.events.push_back(std::move(ev));
                break;
            }
            default:
                break;
            }
            if (!s.world().alive(player)) return; // a handler destroyed the player
        }
    }

    void start(Instance& in, Scene& s, Entity player, float speed, SequencerStats& stats) {
        restore_cameras(in, s);
        for (auto& ts : in.tracks)
            ts.has_base = false;
        in.time = speed < 0 && in.seq ? in.seq->duration : 0.0f;
        in.playing = in.seq != nullptr;
        in.paused = false;
        in.finished = false;
        in.fresh = true;
        in.loops = 0;
        capture_bases(in, s, player, stats);
        apply_state(in, s, player, in.time, stats);
    }

    void finish(Instance& in, Scene& s) {
        in.playing = false;
        in.finished = true;
        restore_cameras(in, s);
    }

    void advance(Instance& in, Scene& s, Entity player, const SequencePlayerComponent& c, float dt,
                 SequencerStats& stats) {
        const float d = in.seq->duration;
        const float delta = dt * c.speed;
        float prev = in.time;
        bool inclusive = in.fresh;
        in.fresh = false;
        if (d <= 0) {
            fire(in, s, player, 0.0f, 0.0f, true, stats);
            in.time = 0;
            if (s.world().alive(player)) apply_state(in, s, player, 0.0f, stats);
            finish(in, s);
            return;
        }
        float now = prev + delta;
        constexpr int kMaxWraps = 8; // a huge dt plays at most this many loops of discrete keys
        if (delta >= 0) {
            int wraps = 0;
            while (now >= d && c.loop && wraps < kMaxWraps) {
                fire(in, s, player, prev, d, inclusive, stats);
                if (!s.world().alive(player)) return;
                now -= d;
                prev = 0;
                inclusive = true;
                ++in.loops;
                ++wraps;
            }
            if (c.loop) now = std::fmod(now, d);
            if (now >= d) {
                fire(in, s, player, prev, d, inclusive, stats);
                if (!s.world().alive(player)) return;
                in.time = d;
                apply_state(in, s, player, d, stats);
                finish(in, s);
                return;
            }
            fire(in, s, player, prev, now, inclusive, stats);
        } else {
            int wraps = 0;
            while (now <= 0 && c.loop && wraps < kMaxWraps) {
                fire(in, s, player, prev, 0.0f, inclusive, stats);
                if (!s.world().alive(player)) return;
                now += d;
                prev = d;
                inclusive = true;
                ++in.loops;
                ++wraps;
            }
            if (c.loop && now < 0) now = d - std::fmod(-now, d);
            if (now <= 0) {
                fire(in, s, player, prev, 0.0f, inclusive, stats);
                if (!s.world().alive(player)) return;
                in.time = 0;
                apply_state(in, s, player, 0.0f, stats);
                finish(in, s);
                return;
            }
            fire(in, s, player, prev, now, inclusive, stats);
        }
        if (!s.world().alive(player)) return;
        in.time = now;
        apply_state(in, s, player, now, stats);
    }

    void bind_scene(Scene& s) {
        if (scene == &s) return;
        scene = &s;
        instances.clear();
    }

    Instance* instance(Scene& s, Entity e) {
        bind_scene(s);
        if (!s.world().alive(e)) return nullptr;
        return &instances[e.bits()];
    }

    // Loads the component's sequence when it changed (or a reload was requested).
    void sync(Instance& in, const SequencePlayerComponent& c, SequencerStats& stats) {
        if (in.override_seq) return;
        if (in.path == c.sequence && !in.needs_reload && (in.seq || !in.error.empty() || c.sequence.empty())) return;
        const bool keep_playhead = in.needs_reload && in.path == c.sequence;
        in.needs_reload = false;
        in.path = c.sequence;
        in.seq = load(in.path, in.error, stats);
        if (keep_playhead && in.seq) {
            in.time = std::clamp(in.time, 0.0f, in.seq->duration);
            std::vector<TrackState> old = std::move(in.tracks);
            in.tracks.assign(in.seq->tracks.size(), {});
            for (std::size_t i = 0; i < in.tracks.size() && i < old.size(); ++i) {
                in.tracks[i].has_base = old[i].has_base;
                in.tracks[i].base = old[i].base;
            }
            return;
        }
        in.tracks.clear();
        in.time = 0;
        in.playing = false;
        in.paused = false;
        in.finished = false;
        in.camera_applied = false;
        in.saved_primary.clear();
        in.camera.clear();
    }
};

SequencerSystem::SequencerSystem(SequenceLoader loader) : impl_(std::make_unique<Impl>()) {
    impl_->loader = std::move(loader);
}

SequencerSystem::~SequencerSystem() = default;

void SequencerSystem::set_event_handler(EventFn fn) { impl_->handler = std::move(fn); }

void SequencerSystem::update(Scene& scene, float dt) {
    VICTUS_PROFILE_SCOPE("sequencer.update");
    Impl& im = *impl_;
    im.bind_scene(scene);
    // Drop events that had a full update cycle to be delivered (or were delivered).
    if (auto* q = scene.world().resource<EventQueue>()) {
        std::erase_if(q->events,
                      [q](const SequenceEvent& ev) { return ev.id <= q->delivered || ev.id < q->last_start; });
        q->last_start = q->next_id;
    }
    struct Item {
        Entity e;
        SequencePlayerComponent c;
    };
    std::vector<Item> items;
    scene.world().each<SequencePlayerComponent>([&](Entity e, SequencePlayerComponent& c) { items.push_back({e, c}); });
    // Forget players whose entity or component went away.
    for (auto it = im.instances.begin(); it != im.instances.end();) {
        const Entity e{static_cast<std::uint32_t>(it->first), static_cast<std::uint32_t>(it->first >> 32)};
        if (!scene.world().alive(e) || !scene.world().get<SequencePlayerComponent>(e)) {
            im.restore_cameras(it->second, scene);
            it = im.instances.erase(it);
        } else {
            ++it;
        }
    }
    std::uint32_t playing = 0;
    for (const Item& item : items) {
        if (!scene.world().alive(item.e)) continue;
        Impl::Instance& in = im.instances[item.e.bits()];
        im.sync(in, item.c, stats_);
        if (!item.c.enabled) continue;
        if (!in.started) {
            in.started = true;
            if (item.c.play_on_start && in.seq) im.start(in, scene, item.e, item.c.speed, stats_);
        }
        if (in.playing && !in.paused && in.seq) im.advance(in, scene, item.e, item.c, std::max(dt, 0.0f), stats_);
        if (in.playing) ++playing;
    }
    stats_.players = static_cast<std::uint32_t>(items.size());
    stats_.playing = playing;
}

bool SequencerSystem::play(Scene& scene, Entity e, std::string_view path) {
    Impl& im = *impl_;
    Impl::Instance* in = im.instance(scene, e);
    if (!in) return false;
    auto* c = scene.world().get<SequencePlayerComponent>(e);
    if (!path.empty()) {
        if (!c) c = &scene.world().add<SequencePlayerComponent>(e, SequencePlayerComponent{.play_on_start = false});
        if (c->sequence != path || in->override_seq) {
            c->sequence = std::string(path);
            in->override_seq = false;
            in->path.clear();
        }
    }
    if (!c) {
        if (!in->override_seq) return false;
        c = &scene.world().add<SequencePlayerComponent>(e, SequencePlayerComponent{.play_on_start = false});
    }
    const SequencePlayerComponent comp = *c;
    im.sync(*in, comp, stats_);
    in->started = true;
    if (!in->seq) return false;
    im.start(*in, scene, e, comp.speed, stats_);
    return true;
}

void SequencerSystem::set_sequence(Scene& scene, Entity e, std::shared_ptr<const Sequence> sequence) {
    Impl& im = *impl_;
    Impl::Instance* in = im.instance(scene, e);
    if (!in) return;
    im.restore_cameras(*in, scene);
    in->override_seq = sequence != nullptr;
    in->seq = std::move(sequence);
    in->path = in->override_seq ? in->seq->name : std::string();
    in->error.clear();
    in->tracks.clear();
    in->time = 0;
    in->playing = in->paused = in->finished = false;
    in->started = in->override_seq; // overrides only play when asked
}

void SequencerSystem::pause(Scene& scene, Entity e, bool paused) {
    if (Impl::Instance* in = impl_->instance(scene, e)) in->paused = paused;
}

void SequencerSystem::stop(Scene& scene, Entity e) {
    Impl& im = *impl_;
    Impl::Instance* in = im.instance(scene, e);
    if (!in) return;
    im.restore_cameras(*in, scene);
    in->playing = in->paused = in->finished = false;
    in->time = 0;
    in->loops = 0;
    for (auto& ts : in->tracks)
        ts.has_base = false;
}

bool SequencerSystem::seek(Scene& scene, Entity e, float time) {
    Impl& im = *impl_;
    Impl::Instance* in = im.instance(scene, e);
    if (!in || !std::isfinite(time)) return false;
    if (const auto* c = scene.world().get<SequencePlayerComponent>(e)) {
        const SequencePlayerComponent comp = *c;
        im.sync(*in, comp, stats_);
    }
    if (!in->seq) return false;
    in->time = std::clamp(time, 0.0f, in->seq->duration);
    in->fresh = false;
    if (in->finished && in->time < in->seq->duration) in->finished = false;
    im.capture_bases(*in, scene, e, stats_);
    im.apply_state(*in, scene, e, in->time, stats_);
    im.apply_animation_at(*in, scene, e, in->time, stats_);
    return true;
}

PlaybackState SequencerSystem::state(Scene& scene, Entity e) const {
    const Impl& im = *impl_;
    PlaybackState st;
    if (im.scene != &scene || !scene.world().alive(e)) return st;
    const auto it = im.instances.find(e.bits());
    if (it == im.instances.end()) {
        if (const auto* c = scene.world().get<SequencePlayerComponent>(e)) st.sequence = c->sequence;
        return st;
    }
    const Impl::Instance& in = it->second;
    st.valid = in.seq != nullptr;
    st.sequence = in.path;
    st.error = in.error;
    st.time = in.time;
    st.duration = in.seq ? in.seq->duration : 0.0f;
    st.playing = in.playing;
    st.paused = in.paused;
    st.finished = in.finished;
    st.loops = in.loops;
    st.camera = in.camera_applied ? in.camera : std::string();
    return st;
}

std::shared_ptr<const Sequence> SequencerSystem::sequence(Scene& scene, Entity e) const {
    const Impl& im = *impl_;
    if (im.scene != &scene) return nullptr;
    const auto it = im.instances.find(e.bits());
    return it == im.instances.end() ? nullptr : it->second.seq;
}

void SequencerSystem::reload(std::string_view path) {
    Impl& im = *impl_;
    if (path.empty())
        im.cache.clear();
    else
        im.cache.erase(std::string(path));
    for (auto& [k, in] : im.instances)
        if (!in.override_seq && (path.empty() || in.path == path)) in.needs_reload = true;
}

// ---- events, registration and binding ---------------------------------------------------------

const std::vector<SequenceEvent>& events(Scene& scene) {
    static const std::vector<SequenceEvent> kNone;
    const auto* q = scene.world().resource<EventQueue>();
    return q ? q->events : kNone;
}

std::uint64_t delivered_through(Scene& scene) {
    const auto* q = scene.world().resource<EventQueue>();
    return q ? q->delivered : 0;
}

void mark_delivered(Scene& scene, std::uint64_t through_id) {
    if (auto* q = scene.world().resource<EventQueue>()) q->delivered = std::max(q->delivered, through_id);
}

SequenceLoader directory_loader(std::filesystem::path assets_root) {
    return [root = std::move(assets_root)](std::string_view path) -> Result<std::string> {
        auto clean = fs::sanitize_relative(path);
        if (!clean) return std::move(clean).error();
        return fs::read_text(root / *clean, 64u * 1024u * 1024u);
    };
}

void register_components() {
    static std::once_flag once;
    std::call_once(once, [] {
        scene::Schema<SequencePlayerComponent>("SequencePlayer", "Cinematics")
            .field(&SequencePlayerComponent::sequence, "sequence",
                   {.tooltip = "Sequence asset (.vseq) relative to the assets folder", .hint = "sequence"})
            .field(&SequencePlayerComponent::play_on_start, "play_on_start",
                   {.tooltip = "Start playing when the scene starts"})
            .field(&SequencePlayerComponent::loop, "loop")
            .field(&SequencePlayerComponent::speed, "speed",
                   {.min = -10, .max = 10, .speed = 0.01f, .tooltip = "Playback rate (negative plays backwards)"})
            .field(&SequencePlayerComponent::enabled, "enabled")
            .commit();
    });
}

void bind(Scene& scene, SequencerSystem* system) {
    if (auto* b = scene.world().resource<Binding>())
        b->system = system;
    else
        scene.world().set_resource<Binding>().system = system;
}

SequencerSystem* sequencer_of(Scene& scene) {
    const auto* b = scene.world().resource<Binding>();
    return b ? b->system : nullptr;
}

void add_sequencer_systems(gameplay::Simulation& sim, SequencerSystem& system) {
    register_components();
    SequencerSystem* s = &system;
    sim.add_system("sequencer.bind", gameplay::Phase::PreUpdate, [s](Scene& scene, const gameplay::FrameTime&) {
        if (sequencer_of(scene) != s) bind(scene, s);
    });
    sim.add_system("sequencer.update", gameplay::Phase::Update, [s](Scene& scene, const gameplay::FrameTime& t) {
        if (sequencer_of(scene) != s) bind(scene, s);
        s->update(scene, t.dt);
    });
}

} // namespace victus::sequencer
