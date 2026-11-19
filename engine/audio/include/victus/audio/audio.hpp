// SPDX-License-Identifier: Apache-2.0
// VICTUS audio: sound sources and a listener as scene components, mixed by miniaudio.
// - Clips are project assets (.wav, .ogg, .mp3, .flac) addressed by asset-relative path. Short
//   clips are decoded once and cached; `stream` sources decode on the fly (music, ambience).
// - Every sound plays through one of the fixed buses below; each bus has its own volume and mute
//   and all of them feed Master. Bus volumes are per AudioEngine (game settings), not per scene.
// - Bus effects: each bus has an optional low-pass filter (2nd-order Butterworth, e.g. muffled
//   sound under water or behind a pause menu), and every bus except Master has a reverb send into
//   one shared reverb (four parallel feedback comb filters, Freeverb tunings) that returns into
//   Master. Both are off by default (a bypassed filter is not in the signal path; an unused
//   reverb still runs its four combs on silence).
// - Spatial sources follow their entity's world transform and are attenuated/panned relative to
//   the active listener: the first enabled AudioListener, else the primary camera, else the origin.
// - The engine can run without an output device (`null_device`, or automatically when no device
//   opens): sounds still advance and `render()` pulls mixed frames, which is how tests and servers
//   run audio deterministically.
// - Voice management: entity sources and one-shots share a budget of `max_voices` real voices,
//   ranked by priority, then audibility (volume x bus gain x distance attenuation). Entity sources
//   that lose, or that are inaudible (below -60 dB), become virtual: silent and not mixed, but
//   their playhead keeps advancing, so they resume in place with a short fade-in when they win
//   again. One-shots that lose are faded out (stolen). Every stop caused by the voice manager or
//   an instance limit fades over a few milliseconds instead of cutting (no clicks).
// - Acoustics (ADR-0096): sources with `occlusion` are traced against scene geometry (the segment
//   query physics installs, see gameplay/queries.hpp) from the listener; each surface between
//   them transmits a fraction of the energy per frequency band taken from its AcousticMaterial
//   (Steam Audio's material table), so walls attenuate and muffle (a per-voice low-pass). Optional
//   volumetric occlusion traces five rays across `occlusion_radius` for partial blocking.
//   ReverbZone boxes drive the reverb (decay, wet send, low-pass) from the listener's position with
//   a fade margin, and a listener under water (gameplay/water.hpp) muffles the world buses. All
//   acoustic changes are smoothed over time and layered on top of the user's bus settings.
#pragma once
#include "victus/core/fs.hpp"
#include "victus/core/result.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/scene/scene.hpp"

#include <array>
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
    std::int32_t priority{128};    // 0..255, higher keeps a real voice when over the budget
    bool occlusion{false};         // trace geometry between listener and source (spatial only)
    float occlusion_radius{0.0f};  // > 0: five rays across this radius (partial occlusion)
    friend bool operator==(const AudioSourceComponent&, const AudioSourceComponent&) = default;
};

struct AudioListenerComponent {
    bool enabled{true};
    friend bool operator==(const AudioListenerComponent&, const AudioListenerComponent&) = default;
};

// Sound transmission of a surface. Presets carry Steam Audio's per-band energy transmission
// coefficients (bands centred at 400 Hz, 2.5 kHz and 15 kHz); Custom uses `transmission`.
// Entities without the component (and without one on an ancestor) occlude as Generic.
enum class AcousticPreset : std::uint8_t { Generic, Brick, Concrete, Ceramic, Gravel, Carpet, Glass, Plaster, Wood, Metal, Rock, Custom };
struct AcousticMaterialComponent {
    AcousticPreset preset{AcousticPreset::Generic};
    math::Vec3 transmission{0.100f, 0.050f, 0.030f}; // energy fractions low/mid/high (Custom)
    friend bool operator==(const AcousticMaterialComponent&, const AcousticMaterialComponent&) = default;
};
// Energy transmission (low, mid, high) of a material, each in [0, 1].
[[nodiscard]] math::Vec3 acoustic_transmission(const AcousticMaterialComponent& m) noexcept;

// A box (centred on the entity, `size` in its local space, so rotation and scale apply) that sets
// the reverb while the listener is inside, fading out over `fade` metres beyond the box. When zones
// overlap the highest priority wins, then the smallest box.
struct ReverbZoneComponent {
    bool enabled{true};
    math::Vec3 size{10.0f, 4.0f, 10.0f};
    float fade{2.0f};      // metres
    float decay{0.8f};     // reverb comb feedback, [0, 0.95]
    float wet{0.35f};      // reverb send for the Sfx, Ambient and Voice buses, [0, 1]
    float lowpass{0.0f};   // Hz on the same buses, 0 = none
    std::int32_t priority{0};
    friend bool operator==(const ReverbZoneComponent&, const ReverbZoneComponent&) = default;
};

// Registers serializers/schemas for "AudioSource", "AudioListener", "AcousticMaterial" and
// "ReverbZone". Idempotent.
void register_components();

struct EngineOptions {
    fs::Path assets_root;          // clips resolve under this directory
    bool null_device{false};       // never open an output device
    std::uint32_t sample_rate{48000};
    std::uint32_t channels{2};
    std::uint32_t max_one_shots{64}; // oldest one-shot is faded out when exceeded
    std::uint32_t max_voices{96};    // real (mixed) voices; the rest are virtual or stolen
    std::uint32_t max_occlusion_rays{64}; // per update, shared round-robin across occluded sources
    float occlusion_smoothing{0.08f};     // seconds (time constant) for occlusion changes
    float zone_smoothing{0.25f};          // seconds for reverb zone / underwater transitions
    bool underwater_filter{true};         // muffle world buses while the listener is under water
    float underwater_cutoff{600.0f};      // Hz
};

inline constexpr float kVoiceFadeMs = 10.0f;        // fade used when virtualising/stealing/resuming
inline constexpr float kInaudibleGain = 0.001f;     // -60 dB: entity sources below this go virtual

struct OneShot {
    Bus bus{Bus::Sfx};
    float volume{1.0f};
    float pitch{1.0f};
    std::optional<math::Vec3> position; // spatial when set
    std::int32_t priority{128};         // 0..255, see AudioSourceComponent::priority
    std::uint32_t max_instances{0};     // > 0: at most this many of this clip; the oldest fades out
};

struct AudioStats {
    std::uint32_t voices{0};       // entity sources with a live sound
    std::uint32_t playing{0};      // of which currently playing (real or virtual)
    std::uint32_t real{0};         // voices being mixed (entity sources + one-shots)
    std::uint32_t virtual_voices{0}; // entity sources playing silently, waiting for a voice
    std::uint64_t stolen{0};       // one-shots faded out by the budget or instance limits (total)
    std::uint64_t virtualised{0};  // real -> virtual transitions (total)
    std::uint32_t one_shots{0};
    std::uint32_t clips_loaded{0}; // distinct clips opened so far
    std::uint32_t load_errors{0};
    bool device{false};            // an output device is open
};

// Result of the latest occlusion trace for one source (smoothed values as applied).
struct OcclusionState {
    float blocked{0};   // fraction of rays that hit geometry, [0, 1]
    float gain{1};      // linear gain applied to the voice
    float cutoff{0};    // per-voice low-pass in Hz, 0 = unfiltered
};

// What the acoustic layer currently applies (effective values, user settings combined with zones
// and water); the bus_* getters keep returning the user's own settings.
struct AcousticState {
    Entity zone;                         // dominant reverb zone (invalid when none)
    float zone_weight{0};                // smoothed, [0, 1]
    float underwater{0};                 // smoothed weight, [0, 1]
    float decay{0.7f};
    std::array<float, kBusCount> send{};
    std::array<float, kBusCount> lowpass{}; // 0 = bypassed
    std::uint32_t rays{0};               // occlusion rays traced in the latest update
    std::uint32_t occluded{0};           // sources currently attenuated by occlusion
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
    // `dt` (seconds) drives the acoustic smoothing.
    void update(scene::Scene& scene, float dt = 1.0f / 60.0f);
    // Stops and releases every sound (e.g. when a play session ends or the scene changes) and
    // turns the bus effects off (low-pass, reverb sends, decay back to 0.7): they belong to the
    // scene being left. Bus volumes and mutes (game settings) and clip caches are kept.
    void stop_all();

    // ---- entity sources ----
    Status play(scene::Scene& scene, Entity e); // (re)starts the entity's source from the beginning
    void stop(Entity e);
    void set_paused(Entity e, bool paused);
    [[nodiscard]] bool is_playing(Entity e) const;   // real or virtual
    [[nodiscard]] bool is_virtual(Entity e) const;
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
    // Low-pass cutoff in Hz; 0 (or anything at or above half the sample rate) bypasses the filter.
    void set_bus_lowpass(Bus b, float cutoff_hz);
    [[nodiscard]] float bus_lowpass(Bus b) const;
    // Fraction of the bus sent to the shared reverb, clamped to [0, 1]; ignored for Master.
    void set_bus_reverb_send(Bus b, float send);
    [[nodiscard]] float bus_reverb_send(Bus b) const;
    // Feedback of the reverb's comb filters, clamped to [0, 0.95]: higher rings longer.
    void set_reverb_decay(float decay);
    [[nodiscard]] float reverb_decay() const;

    // Pulls mixed interleaved float frames (only without a device). Returns frames written.
    std::size_t render(float* out, std::size_t frames);

    [[nodiscard]] AudioStats stats() const;
    [[nodiscard]] AcousticState acoustics() const;
    // Occlusion of an entity source, or nullopt when it has no live voice or occlusion is off.
    [[nodiscard]] std::optional<OcclusionState> occlusion(Entity e) const;

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
