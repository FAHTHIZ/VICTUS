// SPDX-License-Identifier: Apache-2.0
// Morph target weights (ADR-0076): Path::Weights channels expanded into weight nodes, written by
// the Animator into MorphWeightsComponent, blended by cross-fades, and set from code by name/index.
#include "victus/animation/animation.hpp"
#include "victus/scene/scene.hpp"

#include <doctest.h>

#include <cmath>

using namespace victus;
using namespace victus::animation;
using scene::Entity;
using scene::Scene;

namespace {

bool near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps; }

Channel weights(std::uint32_t node, std::vector<float> times, std::vector<float> values, std::uint32_t width,
                Interpolation interp = Interpolation::Linear) {
    Channel ch{.node = node, .path = Path::Weights, .interpolation = interp, .times = std::move(times), .values = std::move(values)};
    ch.width = width;
    return ch;
}

// "Root" -> "Face" (mesh 0: targets Smile, Blink; defaults 0.25, 0).
// Talk: Smile 0 -> 1, Blink 0 -> 0.5 over 1 s. Wink: Smile 0, Blink 1. Nod: Root moves; no weights.
std::shared_ptr<AnimationSet> make_set() {
    auto set = std::make_shared<AnimationSet>();
    set->nodes.push_back({.name = "Root", .parent = -1, .rest = {}});
    set->nodes.push_back({.name = "Face", .parent = 0, .rest = {}});
    set->mesh_morphs.push_back({.names = {"Smile", "Blink"}, .defaults = {0.25f, 0.0f}});
    set->clips.push_back({.name = "Talk", .duration = 1, .channels = {weights(1, {0, 1}, {0, 0, 1, 0.5f}, 2)}});
    set->clips.push_back({.name = "Wink", .duration = 1, .channels = {weights(1, {0, 1}, {0, 1, 0, 1}, 2)}});
    Channel nod{.node = 0, .path = Path::Translation, .interpolation = Interpolation::Linear, .times = {0, 1}, .values = {0, 0, 0, 1, 0, 0}};
    set->clips.push_back({.name = "Nod", .duration = 1, .channels = {nod}});
    const MorphNode face{.node = 1, .names = set->mesh_morphs[0].names, .defaults = set->mesh_morphs[0].defaults};
    expand_morph_weights(*set, std::span<const MorphNode>(&face, 1));
    return set;
}

struct Rig {
    Scene scene;
    Entity character, root, face;
    Uuid model = Uuid::generate();
    AnimationSystem system;
    Rig() {
        system.add_set(model, make_set());
        character = scene.create("Character");
        root = scene.create("Root", character);
        face = scene.create("Face", root);
        auto& mr = scene.world().add<scene::MeshRendererComponent>(face);
        mr.mesh_asset = model;
        mr.mesh_index = 0;
        auto& a = scene.world().add<AnimatorComponent>(character);
        a.model = model;
        a.clip = "Talk";
    }
    [[nodiscard]] std::vector<float> weights_of(Entity e) {
        const auto* mw = scene.world().get<MorphWeightsComponent>(e);
        return mw ? mw->weights : std::vector<float>{};
    }
};

} // namespace

TEST_CASE("expand_morph_weights turns weight channels into per-target weight nodes") {
    const auto set = make_set();
    REQUIRE(set->nodes.size() == 4);
    CHECK(set->nodes[2].name == "Face/Smile");
    CHECK(set->nodes[3].name == "Face/Blink");
    CHECK(set->nodes[2].parent == 1); // masks covering Face cover its weights
    CHECK(set->nodes[3].morph_node == 1);
    CHECK(set->nodes[3].morph_index == 1);
    CHECK(near(set->nodes[2].rest.translation.x, 0.25f));
    const Clip& talk = set->clips[0];
    REQUIRE(talk.channels.size() == 2);
    CHECK(talk.channels[0].path == Path::Translation);
    CHECK(talk.channels[0].node == 2);
    CHECK(talk.channels[1].node == 3);
    for (const Clip& c : set->clips) {
        const bool ok = static_cast<bool>(validate(c, set->nodes.size()));
        CHECK(ok);
    }
    // Sampling a weight node yields the weight in translation.x.
    std::vector<NodePose> pose(set->nodes.size());
    sample(talk, 0.5f, pose);
    CHECK(near(pose[2].translation.x, 0.5f));
    CHECK(near(pose[3].translation.x, 0.25f));

    // Cubic-spline weight channels keep their in-tangent/value/out-tangent layout per target.
    AnimationSet cubic;
    cubic.nodes.push_back({.name = "Face", .parent = -1, .rest = {}});
    // keys at 0 and 1: [in(2), value(2), out(2)] per key; values Smile 0 -> 1, Blink 1 -> 0, flat tangents.
    cubic.clips.push_back({.name = "C", .duration = 1, .channels = {weights(0, {0, 1}, {0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0}, 2,
                                                                             Interpolation::CubicSpline)}});
    const MorphNode mn{.node = 0, .names = {"Smile", "Blink"}, .defaults = {}};
    expand_morph_weights(cubic, std::span<const MorphNode>(&mn, 1));
    REQUIRE(cubic.clips[0].channels.size() == 2);
    std::vector<NodePose> cp(cubic.nodes.size());
    sample(cubic.clips[0], 1.0f, cp);
    CHECK(near(cp[1].translation.x, 1.0f));
    CHECK(near(cp[2].translation.x, 0.0f));
    sample(cubic.clips[0], 0.5f, cp);
    CHECK(near(cp[1].translation.x, 0.5f));
    CHECK(near(cp[2].translation.x, 0.5f));
}

TEST_CASE("weight channels are validated and dropped when they do not match the node's targets") {
    Clip bad{.name = "Bad", .duration = 1, .channels = {weights(0, {0, 1}, {0, 1}, 0)}};
    CHECK_FALSE(validate(bad, 1));
    Clip short_values{.name = "Short", .duration = 1, .channels = {weights(0, {0, 1}, {0, 1, 2}, 2)}};
    CHECK_FALSE(validate(short_values, 1));

    AnimationSet set;
    set.nodes.push_back({.name = "Plain", .parent = -1, .rest = {}});
    set.nodes.push_back({.name = "Face", .parent = -1, .rest = {}});
    set.clips.push_back({.name = "A", .duration = 1,
                         .channels = {weights(0, {0}, {1}, 1), weights(1, {0}, {1, 1, 1}, 3)}}); // no targets / too wide
    const MorphNode mn{.node = 1, .names = {"Smile"}, .defaults = {}};
    expand_morph_weights(set, std::span<const MorphNode>(&mn, 1));
    CHECK(set.nodes.size() == 3);
    CHECK(set.clips[0].channels.empty());
}

TEST_CASE("the animator writes, cross-fades and reports morph weights") {
    Rig rig;
    rig.system.update(rig.scene, 0.5f);
    auto w = rig.weights_of(rig.face);
    REQUIRE(w.size() == 2);
    CHECK(near(w[0], 0.5f));
    CHECK(near(w[1], 0.25f));
    const AnimatorStatus st = rig.system.status(rig.scene, rig.character);
    CHECK(st.valid);
    CHECK(st.bound_nodes == 4); // weight nodes count as bound
    CHECK(st.animated_nodes == 2);

    // Cross-fade Talk (t = 0.75: 0.75, 0.375) halfway into Wink (0, 1).
    REQUIRE(rig.system.play(rig.scene, rig.character, "Wink", 0.5f));
    rig.system.update(rig.scene, 0.25f);
    w = rig.weights_of(rig.face);
    CHECK(near(w[0], 0.375f));
    CHECK(near(w[1], 0.6875f));
    rig.system.update(rig.scene, 0.5f);
    w = rig.weights_of(rig.face);
    CHECK(near(w[0], 0.0f));
    CHECK(near(w[1], 1.0f));

    // A clip without weight channels fades back to the rest (default) weights.
    REQUIRE(rig.system.play(rig.scene, rig.character, "Nod", 0.0f));
    rig.system.update(rig.scene, 0.1f);
    w = rig.weights_of(rig.face);
    CHECK(w.size() == 2); // untouched: not animated by Nod
}

TEST_CASE("morph weights can be set and read by name or index") {
    Rig rig;
    const auto names = rig.system.morph_targets(rig.scene, rig.face);
    REQUIRE(names.size() == 2);
    CHECK(names[1] == "Blink");
    CHECK(rig.system.morph_targets(rig.scene, rig.root).size() == 2); // child MeshRenderer fallback
    CHECK(rig.system.morph_targets(rig.scene, rig.character).empty());

    CHECK(near(*rig.system.morph_weight(rig.scene, rig.face, "Smile"), 0.25f)); // mesh default before any write
    CHECK(rig.system.set_morph_weight(rig.scene, rig.face, "Blink", 0.7f));
    CHECK(near(*rig.system.morph_weight(rig.scene, rig.face, std::size_t{1}), 0.7f));
    CHECK(near(*rig.system.morph_weight(rig.scene, rig.face, std::size_t{0}), 0.25f)); // component seeded with defaults
    CHECK(rig.system.set_morph_weight(rig.scene, rig.face, std::size_t{0}, -0.5f)); // not clamped (glTF)
    CHECK(near(*rig.system.morph_weight(rig.scene, rig.face, "Smile"), -0.5f));
    CHECK_FALSE(rig.system.set_morph_weight(rig.scene, rig.face, "Frown", 1.0f));
    CHECK_FALSE(rig.system.set_morph_weight(rig.scene, rig.face, std::size_t{2}, 1.0f));
    CHECK_FALSE(rig.system.set_morph_weight(rig.scene, rig.face, "Smile", std::nanf("")));
    CHECK_FALSE(rig.system.morph_weight(rig.scene, rig.face, "Frown").has_value());
    CHECK_FALSE(rig.system.set_morph_weight(rig.scene, rig.character, "Smile", 1.0f));
}
