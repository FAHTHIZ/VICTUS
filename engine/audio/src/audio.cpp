// SPDX-License-Identifier: Apache-2.0
#include "victus/audio/audio.hpp"

#include "acoustics.hpp"
#include "victus/gameplay/queries.hpp"
#include "victus/gameplay/water.hpp"

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
#include <optional>
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
            .field(&AudioSourceComponent::priority, "priority",
                   {.min = 0, .max = 255, .tooltip = "0-255; higher keeps a real voice when more sounds play than the voice budget"})
            .field(&AudioSourceComponent::occlusion, "occlusion", {.tooltip = "Walls between listener and source attenuate and muffle it"})
            .field(&AudioSourceComponent::occlusion_radius, "occlusion_radius",
                   {.min = 0, .max = 100, .speed = 0.01f, .tooltip = "> 0: five rays across this radius for partial occlusion"})
            .commit();
        scene::Schema<AudioListenerComponent>("AudioListener", "Audio").field(&AudioListenerComponent::enabled, "enabled").commit();
        scene::Schema<AcousticMaterialComponent>("AcousticMaterial", "Audio")
            .enumeration(&AcousticMaterialComponent::preset, "preset",
                         {"generic", "brick", "concrete", "ceramic", "gravel", "carpet", "glass", "plaster", "wood", "metal", "rock",
                          "custom"})
            .field(&AcousticMaterialComponent::transmission, "transmission",
                   {.min = 0, .max = 1, .speed = 0.001f,
                    .tooltip = "Custom only: energy transmitted at 400 Hz / 2.5 kHz / 15 kHz (0-1)"})
            .commit();
        scene::Schema<ReverbZoneComponent>("ReverbZone", "Audio")
            .field(&ReverbZoneComponent::enabled, "enabled")
            .field(&ReverbZoneComponent::size, "size", {.min = 0, .max = 1e5f, .speed = 0.05f, .tooltip = "Box size in local space"})
            .field(&ReverbZoneComponent::fade, "fade", {.min = 0, .max = 1000, .speed = 0.05f, .tooltip = "Metres outside the box over which the zone fades"})
            .field(&ReverbZoneComponent::decay, "decay", {.min = 0, .max = 0.95f, .speed = 0.005f})
            .field(&ReverbZoneComponent::wet, "wet", {.min = 0, .max = 1, .speed = 0.005f, .tooltip = "Reverb send of the Sfx, Ambient and Voice buses"})
            .field(&ReverbZoneComponent::lowpass, "lowpass", {.min = 0, .max = 24000, .speed = 10, .tooltip = "Hz, 0 = none"})
            .field(&ReverbZoneComponent::priority, "priority", {.min = -1000, .max = 1000})
            .commit();
    });
}

namespace {

struct Voice {
    std::unique_ptr<ma_sound> sound;
    std::string clip;
    bool stream{false};
    Bus bus{Bus::Sfx};
    std::uint64_t seen{0};
    std::int32_t priority{128};
    bool virt{false};             // logically playing, not mixed
    ma_uint64 virt_cursor{0};     // sound frames at the moment it went virtual
    ma_uint64 virt_since{0};      // engine time (frames) at that moment
    // Occlusion (ADR-0096): sound -> lpf -> bus group while filtered.
    float base_volume{1.0f};      // component volume before occlusion
    bool occl{false};
    float occl_radius{0.0f};
    bool occl_traced{false};      // first trace snaps instead of smoothing
    float occl_blocked{0.0f};
    float occl_gain_target{1.0f}, occl_gain{1.0f};
    float occl_cut_target{detail::kNoFilterHz}, occl_cut{detail::kNoFilterHz}; // kNoFilterHz = none
    std::unique_ptr<ma_lpf_node> lpf;
};

struct OneShotVoice {
    std::unique_ptr<ma_sound> sound;
    std::string clip;
    Bus bus{Bus::Sfx};
    std::int32_t priority{128};
    std::uint64_t order{0};
    bool dying{false};            // fading out; reaped once silent
};

// Restarts a sound that may have a fade-out stop scheduled (miniaudio keeps it otherwise).
void start_sound(ma_sound* s, bool fade_in) {
    ma_sound_reset_stop_time_and_fade(s);
    if (fade_in) ma_sound_set_fade_in_milliseconds(s, 0.0f, 1.0f, static_cast<ma_uint64>(kVoiceFadeMs));
    ma_sound_start(s);
}

void fade_out(ma_sound* s) { ma_sound_stop_with_fade_in_milliseconds(s, static_cast<ma_uint64>(kVoiceFadeMs)); }

// miniaudio's distance models, evaluated on the CPU for voice ranking.
float distance_gain(const ma_sound* s, const ma_vec3f& listener) {
    if (!ma_sound_is_spatialization_enabled(s)) return 1.0f;
    const ma_vec3f p = ma_sound_get_position(s);
    const float d = std::sqrt((p.x - listener.x) * (p.x - listener.x) + (p.y - listener.y) * (p.y - listener.y) +
                              (p.z - listener.z) * (p.z - listener.z));
    const float lo = std::max(1e-4f, ma_sound_get_min_distance(s));
    const float hi = std::max(lo, ma_sound_get_max_distance(s));
    const float r = ma_sound_get_rolloff(s);
    const float x = std::clamp(d, lo, hi);
    switch (ma_sound_get_attenuation_model(s)) {
    case ma_attenuation_model_none: return 1.0f;
    case ma_attenuation_model_linear: return hi > lo ? std::clamp(1.0f - r * (x - lo) / (hi - lo), 0.0f, 1.0f) : 1.0f;
    case ma_attenuation_model_exponential: return std::pow(x / lo, -r);
    default: return lo / (lo + r * (x - lo));
    }
}

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
    // Effect graph per bus: group -> splitter; splitter out 0 -> (lpf ->) parent, out 1 ->
    // reverb input (Master has no send: the reverb returns into Master).
    std::array<ma_splitter_node, kBusCount> splitters{};
    std::array<ma_lpf_node, kBusCount> lpfs{};
    std::array<bool, kBusCount> fx_ok{};
    std::array<float, kBusCount> lowpass{};
    std::array<float, kBusCount> send{};
    static constexpr std::size_t kCombs = 4;
    ma_splitter_node reverb_in{};
    bool reverb_in_ok{false};
    std::array<ma_delay_node, kCombs> combs{};
    std::array<bool, kCombs> comb_ok{};
    float decay{0.7f};
    std::unordered_map<Entity, Voice, ecs::EntityHash> voices;
    std::unordered_set<Entity, ecs::EntityHash> autostarted;
    std::deque<OneShotVoice> one_shots;
    std::uint64_t one_shot_order{0};
    std::uint64_t stolen{0}, virtualised{0};
    std::unordered_set<std::string> loaded, failed;
    std::uint32_t load_errors{0};
    std::uint64_t frame{0};

    // Acoustic layer (ADR-0096): user settings above (lowpass/send/decay) plus zones and water
    // give the effective values below, which are what the graph runs.
    struct Acoustics {
        Entity zone;
        float zone_w{0}, zone_decay{0.7f}, zone_wet{0}, zone_log_lp{0};
        float underwater{0};
        std::array<float, kBusCount> send{}, lowpass{};
        float decay{0.7f};
        std::array<float, kBusCount> applied_send{}, applied_lowpass{};
        float applied_decay{0.7f};
        std::uint32_t rays{0}, occluded{0};
        std::uint64_t cursor{0}; // round-robin position (entity bits) for the ray budget
    } ac;
    std::vector<gameplay::SegmentHit> hits; // scratch

    std::unordered_map<std::string, std::unique_ptr<ma_sound>> preloaded; // holds cached clip data

    // Releases every playing sound (used by stop_all and the destructor). Preloads are kept.
    void release_all() {
        for (auto& [e, v] : voices) destroy(v);
        voices.clear();
        for (auto& s : one_shots) ma_sound_uninit(s.sound.get());
        one_shots.clear();
    }

    float bus_gain(Bus b) const {
        const auto i = static_cast<std::size_t>(b);
        float g = muted[i] ? 0.0f : volume[i];
        if (i != 0) g *= muted[0] ? 0.0f : volume[0];
        return g;
    }

    float audibility(const ma_sound* s, Bus b, const ma_vec3f& listener) const {
        return ma_sound_get_volume(s) * bus_gain(b) * distance_gain(s, listener);
    }

    // Where a virtual voice's playhead is now, or nullopt once a non-looping clip has ended.
    std::optional<ma_uint64> virtual_cursor(const Voice& v) {
        ma_sound* s = v.sound.get();
        ma_uint32 rate = 0;
        ma_sound_get_data_format(s, nullptr, nullptr, &rate, nullptr, 0);
        const double engine_rate = ma_engine_get_sample_rate(&engine);
        const double elapsed = static_cast<double>(ma_engine_get_time_in_pcm_frames(&engine) - v.virt_since);
        const double scale = rate && engine_rate > 0 ? rate / engine_rate : 1.0;
        ma_uint64 cursor = v.virt_cursor + static_cast<ma_uint64>(elapsed * ma_sound_get_pitch(s) * scale);
        ma_uint64 length = 0;
        if (ma_sound_get_length_in_pcm_frames(s, &length) == MA_SUCCESS && length > 0 && cursor >= length) {
            if (!ma_sound_is_looping(s)) return std::nullopt;
            cursor %= length;
        }
        return cursor;
    }

    void make_virtual(Voice& v) {
        ma_sound* s = v.sound.get();
        ma_sound_get_cursor_in_pcm_frames(s, &v.virt_cursor);
        v.virt_since = ma_engine_get_time_in_pcm_frames(&engine);
        fade_out(s);
        v.virt = true;
        ++virtualised;
    }

    // Leaves the virtual state: resumes in place (fading in) or, when the clip ended meanwhile, stops.
    void make_real(Voice& v, bool resume) {
        if (!v.virt) return;
        v.virt = false;
        const auto cursor = virtual_cursor(v);
        ma_sound* s = v.sound.get();
        if (!cursor) {
            ma_sound_stop(s);
            ma_sound_seek_to_pcm_frame(s, 0);
            return;
        }
        ma_sound_seek_to_pcm_frame(s, *cursor);
        if (resume) start_sound(s, true);
        else ma_sound_stop(s);
    }

    [[nodiscard]] static bool live(const Voice& v) {
        return v.sound && (v.virt || (ma_sound_is_playing(v.sound.get()) && !ma_sound_at_end(v.sound.get())));
    }

    // Ranks every playing voice and decides which ones are mixed (see the header).
    void arbitrate() {
        const ma_vec3f listener = ma_engine_listener_get_position(&engine, 0);
        struct Candidate {
            std::int32_t priority;
            float gain;
            std::uint64_t order;
            Voice* voice;
            OneShotVoice* shot;
        };
        std::vector<Candidate> c;
        c.reserve(voices.size() + one_shots.size());
        for (auto& [e, v] : voices) {
            if (!live(v)) continue;
            c.push_back({v.priority, audibility(v.sound.get(), v.bus, listener), e.bits(), &v, nullptr});
        }
        for (auto& o : one_shots) {
            if (o.dying || !ma_sound_is_playing(o.sound.get()) || ma_sound_at_end(o.sound.get())) continue;
            c.push_back({o.priority, audibility(o.sound.get(), o.bus, listener), o.order, nullptr, &o});
        }
        std::sort(c.begin(), c.end(), [](const Candidate& a, const Candidate& b) {
            if (a.priority != b.priority) return a.priority > b.priority;
            const bool ha = a.gain >= kInaudibleGain, hb = b.gain >= kInaudibleGain;
            if (ha != hb) return ha;
            if (a.gain != b.gain) return a.gain > b.gain;
            return a.order < b.order;
        });
        const std::size_t budget = std::max<std::uint32_t>(1, options.max_voices);
        std::size_t real = 0;
        for (Candidate& k : c) {
            if (k.voice) {
                const bool win = real < budget && k.gain >= kInaudibleGain;
                if (win) {
                    ++real;
                    make_real(*k.voice, true);
                } else if (!k.voice->virt) {
                    make_virtual(*k.voice);
                }
            } else if (real < budget) {
                ++real; // one-shots are short: they play while they win, even when quiet
            } else {
                fade_out(k.shot->sound.get());
                k.shot->dying = true;
                ++stolen;
            }
        }
    }

    void apply_bus(Bus b) {
        const auto i = static_cast<std::size_t>(b);
        if (group_ok[i]) ma_sound_group_set_volume(&groups[i], muted[i] ? 0.0f : volume[i]);
    }

    ma_node* bus_parent(std::size_t i) {
        return i == 0 ? ma_engine_get_endpoint(&engine) : static_cast<ma_node*>(&groups[0]);
    }

    // Routes the bus's dry path through its low-pass filter when one is set.
    void apply_lowpass(std::size_t i) {
        if (!fx_ok[i]) return;
        const auto rate = ma_engine_get_sample_rate(&engine);
        const float hz = ac.lowpass[i];
        ac.applied_lowpass[i] = hz;
        if (hz > 0.0f && hz < 0.5f * static_cast<float>(rate)) {
            const ma_lpf_config c = ma_lpf_config_init(ma_format_f32, ma_engine_get_channels(&engine), rate, hz, 2);
            ma_lpf_node_reinit(&c, &lpfs[i]);
            ma_node_attach_output_bus(&splitters[i], 0, &lpfs[i], 0);
        } else {
            ma_node_attach_output_bus(&splitters[i], 0, bus_parent(i), 0);
        }
    }

    void apply_send(std::size_t i) {
        if (!fx_ok[i] || i == 0 || !reverb_in_ok) return;
        ac.applied_send[i] = ac.send[i];
        ma_node_set_output_bus_volume(&splitters[i], 1, ac.send[i]);
        if (ac.send[i] > 0.0f) ma_node_attach_output_bus(&splitters[i], 1, &reverb_in, 0);
        else ma_node_detach_output_bus(&splitters[i], 1);
    }

    // Builds the per-bus splitters/filters and the shared reverb. Failure leaves effects off
    // (sound still plays through the groups' default routing).
    void init_effects() {
        ma_node_graph* graph = ma_engine_get_node_graph(&engine);
        const ma_uint32 ch = ma_engine_get_channels(&engine);
        const ma_uint32 rate = ma_engine_get_sample_rate(&engine);
        ma_splitter_node_config rc = ma_splitter_node_config_init(ch);
        rc.outputBusCount = kCombs;
        reverb_in_ok = ma_splitter_node_init(graph, &rc, nullptr, &reverb_in) == MA_SUCCESS;
        // Freeverb comb lengths at 44.1 kHz, scaled to the engine rate.
        constexpr std::array<double, kCombs> tuning{1557, 1617, 1491, 1422};
        for (std::size_t c = 0; reverb_in_ok && c < kCombs; ++c) {
            const auto frames = static_cast<ma_uint32>(tuning[c] * rate / 44100.0);
            ma_delay_node_config dc = ma_delay_node_config_init(ch, rate, frames, decay);
            dc.delay.wet = 1.0f; // ma_delay outputs only the delayed signal; `dry` scales the input
            dc.delay.dry = 1.0f;
            comb_ok[c] = ma_delay_node_init(graph, &dc, nullptr, &combs[c]) == MA_SUCCESS;
            if (!comb_ok[c]) continue;
            ma_node_attach_output_bus(&reverb_in, static_cast<ma_uint32>(c), &combs[c], 0);
            ma_node_set_output_bus_volume(&combs[c], 0, 1.0f / static_cast<float>(kCombs));
            ma_node_attach_output_bus(&combs[c], 0, &groups[0], 0);
        }
        for (std::size_t i = 0; i < kBusCount; ++i) {
            const ma_splitter_node_config sc = ma_splitter_node_config_init(ch);
            if (ma_splitter_node_init(graph, &sc, nullptr, &splitters[i]) != MA_SUCCESS) continue;
            const ma_lpf_node_config lc = ma_lpf_node_config_init(ch, rate, 0.25 * rate, 2);
            if (ma_lpf_node_init(graph, &lc, nullptr, &lpfs[i]) != MA_SUCCESS) {
                ma_splitter_node_uninit(&splitters[i], nullptr);
                continue;
            }
            fx_ok[i] = true;
            ma_node_attach_output_bus(&lpfs[i], 0, bus_parent(i), 0);
            ma_node_detach_output_bus(&splitters[i], 1);
            apply_lowpass(i);
            ma_node_attach_output_bus(&groups[i], 0, &splitters[i], 0);
        }
    }

    void uninit_effects() {
        for (std::size_t i = kBusCount; i-- > 0;) {
            if (!fx_ok[i]) continue;
            if (group_ok[i]) ma_node_detach_output_bus(&groups[i], 0);
            ma_splitter_node_uninit(&splitters[i], nullptr);
            ma_lpf_node_uninit(&lpfs[i], nullptr);
            fx_ok[i] = false;
        }
        if (reverb_in_ok) ma_splitter_node_uninit(&reverb_in, nullptr);
        reverb_in_ok = false;
        for (std::size_t c = 0; c < kCombs; ++c)
            if (comb_ok[c]) ma_delay_node_uninit(&combs[c], nullptr);
        comb_ok.fill(false);
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

    void configure(Voice& v, const AudioSourceComponent& c, const math::Mat4* world) {
        ma_sound* s = v.sound.get();
        v.base_volume = std::max(0.0f, c.volume);
        v.occl = c.occlusion && c.spatial;
        v.occl_radius = std::isfinite(c.occlusion_radius) ? std::max(0.0f, c.occlusion_radius) : 0.0f;
        ma_sound_set_volume(s, v.base_volume * (v.occl && v.occl_traced ? v.occl_gain : 1.0f));
        ma_sound_set_pitch(s, std::max(0.01f, c.pitch));
        ma_sound_set_looping(s, c.loop ? MA_TRUE : MA_FALSE);
        ma_sound_set_spatialization_enabled(s, c.spatial ? MA_TRUE : MA_FALSE);
        if (c.spatial) {
            // miniaudio's spatializer bypasses master volume and panning entirely for
            // ma_attenuation_model_none, so "no attenuation" runs as linear with zero rolloff
            // (gain 1 at every distance) to keep the source's volume, occlusion and panning.
            const bool none = c.attenuation == Attenuation::None;
            ma_sound_set_attenuation_model(s, none ? ma_attenuation_model_linear : to_ma(c.attenuation));
            ma_sound_set_min_distance(s, std::max(0.0f, c.min_distance));
            ma_sound_set_max_distance(s, std::max(c.min_distance, c.max_distance));
            ma_sound_set_rolloff(s, none ? 0.0f : std::max(0.0f, c.rolloff));
            if (world) {
                const ma_vec3f p = world_pos(*world);
                ma_sound_set_position(s, p.x, p.y, p.z);
            }
        }
    }

    void destroy(Voice& v) {
        if (v.sound) ma_sound_uninit(v.sound.get());
        v.sound.reset();
        if (v.lpf) ma_lpf_node_uninit(v.lpf.get(), nullptr);
        v.lpf.reset();
    }

    // ---- acoustics (ADR-0096) ----

    static bool world_bus(std::size_t i) {
        const auto b = static_cast<Bus>(i);
        return b == Bus::Sfx || b == Bus::Ambient || b == Bus::Voice;
    }

    // Combines the user's bus settings with the smoothed zone/water state and pushes changes to
    // the graph. With no zone and no water the effective values equal the user's exactly.
    void refresh_effects() {
        const float rate = static_cast<float>(ma_engine_get_sample_rate(&engine));
        const float none = std::log2(detail::kNoFilterHz);
        for (std::size_t i = 0; i < kBusCount; ++i) {
            float eff_send = send[i];
            float eff_lp = lowpass[i];
            if (world_bus(i) && (ac.zone_w > 0.0f || ac.underwater > 0.0f)) {
                eff_send = std::max(send[i], ac.zone_w * ac.zone_wet);
                const float user_log = lowpass[i] > 0.0f && lowpass[i] < 0.5f * rate ? std::log2(lowpass[i]) : none;
                const float zone_log = none + (ac.zone_log_lp - none) * ac.zone_w;
                const float uw_log = none + (std::log2(std::max(20.0f, options.underwater_cutoff)) - none) * ac.underwater;
                const float hz = std::exp2(std::min({user_log, zone_log, uw_log}));
                eff_lp = hz >= std::min(20000.0f, 0.45f * rate) ? 0.0f : hz;
            }
            ac.send[i] = eff_send;
            ac.lowpass[i] = eff_lp;
            const auto moved = [](float a, float b) { return (a == 0.0f) != (b == 0.0f) || std::abs(a - b) > 0.005f * std::max(a, b); };
            // Small drifts are batched; landing exactly on the user's value always applies.
            if (ac.applied_lowpass[i] != eff_lp && (moved(ac.applied_lowpass[i], eff_lp) || eff_lp == lowpass[i])) apply_lowpass(i);
            if (ac.applied_send[i] != eff_send && (moved(ac.applied_send[i], eff_send) || eff_send == send[i])) apply_send(i);
        }
        ac.decay = decay + (ac.zone_decay - decay) * ac.zone_w;
        if (ac.decay != ac.applied_decay && (std::abs(ac.decay - ac.applied_decay) > 0.002f || ac.decay == decay)) {
            ac.applied_decay = ac.decay;
            for (std::size_t c = 0; c < kCombs; ++c)
                if (comb_ok[c]) ma_delay_node_set_decay(&combs[c], ac.decay);
        }
    }

    void reset_acoustics() {
        ac.zone = {};
        ac.zone_w = 0.0f;
        ac.underwater = 0.0f;
        refresh_effects();
    }

    void update_zones(scene::Scene& s, math::Vec3 listener, float dt) {
        Entity best;
        float best_w = 0.0f, best_vol = 0.0f;
        std::int32_t best_pri = 0;
        const ReverbZoneComponent* best_z = nullptr;
        s.world().each<ReverbZoneComponent>([&](Entity e, ReverbZoneComponent& z) {
            if (!z.enabled) return;
            float vol = 0.0f;
            const float w = detail::zone_weight(s.world_matrix(e), z, listener, &vol);
            if (w <= 0.0f) return;
            const bool better = !best_z || z.priority > best_pri || (z.priority == best_pri && (vol < best_vol || (vol == best_vol && e.bits() < best.bits())));
            if (!better) return;
            best = e;
            best_w = w;
            best_vol = vol;
            best_pri = z.priority;
            best_z = &z;
        });
        const float k = detail::smoothing(dt, options.zone_smoothing);
        if (best_z) {
            const float zd = std::clamp(std::isfinite(best_z->decay) ? best_z->decay : 0.7f, 0.0f, 0.95f);
            const float zw = std::clamp(std::isfinite(best_z->wet) ? best_z->wet : 0.0f, 0.0f, 1.0f);
            const float zl = best_z->lowpass > 0.0f && std::isfinite(best_z->lowpass) ? std::log2(std::max(20.0f, best_z->lowpass))
                                                                                        : std::log2(detail::kNoFilterHz);
            if (ac.zone_w <= 1e-3f) { // entering from silence: take the zone's character directly
                ac.zone_decay = zd;
                ac.zone_wet = zw;
                ac.zone_log_lp = zl;
            } else {
                ac.zone_decay += (zd - ac.zone_decay) * k;
                ac.zone_wet += (zw - ac.zone_wet) * k;
                ac.zone_log_lp += (zl - ac.zone_log_lp) * k;
            }
            ac.zone = best;
        }
        const float target = best_z ? best_w : 0.0f;
        ac.zone_w += (target - ac.zone_w) * k;
        if (target == 0.0f && ac.zone_w < 1e-3f) {
            ac.zone_w = 0.0f;
            ac.zone = {};
        }
        if (std::abs(target - ac.zone_w) < 1e-4f) ac.zone_w = target;

        float uw_target = 0.0f;
        if (options.underwater_filter)
            if (const auto q = gameplay::query_water(s, listener); q && q->depth > 0.0f) {
                const float t = std::clamp(q->depth / 0.3f, 0.0f, 1.0f);
                uw_target = t * t * (3.0f - 2.0f * t);
            }
        ac.underwater += (uw_target - ac.underwater) * detail::smoothing(dt, 0.4f * options.zone_smoothing);
        if (std::abs(uw_target - ac.underwater) < 1e-3f) ac.underwater = uw_target;
    }

    // Routes the voice through its own first-order low-pass (created on first use).
    void set_voice_filter(Voice& v, float cutoff) {
        ma_sound* snd = v.sound.get();
        ma_node* group = group_ok[static_cast<std::size_t>(v.bus)] ? static_cast<ma_node*>(&groups[static_cast<std::size_t>(v.bus)])
                                                                    : ma_engine_get_endpoint(&engine);
        const auto rate = ma_engine_get_sample_rate(&engine);
        const bool want = cutoff > 0.0f && cutoff < 18000.0f && cutoff < 0.45f * static_cast<float>(rate);
        if (!want) {
            if (v.lpf) {
                ma_node_attach_output_bus(snd, 0, group, 0);
                ma_lpf_node_uninit(v.lpf.get(), nullptr);
                v.lpf.reset();
            }
            return;
        }
        const ma_uint32 ch = ma_engine_get_channels(&engine);
        if (!v.lpf) {
            auto node = std::make_unique<ma_lpf_node>();
            const ma_lpf_node_config c = ma_lpf_node_config_init(ch, rate, cutoff, 1);
            if (ma_lpf_node_init(ma_engine_get_node_graph(&engine), &c, nullptr, node.get()) != MA_SUCCESS) return;
            ma_node_attach_output_bus(node.get(), 0, group, 0);
            ma_node_attach_output_bus(snd, 0, node.get(), 0);
            v.lpf = std::move(node);
            return;
        }
        const ma_lpf_config c = ma_lpf_config_init(ma_format_f32, ch, rate, cutoff, 1);
        ma_lpf_node_reinit(&c, v.lpf.get());
    }

    static bool chain_contains(const scene::Scene& s, Entity e, Entity target) {
        for (int guard = 0; !e.is_null() && guard < 256; ++guard, e = s.parent(e))
            if (e == target) return true;
        return false;
    }

    // Energy transmission (low, mid, high) of the segment listener -> point.
    math::Vec3 trace(scene::Scene& s, math::Vec3 from, math::Vec3 to, Entity listener, Entity source, bool& blocked) {
        blocked = false;
        math::Vec3 t{1, 1, 1};
        if (!gameplay::segment_hits(s, from, to, 8, hits)) return t;
        const float len = math::length(to - from);
        Entity last;
        float last_d = -1.0f;
        for (const auto& h : hits) {
            if (h.distance >= len - 1e-3f) continue;
            if (!listener.is_null() && (chain_contains(s, h.entity, listener) || chain_contains(s, listener, h.entity))) continue;
            if (chain_contains(s, h.entity, source) || chain_contains(s, source, h.entity)) continue;
            if (h.entity == last && std::abs(h.distance - last_d) < 0.01f) continue; // duplicate sub-shape hit
            last = h.entity;
            last_d = h.distance;
            AcousticMaterialComponent m;
            for (Entity e = h.entity; !e.is_null(); e = s.parent(e))
                if (const auto* c = s.world().get<AcousticMaterialComponent>(e)) {
                    m = *c;
                    break;
                }
            const math::Vec3 tr = acoustic_transmission(m);
            t = {t.x * tr.x, t.y * tr.y, t.z * tr.z};
            blocked = true;
        }
        return t;
    }

    void update_occlusion(scene::Scene& s, Entity listener_e, math::Vec3 listener, float dt) {
        ac.rays = 0;
        ac.occluded = 0;
        const bool provider = gameplay::has_segment_query(s);
        std::vector<std::pair<Entity, Voice*>> list;
        for (auto& [e, v] : voices) {
            if (!v.sound) continue;
            const bool on = v.occl && provider && ma_sound_is_spatialization_enabled(v.sound.get()) && live(v);
            if (!on) {
                if (v.occl_traced || v.lpf || v.occl_gain != 1.0f) { // turned off: restore at once
                    v.occl_traced = false;
                    v.occl_blocked = 0.0f;
                    v.occl_gain = v.occl_gain_target = 1.0f;
                    v.occl_cut = v.occl_cut_target = detail::kNoFilterHz;
                    set_voice_filter(v, 0.0f);
                    ma_sound_set_volume(v.sound.get(), v.base_volume);
                }
                continue;
            }
            list.emplace_back(e, &v);
        }
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.first.bits() < b.first.bits(); });
        // Round-robin: never-traced voices first, then from the cursor onwards.
        std::stable_partition(list.begin(), list.end(), [](const auto& p) { return !p.second->occl_traced; });
        const auto first_traced = std::find_if(list.begin(), list.end(), [](const auto& p) { return p.second->occl_traced; });
        const auto pivot = std::find_if(first_traced, list.end(), [&](const auto& p) { return p.first.bits() > ac.cursor; });
        std::rotate(first_traced, pivot, list.end());
        std::uint32_t budget = options.max_occlusion_rays;
        for (auto& [e, vp] : list) {
            Voice& v = *vp;
            const std::uint32_t n = v.occl_radius > 0.0f ? 5u : 1u;
            if (budget < n) break;
            budget -= n;
            ac.rays += n;
            if (v.occl_traced) ac.cursor = e.bits();
            const ma_vec3f sp = ma_sound_get_position(v.sound.get());
            const math::Vec3 src{sp.x, sp.y, sp.z};
            std::array<math::Vec3, 5> pts{src, src, src, src, src};
            if (n == 5) {
                math::Vec3 dir = src - listener;
                dir = math::length(dir) > 1e-6f ? math::normalize(dir) : math::Vec3{0, 0, -1};
                const math::Vec3 ref = std::abs(dir.y) < 0.9f ? math::Vec3{0, 1, 0} : math::Vec3{1, 0, 0};
                const math::Vec3 u = math::normalize(math::cross(dir, ref));
                const math::Vec3 w = math::cross(dir, u);
                pts[1] = src + u * v.occl_radius;
                pts[2] = src - u * v.occl_radius;
                pts[3] = src + w * v.occl_radius;
                pts[4] = src - w * v.occl_radius;
            }
            math::Vec3 energy{0, 0, 0};
            std::uint32_t blocked = 0;
            for (std::uint32_t r = 0; r < n; ++r) {
                bool b = false;
                const math::Vec3 t = trace(s, listener, pts[r], listener_e, e, b);
                energy = energy + t;
                blocked += b ? 1u : 0u;
            }
            energy = energy * (1.0f / static_cast<float>(n));
            const float a_lo = std::sqrt(energy.x), a_mid = std::sqrt(energy.y);
            const float cut = a_lo > 1e-6f ? detail::tilt_cutoff(a_mid / a_lo) : detail::kMinOcclusionCutoffHz;
            v.occl_blocked = static_cast<float>(blocked) / static_cast<float>(n);
            // Gain matches the low band; the filter's own loss at 400 Hz is compensated.
            v.occl_gain_target = std::min(1.0f, a_lo / detail::lowpass1_gain(detail::kBandLowHz, cut));
            v.occl_cut_target = cut > 0.0f ? cut : detail::kNoFilterHz;
            if (!v.occl_traced) {
                v.occl_gain = v.occl_gain_target;
                v.occl_cut = v.occl_cut_target;
                v.occl_traced = true;
            }
        }
        if (!list.empty() && ac.rays > 0 && ac.cursor >= list.back().first.bits()) ac.cursor = 0;
        const float k = detail::smoothing(dt, options.occlusion_smoothing);
        for (auto& [e, vp] : list) {
            Voice& v = *vp;
            if (!v.occl_traced) continue;
            v.occl_gain += (v.occl_gain_target - v.occl_gain) * k;
            const float lc = std::log2(v.occl_cut), lt = std::log2(v.occl_cut_target);
            v.occl_cut = std::exp2(lc + (lt - lc) * k);
            if (std::abs(v.occl_gain - v.occl_gain_target) < 1e-4f) v.occl_gain = v.occl_gain_target;
            if (std::abs(lc - lt) < 1e-3f) v.occl_cut = v.occl_cut_target;
            set_voice_filter(v, v.occl_cut >= detail::kNoFilterHz * 0.99f ? 0.0f : v.occl_cut);
            ma_sound_set_volume(v.sound.get(), v.base_volume * v.occl_gain);
            if (v.occl_gain < 0.99f || v.lpf) ++ac.occluded;
        }
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
        it->second.priority = std::clamp(c.priority, 0, 255);
        it->second.virt = false;
        ma_sound* snd = it->second.sound.get();
        configure(it->second, c, &world);
        ma_sound_seek_to_pcm_frame(snd, 0);
        start_sound(snd, false);
        return {};
    }

    Entity update_listener(scene::Scene& s) {
        Entity listener;
        bool found = false;
        math::Mat4 m;
        s.world().each<AudioListenerComponent>([&](Entity e, AudioListenerComponent& l) {
            if (found || !l.enabled) return;
            m = s.world_matrix(e);
            listener = e;
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
                listener = best;
                found = true;
            }
        }
        if (!found) m = math::Mat4::identity();
        const math::Vec3 fwd = math::normalize(-m[2].xyz());
        const math::Vec3 up = math::normalize(m[1].xyz());
        ma_engine_listener_set_position(&engine, 0, m[3].x, m[3].y, m[3].z);
        ma_engine_listener_set_direction(&engine, 0, fwd.x, fwd.y, fwd.z);
        ma_engine_listener_set_world_up(&engine, 0, up.x, up.y, up.z);
        return listener;
    }

    void update(scene::Scene& s, float dt) {
        ++frame;
        const Entity listener_e = update_listener(s);
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
                v.priority = std::clamp(c.priority, 0, 255);
                configure(v, c, &world);
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
        const ma_vec3f lp = ma_engine_listener_get_position(&engine, 0);
        const math::Vec3 listener{lp.x, lp.y, lp.z};
        dt = std::isfinite(dt) ? std::clamp(dt, 0.0f, 1.0f) : 0.0f;
        update_occlusion(s, listener_e, listener, dt);
        update_zones(s, listener, dt);
        refresh_effects();
        arbitrate();
    }

    void reap_one_shots() {
        for (auto it = one_shots.begin(); it != one_shots.end();) {
            ma_sound* snd = it->sound.get();
            if (ma_sound_at_end(snd) || !ma_sound_is_playing(snd)) {
                ma_sound_uninit(snd);
                it = one_shots.erase(it);
            } else {
                ++it;
            }
        }
        // Fades only progress while audio is mixed; without a device or render() calls, dying
        // one-shots would pile up, so the oldest are released outright past a hard limit.
        const std::size_t hard = 2 * static_cast<std::size_t>(std::max<std::uint32_t>(1, options.max_one_shots)) +
                                 std::max<std::uint32_t>(1, options.max_voices);
        for (auto it = one_shots.begin(); one_shots.size() > hard && it != one_shots.end();) {
            if (it->dying) {
                ma_sound_uninit(it->sound.get());
                it = one_shots.erase(it);
            } else {
                ++it;
            }
        }
    }

    void steal(OneShotVoice& o) {
        fade_out(o.sound.get());
        o.dying = true;
        ++stolen;
    }
};

AudioEngine::AudioEngine(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

AudioEngine::~AudioEngine() {
    if (!impl_) return;
    impl_->release_all();
    for (auto& [k, snd] : impl_->preloaded) ma_sound_uninit(snd.get());
    impl_->preloaded.clear();
    impl_->uninit_effects();
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
    impl->init_effects();
    return std::unique_ptr<AudioEngine>(new AudioEngine(std::move(impl)));
}

bool AudioEngine::has_device() const noexcept { return impl_->device; }
std::uint32_t AudioEngine::sample_rate() const noexcept { return ma_engine_get_sample_rate(&impl_->engine); }
std::uint32_t AudioEngine::channels() const noexcept { return ma_engine_get_channels(&impl_->engine); }

void AudioEngine::update(scene::Scene& scene, float dt) { impl_->update(scene, dt); }

void AudioEngine::stop_all() {
    impl_->release_all();
    impl_->autostarted.clear();
    for (std::size_t i = 0; i < kBusCount; ++i) {
        set_bus_lowpass(static_cast<Bus>(i), 0.0f);
        set_bus_reverb_send(static_cast<Bus>(i), 0.0f);
    }
    set_reverb_decay(0.7f);
    impl_->reset_acoustics();
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
    if (auto it = impl_->voices.find(e); it != impl_->voices.end() && it->second.sound) {
        it->second.virt = false;
        ma_sound_stop(it->second.sound.get());
    }
}

void AudioEngine::set_paused(Entity e, bool paused) {
    auto it = impl_->voices.find(e);
    if (it == impl_->voices.end() || !it->second.sound) return;
    Voice& v = it->second;
    if (paused) {
        if (v.virt) impl_->make_real(v, false); // keep the virtual playhead
        else ma_sound_stop(v.sound.get());
    } else if (!v.virt && !ma_sound_at_end(v.sound.get())) {
        start_sound(v.sound.get(), false); // the next update decides whether it stays real
    }
}

bool AudioEngine::is_playing(Entity e) const {
    const auto it = impl_->voices.find(e);
    return it != impl_->voices.end() && Impl::live(it->second);
}

bool AudioEngine::is_virtual(Entity e) const {
    const auto it = impl_->voices.find(e);
    return it != impl_->voices.end() && it->second.sound && it->second.virt;
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
    const std::string key(clip);
    if (o.max_instances > 0) {
        // Same-clip limit: the oldest live instances fade out to make room.
        std::size_t same = 0;
        for (const auto& x : impl_->one_shots) same += !x.dying && x.clip == key;
        for (auto& x : impl_->one_shots) {
            if (same < o.max_instances) break;
            if (x.dying || x.clip != key) continue;
            impl_->steal(x);
            --same;
        }
    }
    ma_sound_start(snd);
    OneShotVoice v;
    v.sound = std::move(s).value();
    v.clip = key;
    v.bus = o.bus;
    v.priority = std::clamp(o.priority, 0, 255);
    v.order = impl_->one_shot_order++;
    impl_->one_shots.push_back(std::move(v));
    const std::size_t cap = std::max<std::uint32_t>(1, impl_->options.max_one_shots);
    std::size_t alive = 0;
    for (const auto& x : impl_->one_shots) alive += !x.dying;
    for (auto& x : impl_->one_shots) {
        if (alive <= cap) break;
        if (x.dying) continue;
        impl_->steal(x);
        --alive;
    }
    impl_->reap_one_shots();
    impl_->arbitrate();
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

void AudioEngine::set_bus_lowpass(Bus b, float cutoff_hz) {
    const auto i = static_cast<std::size_t>(b);
    if (i >= kBusCount) return;
    impl_->lowpass[i] = std::isfinite(cutoff_hz) ? std::max(0.0f, cutoff_hz) : 0.0f;
    impl_->refresh_effects();
}
float AudioEngine::bus_lowpass(Bus b) const {
    const auto i = static_cast<std::size_t>(b);
    return i < kBusCount ? impl_->lowpass[i] : 0.0f;
}
void AudioEngine::set_bus_reverb_send(Bus b, float send) {
    const auto i = static_cast<std::size_t>(b);
    if (i == 0 || i >= kBusCount) return;
    impl_->send[i] = std::isfinite(send) ? std::clamp(send, 0.0f, 1.0f) : 0.0f;
    impl_->refresh_effects();
}
float AudioEngine::bus_reverb_send(Bus b) const {
    const auto i = static_cast<std::size_t>(b);
    return i < kBusCount ? impl_->send[i] : 0.0f;
}
void AudioEngine::set_reverb_decay(float decay) {
    impl_->decay = std::isfinite(decay) ? std::clamp(decay, 0.0f, 0.95f) : 0.7f;
    impl_->refresh_effects();
}
float AudioEngine::reverb_decay() const { return impl_->decay; }

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
    for (const auto& [e, v] : impl_->voices) {
        if (!Impl::live(v)) continue;
        ++st.playing;
        if (v.virt) ++st.virtual_voices;
        else ++st.real;
    }
    for (const auto& o : impl_->one_shots) {
        if (o.dying) continue;
        ++st.one_shots;
        if (ma_sound_is_playing(o.sound.get()) && !ma_sound_at_end(o.sound.get())) ++st.real;
    }
    st.stolen = impl_->stolen;
    st.virtualised = impl_->virtualised;
    st.clips_loaded = static_cast<std::uint32_t>(impl_->loaded.size());
    st.load_errors = impl_->load_errors;
    return st;
}

AcousticState AudioEngine::acoustics() const {
    const auto& ac = impl_->ac;
    AcousticState st;
    st.zone = ac.zone;
    st.zone_weight = ac.zone_w;
    st.underwater = ac.underwater;
    st.decay = ac.decay;
    st.send = ac.send;
    st.lowpass = ac.lowpass;
    st.rays = ac.rays;
    st.occluded = ac.occluded;
    return st;
}

std::optional<OcclusionState> AudioEngine::occlusion(Entity e) const {
    const auto it = impl_->voices.find(e);
    if (it == impl_->voices.end() || !it->second.sound || !it->second.occl_traced) return std::nullopt;
    const Voice& v = it->second;
    return OcclusionState{v.occl_blocked, v.occl_gain, v.lpf ? v.occl_cut : 0.0f};
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
    sim.add_system("audio.update", gameplay::Phase::PostUpdate, [a](scene::Scene& scene, const gameplay::FrameTime& t) {
        if (engine_of(scene) != a) bind(scene, a);
        a->update(scene, t.dt);
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

Result<ClipInfo> inspect_clip(std::span<const std::uint8_t> bytes, std::uint64_t max_frames) {
    if (bytes.empty()) return make_error(ErrorCode::ParseError, "audio clip is empty");
    // f32 output so every decoder goes through the same conversion path the engine uses.
    const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder;
    const ma_result r = ma_decoder_init_memory(bytes.data(), bytes.size(), &config, &decoder);
    if (r != MA_SUCCESS)
        return make_error(ErrorCode::ParseError, std::string("unrecognised audio data: ") + ma_result_description(r));
    ClipInfo info;
    info.channels = decoder.outputChannels;
    info.sample_rate = decoder.outputSampleRate;
    Status status{};
    if (info.channels == 0 || info.channels > MA_MAX_CHANNELS || info.sample_rate == 0) {
        status = make_error(ErrorCode::ParseError, "audio clip has no channels or no sample rate");
    } else {
        constexpr ma_uint64 kChunk = 4096;
        std::vector<float> scratch(static_cast<std::size_t>(kChunk) * info.channels);
        for (;;) {
            ma_uint64 got = 0;
            const ma_result rr = ma_decoder_read_pcm_frames(&decoder, scratch.data(), kChunk, &got);
            info.frames += got;
            if (info.frames > max_frames) {
                status = make_error(ErrorCode::LimitExceeded, "audio clip is longer than the allowed length");
                break;
            }
            if (rr == MA_AT_END || got == 0) break;
            if (rr != MA_SUCCESS) {
                status = make_error(ErrorCode::ParseError, std::string("audio decode failed: ") + ma_result_description(rr));
                break;
            }
        }
        if (status.ok() && info.frames == 0) status = make_error(ErrorCode::ParseError, "audio clip contains no samples");
    }
    ma_decoder_uninit(&decoder);
    if (!status.ok()) return std::move(status).error();
    return info;
}

} // namespace victus::audio
