// SPDX-License-Identifier: Apache-2.0
// Timeline sequencer: `.vseq` sequences of keyed tracks played on an entity (cutscenes, scripted
// doors and lifts, camera fly-throughs, timed events). See docs/SEQUENCER.md.
//
//     { "version": 1, "duration": 6,
//       "tracks": [
//         { "type": "transform", "target": "Door", "space": "relative",
//           "position": [ { "t": 0, "v": [0, 0, 0] }, { "t": 2, "v": [0, 3, 0], "interp": "smooth" } ] },
//         { "type": "camera", "keys": [ { "t": 0, "camera": "IntroCam" }, { "t": 4, "camera": "Main Camera" } ] },
//         { "type": "animation", "target": "Robot", "keys": [ { "t": 0.5, "clip": "Wave", "fade": 0.2 } ] },
//         { "type": "audio", "keys": [ { "t": 1, "clip": "audio/door.wav", "volume": 0.8 } ] },
//         { "type": "event", "keys": [ { "t": 6, "name": "intro_done" } ] } ] }
//
// A SequencePlayer component on an entity names the sequence; track targets are entity names or
// `/`-separated paths below that entity (falling back to any entity of that name in the scene),
// and an empty target means the player entity itself.
//
// Continuous tracks (transform channels) are sampled at the playhead every update and after a
// seek. Camera cuts make their camera the primary one while the sequence plays (the original
// primary flags come back when it stops or finishes). Discrete keys (animation clips, audio
// one-shots, events) fire when the playhead crosses them while playing, in either direction and
// across loop wraps, never on seeks; a seek re-applies the animation clip active at that time.
// Events are queued in the scene for scripts (`on_sequence_event(name, payload, player)`) and
// passed to an optional C++ handler.
#pragma once
#include "victus/core/json.hpp"
#include "victus/core/math.hpp"
#include "victus/core/result.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/scene/scene.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace victus::sequencer {

using scene::Entity;

inline constexpr std::string_view kSequenceExtension = ".vseq";
inline constexpr std::uint32_t kSequenceVersion = 1;

// Interpolation of the segment that starts at a key (the last key holds its value).
enum class Interp : std::uint8_t {
    Step,   // hold until the next key
    Linear, // straight line (slerp for rotations)
    Smooth, // ease in and out between the two keys (zero slope at both)
    Cubic,  // Catmull-Rom through the neighbouring keys (smooth paths; rotations use Smooth)
};
[[nodiscard]] std::string_view interp_name(Interp i) noexcept;
[[nodiscard]] std::optional<Interp> interp_from_name(std::string_view name) noexcept;

template <class T> struct Key {
    float time{0};
    T value{};
    Interp interp{Interp::Linear};
    friend bool operator==(const Key&, const Key&) = default;
};

enum class TrackType : std::uint8_t { Transform, Animation, Audio, Event, Camera };
[[nodiscard]] std::string_view track_type_name(TrackType t) noexcept;

struct AnimationKey {
    float time{0};
    std::string clip;
    float fade{-1.0f}; // cross-fade seconds (< 0: the Animator's crossfade)
    float speed{1.0f};
    bool loop{true};
    friend bool operator==(const AnimationKey&, const AnimationKey&) = default;
};

struct AudioKey {
    float time{0};
    std::string clip; // asset path, e.g. "audio/door.wav"
    float volume{1.0f};
    float pitch{1.0f};
    std::string bus{"Sfx"};
    bool spatial{false}; // play at the target's position
    friend bool operator==(const AudioKey&, const AudioKey&) = default;
};

struct EventKey {
    float time{0};
    std::string name;
    std::string payload;
    friend bool operator==(const EventKey&, const EventKey&) = default;
};

struct CameraKey {
    float time{0};
    std::string camera; // target path of an entity with a Camera component
    friend bool operator==(const CameraKey&, const CameraKey&) = default;
};

struct Track {
    TrackType type{TrackType::Event};
    std::string name;   // optional label (editor)
    std::string target; // entity path relative to the player entity ("" = the player itself)
    bool muted{false};
    // Transform: keys are local-space values; "relative" offsets them from the transform the
    // target had when playback started (position added, rotation applied after, scale multiplied).
    bool relative{false};
    std::vector<Key<math::Vec3>> position;
    std::vector<Key<math::Quat>> rotation;
    std::vector<Key<math::Vec3>> scale;
    std::vector<AnimationKey> animation;
    std::vector<AudioKey> audio;
    std::vector<EventKey> events;
    std::vector<CameraKey> cameras;
    friend bool operator==(const Track&, const Track&) = default;

    [[nodiscard]] std::size_t key_count() const noexcept;
    [[nodiscard]] float end_time() const noexcept; // last key time (0 without keys)
};

class Sequence {
  public:
    static constexpr std::size_t kMaxTracks = 512;
    static constexpr std::size_t kMaxKeysPerChannel = 1u << 16;

    // Validates and sorts keys by time (stable). Errors name the offending track and key.
    [[nodiscard]] static Result<std::shared_ptr<const Sequence>> parse(std::string_view text, std::string name = {});
    [[nodiscard]] static Result<std::shared_ptr<const Sequence>> from_json(const json::Value& doc,
                                                                           std::string name = {});
    // Deterministic document (editor saves; parse(to_json()) round-trips).
    [[nodiscard]] json::Value to_json() const;

    std::string name;  // asset path (debug views)
    float duration{0}; // seconds; parse() uses the last key time when not given
    std::vector<Track> tracks;
};

// Samples a channel at `time` (clamped to the first/last key). Empty channels return nullopt.
[[nodiscard]] std::optional<math::Vec3> sample(const std::vector<Key<math::Vec3>>& keys, float time) noexcept;
[[nodiscard]] std::optional<math::Quat> sample(const std::vector<Key<math::Quat>>& keys, float time) noexcept;

// ---- components -------------------------------------------------------------------------------

struct SequencePlayerComponent {
    std::string sequence; // asset path, e.g. "sequences/intro.vseq"
    bool play_on_start{true};
    bool loop{false};
    float speed{1.0f}; // negative plays backwards
    bool enabled{true};
    friend bool operator==(const SequencePlayerComponent&, const SequencePlayerComponent&) = default;
};

// An event key the playhead crossed. Kept in the scene for one frame cycle (scripts), see events().
struct SequenceEvent {
    std::uint64_t id{0};
    Entity player; // entity with the SequencePlayer
    Entity target; // resolved track target (null when it does not resolve)
    std::string name;
    std::string payload;
    float time{0}; // key time
};
using EventFn = std::function<void(scene::Scene&, const SequenceEvent&)>;

struct PlaybackState {
    bool valid{false};    // the entity has a player with a loaded sequence
    std::string sequence; // asset path (or the name of a sequence set with set_sequence)
    std::string error;    // load/parse error
    float time{0}, duration{0};
    bool playing{false}, paused{false}, finished{false};
    std::uint32_t loops{0}; // wraps since play()
    std::string camera;     // current camera cut target ("" when none)
};

struct SequencerStats {
    std::uint32_t players{0};
    std::uint32_t playing{0};
    std::uint64_t events{0};    // fired since creation
    std::uint64_t one_shots{0}; // audio keys fired
    std::uint64_t clips{0};     // animation keys fired
    std::uint32_t load_errors{0};
    std::uint32_t missing_targets{0}; // unresolved targets seen (warned once per player and track)
};

using SequenceLoader = std::function<Result<std::string>(std::string_view path)>;
// Reads sanitized relative paths under `assets_root` (package mounts included).
[[nodiscard]] SequenceLoader directory_loader(std::filesystem::path assets_root);

class SequencerSystem {
  public:
    explicit SequencerSystem(SequenceLoader loader = {});
    ~SequencerSystem();
    SequencerSystem(const SequencerSystem&) = delete;
    SequencerSystem& operator=(const SequencerSystem&) = delete;

    // Advances every playing SequencePlayer by dt (times its speed) and applies the tracks.
    void update(scene::Scene& scene, float dt);

    // (Re)starts the entity's sequence from the beginning (the end when speed < 0). `path`
    // replaces the component's sequence (the component is added when missing). False when the
    // entity is dead or the sequence does not load.
    bool play(scene::Scene& scene, Entity e, std::string_view path = {});
    // Uses `sequence` for the entity instead of loading SequencePlayer::sequence (editor
    // previews, tests); nullptr goes back to the asset. Stops playback.
    void set_sequence(scene::Scene& scene, Entity e, std::shared_ptr<const Sequence> sequence);
    void pause(scene::Scene& scene, Entity e, bool paused = true);
    // Stops and rewinds; camera cuts are undone. Transforms keep their current values.
    void stop(scene::Scene& scene, Entity e);
    // Moves the playhead (clamped to [0, duration]) and applies the state at that time (transforms,
    // camera, animation clip) without firing events or audio. Works while stopped (preview).
    bool seek(scene::Scene& scene, Entity e, float time);
    [[nodiscard]] PlaybackState state(scene::Scene& scene, Entity e) const;
    // The entity's loaded sequence (nullptr when none).
    [[nodiscard]] std::shared_ptr<const Sequence> sequence(scene::Scene& scene, Entity e) const;
    // Drops cached sequences so changed files load again (all when `path` is empty); players of a
    // reloaded sequence keep their playhead.
    void reload(std::string_view path = {});
    // Called for each event as it fires (in addition to the scene queue).
    void set_event_handler(EventFn fn);

    [[nodiscard]] const SequencerStats& stats() const noexcept { return stats_; }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    SequencerStats stats_;
};

// Events fired by the sequencer in this scene that have not yet been delivered; the script host
// delivers them and calls mark_delivered(). Events are dropped after one more sequencer update.
[[nodiscard]] const std::vector<SequenceEvent>& events(scene::Scene& scene);
void mark_delivered(scene::Scene& scene, std::uint64_t through_id);
// Highest event id passed to mark_delivered (events with larger ids are still pending).
[[nodiscard]] std::uint64_t delivered_through(scene::Scene& scene);

// Resolves a track target below `root` ("" = root, "A/B" = child B of child A, else the first
// descendant named like the target, else any entity of that name). Null when nothing matches.
[[nodiscard]] Entity resolve_target(scene::Scene& scene, Entity root, std::string_view target);

// Registers "SequencePlayer" (inspector category "Cinematics"). Idempotent.
void register_components();
[[nodiscard]] SequencerSystem* sequencer_of(scene::Scene& scene);
void bind(scene::Scene& scene, SequencerSystem* system);
// Binds in PreUpdate and updates in Phase::Update (add after scripts and AI).
void add_sequencer_systems(gameplay::Simulation& sim, SequencerSystem& system);

} // namespace victus::sequencer
