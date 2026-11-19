// SPDX-License-Identifier: Apache-2.0
// Acoustics (ADR-0096): geometry occlusion with per-band material transmission, volumetric
// occlusion, the ray budget, reverb zones and the underwater filter. Runs the real mixer without a
// device against a real Jolt world bound to the scene.
#include "doctest.h"
#include "../../engine/audio/src/acoustics.hpp"
#include "victus/audio/audio.hpp"
#include "victus/gameplay/queries.hpp"
#include "victus/gameplay/water.hpp"
#include "victus/physics/physics.hpp"
#include "victus/scene/reflect.hpp"

#include <cmath>
#include <filesystem>
#include <numbers>
#include <vector>

using namespace victus;
namespace sfs = std::filesystem;
using audio::AcousticPreset;

namespace {

constexpr float kDt = 1.0f / 60.0f;

sfs::path temp_dir(const char* name) {
    const auto d = sfs::temp_directory_path() / (std::string("victus_acoustics_") + name);
    std::error_code ec;
    sfs::remove_all(d, ec);
    sfs::create_directories(d);
    return d;
}

void write_tone(const sfs::path& file, float seconds, float hz, float amplitude = 0.5f) {
    const auto frames = static_cast<std::size_t>(seconds * 48000.0f);
    std::vector<float> pcm(frames);
    for (std::size_t i = 0; i < frames; ++i)
        pcm[i] = amplitude * std::sin(2.0f * std::numbers::pi_v<float> * hz * static_cast<float>(i) / 48000.0f);
    REQUIRE(audio::write_wav(file, pcm.data(), frames, 1, 48000).ok());
}

float rms(audio::AudioEngine& a, std::size_t frames) {
    std::vector<float> buf(frames * 2);
    REQUIRE(a.render(buf.data(), frames) == frames);
    double acc = 0;
    for (float v : buf) acc += static_cast<double>(v) * v;
    return static_cast<float>(std::sqrt(acc / static_cast<double>(buf.size())));
}

std::unique_ptr<audio::AudioEngine> make_engine(const sfs::path& root, std::uint32_t rays = 64) {
    audio::EngineOptions o;
    o.assets_root = root;
    o.null_device = true;
    o.max_occlusion_rays = rays;
    auto a = audio::AudioEngine::create(o);
    REQUIRE(a.ok());
    return std::move(a).value();
}

struct Rig {
    sfs::path dir;
    scene::Scene scene;
    std::unique_ptr<physics::PhysicsWorld> world;
    std::unique_ptr<audio::AudioEngine> a;
    scene::Entity listener;

    explicit Rig(const char* name, std::uint32_t rays = 64) : dir(temp_dir(name)) {
        audio::register_components();
        write_tone(dir / "low.wav", 2.0f, 200.0f);
        write_tone(dir / "high.wav", 2.0f, 6000.0f);
        write_tone(dir / "blip.wav", 0.05f, 440.0f);
        auto w = physics::PhysicsWorld::create({});
        REQUIRE(w.ok());
        world = std::move(w).value();
        a = make_engine(dir, rays);
        listener = scene.create("Listener");
        scene.world().add<audio::AudioListenerComponent>(listener);
    }

    scene::Entity wall(math::Vec3 at, math::Vec3 size, std::optional<AcousticPreset> material = {}) {
        const auto e = scene.create("Wall");
        scene.world().get<scene::Transform>(e)->position = at;
        physics::ColliderComponent c;
        c.size = size;
        scene.world().add<physics::ColliderComponent>(e, c);
        physics::RigidBodyComponent rb;
        rb.type = physics::BodyType::Static;
        scene.world().add<physics::RigidBodyComponent>(e, rb);
        if (material) scene.world().add<audio::AcousticMaterialComponent>(e).preset = *material;
        return e;
    }

    scene::Entity source(const char* clip, math::Vec3 at, bool occlusion = true, float radius = 0.0f) {
        const auto e = scene.create("Source");
        scene.world().get<scene::Transform>(e)->position = at;
        auto& c = scene.world().add<audio::AudioSourceComponent>(e);
        c.clip = clip;
        c.loop = true;
        c.attenuation = audio::Attenuation::None;
        c.occlusion = occlusion;
        c.occlusion_radius = radius;
        return e;
    }

    // Builds physics bodies and binds the world (installs the segment query).
    void bind_physics() {
        world->step(scene, kDt);
        physics::bind(scene, world.get());
    }

    // Runs `frames` audio updates (each mixing one update's worth of samples), then measures.
    float settle_and_measure(int updates = 30) {
        for (int i = 0; i < updates; ++i) {
            a->update(scene, kDt);
            (void)rms(*a, 800);
        }
        return rms(*a, 4800);
    }
};

float level(const char* clip, std::optional<AcousticPreset> material, bool with_wall = true) {
    Rig r("level");
    if (with_wall) r.wall({0, 0, -5}, {6, 6, 0.2f}, material);
    (void)r.source(clip, {0, 0, -10});
    r.bind_physics();
    return r.settle_and_measure();
}

} // namespace

TEST_CASE("acoustics: material presets, tilt filter and zone weights") {
    const auto concrete = audio::acoustic_transmission({AcousticPreset::Concrete, {}});
    CHECK(concrete.x == doctest::Approx(0.015f));
    CHECK(concrete.y == doctest::Approx(0.002f));
    CHECK(audio::acoustic_transmission({}).x == doctest::Approx(0.1f)); // generic
    const auto custom = audio::acoustic_transmission({AcousticPreset::Custom, {0.5f, 2.0f, -1.0f}});
    CHECK(custom.x == doctest::Approx(0.5f));
    CHECK(custom.y == 1.0f);
    CHECK(custom.z == 0.0f);

    using namespace audio::detail;
    CHECK(tilt_cutoff(1.0f) == 0.0f);
    CHECK(tilt_cutoff(0.1f) == kMinOcclusionCutoffHz); // steeper than one pole can do
    for (float r : {0.3f, 0.5f, 0.8f, 0.95f}) {
        const float fc = tilt_cutoff(r);
        CHECK(lowpass1_gain(kBandMidHz, fc) / lowpass1_gain(kBandLowHz, fc) == doctest::Approx(r).epsilon(0.01));
    }

    audio::ReverbZoneComponent z;
    z.size = {4, 2, 4};
    z.fade = 2.0f;
    float vol = 0;
    CHECK(zone_weight(math::Mat4::identity(), z, {1, 0.5f, -1}, &vol) == 1.0f);
    CHECK(vol == doctest::Approx(32.0f));
    CHECK(zone_weight(math::Mat4::identity(), z, {3, 0, 0}) == doctest::Approx(0.5f)); // 1 m outside
    CHECK(zone_weight(math::Mat4::identity(), z, {5, 0, 0}) == 0.0f);
    z.fade = 0.0f;
    CHECK(zone_weight(math::Mat4::identity(), z, {2.01f, 0, 0}) == 0.0f);
    // Scale and rotation apply: a 90 degree yaw swaps the X/Z extents; scale doubles them.
    scene::Transform t;
    t.rotation = math::Quat::axis_angle({0, 1, 0}, std::numbers::pi_v<float> / 2);
    t.scale = {2, 1, 1};
    z.size = {4, 2, 1};
    const math::Mat4 m = t.matrix();
    CHECK(zone_weight(m, z, {0, 0, 3.9f}, &vol) == 1.0f);
    CHECK(vol == doctest::Approx(16.0f));
    CHECK(zone_weight(m, z, {3.9f, 0, 0}) == 0.0f);
}

TEST_CASE("acoustics: walls attenuate and muffle by material") {
    const float low_open = level("low.wav", {}, false);
    const float high_open = level("high.wav", {}, false);
    const float low_concrete = level("low.wav", AcousticPreset::Concrete);
    const float high_concrete = level("high.wav", AcousticPreset::Concrete);
    const float low_wood = level("low.wav", AcousticPreset::Wood);
    const float low_generic = level("low.wav", std::nullopt);
    MESSAGE("200 Hz RMS open " << low_open << ", concrete " << low_concrete << ", wood " << low_wood << ", generic " << low_generic);
    MESSAGE("6 kHz RMS open " << high_open << ", concrete " << high_concrete);
    REQUIRE(low_open > 0.1f); // panned mono source in front: about half per channel
    REQUIRE(high_open > 0.1f);
    // Concrete passes amplitude sqrt(0.015) = 0.12 at 400 Hz.
    CHECK(low_concrete / low_open == doctest::Approx(0.13f).epsilon(0.25));
    // ... and muffles: the 6 kHz tone loses far more than the 200 Hz one.
    CHECK(high_concrete / high_open < 0.35f * (low_concrete / low_open));
    CHECK(low_wood > 1.5f * low_concrete);
    CHECK(low_generic > low_wood); // no material = generic (10% energy)
}

TEST_CASE("acoustics: occlusion state, partial occlusion and opt-in") {
    Rig r("state");
    r.wall({-1.4f, 0, -5}, {3.2f, 6, 0.2f}, AcousticPreset::Concrete); // x in [-3, 0.2]
    const auto full = r.source("low.wav", {0, 0, -10});
    const auto partial = r.source("low.wav", {0, 0, -10}, true, 1.0f);
    const auto off = r.source("low.wav", {0, 0, -10}, false);
    r.bind_physics();
    (void)r.settle_and_measure(20);
    const auto f = r.a->occlusion(full);
    REQUIRE(f);
    CHECK(f->blocked == 1.0f);
    CHECK(f->gain == doctest::Approx(0.134f).epsilon(0.05));
    CHECK(f->cutoff == doctest::Approx(audio::detail::tilt_cutoff(std::sqrt(0.002f / 0.015f))).epsilon(0.02));
    const auto p = r.a->occlusion(partial);
    REQUIRE(p);
    CHECK(p->blocked == doctest::Approx(0.8f)); // the +x ray clears the wall's edge
    CHECK(p->gain > 2.0f * f->gain);
    CHECK(p->gain < 1.0f);
    CHECK_FALSE(r.a->occlusion(off));
    CHECK(r.a->acoustics().occluded == 2);
    CHECK(r.a->acoustics().rays == 6);

    // Turning occlusion off restores the voice at once; moving the source clear smooths back up.
    r.scene.world().get<audio::AudioSourceComponent>(full)->occlusion = false;
    r.a->update(r.scene, kDt);
    CHECK_FALSE(r.a->occlusion(full));
    r.scene.world().get<scene::Transform>(partial)->position = {5, 0, -10};
    r.a->update(r.scene, kDt);
    const float mid = r.a->occlusion(partial)->gain;
    CHECK(mid < 0.9f); // smoothed, not snapped
    (void)r.settle_and_measure(60);
    CHECK(r.a->occlusion(partial)->gain == doctest::Approx(1.0f));
    CHECK(r.a->occlusion(partial)->blocked == 0.0f);
    CHECK(r.a->occlusion(partial)->cutoff == 0.0f);
}

TEST_CASE("acoustics: the listener's and source's own colliders do not occlude; no physics, no change") {
    Rig r("self");
    // The listener and the source sit inside their own colliders (a player capsule, a radio).
    physics::ColliderComponent body;
    body.shape = physics::ColliderComponent::Shape::Sphere;
    r.scene.world().add<physics::ColliderComponent>(r.listener, body);
    physics::RigidBodyComponent rb;
    rb.type = physics::BodyType::Kinematic;
    r.scene.world().add<physics::RigidBodyComponent>(r.listener, rb);
    const auto radio = r.scene.create("Radio");
    r.scene.world().get<scene::Transform>(radio)->position = {0, 0, -10};
    r.scene.world().add<physics::ColliderComponent>(radio, body);
    r.scene.world().add<physics::RigidBodyComponent>(radio, rb);
    const auto s = r.source("low.wav", {0, 0, 0});
    r.scene.set_parent(s, radio);
    r.bind_physics();
    const float level_bound = r.settle_and_measure(10);
    REQUIRE(r.a->occlusion(s));
    CHECK(r.a->occlusion(s)->blocked == 0.0f);

    physics::bind(r.scene, nullptr);
    CHECK_FALSE(gameplay::has_segment_query(r.scene));
    (void)r.settle_and_measure(5);
    CHECK_FALSE(r.a->occlusion(s));
    CHECK(r.a->acoustics().rays == 0);
    CHECK(r.settle_and_measure(5) == doctest::Approx(level_bound).epsilon(0.02));
}

TEST_CASE("acoustics: the ray budget is shared round-robin") {
    Rig r("budget", 3);
    r.wall({0, 0, -5}, {40, 6, 0.2f}, AcousticPreset::Brick);
    std::vector<scene::Entity> sources;
    for (int i = 0; i < 7; ++i) sources.push_back(r.source("low.wav", {static_cast<float>(i) - 3.0f, 0, -10}));
    r.bind_physics();
    r.a->update(r.scene, kDt);
    CHECK(r.a->acoustics().rays == 3);
    int traced = 0;
    for (auto e : sources) traced += r.a->occlusion(e) ? 1 : 0;
    CHECK(traced == 3);
    r.a->update(r.scene, kDt);
    r.a->update(r.scene, kDt);
    traced = 0;
    for (auto e : sources) traced += r.a->occlusion(e) ? 1 : 0;
    CHECK(traced == 7);
    CHECK(r.a->acoustics().rays == 3);
    // Volumetric sources cost five rays; one that does not fit waits for a later update.
    r.scene.world().get<audio::AudioSourceComponent>(sources[0])->occlusion_radius = 0.5f;
    for (int i = 0; i < 6; ++i) {
        r.a->update(r.scene, kDt);
        CHECK(r.a->acoustics().rays <= 3);
    }
}

TEST_CASE("acoustics: reverb zones drive the reverb from the listener's position") {
    Rig r("zone");
    const auto hall = r.scene.create("Hall");
    auto& z = r.scene.world().add<audio::ReverbZoneComponent>(hall);
    z.size = {10, 6, 10};
    z.fade = 2.0f;
    z.decay = 0.9f;
    z.wet = 0.6f;
    const auto booth = r.scene.create("Booth");
    r.scene.world().get<scene::Transform>(booth)->position = {3, 0, 0};
    auto& b = r.scene.world().add<audio::ReverbZoneComponent>(booth);
    b.size = {2, 3, 2};
    b.fade = 0.0f;
    b.decay = 0.2f;
    b.wet = 0.1f;
    b.lowpass = 2000.0f;
    b.priority = 1;
    auto at = [&](math::Vec3 p) { r.scene.world().get<scene::Transform>(r.listener)->position = p; };

    at({0, 0, 0});
    r.a->update(r.scene, kDt);
    const float first = r.a->acoustics().zone_weight;
    CHECK(first > 0.0f);
    CHECK(first < 0.2f); // fades in
    (void)r.settle_and_measure(120);
    auto st = r.a->acoustics();
    CHECK(st.zone == hall);
    CHECK(st.zone_weight == doctest::Approx(1.0f).epsilon(0.005));
    CHECK(st.decay == doctest::Approx(0.9f).epsilon(0.005));
    CHECK(st.send[static_cast<std::size_t>(audio::Bus::Sfx)] == doctest::Approx(0.6f).epsilon(0.005));
    CHECK(st.send[static_cast<std::size_t>(audio::Bus::Music)] == 0.0f);
    CHECK(st.send[static_cast<std::size_t>(audio::Bus::Ui)] == 0.0f);
    CHECK(r.a->bus_reverb_send(audio::Bus::Sfx) == 0.0f); // user settings untouched
    CHECK(r.a->reverb_decay() == doctest::Approx(0.7f));

    // The higher-priority booth wins inside it and carries its low-pass.
    at({3, 0, 0});
    (void)r.settle_and_measure(120);
    st = r.a->acoustics();
    CHECK(st.zone == booth);
    CHECK(st.decay == doctest::Approx(0.2f).epsilon(0.02));
    CHECK(st.lowpass[static_cast<std::size_t>(audio::Bus::Sfx)] == doctest::Approx(2000.0f).epsilon(0.03));

    // One metre outside the hall: half weight. Far away: exactly the user's settings again.
    at({6, 0, 0});
    (void)r.settle_and_measure(120);
    CHECK(r.a->acoustics().zone == hall);
    CHECK(r.a->acoustics().zone_weight == doctest::Approx(0.5f).epsilon(0.02));
    at({50, 0, 0});
    (void)r.settle_and_measure(180);
    st = r.a->acoustics();
    CHECK(st.zone.is_null());
    CHECK(st.zone_weight == 0.0f);
    CHECK(st.decay == 0.7f);
    CHECK(st.send[static_cast<std::size_t>(audio::Bus::Sfx)] == 0.0f);
    CHECK(st.lowpass[static_cast<std::size_t>(audio::Bus::Sfx)] == 0.0f);

    // A blip rings on inside the hall and not outside it.
    auto tail = [&](math::Vec3 p) {
        at(p);
        (void)r.settle_and_measure(120);
        REQUIRE(r.a->play_one_shot("blip.wav").ok());
        r.a->update(r.scene, kDt);
        (void)rms(*r.a, 4800);
        return rms(*r.a, 9600);
    };
    const float dry = tail({50, 0, 0});
    const float wet = tail({0, 0, 0});
    MESSAGE("tail RMS outside " << dry << ", inside the hall " << wet);
    CHECK(dry < 1e-4f);
    CHECK(wet > 0.005f);
    r.a->stop_all();
    CHECK(r.a->acoustics().zone_weight == 0.0f);
}

TEST_CASE("acoustics: a listener under water hears the world muffled") {
    Rig r("water");
    gameplay::register_components();
    const auto pool = r.scene.create("Pool");
    gameplay::WaterComponent w;
    w.size = {40, 40};
    r.scene.world().add<gameplay::WaterComponent>(pool, w);
    (void)r.source("high.wav", {0, 0, -3}, false);
    auto at = [&](float y) { r.scene.world().get<scene::Transform>(r.listener)->position = {0, y, 0}; };
    at(2.0f);
    const float above = r.settle_and_measure(30);
    CHECK(r.a->acoustics().underwater == 0.0f);
    at(-1.0f);
    const float below = r.settle_and_measure(60);
    const auto st = r.a->acoustics();
    CHECK(st.underwater == 1.0f);
    CHECK(st.lowpass[static_cast<std::size_t>(audio::Bus::Sfx)] == doctest::Approx(600.0f).epsilon(0.02));
    CHECK(st.lowpass[static_cast<std::size_t>(audio::Bus::Ui)] == 0.0f);
    MESSAGE("6 kHz RMS above water " << above << ", below " << below);
    CHECK(below < 0.05f * above);
    at(2.0f);
    CHECK(r.settle_and_measure(60) == doctest::Approx(above).epsilon(0.05));
    CHECK(r.a->acoustics().lowpass[static_cast<std::size_t>(audio::Bus::Sfx)] == 0.0f);
}

TEST_CASE("acoustics: components serialize") {
    audio::register_components();
    const auto* m = scene::find_schema("AcousticMaterial");
    REQUIRE(m != nullptr);
    CHECK(m->category == "Audio");
    audio::AcousticMaterialComponent mat{AcousticPreset::Custom, {0.3f, 0.2f, 0.1f}};
    audio::AcousticMaterialComponent mat_back;
    REQUIRE(m->load(&mat_back, m->save(&mat)).ok());
    CHECK(mat_back == mat);
    const auto* z = scene::find_schema("ReverbZone");
    REQUIRE(z != nullptr);
    audio::ReverbZoneComponent zone;
    zone.size = {3, 4, 5};
    zone.decay = 0.85f;
    zone.lowpass = 900.0f;
    zone.priority = -2;
    zone.enabled = false;
    audio::ReverbZoneComponent zone_back;
    REQUIRE(z->load(&zone_back, z->save(&zone)).ok());
    CHECK(zone_back == zone);
    const auto* s = scene::find_schema("AudioSource");
    audio::AudioSourceComponent src;
    src.occlusion = true;
    src.occlusion_radius = 0.75f;
    audio::AudioSourceComponent src_back;
    REQUIRE(s->load(&src_back, s->save(&src)).ok());
    CHECK(src_back == src);
}
