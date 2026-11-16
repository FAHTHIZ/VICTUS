// SPDX-License-Identifier: Apache-2.0
#include "victus/audio/audio.hpp"

#include "victus/core/fs.hpp"
#include "victus/core/log.hpp"
#include "victus/scene/reflect.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace victus::audio {

namespace {
constexpr std::array<std::string_view, kBusCount> kBusNames{"Master", "Music", "Sfx", "Ui", "Ambient", "Voice"};
} // namespace

std::string_view bus_name(Bus b) noexcept {
    const auto i = static_cast<std::size_t>(b);
    return i < kBusCount ? kBusNames[i] : std::string_view{"Sfx"};
}

std::optional<Bus> parse_bus(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kBusCount; ++i) {
        const auto n = kBusNames[i];
        if (n.size() == name.size() &&
            std::equal(n.begin(), n.end(), name.begin(), [](char a, char b) { return std::tolower(a) == std::tolower(b); }))
            return static_cast<Bus>(i);
    }
    return std::nullopt;
}

void register_components() {
    static std::once_flag once;
    std::call_once(once, [] {
        scene::Schema<AudioSourceComponent>("AudioSource", "Audio")
            .field(&AudioSourceComponent::clip, "clip", {.tooltip = "Asset-relative path (.wav .ogg .mp3 .flac)", .hint = "audio"})
            .enumeration(&AudioSourceComponent::bus, "bus", {"master", "music", "sfx", "ui", "ambient", "voice"})
            .field(&AudioSourceComponent::volume, "volume", {.min = 0, .max = 4, .speed = 0.01f})
            .field(&AudioSourceComponent::pitch, "pitch", {.min = 0.05f, .max = 4, .speed = 0.01f})
            .field(&AudioSourceComponent::loop, "loop")
            .field(&AudioSourceComponent::play_on_start, "play_on_start")
            .field(&AudioSourceComponent::stream, "stream", {.tooltip = "Decode while playing (music, long ambience)"})
            .field(&AudioSourceComponent::spatial, "spatial", {.tooltip = "Position from the entity, attenuated relative to the listener"})
            .enumeration(&AudioSourceComponent::attenuation, "attenuation", {"none", "inverse", "linear", "exponential"})
            .field(&AudioSourceComponent::min_distance, "min_distance", {.min = 0.01f, .max = 1e5f, .speed = 0.05f})
            .field(&AudioSourceComponent::max_distance, "max_distance", {.min = 0.01f, .max = 1e6f, .speed = 0.1f})
            .field(&AudioSourceComponent::rolloff, "rolloff", {.min = 0, .max = 20, .speed = 0.01f})
            .commit();
        scene::Schema<AudioListenerComponent>("AudioListener", "Audio").field(&AudioListenerComponent::enabled, "enabled").commit();
    });
}

namespace {

struct Voice {
    std::unique_ptr<ma_sound> sound;
    std::string clip;
    bool stream{false};
    Bus bus{Bus::Sfx};
    std::uint64_t seen{0};
};

ma_attenuation_model to_ma(Attenuation a) {
    switch (a) {
    case Attenuation::None: return ma_attenuation_model_none;
    case Attenuation::Linear: return ma_attenuation_model_linear;
    case Attenuation::Exponential: return ma_attenuation_model_exponential;
    default: return ma_attenuation_model_inverse;
    }
}

ma_vec3f world_pos(const math::Mat4& m) { return {m[3].x, m[3].y, m[3].z}; }

} // namespace

// miniaudio VFS that serves clips from mounted packs (exported games) and falls back to the
// default stdio VFS for loose files. Packed clips are read into memory once per open; streamed
// music therefore streams from memory (decoding stays incremental).
struct PackVfs {
    ma_vfs_callbacks cb{}; // must be first: miniaudio casts ma_vfs* to ma_vfs_callbacks*
    ma_default_vfs fallback{};

    struct File {
        bool memory{false};
        std::vector<std::uint8_t> data;
        std::size_t pos{0};
        ma_vfs_file inner{nullptr};
    };

    static PackVfs& self(ma_vfs* v) { return *reinterpret_cast<PackVfs*>(v); }

    static ma_result open_path(ma_vfs* v, const fs::Path& path, ma_uint32 mode, ma_vfs_file* out, const char* narrow,
                               const wchar_t* wide) {
        if (!out) return MA_INVALID_ARGS;
        *out = nullptr;
        auto f = std::make_unique<File>();
        if (fs::is_virtual(path)) {
            if (mode & MA_OPEN_MODE_WRITE) return MA_ACCESS_DENIED;
            auto bytes = fs::read_binary(path, 1ull << 31);
            if (!bytes) return MA_DOES_NOT_EXIST;
            f->memory = true;
            f->data = std::move(bytes).value();
        } else {
            ma_vfs* fb = reinterpret_cast<ma_vfs*>(&self(v).fallback);
            const ma_result r = wide ? ma_vfs_open_w(fb, wide, mode, &f->inner) : ma_vfs_open(fb, narrow, mode, &f->inner);
            if (r != MA_SUCCESS) return r;
        }
        *out = f.release();
        return MA_SUCCESS;
    }
    static ma_result on_open(ma_vfs* v, const char* p, ma_uint32 mode, ma_vfs_file* out) {
        if (!p) return MA_INVALID_ARGS;
        return open_path(v, fs::Path(p), mode, out, p, nullptr);
    }
    static ma_result on_open_w(ma_vfs* v, const wchar_t* p, ma_uint32 mode, ma_vfs_file* out) {
        if (!p) return MA_INVALID_ARGS;
        return open_path(v, fs::Path(p), mode, out, nullptr, p);
    }
    static ma_result on_close(ma_vfs* v, ma_vfs_file h) {
        std::unique_ptr<File> f(static_cast<File*>(h));
        if (f && !f->memory && f->inner) return ma_vfs_close(reinterpret_cast<ma_vfs*>(&self(v).fallback), f->inner);
        return MA_SUCCESS;
    }
    static ma_result on_read(ma_vfs* v, ma_vfs_file h, void* dst, size_t n, size_t* read) {
        File& f = *static_cast<File*>(h);
        if (!f.memory) return ma_vfs_read(reinterpret_cast<ma_vfs*>(&self(v).fallback), f.inner, dst, n, read);
        const std::size_t avail = f.pos < f.data.size() ? f.data.size() - f.pos : 0;
        const std::size_t count = std::min(avail, n);
        if (count) std::memcpy(dst, f.data.data() + f.pos, count);
        f.pos += count;
        if (read) *read = count;
        return count == 0 && n > 0 ? MA_AT_END : MA_SUCCESS;
    }
    static ma_result on_write(ma_vfs* v, ma_vfs_file h, const void* src, size_t n, size_t* written) {
        File& f = *static_cast<File*>(h);
        if (f.memory) return MA_ACCESS_DENIED;
        return ma_vfs_write(reinterpret_cast<ma_vfs*>(&self(v).fallback), f.inner, src, n, written);
    }
    static ma_result on_seek(ma_vfs* v, ma_vfs_file h, ma_int64 offset, ma_seek_origin origin) {
        File& f = *static_cast<File*>(h);
        if (!f.memory) return ma_vfs_seek(reinterpret_cast<ma_vfs*>(&self(v).fallback), f.inner, offset, origin);
        ma_int64 base = 0;
        if (origin == ma_seek_origin_current) base = static_cast<ma_int64>(f.pos);
        else if (origin == ma_seek_origin_end) base = static_cast<ma_int64>(f.data.size());
        const ma_int64 target = base + offset;
        if (target < 0 || target > static_cast<ma_int64>(f.data.size())) return MA_BAD_SEEK;
        f.pos = static_cast<std::size_t>(target);
        return MA_SUCCESS;
    }
    static ma_result on_tell(ma_vfs* v, ma_vfs_file h, ma_int64* cursor) {
        File& f = *static_cast<File*>(h);
        if (!f.memory) return ma_vfs_tell(reinterpret_cast<ma_vfs*>(&self(v).fallback), f.inner, cursor);
        if (cursor) *cursor = static_cast<ma_int64>(f.pos);
        return MA_SUCCESS;
    }
    static ma_result on_info(ma_vfs* v, ma_vfs_file h, ma_file_info* info) {
        File& f = *static_cast<File*>(h);
        if (!f.memory) return ma_vfs_info(reinterpret_cast<ma_vfs*>(&self(v).fallback), f.inner, info);
        if (info) info->sizeInBytes = f.data.size();
        return MA_SUCCESS;
    }

    PackVfs() {
        cb.onOpen = on_open;
        cb.onOpenW = on_open_w;
        cb.onClose = on_close;
        cb.onRead = on_read;
        cb.onWrite = on_write;
        cb.onSeek = on_seek;
        cb.onTell = on_tell;
        cb.onInfo = on_info;
        ma_default_vfs_init(&fallback, nullptr);
    }
};

struct AudioEngine::Impl {
    EngineOptions options;
    PackVfs vfs; // outlives `engine` (uninitialized explicitly in ~AudioEngine)
    ma_engine engine{};
    bool engine_ok{false};
    bool device{false};
    std::array<ma_sound_group, kBusCount> groups{};
    std::array<bool, kBusCount> group_ok{};
    std::array<float, kBusCount> volume{};
    std::array<bool, kBusCount> muted{};
    std::unordered_map<Entity, Voice, ecs::EntityHash> voices;
    std::unordered_set<Entity, ecs::EntityHash> autostarted;
    std::deque<std::unique_ptr<ma_sound>> one_shots;
    std::unordered_set<std::string> loaded, failed;
    std::uint32_t load_errors{0};
    std::uint64_t frame{0};

    std::unordered_map<std::string, std::unique_ptr<ma_sound>> preloaded; // holds cached clip data

    // Releases every playing sound (used by stop_all and the destructor). Preloads are kept.
    void release_all() {
        for (auto& [e, v] : voices)
            if (v.sound) ma_sound_uninit(v.sound.get());
        voices.clear();
        for (auto& s : one_shots) ma_sound_uninit(s.get());
        one_shots.clear();
    }

    void apply_bus(Bus b) {
        const auto i = static_cast<std::size_t>(b);
        if (group_ok[i]) ma_sound_group_set_volume(&groups[i], muted[i] ? 0.0f : volume[i]);
    }

    Result<fs::Path> resolve(std::string_view clip) const {
        auto p = fs::resolve_under(options.assets_root, clip);
        if (!p) return make_error(ErrorCode::InvalidArgument, "invalid audio clip path '" + std::string(clip) + "'");
        return std::move(p).value();
    }

    // Creates (does not start) a sound for `clip` on `bus`. Decoded clips are cached by the
    // resource manager, so repeated sounds of the same clip share one decoded buffer.
    Result<std::unique_ptr<ma_sound>> open(std::string_view clip, Bus bus, bool stream) {
        auto path = resolve(clip);
        if (!path) return path.error();
        auto sound = std::make_unique<ma_sound>();
        const ma_uint32 flags = stream ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
        ma_sound_group* group = group_ok[static_cast<std::size_t>(bus)] ? &groups[static_cast<std::size_t>(bus)] : nullptr;
#if defined(_WIN32)
        const ma_result r = ma_sound_init_from_file_w(&engine, path->wstring().c_str(), flags, group, nullptr, sound.get());
#else
        const ma_result r = ma_sound_init_from_file(&engine, path->string().c_str(), flags, group, nullptr, sound.get());
#endif
        if (r != MA_SUCCESS) {
            ++load_errors;
            const std::string key(clip);
            if (failed.insert(key).second)
                VICTUS_LOG_WARN("audio", "cannot open clip '{}': {}", key, ma_result_description(r));
            return make_error(ErrorCode::IoError, "cannot open audio clip '" + key + "': " + ma_result_description(r));
        }
        loaded.insert(std::string(clip));
        return sound;
    }

    void configure(ma_sound* s, const AudioSourceComponent& c, const math::Mat4* world) {
        ma_sound_set_volume(s, std::max(0.0f, c.volume));
        ma_sound_set_pitch(s, std::max(0.01f, c.pitch));
        ma_sound_set_looping(s, c.loop ? MA_TRUE : MA_FALSE);
        ma_sound_set_spatialization_enabled(s, c.spatial ? MA_TRUE : MA_FALSE);
        if (c.spatial) {
            ma_sound_set_attenuation_model(s, to_ma(c.attenuation));
            ma_sound_set_min_distance(s, std::max(0.0f, c.min_distance));
            ma_sound_set_max_distance(s, std::max(c.min_distance, c.max_distance));
            ma_sound_set_rolloff(s, std::max(0.0f, c.rolloff));
            if (world) {
                const ma_vec3f p = world_pos(*world);
                ma_sound_set_position(s, p.x, p.y, p.z);
            }
        }
    }

    void destroy(Voice& v) {
        if (v.sound) ma_sound_uninit(v.sound.get());
        v.sound.reset();
    }

    Status start_voice(Entity e, const AudioSourceComponent& c, const math::Mat4& world) {
        auto it = voices.find(e);
        if (it != voices.end() && (it->second.clip != c.clip || it->second.stream != c.stream || it->second.bus != c.bus)) {
            destroy(it->second);
            voices.erase(it);
            it = voices.end();
        }
        if (it == voices.end()) {
            if (c.clip.empty()) return make_error(ErrorCode::InvalidArgument, "audio source has no clip");
            auto s = open(c.clip, c.bus, c.stream);
            if (!s) return s.error();
            Voice v;
            v.sound = std::move(s).value();
            v.clip = c.clip;
            v.stream = c.stream;
            v.bus = c.bus;
            v.seen = frame;
            it = voices.emplace(e, std::move(v)).first;
        }
        ma_sound* snd = it->second.sound.get();
        configure(snd, c, &world);
        ma_sound_seek_to_pcm_frame(snd, 0);
        ma_sound_start(snd);
        return {};
    }

    void update_listener(scene::Scene& s) {
        bool found = false;
        math::Mat4 m;
        s.world().each<AudioListenerComponent>([&](Entity e, AudioListenerComponent& l) {
            if (found || !l.enabled) return;
            m = s.world_matrix(e);
            found = true;
        });
        if (!found) {
            Entity best;
            bool best_primary = false;
            s.world().each<scene::CameraComponent>([&](Entity e, scene::CameraComponent& c) {
                if (best.is_null() || (c.primary && !best_primary)) {
                    best = e;
                    best_primary = c.primary;
                }
            });
            if (!best.is_null()) {
                m = s.world_matrix(best);
                found = true;
            }
        }
        if (!found) m = math::Mat4::identity();
        const math::Vec3 fwd = math::normalize(-m[2].xyz());
        const math::Vec3 up = math::normalize(m[1].xyz());
        ma_engine_listener_set_position(&engine, 0, m[3].x, m[3].y, m[3].z);
        ma_engine_listener_set_direction(&engine, 0, fwd.x, fwd.y, fwd.z);
        ma_engine_listener_set_world_up(&engine, 0, up.x, up.y, up.z);
    }

    void update(scene::Scene& s) {
        ++frame;
        update_listener(s);
        s.world().each<AudioSourceComponent>([&](Entity e, AudioSourceComponent& c) {
            const math::Mat4 world = s.world_matrix(e);
            auto it = voices.find(e);
            if (it != voices.end()) {
                Voice& v = it->second;
                if (v.clip != c.clip || v.stream != c.stream || v.bus != c.bus) {
                    const bool was_playing = v.sound && ma_sound_is_playing(v.sound.get());
                    destroy(v);
                    voices.erase(it);
                    if (was_playing && !c.clip.empty()) (void)start_voice(e, c, world);
                    if (auto again = voices.find(e); again != voices.end()) again->second.seen = frame;
                    return;
                }
                v.seen = frame;
                configure(v.sound.get(), c, &world);
                return;
            }
            if (c.play_on_start && !c.clip.empty() && autostarted.insert(e).second) {
                if (start_voice(e, c, world)) voices[e].seen = frame;
            }
        });
        for (auto it = voices.begin(); it != voices.end();) {
            if (it->second.seen != frame) {
                destroy(it->second);
                it = voices.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = autostarted.begin(); it != autostarted.end();) {
            if (!s.world().alive(*it) || !s.world().has<AudioSourceComponent>(*it)) it = autostarted.erase(it);
            else ++it;
        }
        reap_one_shots();
    }

    void reap_one_shots() {
        for (auto it = one_shots.begin(); it != one_shots.end();) {
            ma_sound* snd = it->get();
            if (ma_sound_at_end(snd) || !ma_sound_is_playing(snd)) {
                ma_sound_uninit(snd);
                it = one_shots.erase(it);
            } else {
                ++it;
            }
        }
    }
};

AudioEngine::AudioEngine(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

AudioEngine::~AudioEngine() {
    if (!impl_) return;
    impl_->release_all();
    for (auto& [k, snd] : impl_->preloaded) ma_sound_uninit(snd.get());
    impl_->preloaded.clear();
    for (std::size_t i = kBusCount; i-- > 0;)
        if (impl_->group_ok[i]) ma_sound_group_uninit(&impl_->groups[i]);
    if (impl_->engine_ok) ma_engine_uninit(&impl_->engine);
}

Result<std::unique_ptr<AudioEngine>> AudioEngine::create(const EngineOptions& options) {
    auto impl = std::make_unique<Impl>();
    impl->options = options;
    impl->volume.fill(1.0f);
    ma_engine_config cfg = ma_engine_config_init();
    cfg.pResourceManagerVFS = reinterpret_cast<ma_vfs*>(&impl->vfs);
    cfg.channels = std::clamp<std::uint32_t>(options.channels, 1, 8);
    cfg.sampleRate = std::clamp<std::uint32_t>(options.sample_rate, 8000, 192000);
    ma_result r = MA_ERROR;
    if (!options.null_device) {
        r = ma_engine_init(&cfg, &impl->engine);
        if (r == MA_SUCCESS) impl->device = true;
        else VICTUS_LOG_WARN("audio", "no audio output device ({}); mixing without output", ma_result_description(r));
    }
    if (r != MA_SUCCESS) {
        cfg.noDevice = MA_TRUE;
        r = ma_engine_init(&cfg, &impl->engine);
        if (r != MA_SUCCESS)
            return make_error(ErrorCode::DeviceError, std::string("audio engine init failed: ") + ma_result_description(r));
    }
    impl->engine_ok = true;
    for (std::size_t i = 0; i < kBusCount; ++i) {
        ma_sound_group* parent = i == 0 ? nullptr : (impl->group_ok[0] ? &impl->groups[0] : nullptr);
        impl->group_ok[i] = ma_sound_group_init(&impl->engine, 0, parent, &impl->groups[i]) == MA_SUCCESS;
        if (!impl->group_ok[i]) return make_error(ErrorCode::DeviceError, "audio bus creation failed");
    }
    return std::unique_ptr<AudioEngine>(new AudioEngine(std::move(impl)));
}

bool AudioEngine::has_device() const noexcept { return impl_->device; }
std::uint32_t AudioEngine::sample_rate() const noexcept { return ma_engine_get_sample_rate(&impl_->engine); }
std::uint32_t AudioEngine::channels() const noexcept { return ma_engine_get_channels(&impl_->engine); }

void AudioEngine::update(scene::Scene& scene) { impl_->update(scene); }

void AudioEngine::stop_all() {
    impl_->release_all();
    impl_->autostarted.clear();
}

Status AudioEngine::play(scene::Scene& scene, Entity e) {
    scene::Scene* s = &scene;
    if (!s->world().alive(e)) return make_error(ErrorCode::NotFound, "entity is not in the audio scene");
    const auto* c = s->world().get<AudioSourceComponent>(e);
    if (!c) return make_error(ErrorCode::NotFound, "entity has no AudioSource");
    impl_->autostarted.insert(e);
    auto st = impl_->start_voice(e, *c, s->world_matrix(e));
    if (st) impl_->voices[e].seen = impl_->frame;
    return st;
}

void AudioEngine::stop(Entity e) {
    if (auto it = impl_->voices.find(e); it != impl_->voices.end() && it->second.sound) ma_sound_stop(it->second.sound.get());
}

void AudioEngine::set_paused(Entity e, bool paused) {
    auto it = impl_->voices.find(e);
    if (it == impl_->voices.end() || !it->second.sound) return;
    if (paused) ma_sound_stop(it->second.sound.get());
    else if (!ma_sound_at_end(it->second.sound.get())) ma_sound_start(it->second.sound.get());
}

bool AudioEngine::is_playing(Entity e) const {
    const auto it = impl_->voices.find(e);
    return it != impl_->voices.end() && it->second.sound && ma_sound_is_playing(it->second.sound.get()) &&
           !ma_sound_at_end(it->second.sound.get());
}

Status AudioEngine::play_one_shot(std::string_view clip, const OneShot& o) {
    impl_->reap_one_shots();
    auto s = impl_->open(clip, o.bus, false);
    if (!s) return s.error();
    ma_sound* snd = s->get();
    ma_sound_set_volume(snd, std::max(0.0f, o.volume));
    ma_sound_set_pitch(snd, std::max(0.01f, o.pitch));
    ma_sound_set_spatialization_enabled(snd, o.position ? MA_TRUE : MA_FALSE);
    if (o.position) ma_sound_set_position(snd, o.position->x, o.position->y, o.position->z);
    ma_sound_start(snd);
    impl_->one_shots.push_back(std::move(s).value());
    const std::size_t cap = std::max<std::uint32_t>(1, impl_->options.max_one_shots);
    while (impl_->one_shots.size() > cap) {
        ma_sound_uninit(impl_->one_shots.front().get());
        impl_->one_shots.pop_front();
    }
    return {};
}

Status AudioEngine::preload(std::string_view clip) {
    const std::string key(clip);
    if (impl_->preloaded.contains(key)) return {};
    // The resource manager reference-counts decoded data; keeping one idle sound alive keeps the
    // clip resident so later sounds of it start without decoding.
    auto s = impl_->open(clip, Bus::Sfx, false);
    if (!s) return s.error();
    impl_->preloaded.emplace(key, std::move(s).value());
    return {};
}

std::size_t AudioEngine::reload_clip(std::string_view clip) {
    const std::string key(clip);
    if (auto it = impl_->preloaded.find(key); it != impl_->preloaded.end()) {
        ma_sound_uninit(it->second.get());
        impl_->preloaded.erase(it);
    }
    impl_->failed.erase(key);
    // Release every voice of the clip first: decoded data is shared and reference-counted, so
    // it is only read again once nothing holds it.
    struct Restart {
        Entity e;
        std::unique_ptr<ma_sound> old;
        bool stream;
        Bus bus;
        bool playing;
        float volume, pitch;
        bool looping, spatial;
        ma_vec3f position;
        float min_d, max_d, rolloff;
        ma_attenuation_model model;
    };
    std::vector<Restart> restarts;
    for (auto& [e, v] : impl_->voices) {
        if (v.clip != key || !v.sound) continue;
        ma_sound* s = v.sound.get();
        restarts.push_back({e, std::move(v.sound), v.stream, v.bus,
                            ma_sound_is_playing(s) == MA_TRUE && ma_sound_at_end(s) != MA_TRUE, ma_sound_get_volume(s),
                            ma_sound_get_pitch(s), ma_sound_is_looping(s) == MA_TRUE,
                            ma_sound_is_spatialization_enabled(s) == MA_TRUE, ma_sound_get_position(s),
                            ma_sound_get_min_distance(s), ma_sound_get_max_distance(s), ma_sound_get_rolloff(s),
                            ma_sound_get_attenuation_model(s)});
        ma_sound_uninit(s);
    }
    std::size_t restarted = 0;
    for (auto& r : restarts) {
        auto& v = impl_->voices[r.e];
        auto s = impl_->open(key, r.bus, r.stream);
        if (!s) {
            impl_->voices.erase(r.e); // the new file is broken; the source stays silent
            continue;
        }
        v.sound = std::move(s).value();
        ma_sound* n = v.sound.get();
        ma_sound_set_volume(n, r.volume);
        ma_sound_set_pitch(n, r.pitch);
        ma_sound_set_looping(n, r.looping ? MA_TRUE : MA_FALSE);
        ma_sound_set_spatialization_enabled(n, r.spatial ? MA_TRUE : MA_FALSE);
        ma_sound_set_position(n, r.position.x, r.position.y, r.position.z);
        ma_sound_set_min_distance(n, r.min_d);
        ma_sound_set_max_distance(n, r.max_d);
        ma_sound_set_rolloff(n, r.rolloff);
        ma_sound_set_attenuation_model(n, r.model);
        if (r.playing) {
            ma_sound_start(n);
            ++restarted;
        }
    }
    return restarted;
}

void AudioEngine::set_bus_volume(Bus b, float v) {
    const auto i = static_cast<std::size_t>(b);
    if (i >= kBusCount) return;
    impl_->volume[i] = std::max(0.0f, v);
    impl_->apply_bus(b);
}
float AudioEngine::bus_volume(Bus b) const {
    const auto i = static_cast<std::size_t>(b);
    return i < kBusCount ? impl_->volume[i] : 0.0f;
}
void AudioEngine::set_bus_muted(Bus b, bool m) {
    const auto i = static_cast<std::size_t>(b);
    if (i >= kBusCount) return;
    impl_->muted[i] = m;
    impl_->apply_bus(b);
}
bool AudioEngine::bus_muted(Bus b) const {
    const auto i = static_cast<std::size_t>(b);
    return i < kBusCount && impl_->muted[i];
}

std::size_t AudioEngine::render(float* out, std::size_t frames) {
    if (impl_->device || !out || frames == 0) return 0;
    ma_uint64 read = 0;
    if (ma_engine_read_pcm_frames(&impl_->engine, out, frames, &read) != MA_SUCCESS) return 0;
    return static_cast<std::size_t>(read);
}

AudioStats AudioEngine::stats() const {
    AudioStats st;
    st.device = impl_->device;
    st.voices = static_cast<std::uint32_t>(impl_->voices.size());
    for (const auto& [e, v] : impl_->voices)
        if (v.sound && ma_sound_is_playing(v.sound.get()) && !ma_sound_at_end(v.sound.get())) ++st.playing;
    st.one_shots = static_cast<std::uint32_t>(impl_->one_shots.size());
    st.clips_loaded = static_cast<std::uint32_t>(impl_->loaded.size());
    st.load_errors = impl_->load_errors;
    return st;
}

// ---- systems and binding ------------------------------------------------------------------------

namespace {
struct Binding {
    AudioEngine* engine{nullptr};
};
} // namespace

void bind(scene::Scene& scene, AudioEngine* engine) {
    if (auto* b = scene.world().resource<Binding>()) b->engine = engine;
    else scene.world().set_resource<Binding>().engine = engine;
}

AudioEngine* engine_of(scene::Scene& scene) {
    const auto* b = scene.world().resource<Binding>();
    return b ? b->engine : nullptr;
}

void add_audio_systems(gameplay::Simulation& sim, AudioEngine& engine) {
    AudioEngine* a = &engine;
    sim.add_system("audio.bind", gameplay::Phase::PreUpdate, [a](scene::Scene& scene, const gameplay::FrameTime&) {
        if (engine_of(scene) != a) bind(scene, a);
    });
    sim.add_system("audio.update", gameplay::Phase::PostUpdate, [a](scene::Scene& scene, const gameplay::FrameTime&) {
        if (engine_of(scene) != a) bind(scene, a);
        a->update(scene);
    });
}

bool audio_disabled_by_env() noexcept {
    const char* v = std::getenv("VICTUS_NO_AUDIO");
    return v && *v && std::strcmp(v, "0") != 0;
}

// ---- WAV writer ----------------------------------------------------------------------------------

std::vector<std::uint8_t> encode_wav(const float* data, std::size_t frames, std::uint32_t channels, std::uint32_t rate) {
    const std::uint64_t samples = static_cast<std::uint64_t>(frames) * channels;
    const std::uint64_t bytes = samples * 2;
    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(44 + bytes));
    auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    auto u16 = [&](std::uint16_t v) {
        out.push_back(static_cast<std::uint8_t>(v));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    auto tag = [&](const char* t) { out.insert(out.end(), t, t + 4); };
    tag("RIFF");
    u32(static_cast<std::uint32_t>(36 + bytes));
    tag("WAVE");
    tag("fmt ");
    u32(16);
    u16(1); // PCM
    u16(static_cast<std::uint16_t>(channels));
    u32(rate);
    u32(rate * channels * 2);
    u16(static_cast<std::uint16_t>(channels * 2));
    u16(16);
    tag("data");
    u32(static_cast<std::uint32_t>(bytes));
    for (std::uint64_t i = 0; i < samples; ++i) {
        const float v = std::clamp(data[i], -1.0f, 1.0f);
        u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lrint(v * 32767.0f))));
    }
    return out;
}

Status write_wav(const fs::Path& path, const float* data, std::size_t frames, std::uint32_t channels, std::uint32_t rate) {
    if (channels == 0 || rate == 0) return make_error(ErrorCode::InvalidArgument, "write_wav: channels and rate must be > 0");
    if (static_cast<std::uint64_t>(frames) * channels * 2 > 0xFFFFFFFFull - 44)
        return make_error(ErrorCode::LimitExceeded, "write_wav: file too large");
    return fs::write_atomic(path, encode_wav(data, frames, channels, rate));
}

} // namespace victus::audio
