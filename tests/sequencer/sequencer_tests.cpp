// SPDX-License-Identifier: Apache-2.0
// Timeline sequencer tests (Slice 75): sequence parsing/validation/serialization, channel
// sampling, target resolution, playback (events, loops, reverse, seek, pause, stop), camera cuts,
// animation and audio tracks, loading, hot reload and simulation integration.
#include "victus/animation/animation.hpp"
#include "victus/audio/audio.hpp"
#include "victus/core/fs.hpp"
#include "victus/scene/reflect.hpp"
#include "victus/sequencer/sequencer.hpp"

#include <doctest.h>

#include <cmath>
#include <filesystem>
#include <map>
#include <numbers>

using namespace victus;
using namespace victus::sequencer;
using scene::Scene;
using scene::Transform;

namespace {

std::shared_ptr<const Sequence> seq(std::string_view text) {
    auto s = Sequence::parse(text, "test.vseq");
    if (!s) FAIL_CHECK(s.error().message);
    REQUIRE(s.ok());
    return *s;
}

std::string parse_error(std::string_view text) {
    auto s = Sequence::parse(text);
    CHECK_FALSE(s.ok());
    return s ? std::string() : s.error().message;
}

bool contains(const std::string& s, std::string_view what) { return s.find(what) != std::string::npos; }

Transform& xf(Scene& s, Entity e) { return *s.world().get<Transform>(e); }

std::vector<std::string> names(const std::vector<SequenceEvent>& evs) {
    std::vector<std::string> out;
    for (const auto& e : evs)
        out.push_back(e.name);
    return out;
}

constexpr const char* kDoor = R"({
  "duration": 2,
  "tracks": [
    { "type": "transform", "target": "Door",
      "position": [ { "t": 0, "v": [0, 0, 0] }, { "t": 2, "v": [0, 4, 0] } ] },
    { "type": "event", "keys": [ { "t": 0, "name": "start" }, { "t": 1, "name": "half", "payload": "p" },
                                 { "t": 2, "name": "end" } ] } ] })";

} // namespace

TEST_CASE("sequencer: parsing, defaults, sorting and validation errors") {
    auto s = seq(R"({ "tracks": [
        { "type": "transform", "name": "Lift", "target": "A/B", "space": "relative",
          "position": [ { "t": 3, "v": [0, 3, 0] }, { "t": 1, "v": [0, 1, 0], "interp": "step" } ],
          "rotation": [ { "t": 0, "v": [0, 90, 0] }, { "time": 2, "v": [0, 0, 0, 2] } ] },
        { "type": "animation", "target": "Robot", "keys": [ { "t": 0.5, "clip": "Wave", "fade": 0.1, "loop": false } ] },
        { "type": "audio", "muted": true, "keys": [ { "t": 4, "clip": "a.wav", "bus": "Music", "spatial": true } ] },
        { "type": "camera", "keys": [ { "t": 0, "camera": "Cam" } ] },
        { "type": "event", "keys": [ { "t": 1, "name": "x" } ] } ] })");
    CHECK(s->duration == doctest::Approx(4)); // last key time when no duration is given
    REQUIRE(s->tracks.size() == 5);
    const Track& lift = s->tracks[0];
    CHECK(lift.type == TrackType::Transform);
    CHECK(lift.relative);
    CHECK(lift.name == "Lift");
    CHECK(lift.target == "A/B");
    REQUIRE(lift.position.size() == 2);
    CHECK(lift.position[0].time == doctest::Approx(1)); // sorted
    CHECK(lift.position[0].interp == Interp::Step);
    const math::Quat yaw90 = math::Quat::euler(0, math::radians(90.0f), 0);
    CHECK(std::abs(lift.rotation[0].value.y - yaw90.y) < 1e-5f);
    CHECK(lift.rotation[1].value.w == doctest::Approx(1)); // quaternion normalized
    CHECK(s->tracks[1].animation.at(0).clip == "Wave");
    CHECK_FALSE(s->tracks[1].animation.at(0).loop);
    CHECK(s->tracks[2].muted);
    CHECK(s->tracks[2].audio.at(0).bus == "Music");
    CHECK(s->tracks[2].audio.at(0).spatial);
    CHECK(s->tracks[3].cameras.at(0).camera == "Cam");
    CHECK(s->tracks[0].key_count() == 4);
    CHECK(s->tracks[0].end_time() == doctest::Approx(3));

    CHECK(seq(R"({"duration": 10, "tracks": []})")->duration == doctest::Approx(10));
    CHECK(seq("{}")->tracks.empty());

    CHECK(contains(parse_error("[]"), "expected an object"));
    CHECK(contains(parse_error("{ nope"), "sequence"));
    CHECK(contains(parse_error(R"({"version": 99})"), "newer"));
    CHECK(contains(parse_error(R"({"tracks": {}})"), "\"tracks\" must be an array"));
    CHECK(contains(parse_error(R"({"tracks": [{"type": "bogus"}]})"), "unknown track type"));
    CHECK(contains(parse_error(R"({"tracks": [{}]})"), "missing \"type\""));
    CHECK(contains(parse_error(R"({"tracks": [{"type": "event", "keys": [{"name": "a"}]}]})"), "tracks[0].keys[0]"));
    CHECK(contains(parse_error(R"({"tracks": [{"type": "event", "keys": [{"t": -1, "name": "a"}]}]})"), ">= 0"));
    CHECK(contains(parse_error(R"({"tracks": [{"type": "event", "keys": [{"t": 1}]}]})"), "needs a \"name\""));
    CHECK(contains(parse_error(R"({"tracks": [{"type": "animation", "keys": [{"t": 1}]}]})"), "needs a \"clip\""));
    CHECK(contains(parse_error(R"({"tracks": [{"type": "audio", "keys": [{"t": 1, "clip": "a", "bus": "Nope"}]}]})"),
                   "unknown bus"));
    CHECK(contains(parse_error(R"({"tracks": [{"type": "audio", "keys": [{"t": 1, "clip": "a", "pitch": 0}]}]})"),
                   "pitch"));
    CHECK(contains(parse_error(R"({"tracks": [{"type": "camera", "keys": [{"t": 1}]}]})"), "camera"));
    CHECK(contains(parse_error(R"({"tracks": [{"type": "transform", "position": [{"t": 0, "v": [1, 2]}]}]})"),
                   "[x, y, z]"));
    CHECK(contains(parse_error(R"({"tracks": [{"type": "transform", "rotation": [{"t": 0, "v": [0, 0, 0, 0]}]}]})"),
                   "zero"));
    CHECK(contains(
        parse_error(
            R"({"tracks": [{"type": "transform", "position": [{"t": 0, "v": [0, 0, 0], "interp": "wobble"}]}]})"),
        "unknown interp"));
    CHECK(contains(parse_error(R"({"tracks": [{"type": "transform", "space": "world"}]})"), "space"));
    CHECK(contains(parse_error(R"({"tracks": [{"type": "transform", "position": 3}]})"), "must be an array"));
    CHECK(contains(parse_error(R"({"duration": -2})"), "duration"));
    CHECK(contains(parse_error(R"({"tracks": [{"type": "event", "target": 5}]})"), "target"));
}

TEST_CASE("sequencer: to_json round-trips every track type") {
    auto s = seq(R"({ "duration": 7, "tracks": [
        { "type": "transform", "target": "T", "space": "relative",
          "position": [ { "t": 0, "v": [1, 2, 3], "interp": "cubic" }, { "t": 1.25, "v": [4, 5, 6] } ],
          "rotation": [ { "t": 0, "v": [10, 20, 30] } ], "scale": [ { "t": 2, "v": [2, 2, 2], "interp": "smooth" } ] },
        { "type": "animation", "name": "anim", "keys": [ { "t": 1, "clip": "Run", "fade": 0.3, "speed": 2, "loop": false } ] },
        { "type": "audio", "muted": true, "keys": [ { "t": 2, "clip": "a.wav", "volume": 0.5, "pitch": 1.5, "bus": "Voice", "spatial": true } ] },
        { "type": "event", "keys": [ { "t": 3, "name": "e", "payload": "data" } ] },
        { "type": "camera", "keys": [ { "t": 4, "camera": "Cam/Sub" } ] } ] })");
    const std::string text = json::stringify(s->to_json());
    auto back = seq(text);
    CHECK(back->duration == s->duration);
    REQUIRE(back->tracks.size() == s->tracks.size());
    for (std::size_t i = 0; i < s->tracks.size(); ++i)
        CHECK(back->tracks[i] == s->tracks[i]);
    CHECK(json::stringify(back->to_json()) == text); // deterministic
}

TEST_CASE("sequencer: channel sampling for step, linear, smooth and cubic keys") {
    std::vector<Key<math::Vec3>> keys{{0, {0, 0, 0}, Interp::Linear},
                                      {1, {10, 0, 0}, Interp::Step},
                                      {2, {20, 0, 0}, Interp::Smooth},
                                      {4, {40, 0, 0}, Interp::Linear}};
    CHECK(sample(keys, -1)->x == doctest::Approx(0));
    CHECK(sample(keys, 0.5f)->x == doctest::Approx(5));
    CHECK(sample(keys, 1.5f)->x == doctest::Approx(10)); // step holds
    CHECK(sample(keys, 2.0f)->x == doctest::Approx(20));
    CHECK(sample(keys, 2.5f)->x < 25.0f);                // eased: slower at the start
    CHECK(sample(keys, 3.0f)->x == doctest::Approx(30)); // symmetric midpoint
    CHECK(sample(keys, 9)->x == doctest::Approx(40));
    CHECK_FALSE(sample(std::vector<Key<math::Vec3>>{}, 1));

    // Catmull-Rom passes through the keys and is smooth across them.
    std::vector<Key<math::Vec3>> path{{0, {0, 0, 0}, Interp::Cubic},
                                      {1, {1, 1, 0}, Interp::Cubic},
                                      {2, {2, 0, 0}, Interp::Cubic},
                                      {3, {3, 1, 0}, Interp::Cubic}};
    CHECK(sample(path, 1)->y == doctest::Approx(1));
    CHECK(sample(path, 1.5f)->x == doctest::Approx(1.5f));
    CHECK(sample(path, 0.95f)->y > 0.9f);

    const math::Quat a{}, b = math::Quat::euler(0, math::radians(90.0f), 0);
    std::vector<Key<math::Quat>> rot{{0, a, Interp::Linear}, {1, b, Interp::Linear}};
    const math::Quat half = *sample(rot, 0.5f);
    const math::Quat expect = math::Quat::euler(0, math::radians(45.0f), 0);
    CHECK(std::abs(half.y - expect.y) < 1e-4f);
    CHECK(std::abs(half.w - expect.w) < 1e-4f);
}

TEST_CASE("sequencer: track targets resolve by path, descendant name and scene name") {
    Scene s;
    const Entity root = s.create("Root");
    const Entity a = s.create("A", root);
    const Entity b = s.create("B", a);
    const Entity deep = s.create("Deep", b);
    const Entity other = s.create("Elsewhere");
    const Entity dup = s.create("B"); // same name outside the root: the descendant wins
    (void)dup;
    CHECK(resolve_target(s, root, "") == root);
    CHECK(resolve_target(s, root, ".") == root);
    CHECK(resolve_target(s, root, "A/B") == b);
    CHECK(resolve_target(s, root, "A/B/Deep") == deep);
    CHECK(resolve_target(s, b, "../B/Deep") == deep);
    CHECK(resolve_target(s, root, "B") == b);
    CHECK(resolve_target(s, root, "Deep") == deep);
    CHECK(resolve_target(s, root, "Elsewhere") == other);
    CHECK(resolve_target(s, root, "A/Nope").is_null());
    CHECK(resolve_target(s, root, "Missing").is_null());
}

TEST_CASE("sequencer: playback moves targets, fires events once and holds the end") {
    Scene s;
    const Entity player = s.create("Player");
    const Entity door = s.create("Door", player);
    xf(s, door).position = {5, 5, 5};
    s.world().add<SequencePlayerComponent>(player);
    SequencerSystem sys;
    std::vector<std::string> handled;
    sys.set_event_handler([&](Scene&, const SequenceEvent& ev) { handled.push_back(ev.name); });
    sys.set_sequence(s, player, seq(kDoor));
    CHECK_FALSE(sys.state(s, player).playing); // overrides wait for play()
    sys.update(s, 0.1f);
    CHECK(xf(s, door).position.y == doctest::Approx(5));

    REQUIRE(sys.play(s, player));
    CHECK(xf(s, door).position.y == doctest::Approx(0)); // applied immediately
    sys.update(s, 0.5f);
    CHECK(xf(s, door).position.y == doctest::Approx(1));
    CHECK(handled == std::vector<std::string>{"start"}); // key at t = 0 fires on the first update
    const auto& q = events(s);
    REQUIRE(q.size() == 1);
    CHECK(q[0].player == player);
    CHECK(q[0].target == player);
    sys.update(s, 0.5f); // exactly reaches 1.0
    CHECK(handled == std::vector<std::string>{"start", "half"});
    CHECK(events(s).back().payload == "p");
    sys.update(s, 0.25f);
    CHECK(handled.size() == 2);
    sys.update(s, 5.0f);
    CHECK(handled == std::vector<std::string>{"start", "half", "end"});
    PlaybackState st = sys.state(s, player);
    CHECK(st.finished);
    CHECK_FALSE(st.playing);
    CHECK(st.time == doctest::Approx(2));
    CHECK(xf(s, door).position.y == doctest::Approx(4));
    sys.update(s, 1.0f);
    CHECK(handled.size() == 3);
    CHECK(sys.stats().events == 3);

    // Undelivered events survive one more update; delivered ones are dropped.
    CHECK(names(events(s)) == std::vector<std::string>{"end"});
    sys.update(s, 0.1f);
    CHECK(events(s).empty());
    REQUIRE(sys.play(s, player));
    sys.update(s, 0.1f);
    REQUIRE(events(s).size() == 1);
    mark_delivered(s, events(s).back().id);
    CHECK(delivered_through(s) == events(s).back().id);
    sys.update(s, 0.1f);
    CHECK(events(s).empty());
}

TEST_CASE("sequencer: relative transforms, loops, reverse playback, seek, pause and stop") {
    Scene s;
    const Entity lift = s.create("Lift");
    xf(s, lift).position = {10, 0, 0};
    xf(s, lift).scale = {2, 2, 2};
    s.world().add<SequencePlayerComponent>(lift, SequencePlayerComponent{.loop = true});
    SequencerSystem sys;
    std::vector<std::string> fired;
    sys.set_event_handler([&](Scene&, const SequenceEvent& ev) { fired.push_back(ev.name); });
    sys.set_sequence(s, lift, seq(R"({ "duration": 1, "tracks": [
        { "type": "transform", "space": "relative",
          "position": [ { "t": 0, "v": [0, 0, 0] }, { "t": 1, "v": [0, 2, 0] } ],
          "scale": [ { "t": 0, "v": [1, 1, 1] }, { "t": 1, "v": [2, 1, 1] } ] },
        { "type": "event", "keys": [ { "t": 0.25, "name": "a" }, { "t": 0.75, "name": "b" } ] } ] })"));
    REQUIRE(sys.play(s, lift));
    sys.update(s, 0.5f);
    CHECK(xf(s, lift).position.x == doctest::Approx(10));
    CHECK(xf(s, lift).position.y == doctest::Approx(1));
    CHECK(xf(s, lift).scale.x == doctest::Approx(3));
    CHECK(xf(s, lift).scale.y == doctest::Approx(2));
    sys.update(s, 0.75f); // wraps: b, then a of the next loop
    CHECK(fired == std::vector<std::string>{"a", "b", "a"});
    CHECK(sys.state(s, lift).loops == 1);
    CHECK(sys.state(s, lift).time == doctest::Approx(0.25f));
    CHECK(xf(s, lift).position.y == doctest::Approx(0.5f)); // base kept across loops

    // Pause holds the playhead; seek applies state without firing.
    sys.pause(s, lift, true);
    sys.update(s, 0.5f);
    CHECK(sys.state(s, lift).time == doctest::Approx(0.25f));
    CHECK(sys.state(s, lift).paused);
    fired.clear();
    REQUIRE(sys.seek(s, lift, 0.9f));
    CHECK(xf(s, lift).position.y == doctest::Approx(1.8f));
    CHECK(fired.empty());
    CHECK(sys.seek(s, lift, 50.0f));
    CHECK(sys.state(s, lift).time == doctest::Approx(1.0f)); // clamped
    CHECK_FALSE(sys.seek(s, lift, NAN));
    sys.pause(s, lift, false);

    // Reverse: keys fire in reverse order and the loop wraps at the start.
    s.world().get<SequencePlayerComponent>(lift)->speed = -1.0f;
    REQUIRE(sys.seek(s, lift, 0.8f));
    sys.update(s, 0.7f); // 0.8 -> 0.1 crosses b then a
    CHECK(fired == std::vector<std::string>{"b", "a"});
    sys.update(s, 0.2f); // wraps to 0.9: crosses nothing else
    CHECK(sys.state(s, lift).time == doctest::Approx(0.9f));
    CHECK(fired.size() == 2);

    // Stop rewinds; play recaptures the relative base from the current transform.
    sys.stop(s, lift);
    CHECK_FALSE(sys.state(s, lift).playing);
    CHECK(sys.state(s, lift).time == doctest::Approx(0));
    s.world().get<SequencePlayerComponent>(lift)->speed = 1.0f;
    s.world().get<SequencePlayerComponent>(lift)->loop = false;
    xf(s, lift).position = {0, 100, 0};
    REQUIRE(sys.play(s, lift));
    sys.update(s, 0.5f);
    CHECK(xf(s, lift).position.y == doctest::Approx(101));

    // Speed 2 with a huge step finishes (no loop) after firing the remaining keys once.
    fired.clear();
    s.world().get<SequencePlayerComponent>(lift)->speed = 2.0f;
    sys.update(s, 10.0f);
    CHECK(fired == std::vector<std::string>{"b"});
    CHECK(sys.state(s, lift).finished);
}

TEST_CASE("sequencer: camera cuts switch the primary camera and restore it") {
    Scene s;
    const Entity main = s.create("Main Camera");
    s.world().add<scene::CameraComponent>(main);
    const Entity intro = s.create("Intro");
    s.world().add<scene::CameraComponent>(intro, scene::CameraComponent{.primary = false});
    const Entity close = s.create("Close");
    s.world().add<scene::CameraComponent>(close, scene::CameraComponent{.primary = false});
    const Entity director = s.create("Director");
    s.world().add<SequencePlayerComponent>(director);
    SequencerSystem sys;
    sys.set_sequence(s, director, seq(R"({ "duration": 3, "tracks": [
        { "type": "camera", "keys": [ { "t": 0.5, "camera": "Intro" }, { "t": 1.5, "camera": "Close" },
                                      { "t": 2, "camera": "Missing" } ] } ] })"));
    auto primary = [&](Entity e) { return s.world().get<scene::CameraComponent>(e)->primary; };
    REQUIRE(sys.play(s, director));
    CHECK(primary(main)); // before the first cut
    sys.update(s, 0.6f);
    CHECK(primary(intro));
    CHECK_FALSE(primary(main));
    CHECK(sys.state(s, director).camera == "Intro");
    sys.update(s, 1.0f);
    CHECK(primary(close));
    CHECK_FALSE(primary(intro));
    sys.update(s, 0.5f); // missing camera: keeps the last cut and warns
    CHECK(primary(close));
    CHECK(sys.stats().missing_targets >= 1);
    sys.update(s, 5.0f); // finished: original flags back
    CHECK(primary(main));
    CHECK_FALSE(primary(intro));
    CHECK_FALSE(primary(close));

    REQUIRE(sys.play(s, director));
    sys.update(s, 0.6f);
    CHECK(primary(intro));
    sys.stop(s, director);
    CHECK(primary(main));
    CHECK_FALSE(primary(intro));

    // Removing the player while a cut is active restores the flags too.
    REQUIRE(sys.play(s, director));
    sys.update(s, 0.6f);
    s.world().remove<SequencePlayerComponent>(director);
    sys.update(s, 0.1f);
    CHECK(primary(main));
    CHECK_FALSE(sys.state(s, director).valid);
}

TEST_CASE("sequencer: animation tracks play clips and seeks restore the active clip") {
    using namespace animation;
    auto set = std::make_shared<AnimationSet>();
    set->nodes.push_back({.name = "Hips", .parent = -1, .rest = {}});
    set->clips.push_back(Clip{.name = "Idle", .duration = 1.0f});
    set->clips.push_back(Clip{.name = "Wave", .duration = 2.0f});
    const Uuid model = Uuid::generate();
    AnimationSystem anim;
    anim.add_set(model, set);
    Scene s;
    bind(s, &anim);
    const Entity robot = s.create("Robot");
    s.create("Hips", robot);
    s.world().add<AnimatorComponent>(robot, AnimatorComponent{.model = model, .clip = "Idle"});
    const Entity director = s.create("Director");
    s.world().add<SequencePlayerComponent>(director, SequencePlayerComponent{.play_on_start = false});
    SequencerSystem sys;
    sys.set_sequence(s, director, seq(R"({ "duration": 4, "tracks": [
        { "type": "animation", "target": "Robot", "keys": [ { "t": 1, "clip": "Wave", "loop": false, "speed": 0.5 },
                                                          { "t": 3, "clip": "Nope" } ] } ] })"));
    anim.update(s, 0.0f);
    REQUIRE(sys.play(s, director));
    sys.update(s, 0.5f);
    CHECK(anim.status(s, robot).clip == "Idle");
    sys.update(s, 0.6f);
    CHECK(anim.status(s, robot).clip == "Wave");
    CHECK_FALSE(s.world().get<AnimatorComponent>(robot)->loop);
    CHECK(s.world().get<AnimatorComponent>(robot)->speed == doctest::Approx(0.5f));
    CHECK(sys.stats().clips == 1);
    sys.update(s, 2.0f); // "Nope" does not exist: warned, not counted
    CHECK(sys.stats().clips == 1);

    s.world().get<AnimatorComponent>(robot)->clip = "Idle";
    anim.update(s, 0.0f);
    REQUIRE(sys.seek(s, director, 1.8f));
    CHECK(anim.status(s, robot).clip == "Wave");
    CHECK(s.world().get<AnimatorComponent>(robot)->time == doctest::Approx(0.4f)); // (1.8 - 1) * 0.5
}

TEST_CASE("sequencer: audio tracks play one-shots on the bound audio engine") {
    const auto dir = std::filesystem::temp_directory_path() / "victus_sequencer_audio";
    std::filesystem::create_directories(dir);
    std::vector<float> samples(4800, 0.25f);
    REQUIRE(audio::write_wav(dir / "beep.wav", samples.data(), samples.size(), 1, 48000).ok());
    audio::EngineOptions o;
    o.null_device = true;
    o.assets_root = dir;
    auto engine = audio::AudioEngine::create(o);
    REQUIRE(engine.ok());
    Scene s;
    audio::bind(s, engine->get());
    const Entity e = s.create("Speaker");
    s.world().add<SequencePlayerComponent>(e);
    SequencerSystem sys;
    sys.set_sequence(s, e, seq(R"({ "duration": 1, "tracks": [
        { "type": "audio", "keys": [ { "t": 0.1, "clip": "beep.wav", "spatial": true }, { "t": 0.2, "clip": "missing.wav" } ] },
        { "type": "audio", "muted": true, "keys": [ { "t": 0.1, "clip": "beep.wav" } ] } ] })"));
    REQUIRE(sys.play(s, e));
    sys.update(s, 0.5f);
    CHECK(sys.stats().one_shots == 1);
    CHECK((*engine)->stats().one_shots >= 1);
    std::filesystem::remove_all(dir);
}

TEST_CASE("sequencer: SequencePlayer components load through the loader, report errors and hot reload") {
    std::map<std::string, std::string> files{
        {"seq/door.vseq", kDoor},
        {"seq/broken.vseq", R"({"tracks": [{"type": "nope"}]})"},
    };
    int loads = 0;
    SequencerSystem sys([&](std::string_view path) -> Result<std::string> {
        ++loads;
        const auto it = files.find(std::string(path));
        if (it == files.end()) return make_error(ErrorCode::NotFound, "missing");
        return it->second;
    });
    Scene s;
    const Entity a = s.create("A"), b = s.create("B"), c = s.create("C");
    s.create("Door", a);
    s.create("Door", b);
    s.world().add<SequencePlayerComponent>(a, SequencePlayerComponent{.sequence = "seq/door.vseq"});
    s.world().add<SequencePlayerComponent>(
        b, SequencePlayerComponent{.sequence = "seq/door.vseq", .play_on_start = false});
    s.world().add<SequencePlayerComponent>(c, SequencePlayerComponent{.sequence = "seq/broken.vseq"});
    sys.update(s, 0.5f);
    CHECK(loads == 2); // cached per path
    CHECK(sys.state(s, a).playing);
    CHECK(sys.state(s, a).time == doctest::Approx(0.5f));
    CHECK_FALSE(sys.state(s, b).playing);
    CHECK(sys.state(s, b).valid);
    CHECK_FALSE(sys.state(s, c).valid);
    CHECK(contains(sys.state(s, c).error, "unknown track type"));
    CHECK(sys.stats().load_errors == 1);
    CHECK(sys.stats().players == 3);
    CHECK(sys.stats().playing == 1);
    // Each player resolves "Door" below itself.
    const Entity door_a = resolve_target(s, a, "Door");
    CHECK(xf(s, door_a).position.y == doctest::Approx(1));
    CHECK(xf(s, resolve_target(s, b, "Door")).position.y == doctest::Approx(0));

    // Reload keeps the playhead of players using the changed file; a fixed file loads again.
    files["seq/door.vseq"] = R"({"duration": 2, "tracks": [{"type": "transform", "target": "Door",
        "position": [{"t": 0, "v": [0, 0, 0]}, {"t": 2, "v": [0, 0, 8]}]}]})";
    files["seq/broken.vseq"] = kDoor;
    sys.reload();
    sys.update(s, 0.5f);
    CHECK(sys.state(s, a).time == doctest::Approx(1.0f));
    CHECK(xf(s, door_a).position.z == doctest::Approx(4));
    CHECK(sys.state(s, c).valid);

    // Playing another path through play() replaces the component's sequence.
    files["seq/other.vseq"] = R"({"duration": 1, "tracks": []})";
    REQUIRE(sys.play(s, b, "seq/other.vseq"));
    CHECK(s.world().get<SequencePlayerComponent>(b)->sequence == "seq/other.vseq");
    CHECK(sys.state(s, b).duration == doctest::Approx(1));
    CHECK_FALSE(sys.play(s, b, "seq/missing.vseq"));
    // play() adds a player to entities without one.
    const Entity d = s.create("D");
    REQUIRE(sys.play(s, d, "seq/other.vseq"));
    CHECK(s.world().get<SequencePlayerComponent>(d));
    CHECK_FALSE(sys.play(s, s.create("E")));

    // Disabled players do not advance.
    s.world().get<SequencePlayerComponent>(a)->enabled = false;
    const float t = sys.state(s, a).time;
    sys.update(s, 0.25f);
    CHECK(sys.state(s, a).time == doctest::Approx(t));

    // Reflection: scenes save and load SequencePlayer.
    register_components();
    const auto* schema = scene::find_schema("SequencePlayer");
    REQUIRE(schema);
    CHECK(schema->field("sequence")->hint == "sequence");
    Scene copy;
    REQUIRE(copy.load(s.save()).ok());
    const Entity a2 = copy.find_by_name("A");
    REQUIRE(copy.world().get<SequencePlayerComponent>(a2));
    CHECK(copy.world().get<SequencePlayerComponent>(a2)->sequence == "seq/door.vseq");
    CHECK_FALSE(copy.world().get<SequencePlayerComponent>(a2)->enabled);
}

TEST_CASE("sequencer: runs inside a Simulation and reads files with directory_loader") {
    const auto dir = std::filesystem::temp_directory_path() / "victus_sequencer_loader";
    std::filesystem::create_directories(dir / "seq");
    REQUIRE(fs::write_text_atomic(dir / "seq" / "door.vseq", kDoor).ok());
    SequencerSystem sys(directory_loader(dir));
    gameplay::Simulation sim;
    add_sequencer_systems(sim, sys);
    Scene s;
    const Entity p = s.create("P");
    const Entity door = s.create("Door", p);
    s.world().add<SequencePlayerComponent>(p, SequencePlayerComponent{.sequence = "seq/door.vseq"});
    for (int i = 0; i < 30; ++i)
        sim.tick(s, 1.0f / 30.0f);
    CHECK(sequencer_of(s) == &sys);
    CHECK(xf(s, door).position.y == doctest::Approx(2).epsilon(0.02));
    CHECK(s.world_matrix(door).transform_point({}).y == doctest::Approx(2).epsilon(0.02));
    CHECK(sys.stats().events == 2);

    SequencerSystem bad(directory_loader(dir));
    Scene s2;
    const Entity q = s2.create("Q");
    s2.world().add<SequencePlayerComponent>(q, SequencePlayerComponent{.sequence = "../escape.vseq"});
    bad.update(s2, 0.1f);
    CHECK_FALSE(bad.state(s2, q).valid);
    CHECK_FALSE(bad.state(s2, q).error.empty());
    std::filesystem::remove_all(dir);
}
