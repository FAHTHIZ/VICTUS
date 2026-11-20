// SPDX-License-Identifier: Apache-2.0
// Project lighting bakes (victus/player/bake_lighting.hpp, ADR-0134): what counts as static for
// lighting, and 3D templates that ship baked.
#include "doctest.h"
#include "victus/core/fs.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/physics/physics.hpp"
#include "victus/player/bake_lighting.hpp"
#include "victus/project/project.hpp"
#include "victus/render/lightmap.hpp"
#include "victus/scene/scene.hpp"
#include "victus/script/script.hpp"

#include <filesystem>

using namespace victus;

namespace {

struct TempDir {
    fs::Path path;
    explicit TempDir(const char* name) {
        path = std::filesystem::temp_directory_path() / (std::string(name) + "_" + Uuid::generate().to_string().substr(0, 8));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

scene::MeshRendererComponent* renderer(scene::Scene& s, std::string_view name) {
    const scene::Entity e = s.find_by_name(name);
    return e.is_null() ? nullptr : s.world().get<scene::MeshRendererComponent>(e);
}

} // namespace

TEST_CASE("bake lighting: only meshes nothing can move are static for lighting") {
    project::register_components();
    scene::Scene s;
    auto make = [&](std::string_view name) {
        const scene::Entity e = s.create(name);
        s.world().add<scene::MeshRendererComponent>(e).primitive = "cube";
        return e;
    };
    const scene::Entity wall = make("Wall");
    s.world().add<physics::RigidBodyComponent>(wall).type = physics::BodyType::Static;
    const scene::Entity crate = make("Crate");
    s.world().add<physics::RigidBodyComponent>(crate).type = physics::BodyType::Dynamic;
    const scene::Entity door = make("Door");
    s.world().add<physics::RigidBodyComponent>(door).type = physics::BodyType::Kinematic;
    const scene::Entity gem = make("Gem");
    s.world().add<gameplay::RotatorComponent>(gem);
    const scene::Entity bob = make("Bob");
    s.world().add<gameplay::OscillatorComponent>(bob);
    const scene::Entity turret = s.create("Turret");
    s.world().add<script::ScriptComponent>(turret);
    const scene::Entity head = make("Head");
    REQUIRE(s.set_parent(head, turret));
    const scene::Entity hidden = make("Hidden");
    s.world().get<scene::MeshRendererComponent>(hidden)->visible = false;

    CHECK(player::is_static_for_lighting(s, wall));
    CHECK_FALSE(player::is_static_for_lighting(s, crate));
    CHECK_FALSE(player::is_static_for_lighting(s, door));
    CHECK_FALSE(player::is_static_for_lighting(s, gem));
    CHECK_FALSE(player::is_static_for_lighting(s, bob));
    CHECK_FALSE(player::is_static_for_lighting(s, head)); // its scripted parent moves it
    CHECK(player::mark_static_lighting(s) == 1);
    CHECK(renderer(s, "Wall")->lightmapped);
    for (const char* n : {"Crate", "Door", "Gem", "Bob", "Head", "Hidden"}) CHECK_FALSE(renderer(s, n)->lightmapped);
    CHECK(player::mark_static_lighting(s) == 0); // already marked
}

TEST_CASE("bake lighting: 3D templates mark their scenery static and bake it on creation") {
    project::register_components();
    TempDir tmp("victus_template_bake");
    for (const char* id : {"3d", "nocode3d", "multiplayer"}) {
        CAPTURE(id);
        // Every Static Lighting renderer in a template really is static.
        scene::Scene s;
        REQUIRE(project::build_template_scene(id, s).ok());
        std::uint32_t lightmapped = 0;
        std::vector<scene::Entity> marked;
        s.world().each<scene::MeshRendererComponent>([&](ecs::Entity e, scene::MeshRendererComponent& mr) {
            if (mr.lightmapped) marked.push_back(e);
        });
        for (scene::Entity e : marked) {
            CHECK(player::is_static_for_lighting(s, e));
            ++lightmapped;
        }
        CHECK(lightmapped >= 3);

        auto p = project::Project::create(tmp.path / id, "Baked", id);
        REQUIRE(p.ok());
        auto baked = player::bake_template_lighting(*p);
        REQUIRE_MESSAGE(baked.ok(), (baked.ok() ? "" : baked.error().message));
        CHECK(*baked);
        auto bytes = fs::read_binary(p->root() / "scenes" / "main.vlightmap");
        REQUIRE(bytes.ok());
        auto file = render::decode_lightmap_file(*bytes);
        REQUIRE(file.ok());
        CHECK(file->entries.size() == lightmapped);
        CHECK_FALSE(file->probes.empty());
        scene::Scene saved;
        REQUIRE(saved.load_file(p->root() / "scenes" / "main.vscene").ok());
        std::string ref;
        saved.world().each<gameplay::EnvironmentComponent>([&](gameplay::EnvironmentComponent& env) { ref = env.lightmap; });
        CHECK(ref == "scenes/main.vlightmap");
    }
    // Templates without Static Lighting renderers are left alone.
    auto flat = project::Project::create(tmp.path / "flat", "Flat", "2d");
    REQUIRE(flat.ok());
    auto baked = player::bake_template_lighting(*flat);
    REQUIRE(baked.ok());
    CHECK_FALSE(*baked);
    CHECK_FALSE(std::filesystem::exists(flat->root() / "scenes" / "main.vlightmap"));
}
