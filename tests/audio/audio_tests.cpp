// SPDX-License-Identifier: Apache-2.0
// Audio tests run the real miniaudio mixer without an output device and inspect the mixed frames.
#include "doctest.h"
#include "victus/audio/audio.hpp"
#include "victus/core/fs.hpp"
#include "victus/scene/reflect.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <vector>

using namespace victus;
namespace sfs = std::filesystem;

namespace {

sfs::path temp_dir(const char* name) {
    const auto d = sfs::temp_directory_path() / (std::string("victus_audio_") + name);
    std::error_code ec;
    sfs::remove_all(d, ec);
    sfs::create_directories(d);
    return d;
}

// Mono sine tone of `seconds` at `amplitude`.
void write_tone(const sfs::path& file, float seconds, float amplitude = 0.5f, float hz = 440.0f) {
    const auto frames = static_cast<std::size_t>(seconds * 48000.0f);
    std::vector<float> pcm(frames);
    for (std::size_t i = 0; i < frames; ++i)
        pcm[i] = amplitude * std::sin(2.0f * std::numbers::pi_v<float> * hz * static_cast<float>(i) / 48000.0f);
    REQUIRE(audio::write_wav(file, pcm.data(), frames, 1, 48000).ok());
}

struct Mixed {
    float rms_l{0}, rms_r{0};
    [[nodiscard]] float rms() const { return std::sqrt((rms_l * rms_l + rms_r * rms_r) * 0.5f); }
};

Mixed mix(audio::AudioEngine& a, std::size_t frames) {
    std::vector<float> buf(frames * 2);
    REQUIRE(a.render(buf.data(), frames) == frames);
    double l = 0, r = 0;
    for (std::size_t i = 0; i < frames; ++i) {
        l += static_cast<double>(buf[2 * i]) * buf[2 * i];
        r += static_cast<double>(buf[2 * i + 1]) * buf[2 * i + 1];
    }
    return {static_cast<float>(std::sqrt(l / static_cast<double>(frames))), static_cast<float>(std::sqrt(r / static_cast<double>(frames)))};
}

std::unique_ptr<audio::AudioEngine> make_engine(const sfs::path& root) {
    audio::EngineOptions o;
    o.assets_root = root;
    o.null_device = true;
    auto a = audio::AudioEngine::create(o);
    REQUIRE(a.ok());
    REQUIRE_FALSE((*a)->has_device());
    CHECK((*a)->sample_rate() == 48000);
    CHECK((*a)->channels() == 2);
    return std::move(a).value();
}

scene::Entity add_source(scene::Scene& s, const char* clip, bool spatial, math::Vec3 at = {}) {
    const auto e = s.create("Source");
    s.world().get<scene::Transform>(e)->position = at;
    auto& c = s.world().add<audio::AudioSourceComponent>(e);
    c.clip = clip;
    c.spatial = spatial;
    return e;
}

} // namespace

TEST_CASE("audio: bus names round-trip") {
    for (std::size_t i = 0; i < audio::kBusCount; ++i) {
        const auto b = static_cast<audio::Bus>(i);
        CHECK(audio::parse_bus(audio::bus_name(b)) == b);
    }
    CHECK(audio::parse_bus("music") == audio::Bus::Music);
    CHECK_FALSE(audio::parse_bus("nope").has_value());
}

TEST_CASE("audio: source plays its clip, finishes, loops, buses scale it") {
    const auto dir = temp_dir("play");
    write_tone(dir / "tone.wav", 0.25f);
    auto a = make_engine(dir);
    scene::Scene s;
    const auto e = add_source(s, "tone.wav", false);
    a->update(s);
    CHECK(a->stats().voices == 1);
    CHECK(a->stats().playing == 1);
    CHECK(a->is_playing(e));
    const Mixed m = mix(*a, 4800);
    CHECK(m.rms() > 0.2f); // 0.5 amplitude sine -> ~0.35 RMS
    CHECK(m.rms() < 0.45f);
    // The 0.25 s clip ends; it does not restart on its own.
    (void)mix(*a, 12000);
    a->update(s);
    CHECK_FALSE(a->is_playing(e));
    CHECK(mix(*a, 4800).rms() < 1e-4f);
    // play() restarts it; looping keeps it alive past its length.
    s.world().get<audio::AudioSourceComponent>(e)->loop = true;
    REQUIRE(a->play(s, e).ok());
    a->update(s);
    (void)mix(*a, 48000);
    CHECK(a->is_playing(e));
    const float full = mix(*a, 4800).rms();
    CHECK(full > 0.2f);
    // Bus volume and Master scale the output; mute silences it.
    a->set_bus_volume(audio::Bus::Sfx, 0.5f);
    CHECK(mix(*a, 4800).rms() == doctest::Approx(full * 0.5f).epsilon(0.05));
    a->set_bus_volume(audio::Bus::Master, 0.5f);
    CHECK(mix(*a, 4800).rms() == doctest::Approx(full * 0.25f).epsilon(0.05));
    a->set_bus_muted(audio::Bus::Master, true);
    CHECK(a->bus_muted(audio::Bus::Master));
    CHECK(mix(*a, 4800).rms() < 1e-4f);
    a->set_bus_muted(audio::Bus::Master, false);
    a->set_bus_volume(audio::Bus::Master, 1.0f);
    a->set_bus_volume(audio::Bus::Sfx, 1.0f);
    // Component volume, pause, stop.
    s.world().get<audio::AudioSourceComponent>(e)->volume = 0.25f;
    a->update(s);
    CHECK(mix(*a, 4800).rms() == doctest::Approx(full * 0.25f).epsilon(0.05));
    a->set_paused(e, true);
    (void)mix(*a, 480); // the voice already queued up to one period before pausing
    CHECK(mix(*a, 4800).rms() < 1e-4f);
    a->set_paused(e, false);
    CHECK(mix(*a, 4800).rms() > 0.05f);
    a->stop(e);
    CHECK_FALSE(a->is_playing(e));
    // Removing the component releases the voice.
    s.world().remove<audio::AudioSourceComponent>(e);
    a->update(s);
    CHECK(a->stats().voices == 0);
    std::error_code ec;
    sfs::remove_all(dir, ec);
}

TEST_CASE("audio: spatial sources attenuate with distance and pan toward their side") {
    const auto dir = temp_dir("spatial");
    write_tone(dir / "tone.wav", 2.0f);
    auto a = make_engine(dir);
    scene::Scene s;
    const auto listener = s.create("Ears");
    s.world().add<audio::AudioListenerComponent>(listener);
    const auto e = add_source(s, "tone.wav", true, {0, 0, -1});
    auto* c = s.world().get<audio::AudioSourceComponent>(e);
    c->attenuation = audio::Attenuation::Inverse;
    c->min_distance = 1.0f;
    c->max_distance = 100.0f;
    a->update(s);
    (void)mix(*a, 2400); // let spatializer smoothing settle
    const float near = mix(*a, 4800).rms();
    s.world().get<scene::Transform>(e)->position = {0, 0, -10};
    a->update(s);
    (void)mix(*a, 2400);
    const float far = mix(*a, 4800).rms();
    CAPTURE(near);
    CAPTURE(far);
    CHECK(near > 0.1f);
    CHECK(far == doctest::Approx(near * 0.1f).epsilon(0.25)); // inverse law: 1/distance
    // To the listener's right (+X when facing -Z): the right channel dominates.
    s.world().get<scene::Transform>(e)->position = {3, 0, 0};
    a->update(s);
    (void)mix(*a, 2400);
    const Mixed right = mix(*a, 4800);
    CHECK(right.rms_r > right.rms_l * 1.5f);
    // Turn the listener around (face +Z): the same source is now on the left.
    s.world().get<scene::Transform>(listener)->rotation = math::Quat::euler(0, std::numbers::pi_v<float>, 0);
    a->update(s);
    (void)mix(*a, 2400);
    const Mixed left = mix(*a, 4800);
    CHECK(left.rms_l > left.rms_r * 1.5f);
    std::error_code ec;
    sfs::remove_all(dir, ec);
}

TEST_CASE("audio: one-shots play, are reaped and capped; bad clips are reported") {
    const auto dir = temp_dir("oneshot");
    write_tone(dir / "blip.wav", 0.1f);
    audio::EngineOptions o;
    o.assets_root = dir;
    o.null_device = true;
    o.max_one_shots = 4;
    auto made = audio::AudioEngine::create(o);
    REQUIRE(made.ok());
    auto& a = **made;
    REQUIRE(a.preload("blip.wav").ok());
    REQUIRE(a.play_one_shot("blip.wav", {.bus = audio::Bus::Ui, .volume = 1.0f}).ok());
    CHECK(a.stats().one_shots == 1);
    CHECK(mix(a, 2400).rms() > 0.2f);
    (void)mix(a, 9600);
    scene::Scene s;
    a.update(s);
    CHECK(a.stats().one_shots == 0);
    for (int i = 0; i < 10; ++i) REQUIRE(a.play_one_shot("blip.wav").ok());
    CHECK(a.stats().one_shots == 4);
    CHECK(a.stats().clips_loaded == 1);
    CHECK_FALSE(a.play_one_shot("missing.wav").ok());
    CHECK_FALSE(a.play_one_shot("../escape.wav").ok());
    CHECK(a.stats().load_errors >= 1);
    // A source with a missing clip does not stop others from playing.
    const auto bad = add_source(s, "missing.wav", false);
    const auto good = add_source(s, "blip.wav", false);
    a.update(s);
    CHECK_FALSE(a.is_playing(bad));
    CHECK(a.is_playing(good));
    a.stop_all();
    CHECK(a.stats().voices == 0);
    CHECK(a.stats().one_shots == 0);
    std::error_code ec;
    sfs::remove_all(dir, ec);
}

TEST_CASE("audio: Ogg Vorbis (streamed) and FLAC clips decode") {
    auto a = make_engine(VICTUS_MINIAUDIO_DATA_DIR);
    scene::Scene s;
    const auto ogg = add_source(s, "48000-stereo.ogg", false);
    s.world().get<audio::AudioSourceComponent>(ogg)->stream = true;
    a->update(s);
    CHECK(a->is_playing(ogg));
    CHECK(mix(*a, 48000).rms() > 0.01f);
    s.destroy(ogg);
    const auto flac = add_source(s, "16-44100-stereo.flac", false);
    a->update(s);
    CHECK(a->stats().voices == 1);
    CHECK(a->is_playing(flac));
    CHECK(mix(*a, 48000).rms() > 0.01f); // resampled 44.1 -> 48 kHz
    CHECK(a->stats().load_errors == 0);
}

TEST_CASE("audio: components serialize and systems drive the engine") {
    audio::register_components();
    const auto* schema = scene::find_schema("AudioSource");
    REQUIRE(schema != nullptr);
    CHECK(schema->category == "Audio");
    audio::AudioSourceComponent c;
    c.clip = "music/theme.ogg";
    c.bus = audio::Bus::Music;
    c.loop = true;
    c.stream = true;
    c.volume = 0.7f;
    c.attenuation = audio::Attenuation::Linear;
    const json::Value v = schema->save(&c);
    audio::AudioSourceComponent back;
    REQUIRE(schema->load(&back, v).ok());
    CHECK(back == c);
    REQUIRE(scene::find_schema("AudioListener") != nullptr);

    const auto dir = temp_dir("systems");
    write_tone(dir / "tone.wav", 1.0f);
    auto a = make_engine(dir);
    scene::Scene s;
    const auto e = add_source(s, "tone.wav", false);
    gameplay::Simulation sim;
    audio::add_audio_systems(sim, *a);
    sim.tick(s, 1.0f / 60.0f);
    CHECK(audio::engine_of(s) == a.get());
    CHECK(a->is_playing(e));
    // play_on_start fires once per source: after stopping, ticks do not restart it.
    a->stop(e);
    sim.tick(s, 1.0f / 60.0f);
    CHECK_FALSE(a->is_playing(e));
    std::error_code ec;
    sfs::remove_all(dir, ec);
}

TEST_CASE("audio: reload_clip picks up a changed file and restarts its sources") {
    const auto dir = temp_dir("reload");
    write_tone(dir / "music.wav", 1.0f, 0.2f);
    auto a = make_engine(dir);
    scene::Scene s;
    const auto e = add_source(s, "music.wav", false);
    s.world().get<audio::AudioSourceComponent>(e)->loop = true;
    a->update(s);
    (void)mix(*a, 4800);
    const float quiet = mix(*a, 4800).rms();
    CHECK(quiet == doctest::Approx(0.2f / std::sqrt(2.0f)).epsilon(0.1));
    // The file changes on disk: the old decoded data stays until the clip is reloaded.
    write_tone(dir / "music.wav", 1.0f, 0.6f);
    CHECK(mix(*a, 4800).rms() == doctest::Approx(quiet).epsilon(0.1));
    CHECK(a->reload_clip("music.wav") == 1);
    a->update(s);
    (void)mix(*a, 480);
    const float loud = mix(*a, 4800).rms();
    CHECK(loud == doctest::Approx(0.6f / std::sqrt(2.0f)).epsilon(0.1));
    CHECK(a->is_playing(e));
    CHECK(a->stats().voices == 1);
    // A clip nothing plays reloads nothing; a broken file silences the source instead of crashing.
    CHECK(a->reload_clip("other.wav") == 0);
    REQUIRE(fs::write_atomic(dir / "music.wav", std::vector<std::uint8_t>{'n', 'o', 'p', 'e'}).ok());
    CHECK(a->reload_clip("music.wav") == 0);
    a->update(s);
    (void)mix(*a, 480); // up to one period was already mixed before the reload
    CHECK(mix(*a, 4800).rms() < 1e-4f);
    std::error_code ec;
    sfs::remove_all(dir, ec);
}

TEST_CASE("audio: inspect_clip decodes from memory and rejects bad data") {
    std::vector<float> pcm(4800 * 2, 0.25f);
    auto wav = audio::inspect_clip(audio::encode_wav(pcm.data(), 4800, 2, 22050));
    REQUIRE(wav.ok());
    CHECK(wav->channels == 2);
    CHECK(wav->sample_rate == 22050);
    CHECK(wav->frames == 4800);
    for (const char* name : {"48000-stereo.ogg", "16-44100-stereo.flac"}) {
        std::ifstream in(sfs::path(VICTUS_MINIAUDIO_DATA_DIR) / name, std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        REQUIRE_FALSE(bytes.empty());
        auto info = audio::inspect_clip(bytes);
        REQUIRE_MESSAGE(info.ok(), name);
        CHECK(info->channels == 2);
        CHECK(info->frames > 44100);
        auto capped = audio::inspect_clip(bytes, 1000);
        REQUIRE_FALSE(capped.ok());
        CHECK(capped.error().code == ErrorCode::LimitExceeded);
    }
    CHECK(audio::inspect_clip(std::vector<std::uint8_t>{}).error().code == ErrorCode::ParseError);
    auto junk = audio::inspect_clip(std::vector<std::uint8_t>{'n', 'o', 'p', 'e', 0, 1, 2, 3});
    REQUIRE_FALSE(junk.ok());
    CHECK(junk.error().code == ErrorCode::ParseError);
    CHECK_FALSE(junk.error().message.empty());
}
