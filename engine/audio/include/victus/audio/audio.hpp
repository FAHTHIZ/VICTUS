// SPDX-License-Identifier: Apache-2.0
// VICTUS audio: sound sources and a listener as scene components, mixed by miniaudio.
// - Clips are project assets (.wav, .ogg, .mp3, .flac) addressed by asset-relative path. Short
//   clips are decoded once and cached; `stream` sources decode on the fly (music, ambience).
// - Every sound plays through one of the fixed buses below; each bus has its own volume and mute
//   and all of them feed Master. Bus volumes are per AudioEngine (game settings), not per scene.
// - Spatial sources follow their entity's world transform and are attenuated/panned relative to
//   the active listener: the first enabled AudioListener, else the primary camera, else the origin.
// - The engine can run without an output device (`null_device`, or automatically when no device
//   opens): sounds still advance and `render()` pulls mixed frames, which is how tests and servers
//   run audio deterministically.
#pragma once
#include "victus/core/fs.hpp"
#include "victus/core/result.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/scene/scene.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace victus::audio {

using scene::Entity;

enum class Bus : std::uint8_t { Master, Music, Sfx, Ui, Ambient, Voice };
inline constexpr std::size_t kBusCount = 6;
[[nodiscard]] std::string_view bus_name(Bus b) noexcept;
[[nodiscard]] std::optional<Bus> parse_bus(std::string_view name) noexcept;

enum class Attenuation : std::uint8_t { None, Inverse, Linear, Exponential };

struct AudioSourceComponent {
    std::string clip;              // asset-relative path, e.g. "audio/jump.wav"
    Bus bus{Bus::Sfx};
    float volume{1.0f};            // linear gain
    float pitch{1.0f};             // playback rate multiplier
    bool loop{false};
    bool play_on_start{true};      // start automatically when the source first appears
    bool stream{false};            // decode while playing instead of up front
    bool spatial{true};            // follow the entity and attenuate relative to the listener
    Attenuation attenuation{Attenuation::Inverse};
    float min_distance{1.0f};      // full volume inside this distance
    float max_distance{50.0f};     // no further attenuation beyond this distance
    float rolloff{1.0f};
    friend bool operator==(const AudioSourceComponent&, const AudioSourceComponent&) = default;
};

struct AudioListenerComponent {
    bool enabled{true};
    friend bool operator==(const AudioListenerComponent&, const AudioListenerComponent&) = default;
};

// Registers serializers/schemas for "AudioSource" and "AudioListener". Idempotent.
void register_components();

struct EngineOptions {
    fs::Path assets_root;          // clips resolve under this directory
    bool null_device{false};       // never open an output device
    std::uint32_t sample_rate{48000};
    std::uint32_t channels{2};
    std::uint32_t max_one_shots{64}; // oldest one-shot is stopped when exceeded
};

struct OneShot {
    Bus bus{Bus::Sfx};
    float volume{1.0f};
    float pitch{1.0f};
    std::optional<math::Vec3> position; // spatial when set
};

struct AudioStats {
    std::uint32_t voices{0};       // entity sources with a live sound
    std::uint32_t playing{0};      // of which currently playing
    std::uint32_t one_shots{0};
    std::uint32_t clips_loaded{0}; // distinct clips opened so far
    std::uint32_t load_errors{0};
    bool device{false};            // an output device is open
};

class AudioEngine {
public:
    static Result<std::unique_ptr<AudioEngine>> create(const EngineOptions& options = {});
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    [[nodiscard]] bool has_device() const noexcept;
    [[nodiscard]] std::uint32_t sample_rate() const noexcept;
    [[nodiscard]] std::uint32_t channels() const noexcept;

    // Matches sounds to the scene's AudioSource components (start/stop/update/destroy), moves
    // spatial sounds and the listener, and reaps finished one-shots. Call once per frame.
    void update(scene::Scene& scene);
    // Stops and releases every sound (e.g. when a play session ends). Clip caches are kept.
    void stop_all();

    // ---- entity sources ----
    Status play(scene::Scene& scene, Entity e); // (re)starts the entity's source from the beginning
    void stop(Entity e);
    void set_paused(Entity e, bool paused);
    [[nodiscard]] bool is_playing(Entity e) const;
    // ---- one-shots ----
    Status play_one_shot(std::string_view clip, const OneShot& options = {});
    // Decodes and caches a clip ahead of first use.
    Status preload(std::string_view clip);
    // Hot reload: forgets the decoded clip so the file is read again, and restarts entity
    // sources that were playing it (from the beginning, with the new data). One-shots already
    // playing finish with the old data. Returns the number of sources restarted.
    std::size_t reload_clip(std::string_view clip);

    // ---- buses (Master scales everything) ----
    void set_bus_volume(Bus b, float volume);
    [[nodiscard]] float bus_volume(Bus b) const;
    void set_bus_muted(Bus b, bool muted);
    [[nodiscard]] bool bus_muted(Bus b) const;

    // Pulls mixed interleaved float frames (only without a device). Returns frames written.
    std::size_t render(float* out, std::size_t frames);

    [[nodiscard]] AudioStats stats() const;

    struct Impl;

private:
    explicit AudioEngine(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Installs "audio.update" in Phase::PostUpdate (after scripts moved things) and binds the engine
// to the scene so scripts can reach it. The engine must outlive the simulation's use of it.
void add_audio_systems(gameplay::Simulation& sim, AudioEngine& engine);
[[nodiscard]] AudioEngine* engine_of(scene::Scene& scene);
void bind(scene::Scene& scene, AudioEngine* engine);

// True when VICTUS_NO_AUDIO is set to a non-empty value other than "0": apps then mix without
// opening an output device (CI, servers, silent runs).
[[nodiscard]] bool audio_disabled_by_env() noexcept;

// Encodes interleaved float samples (clamped to [-1, 1]) as a 16-bit PCM WAV file image, and
// writes one (tests, tools, procedurally generated sounds).
[[nodiscard]] std::vector<std::uint8_t> encode_wav(const float* interleaved, std::size_t frames, std::uint32_t channels,
                                                   std::uint32_t sample_rate);
Status write_wav(const fs::Path& path, const float* interleaved, std::size_t frames, std::uint32_t channels,
                 std::uint32_t sample_rate);

// Format of a decoded clip.
struct ClipInfo {
    std::uint32_t channels{0};
    std::uint32_t sample_rate{0};
    std::uint64_t frames{0};  // PCM frames actually decoded
};

// Decodes a clip file image (WAV, Ogg Vorbis, MP3, FLAC) from memory with the same decoders the
// engine plays it with, without an AudioEngine or output device. Fails with ParseError for data no
// decoder accepts or that decodes to nothing, and with LimitExceeded once more than `max_frames`
// frames come out. Used to validate clips before shipping and by the robustness fuzz suite.
[[nodiscard]] Result<ClipInfo> inspect_clip(std::span<const std::uint8_t> bytes,
                                            std::uint64_t max_frames = 48000ull * 60 * 30);

} // namespace victus::audio
