// SPDX-License-Identifier: Apache-2.0
// Facial expressions and equipment on Orion and Nova. The 26 face-display blend shapes must move
// only the visor's glyphs, stay finite and mirror left to right; the starter controller's Face
// layer must play expressions over locomotion without touching the body or popping; and props
// attached to the eight sockets must stay on the deforming surface through every clip, also when
// skeleton LOD has eased the detail joints out.
#include "doctest.h"

#include "victus/animation/animation.hpp"
#include "victus/assets/cooked.hpp"
#include "victus/assets/importers.hpp"
#include "victus/assets/skeleton.hpp"
#include "victus/project/characters.hpp"
#include "victus/render/skinning.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <string>

using namespace victus;
using namespace victus::assets;
using math::Quat;
using math::Vec3;

namespace {

const CookedModel& model_of(const char* id) {
    static std::map<std::string, CookedModel> cache;
    auto it = cache.find(id);
    if (it == cache.end()) {
        const project::StarterCharacter* c = project::find_starter_character(id);
        REQUIRE(c);
        ModelImportSettings s;
        s.import_textures = false;
        s.lods = 0;
        auto m = import_gltf_memory(project::starter_character_glb(*c), c->model_path, s);
        REQUIRE(m.ok());
        it = cache.emplace(id, std::move(m).value()).first;
    }
    return it->second;
}

int node_index(const CookedModel& m, std::string_view name) {
    for (std::size_t i = 0; i < m.nodes.size(); ++i)
        if (m.nodes[i].name == name) return static_cast<int>(i);
    return -1;
}

Vec3 position_of(const math::Mat4& m) { return m.transform_point({0, 0, 0}); }

std::string mirrored(std::string name) {
    if (name.ends_with("_L")) name.back() = 'R';
    else if (name.ends_with("_R")) name.back() = 'L';
    return name;
}

struct Rig {
    scene::Scene scene;
    scene::Entity root;
    std::unique_ptr<animation::AnimationSystem> sys;
    std::string controller = project::starter_character_controller();
};

void build(Rig& r, const char* id) {
    const auto* c = project::find_starter_character(id);
    Uuid uuid;
    REQUIRE(Uuid::parse(c->uuid, uuid));
    auto e = project::instantiate_starter_character(r.scene, *c, "Robot", scene::kNullEntity);
    REQUIRE(e.ok());
    r.root = *e;
    animation::AnimationSettings settings;
    settings.controllers = [&r](std::string_view) -> Result<std::string> { return r.controller; };
    r.sys = std::make_unique<animation::AnimationSystem>(settings);
    r.sys->add_set(uuid, std::make_shared<animation::AnimationSet>(make_animation_set(model_of(id))));
    r.sys->update(r.scene, 0.0f);
    REQUIRE(r.sys->set_parameter(r.scene, r.root, "speed", 3.5f));
}

// The entity carrying the face-display morph weights (the first with morph targets).
scene::Entity face_entity(Rig& r) {
    scene::Entity found{};
    std::vector<scene::Entity> stack{r.root};
    while (!stack.empty() && found.is_null()) {
        const scene::Entity e = stack.back();
        stack.pop_back();
        if (!r.sys->morph_targets(r.scene, e).empty()) found = e;
        for (const auto ch : r.scene.children(e))
            stack.push_back(ch);
    }
    return found;
}

std::vector<float> weights_of(Rig& r, scene::Entity e) {
    const auto* mw = r.scene.world().get<animation::MorphWeightsComponent>(e);
    return mw ? mw->weights : std::vector<float>{};
}

} // namespace

TEST_CASE("character expressions: the 26 face shapes move the visor glyphs and the bare face, stay finite and mirror") {
    for (const char* id : {"orion", "nova"}) {
        INFO(id);
        const CookedModel& m = model_of(id);
        REQUIRE(!m.meshes.empty());
        const std::size_t targets = m.meshes[0].data.morphs.size();
        REQUIRE(targets == 26);
        std::size_t moving_meshes = 0, total_vertices = 0;
        // moved vertices, max offset: on the visor (exact), and on the bare head (ADR-0247: the
        // MakeHuman face, eyes, brows and lashes; counted above 0.1 mm, below which the sculpted
        // pose units have small asymmetric tails)
        std::map<std::string, std::pair<std::size_t, float>> per_target, per_head;
        std::string moving;
        for (std::size_t mi = 0; mi < m.meshes.size(); ++mi) {
            const auto& mesh = m.meshes[mi];
            const bool head = project::character_mesh_is_head(static_cast<std::uint32_t>(mi));
            total_vertices += mesh.data.vertices.size();
            bool moves = false;
            CHECK(render::validate_morphs(mesh.data).empty());
            for (const auto& t : mesh.data.morphs) {
                auto& [count, offset] = (head ? per_head : per_target)[t.name];
                for (const Vec3& d : t.positions) {
                    REQUIRE(std::isfinite(d.x));
                    REQUIRE(std::isfinite(d.y));
                    REQUIRE(std::isfinite(d.z));
                    const float len = math::length(d);
                    if (len > (head ? 1e-4f : 1e-6f)) {
                        ++count;
                        moves = true;
                    }
                    offset = std::max(offset, len);
                }
            }
            if (moves) {
                ++moving_meshes;
                moving += mesh.name + " ";
            }
        }
        MESSAGE(std::string(id) << ": " << m.meshes.size() << " primitives, " << total_vertices << " vertices; primitives the face shapes move: "
                   << moving_meshes << " (" << moving << ")");
        // The Visor (helmet set) and Skin, Eye and Hair (bare-head set); never the body or armour.
        CHECK(moving_meshes == 4);
        CHECK(moving.find("#0 ") == std::string::npos);
        CHECK(moving.find("#1 ") == std::string::npos);
        CHECK(moving.find("#3 ") == std::string::npos);
        for (const auto& [name, v] : per_head) {
            INFO("bare head " << name);
            CHECK(v.second < 0.03f); // face-sized offsets
            const std::string other = mirrored(name);
            if (other != name && per_head.contains(other)) {
                const double a = double(v.first), b = double(per_head[other].first);
                // MakeHuman's frown pose units are lopsided at the source (about 28 % more
                // vertices on one side); the largest offsets mirror to 10 %.
                CHECK(std::abs(a - b) <= 0.30 * std::max(a, b) + 8.0);
                CHECK(per_head[other].second == doctest::Approx(v.second).epsilon(0.10));
            }
        }
        for (const auto& [name, v] : per_target) {
            INFO(name);
            CHECK(v.first > 0);         // every shape does something
            CHECK(v.second < 0.03f);    // and stays on the visor (offsets of glyphs, not of the head)
            const std::string other = mirrored(name);
            if (other != name) {
                REQUIRE(per_target.contains(other));
                CHECK(per_target[other].first == v.first);
                CHECK(per_target[other].second == doctest::Approx(v.second).epsilon(0.02));
            }
        }
        MESSAGE(std::string(id) << ": jawOpen moves " << per_target["jawOpen"].first << " vertices up to " << per_target["jawOpen"].second * 1000
                   << " mm; eyeBlink_L " << per_target["eyeBlink_L"].first << " up to " << per_target["eyeBlink_L"].second * 1000 << " mm");

        // Every shape at once at full weight: the visor surface stays inside its own bounds grown
        // by the largest single offset (no shape stacks into a spike).
        const render::MeshData* visor = nullptr;
        for (const auto& mesh : m.meshes)
            for (const auto& t : mesh.data.morphs)
                if (!t.vertices.empty() && !visor) visor = &mesh.data;
        REQUIRE(visor);
        std::vector<float> all(targets, 1.0f);
        std::vector<render::Vertex3D> out;
        REQUIRE(render::apply_morphs(*visor, all, out));
        const math::Aabb before = visor->bounds();
        math::Aabb after;
        for (const auto& v : out) {
            REQUIRE(std::isfinite(v.position.x));
            after.expand(v.position);
            CHECK(std::abs(math::length(v.normal) - 1.0f) < 1e-3f);
        }
        const float grow = std::max({before.min.x - after.min.x, before.min.y - after.min.y, before.min.z - after.min.z,
                                     after.max.x - before.max.x, after.max.y - before.max.y, after.max.z - before.max.z});
        MESSAGE(std::string(id) << ": all 26 shapes at weight 1 grow the visor bounds by " << grow * 1000 << " mm");
        CHECK(grow < 0.03f);
    }
}

TEST_CASE("character expressions: the Face layer plays expressions over locomotion without touching the body or popping") {
    for (const char* id : {"orion", "nova"}) {
        INFO(id);
        Rig face, plain;
        build(face, id);
        build(plain, id);
        const scene::Entity fe = face_entity(face);
        REQUIRE_FALSE(fe.is_null());
        const auto names = face.sys->morph_targets(face.scene, fe);
        REQUIRE(names.size() == 26);
        auto index = [&](std::string_view n) {
            return static_cast<std::size_t>(std::find(names.begin(), names.end(), n) - names.begin());
        };
        constexpr float kDt = 1.0f / 60;
        for (int f = 0; f < 30; ++f) {
            face.sys->update(face.scene, kDt);
            plain.sys->update(plain.scene, kDt);
        }
        // Expressions in the controller's order: 1 Talk, 2 Smile, 3 Frown, 4 Surprise, 0 back to neutral.
        struct Expect {
            int expression;
            const char* shape;
            float at_least;
        };
        const Expect expects[] = {{2, "mouthSmile_L", 0.5f}, {3, "mouthFrown_R", 0.5f}, {4, "eyeWide_L", 0.5f}, {1, "jawOpen", 0.05f},
                                  {0, "mouthSmile_L", -1.0f}};
        float max_step = 0;
        std::vector<float> prev = weights_of(face, fe);
        for (const Expect& x : expects) {
            REQUIRE(face.sys->set_parameter(face.scene, face.root, "expression", static_cast<float>(x.expression)));
            float peak = 0;
            for (int f = 0; f < 60; ++f) {
                face.sys->update(face.scene, kDt);
                plain.sys->update(plain.scene, kDt);
                const auto w = weights_of(face, fe);
                REQUIRE(w.size() == names.size());
                for (std::size_t i = 0; i < w.size(); ++i) {
                    REQUIRE(std::isfinite(w[i]));
                    if (i < prev.size()) max_step = std::max(max_step, std::abs(w[i] - prev[i]));
                }
                peak = std::max(peak, w[index(x.shape)]);
                prev = w;
            }
            INFO(x.shape);
            const auto status = face.sys->controller_status(face.scene, face.root);
            if (x.at_least > 0) CHECK(peak >= x.at_least);
            else CHECK(prev[index(x.shape)] < 0.05f); // neutral: the smile has faded out
        }
        MESSAGE(std::string(id) << ": largest per-frame face weight change while switching expressions " << max_step);
        CHECK(max_step < 0.2f); // cross-faded, not cut

        // The body is the plain locomotion: same joint rotations as the animator without expressions.
        float body = 0;
        for (const char* joint : {"Hips", "Spine", "Head", "UpperArm.L", "LowerArm.R", "Hand.R", "UpperLeg.L", "Foot.R"}) {
            const scene::Entity a = face.scene.find_descendant(face.root, joint), b = plain.scene.find_descendant(plain.root, joint);
            REQUIRE_FALSE(a.is_null());
            const Quat qa = face.scene.world().get<scene::Transform>(a)->rotation, qb = plain.scene.world().get<scene::Transform>(b)->rotation;
            const Quat d = qa.conjugate() * qb;
            body = std::max(body, 2.0f * std::atan2(std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z), std::abs(d.w)));
        }
        MESSAGE(std::string(id) << ": body joint difference with expressions " << body * 57.2958f << " deg");
        CHECK(body < 1e-4f);
    }
}

TEST_CASE("character equipment: props on the eight sockets stay on the deforming surface through every clip") {
    for (const char* id : {"orion", "nova"}) {
        INFO(id);
        const CookedModel& m = model_of(id);
        REQUIRE(m.skins.size() == 1);
        const ModelSkin& skin = m.skins[0];
        const char* sockets[] = {"Socket.Head", "Socket.Face", "Socket.Back", "Socket.Hand.L", "Socket.Hand.R", "Socket.Hip.L", "Socket.Hip.R"};
        // For each socket: skinned vertices weighted at least 0.9 to the socket's parent joint
        // (its carrier). Measure the socket's distance to the nearest of them at bind and through
        // every clip; a prop gripped or worn there keeps that distance only if the socket follows
        // the skinned surface.
        std::vector<Vec3> bind_positions;
        std::vector<std::pair<std::size_t, std::size_t>> refs; // mesh, vertex
        const auto rest = pose_world(m, nullptr, 0.0f);
        for (const char* sname : sockets) {
            INFO(sname);
            const int sock = node_index(m, sname);
            REQUIRE(sock >= 0);
            const int carrier = m.nodes[sock].parent;
            REQUIRE(carrier >= 0);
            const auto jit = std::find(skin.joints.begin(), skin.joints.end(), static_cast<std::uint32_t>(carrier));
            REQUIRE(jit != skin.joints.end());
            const auto j = static_cast<std::uint16_t>(jit - skin.joints.begin());
            // The nearest carried vertex at rest.
            float best = 1e9f;
            std::pair<std::size_t, std::size_t> ref{0, 0};
            const Vec3 s0 = position_of(rest[sock]);
            for (std::size_t mi = 0; mi < m.meshes.size(); ++mi) {
                const auto& d = m.meshes[mi].data;
                for (std::size_t v = 0; v < d.skin.size(); ++v) {
                    float w = 0;
                    for (int k = 0; k < 4; ++k)
                        if (d.skin[v].joints[k] == j) w += d.skin[v].weights[k];
                    if (w < 0.9f) continue;
                    const float dist = math::length(d.vertices[v].position - s0);
                    if (dist < best) {
                        best = dist;
                        ref = {mi, v};
                    }
                }
            }
            REQUIRE(best < 0.25f);
            refs.push_back(ref);
        }
        float worst = 0;
        std::string worst_at;
        std::size_t samples = 0;
        std::vector<render::Vertex3D> out;
        for (const auto& clip : m.animations) {
            for (int k = 0; k <= 8; ++k) {
                const float t = clip.duration * static_cast<float>(k) / 8.0f;
                const auto world = pose_world(m, &clip, t);
                const auto palette = pose_palette(m, world);
                ++samples;
                for (std::size_t s = 0; s < refs.size(); ++s) {
                    const auto& d = m.meshes[refs[s].first].data;
                    const math::Mat4 sm = render::blend_skin_matrix(d.skin[refs[s].second], palette, render::SkinningMethod::Linear);
                    const Vec3 v = sm.transform_point(d.vertices[refs[s].second].position);
                    const float drift = std::abs(math::length(v - position_of(world[node_index(m, sockets[s])])) -
                                                 math::length(d.vertices[refs[s].second].position - position_of(rest[node_index(m, sockets[s])])));
                    if (drift > worst) {
                        worst = drift;
                        worst_at = std::string(sockets[s]) + " in " + clip.name;
                    }
                }
            }
        }
        MESSAGE(std::string(id) << ": " << samples << " poses over " << m.animations.size() << " clips; largest change of a socket's distance to its carrying surface "
                   << worst * 1000 << " mm (" << worst_at << ")");
        CHECK(worst < 0.005f);
    }
}

TEST_CASE("character equipment: an attached prop follows its socket every frame, with and without skeleton LOD") {
    Rig full, reduced;
    build(full, "orion");
    build(reduced, "orion");
    reduced.scene.world().get<animation::AnimatorComponent>(reduced.root)->detail_lod_tier = 0;
    auto attach = [](Rig& r, const char* socket) {
        const scene::Entity prop = r.scene.create("Prop");
        r.scene.world().get<scene::Transform>(prop)->position = {0.3f, 0.2f, 0.1f};
        const scene::Entity s = r.scene.attach_to_socket(prop, r.root, socket);
        REQUIRE_FALSE(s.is_null());
        return std::pair{prop, s};
    };
    const auto [pf, sf] = attach(full, "Socket.Hand.R");
    const auto [pr, sr] = attach(reduced, "Socket.Hand.R");
    const auto [bf, bsf] = attach(full, "Socket.Back");
    float snap = 0, lod = 0, travel = 0;
    Vec3 first{};
    constexpr float kDt = 1.0f / 60;
    for (int f = 0; f < 120; ++f) {
        full.sys->update(full.scene, kDt);
        reduced.sys->update(reduced.scene, kDt);
        full.scene.update_transforms();
        reduced.scene.update_transforms();
        const Vec3 p = position_of(full.scene.world_matrix(pf)), s = position_of(full.scene.world_matrix(sf));
        snap = std::max({snap, math::length(p - s), math::length(position_of(full.scene.world_matrix(bf)) - position_of(full.scene.world_matrix(bsf)))});
        lod = std::max(lod, math::length(position_of(reduced.scene.world_matrix(pr)) - p));
        if (f == 0) first = p;
        travel = std::max(travel, math::length(p - first));
    }
    MESSAGE("Orion running 2 s with a prop in the right hand: prop-to-socket offset " << snap * 1000 << " mm, hand travel " << travel * 100
                                                                                     << " cm, full vs skeleton-LOD prop difference " << lod * 1000 << " mm");
    CHECK(snap < 1e-4f);
    CHECK(travel > 0.2f); // the hand really swings
    CHECK(lod < 1e-4f);   // skeleton LOD never moves sockets
}
