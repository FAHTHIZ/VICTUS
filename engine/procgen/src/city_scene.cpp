// SPDX-License-Identifier: Apache-2.0
// City component, generation into a scene, and pedestrians (ADR-0202).
#include "victus/procgen/city.hpp"

#include "victus/animation/crowd.hpp"
#include "victus/core/log.hpp"
#include "victus/core/profiler.hpp"
#include "victus/physics/physics.hpp"
#include "victus/scene/reflect.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>

namespace victus::procgen {

using math::Vec3;
using scene::Entity;

namespace {

constexpr const char* kLayerNames[kCityLayerCount] = {"Ground", "Buildings", "Props", "Foliage", "Glass"};

CityWorld& city_world(scene::Scene& scene) {
    auto& w = scene.world();
    if (auto* c = w.resource<CityWorld>()) return *c;
    return w.set_resource<CityWorld>();
}

std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    return x ^ (x >> 33);
}
float hash_unit(std::uint64_t seed, std::uint64_t i) { return static_cast<float>(mix(seed * 0x9E3779B97F4A7C15ull + i) >> 40) / 16777216.0f; }

Entity transient_child(scene::Scene& scene, Entity parent, std::string_view name) {
    const Entity e = scene.create(name, parent);
    scene.world().add<scene::TransientComponent>(e);
    return e;
}

void static_box(scene::Scene& scene, Entity parent, Vec3 center, Vec3 size) {
    const Entity e = transient_child(scene, parent, "Collider");
    scene.world().get<scene::Transform>(e)->position = center;
    scene.world().add<physics::RigidBodyComponent>(e).type = physics::BodyType::Static;
    auto& col = scene.world().add<physics::ColliderComponent>(e);
    col.shape = physics::ColliderComponent::Shape::Box;
    col.size = size;
}

} // namespace

void register_components() {
    static std::once_flag once;
    std::call_once(once, [] {
        using C = CityComponent;
        scene::Schema<C>("City", "World")
            .field(&C::seed, "seed", {.tooltip = "Same seed, same city"})
            .field(&C::columns, "columns", {.min = 1, .max = 64, .tooltip = "Blocks along X"})
            .field(&C::rows, "rows", {.min = 1, .max = 64, .tooltip = "Blocks along Z"})
            .field(&C::block_min, "block_min", {.min = 20, .max = 400, .tooltip = "Shortest spacing between road centre lines, m"})
            .field(&C::block_max, "block_max", {.min = 20, .max = 400, .tooltip = "Longest spacing between road centre lines, m"})
            .field(&C::lanes, "lanes", {.min = 1, .max = 4, .tooltip = "Street lanes each way"})
            .field(&C::avenue_lanes, "avenue_lanes", {.min = 1, .max = 4, .tooltip = "Avenue lanes each way"})
            .field(&C::avenue_every, "avenue_every", {.min = 0, .max = 64, .tooltip = "Every n-th road is an avenue (0 = none)"})
            .field(&C::sidewalk, "sidewalk", {.min = 0, .max = 12, .tooltip = "Sidewalk width, m"})
            .field(&C::lot_min, "lot_min", {.min = 3, .max = 200, .tooltip = "Narrowest lot frontage, m"})
            .field(&C::lot_max, "lot_max", {.min = 3, .max = 200, .tooltip = "Widest lot frontage, m"})
            .field(&C::lot_depth, "lot_depth", {.min = 3, .max = 200, .tooltip = "Deep blocks keep a courtyard behind lots this deep, m"})
            .field(&C::floors_min, "floors_min", {.min = 1, .max = 200})
            .field(&C::floors_max, "floors_max", {.min = 1, .max = 200})
            .field(&C::floor_height, "floor_height", {.min = 2, .max = 10, .tooltip = "m"})
            .field(&C::downtown_radius, "downtown_radius", {.min = 0, .max = 10000, .tooltip = "Height falloff from the centre, m (0 = automatic)"})
            .field(&C::setback_chance, "setback_chance", {.min = 0, .max = 1})
            .field(&C::park_chance, "park_chance", {.min = 0, .max = 1})
            .field(&C::light_spacing, "light_spacing", {.min = 0, .max = 200, .tooltip = "Street lights along sidewalks, m (0 = none)"})
            .field(&C::tree_spacing, "tree_spacing", {.min = 0, .max = 200, .tooltip = "Sidewalk trees, m (0 = none)"})
            .field(&C::cell_size, "cell_size", {.min = 0, .max = 5000, .tooltip = "Mesh cell size for culling, m (0 = one cell)"})
            .field(&C::colliders, "colliders", {.tooltip = "Static colliders for buildings, kerbs and the ground"})
            .field(&C::vehicles, "vehicles", {.min = 0, .max = 20000, .tooltip = "Traffic agents; the first child is the vehicle template"})
            .field(&C::pedestrians, "pedestrians", {.min = 0, .max = 100000, .tooltip = "Crowd agents walking the sidewalks"})
            .field(&C::pedestrian_model, "pedestrian_model", {.tooltip = "Skinned model drawn by each pedestrian", .hint = "model"})
            .field(&C::pedestrian_clip, "pedestrian_clip", {.tooltip = "Walk clip of the pedestrian model"})
            .field(&C::walk_speed, "walk_speed", {.min = 0, .max = 5, .tooltip = "m/s"})
            .field(&C::clip_speed, "clip_speed", {.min = 0.1f, .max = 10, .tooltip = "m/s the walk clip covers at speed 1"})
            .field(&C::built, "built")
            .commit();
        scene::Schema<PedestrianComponent>("Pedestrian", "World")
            .field(&PedestrianComponent::loop, "loop", {.min = 0, .max = 1000000})
            .field(&PedestrianComponent::distance, "distance", {.min = -1e6f, .max = 1e6f})
            .field(&PedestrianComponent::speed, "speed", {.min = -10, .max = 10, .tooltip = "m/s; negative walks the loop the other way"})
            .field(&PedestrianComponent::offset, "offset", {.min = -10, .max = 10, .tooltip = "m to the side of the path"})
            .field(&PedestrianComponent::enabled, "enabled")
            .commit();
    });
    animation::register_components();
    physics::register_components();
    traffic::register_components();
}

CitySettings CityComponent::settings() const {
    CitySettings s;
    s.seed = seed;
    s.columns = columns;
    s.rows = rows;
    s.block_min = block_min;
    s.block_max = block_max;
    s.lanes = lanes;
    s.avenue_lanes = avenue_lanes;
    s.avenue_every = avenue_every;
    s.sidewalk = sidewalk;
    s.lot_min = lot_min;
    s.lot_max = lot_max;
    s.lot_depth = lot_depth;
    s.floor_height = floor_height;
    s.floors_min = floors_min;
    s.floors_max = floors_max;
    s.downtown_radius = downtown_radius;
    s.setback_chance = setback_chance;
    s.park_chance = park_chance;
    s.light_spacing = light_spacing;
    s.tree_spacing = tree_spacing;
    return s;
}

bool build_city(scene::Scene& scene) {
    VICTUS_PROFILE_SCOPE("procgen.build_city");
    auto& w = scene.world();
    Entity owner{};
    CityComponent* cc = nullptr;
    w.each<CityComponent>([&](Entity e, CityComponent& c) {
        if (!cc && !c.built) {
            owner = e;
            cc = &c;
        }
    });
    if (!cc) return true;
    cc->built = true;
    const CityComponent conf = *cc; // the ECS may move components while entities are created
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    auto generated = generate_city(conf.settings());
    if (!generated) {
        VICTUS_LOG_WARN("procgen", "city: {}", generated.error().message);
        return false;
    }
    auto city = std::make_shared<const City>(std::move(generated).value());
    const auto t1 = clock::now();
    CityStats st;
    st.blocks = static_cast<std::uint32_t>(city->blocks.size());
    st.buildings = static_cast<std::uint32_t>(city->buildings.size());
    st.props = static_cast<std::uint32_t>(city->props.size());
    st.generate_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // The vehicle template is the owner's first child, picked before anything is generated.
    const std::vector<Entity> kids = scene.children(owner);
    const Entity templ = kids.empty() ? Entity{} : kids.front();

    const Entity root = transient_child(scene, owner, "Generated City");
    for (CityMesh& m : build_city_meshes(*city, conf.cell_size)) {
        st.triangles += static_cast<std::uint32_t>(m.mesh.indices.size() / 3);
        auto mesh = scene::publish_mesh(std::move(m.mesh));
        if (!mesh) {
            VICTUS_LOG_WARN("procgen", "city mesh: {}", mesh.error().message);
            continue;
        }
        const auto layer = static_cast<std::uint32_t>(m.layer);
        const Entity e = transient_child(scene, root, std::format("{} {},{}", kLayerNames[layer], m.cell.x, m.cell.y));
        auto& mr = w.add<scene::MeshRendererComponent>(e);
        switch (m.layer) {
        case CityLayer::Buildings: mr.roughness = 0.7f; break;
        case CityLayer::Props: mr.roughness = 0.45f; break;
        case CityLayer::Glass: mr.roughness = 0.14f; break; // reflects the sky and the street
        default: mr.roughness = 0.9f; break;
        }
        mr.cast_shadows = m.layer != CityLayer::Ground;
        w.add<scene::ProceduralMeshComponent>(e).mesh = std::move(*mesh);
        ++st.meshes;
    }
    if (conf.colliders) {
        const Entity cols = transient_child(scene, root, "Colliders");
        const math::Aabb& b = city->bounds;
        static_box(scene, cols, {(b.min.x + b.max.x) * 0.5f, -0.5f, (b.min.z + b.max.z) * 0.5f}, {b.max.x - b.min.x, 1.0f, b.max.z - b.min.z});
        ++st.colliders;
        const float kerb = city->settings.kerb_height;
        for (const Block& bl : city->blocks) {
            if (kerb > 0.0f) {
                const auto c = bl.kerb.center();
                const auto sz = bl.kerb.size();
                static_box(scene, cols, {c.x, kerb * 0.5f, c.y}, {sz.x, kerb, sz.y});
                ++st.colliders;
            }
        }
        for (const Building& bd : city->buildings) {
            float y0 = kerb;
            for (const Tier& t : bd.tiers) {
                const float y1 = kerb + t.top;
                const auto c = t.footprint.center();
                const auto sz = t.footprint.size();
                static_box(scene, cols, {c.x, (y0 + y1) * 0.5f, c.y}, {sz.x, y1 - y0, sz.y});
                ++st.colliders;
                y0 = y1;
            }
        }
    }
    if (conf.vehicles > 0) {
        traffic::LaneGraphSettings ls;
        traffic::set_traffic_graph(scene, std::make_shared<traffic::LaneGraph>(traffic::build_lane_graph(city->roads, ls)));
        const std::vector<Entity> cars = traffic::spawn_traffic(scene, conf.vehicles, conf.seed, owner);
        for (const Entity car : cars) {
            w.add<scene::TransientComponent>(car);
            if (templ != Entity{}) scene.set_parent(scene.duplicate(templ), car);
        }
        if (templ != Entity{}) {
            std::vector<Entity> stack{templ};
            while (!stack.empty()) {
                const Entity e = stack.back();
                stack.pop_back();
                if (auto* mr = w.get<scene::MeshRendererComponent>(e)) mr->visible = false;
                for (const Entity k : scene.children(e)) stack.push_back(k);
            }
        }
        st.vehicles = static_cast<std::uint32_t>(cars.size());
    }
    if (conf.pedestrians > 0 && !conf.pedestrian_model.is_nil() && !city->walks.empty()) {
        float total = 0;
        for (const WalkLoop& l : city->walks) total += l.length;
        const Entity peds = transient_child(scene, root, "Pedestrians");
        const float clip_speed = std::max(conf.clip_speed, 0.1f);
        std::size_t loop = 0;
        float acc = city->walks[0].length;
        for (std::uint32_t i = 0; i < conf.pedestrians; ++i) {
            // Spread evenly over the summed loop length, jittered, so every block gets walkers.
            const float s = (static_cast<float>(i) + hash_unit(conf.seed, i * 4)) * total / static_cast<float>(conf.pedestrians);
            while (s > acc && loop + 1 < city->walks.size()) acc += city->walks[++loop].length;
            const WalkLoop& wl = city->walks[loop];
            PedestrianComponent pc;
            pc.loop = static_cast<std::uint32_t>(loop);
            pc.distance = wl.length - (acc - s);
            const float speed = conf.walk_speed * (0.8f + 0.4f * hash_unit(conf.seed, i * 4 + 1));
            pc.speed = hash_unit(conf.seed, i * 4 + 2) < 0.5f ? speed : -speed;
            pc.offset = (hash_unit(conf.seed, i * 4 + 3) - 0.5f) * city->settings.sidewalk * 0.6f;
            const Entity e = scene.create("Pedestrian", peds);
            animation::CrowdAgentComponent ca;
            ca.model = conf.pedestrian_model;
            ca.clip = conf.pedestrian_clip;
            ca.time = 10.0f * hash_unit(conf.seed, i * 4 + 5);
            ca.speed = speed / clip_speed;
            const float tint = 0.75f + 0.25f * hash_unit(conf.seed, i * 4 + 6);
            ca.tint = {tint, tint * (0.9f + 0.1f * hash_unit(conf.seed, i * 4 + 7)), tint, 1.0f};
            w.add<animation::CrowdAgentComponent>(e, ca);
            w.add<PedestrianComponent>(e, pc);
            ++st.pedestrians;
        }
    }
    st.mesh_ms = std::chrono::duration<double, std::milli>(clock::now() - t1).count();
    CityWorld& cw = city_world(scene);
    cw.city = std::move(city);
    cw.owner = owner;
    cw.stats = st;
    update_pedestrians(scene, 0.0f); // place them before the first frame
    VICTUS_LOG_INFO("procgen", "city: {} blocks, {} buildings, {} props, {} meshes ({} triangles), {} colliders, {} vehicles, {} pedestrians in {:.1f} + {:.1f} ms",
                    st.blocks, st.buildings, st.props, st.meshes, st.triangles, st.colliders, st.vehicles, st.pedestrians, st.generate_ms,
                    st.mesh_ms);
    return true;
}

const City* scene_city(const scene::Scene& scene) {
    const auto* c = const_cast<scene::Scene&>(scene).world().resource<CityWorld>();
    return c ? c->city.get() : nullptr;
}

const CityStats* city_stats(const scene::Scene& scene) {
    const auto* c = const_cast<scene::Scene&>(scene).world().resource<CityWorld>();
    return c && c->city ? &c->stats : nullptr;
}

void update_pedestrians(scene::Scene& scene, float dt) {
    const City* city = scene_city(scene);
    if (!city || city->walks.empty()) return;
    scene.world().each<PedestrianComponent, scene::Transform>([&](Entity, PedestrianComponent& p, scene::Transform& t) {
        if (!p.enabled || p.loop >= city->walks.size()) return;
        const WalkLoop& wl = city->walks[p.loop];
        p.distance += p.speed * dt;
        if (wl.length > 0.0f) {
            p.distance = std::fmod(p.distance, wl.length);
            if (p.distance < 0.0f) p.distance += wl.length;
        }
        Vec3 tan;
        const Vec3 at = wl.point_at(p.distance, &tan);
        const Vec3 fwd = p.speed < 0.0f ? tan * -1.0f : tan;
        const Vec3 side{tan.z, 0.0f, -tan.x}; // to the right of the loop direction; fixed for both walking directions
        t.position = at + side * p.offset;
        t.rotation = math::Quat::axis_angle({0, 1, 0}, std::atan2(fwd.x, fwd.z));
    });
}

void add_procgen_systems(gameplay::Simulation& sim) {
    register_components();
    sim.add_system("procgen.update", gameplay::Phase::Update, [](scene::Scene& s, const gameplay::FrameTime& t) {
        if (s.world().count<CityComponent>() > 0) (void)build_city(s);
        if (gameplay::game_paused(s)) return;
        update_pedestrians(s, t.dt);
    });
}

} // namespace victus::procgen
