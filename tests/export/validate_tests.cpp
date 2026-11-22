// SPDX-License-Identifier: Apache-2.0
// Project validation library (victus/player/validate.hpp), the code behind `victus check` and the
// editor's Problems panel: structured diagnostics with severity, category, path, entity and fix.
#include "doctest.h"
#include "victus/core/json.hpp"
#include "victus/player/validate.hpp"
#include "victus/physics/physics.hpp"
#include "victus/project/project.hpp"
#include "victus/scene/scene.hpp"

#include <algorithm>
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

const player::Diagnostic* find(const player::ValidationReport& r, std::string_view needle) {
    const auto it = std::find_if(r.diagnostics.begin(), r.diagnostics.end(),
                                 [&](const player::Diagnostic& d) { return d.message.find(needle) != std::string::npos; });
    return it == r.diagnostics.end() ? nullptr : &*it;
}

} // namespace

TEST_CASE("validate: clean templates have no diagnostics") {
    TempDir tmp("victus_validate_clean");
    for (const char* t : {"2d", "3d", "nocode", "nocode3d", "simulation"}) {
        CAPTURE(t);
        const fs::Path dir = tmp.path / t;
        REQUIRE(project::Project::create(dir, "Clean", t).ok());
        auto r = player::validate_project(dir);
        REQUIRE(r.ok());
        for (const auto& d : r->diagnostics) MESSAGE(d.line());
        CHECK(r->diagnostics.empty());
        CHECK(r->ok());
        CHECK(r->scenes >= 1);
        if (!std::string_view(t).starts_with("nocode")) CHECK(r->scripts >= 1);
        CHECK(r->summary().starts_with("checked " + std::to_string(r->scenes) + " scenes, "));
        CHECK(r->summary().ends_with(": 0 problems"));
    }
}

TEST_CASE("validate: broken content becomes structured diagnostics") {
    TempDir tmp("victus_validate_broken");
    const fs::Path dir = tmp.path / "game";
    auto created = project::Project::create(dir, "Broken", "2d");
    REQUIRE(created.ok());

    // A script syntax error and a scene.load to a scene that does not exist.
    REQUIRE(fs::write_text_atomic(dir / "assets" / "scripts" / "broken.luau", "local M = {}\nfunction M:update(dt\nreturn M\n").ok());
    REQUIRE(fs::write_text_atomic(dir / "assets" / "scripts" / "goto.luau",
                                  "local M = {}\nfunction M:start() scene.load(\"scenes/nowhere.vscene\") end\nreturn M\n")
                .ok());
    // A behaviour tree that does not parse.
    std::filesystem::create_directories(dir / "assets" / "ai");
    REQUIRE(fs::write_text_atomic(dir / "assets" / "ai" / "broken.vbt", "{\"root\": {\"type\": \"sequence\"}}").ok());
    // A scene file that does not load.
    REQUIRE(fs::write_text_atomic(dir / "scenes" / "corrupt.vscene", "{ not json").ok());
    // Entities with a missing material and an unknown component in the startup scene.
    const Uuid missing_material = Uuid::generate();
    Uuid crate_id, ghost_id;
    {
        scene::Scene s;
        auto path = created->startup_scene_path();
        REQUIRE(path.ok());
        REQUIRE(s.load_file(*path).ok());
        const scene::Entity crate = s.create("Crate");
        s.world().add<scene::MeshRendererComponent>(crate).material_asset = missing_material;
        crate_id = s.uuid(crate);
        const scene::Entity ghost = s.create("Ghost");
        s.world().add<scene::UnknownComponents>(ghost).components.emplace_back("FutureThing", json::Value::object());
        ghost_id = s.uuid(ghost);
        REQUIRE(s.save_file(*path).ok());
    }

    auto r = player::validate_project(*created);
    REQUIRE(r.ok());
    for (const auto& d : r->diagnostics) MESSAGE(d.line());
    CHECK_FALSE(r->ok());
    CHECK(r->warnings() == 1);
    CHECK(r->errors() == 5);
    CHECK(r->summary().ends_with(": 5 problems"));

    const auto* syntax = find(*r, "broken.luau");
    REQUIRE(syntax);
    CHECK(syntax->category == player::Category::Script);
    CHECK(syntax->line().starts_with("error: scripts/broken.luau:3: "));
    CHECK_FALSE(syntax->suggested_fix.empty());
    CHECK(syntax->path == "scripts/broken.luau");

    const auto* load = find(*r, "names a scene that does not exist");
    REQUIRE(load);
    CHECK(load->category == player::Category::Script);
    CHECK(load->message == "scripts/goto.luau: scene.load(\"scenes/nowhere.vscene\") names a scene that does not exist");

    const auto* tree = find(*r, "needs children");
    REQUIRE(tree);
    CHECK(tree->category == player::Category::BehaviorTree);
    CHECK(tree->path == "ai/broken.vbt");
    CHECK_FALSE(tree->asset.is_nil());

    const auto* corrupt = find(*r, "corrupt.vscene");
    REQUIRE(corrupt);
    CHECK(corrupt->category == player::Category::Scene);
    CHECK(corrupt->path == "scenes/corrupt.vscene");

    const auto* material = find(*r, "uses missing material");
    REQUIRE(material);
    CHECK(material->severity == player::Severity::Error);
    CHECK(material->category == player::Category::Entity);
    CHECK(material->path == created->settings().startup_scene);
    CHECK(material->entity_name == "Crate");
    CHECK(material->entity_uuid == crate_id);
    CHECK(material->asset == missing_material);
    CHECK(material->message == created->settings().startup_scene + ": entity 'Crate' uses missing material " + missing_material.to_string());

    const auto* unknown = find(*r, "unknown component 'FutureThing'");
    REQUIRE(unknown);
    CHECK(unknown->severity == player::Severity::Warning);
    CHECK(unknown->entity_name == "Ghost");
    CHECK(unknown->entity_uuid == ghost_id);
    CHECK(unknown->line().starts_with("warning: "));

    CHECK(player::to_string(player::Category::AnimController) == "animation controller");
    CHECK(player::to_string(player::Severity::Warning) == "warning");
}

TEST_CASE("validate: projects that cannot be opened are an error, not a report") {
    TempDir tmp("victus_validate_none");
    CHECK_FALSE(player::validate_project(tmp.path).ok());
    // Input actions are validated when the project opens, so a bad key name fails the open.
    const fs::Path dir = tmp.path / "game";
    REQUIRE(project::Project::create(dir, "Keys", "2d").ok());
    auto text = fs::read_text(dir / "victus.project");
    REQUIRE(text.ok());
    const auto at = text->find("\"Pad.South\"");
    REQUIRE(at != std::string::npos);
    text->replace(at, 11, "\"Pad.Soth\"");
    REQUIRE(fs::write_text_atomic(dir / "victus.project", *text).ok());
    auto r = player::validate_project(dir);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("Pad.Soth") != std::string::npos);
}

TEST_CASE("validate: convex and mesh colliders need geometry") {
    TempDir tmp("victus_validate_mesh_colliders");
    const fs::Path dir = tmp.path / "game";
    auto created = project::Project::create(dir, "Colliders", "3d");
    REQUIRE(created.ok());
    const Uuid missing_model = Uuid::generate();
    {
        using physics::ColliderComponent;
        scene::Scene s;
        auto path = created->startup_scene_path();
        REQUIRE(path.ok());
        REQUIRE(s.load_file(*path).ok());
        const auto collider = [&](const char* name, ColliderComponent::Shape shape) {
            const scene::Entity e = s.create(name);
            s.world().add<ColliderComponent>(e).shape = shape;
            return e;
        };
        collider("Rock", ColliderComponent::Shape::Convex); // nothing to build from
        s.world().get<ColliderComponent>(collider("Statue", ColliderComponent::Shape::Mesh))->mesh_asset = missing_model;
        const scene::Entity ball = collider("Ball", ColliderComponent::Shape::Convex);
        s.world().add<scene::MeshRendererComponent>(ball).primitive = "sphere"; // fine
        const scene::Entity barrel = collider("Barrel", ColliderComponent::Shape::Mesh);
        s.world().add<scene::MeshRendererComponent>(barrel).primitive = "cylinder";
        s.world().add<physics::RigidBodyComponent>(barrel); // dynamic: simulates the hull
        // 2D outlines: a crossing polygon and a one-point chain are errors; a dynamic chain warns.
        const auto outline = [&](const char* name, physics::Collider2DComponent::Shape shape, std::vector<math::Vec2> pts) {
            const scene::Entity e = s.create(name);
            auto& c = s.world().add<physics::Collider2DComponent>(e);
            c.shape = shape;
            c.points = std::move(pts);
            return e;
        };
        outline("Bowtie", physics::Collider2DComponent::Shape::Polygon, {{0, 0}, {1, 1}, {1, 0}, {0, 1}});
        outline("Dot", physics::Collider2DComponent::Shape::Chain, {{0, 0}});
        s.world().add<physics::RigidBody2DComponent>(
            outline("Crate2D", physics::Collider2DComponent::Shape::Chain, {{0, 0}, {1, 0}, {1, 1}, {0, 1}}));
        outline("Hill", physics::Collider2DComponent::Shape::Chain, {{0, 0}, {1, 0.5f}, {2, 0}}); // fine
        REQUIRE(s.save_file(*path).ok());
    }
    auto r = player::validate_project(*created);
    REQUIRE(r.ok());
    for (const auto& d : r->diagnostics) MESSAGE(d.line());
    CHECK(r->errors() == 4);
    CHECK(r->warnings() == 2);
    CHECK(find(*r, "'Bowtie': Collider2D polygon: edges cross each other"));
    CHECK(find(*r, "'Dot': Collider2D chain needs at least 3 points"));
    CHECK(find(*r, "'Crate2D': chain Collider2D on a dynamic body is filled as a polygon"));
    CHECK_FALSE(find(*r, "'Hill'"));
    const auto* rock = find(*r, "'Rock' has a convex collider without geometry");
    REQUIRE(rock);
    CHECK(rock->severity == player::Severity::Error);
    const auto* statue = find(*r, "collider uses missing model");
    REQUIRE(statue);
    CHECK(statue->entity_name == "Statue");
    CHECK(statue->asset == missing_model);
    const auto* barrel = find(*r, "dynamic body with a mesh collider");
    REQUIRE(barrel);
    CHECK(barrel->severity == player::Severity::Warning);
    CHECK(barrel->entity_name == "Barrel");
    CHECK_FALSE(find(*r, "'Ball'"));
}

TEST_CASE("validate: light cookies must be textures on spot, point or directional lights (ADR-0195, ADR-0201)") {
    TempDir tmp("victus_validate_cookies");
    const fs::Path dir = tmp.path / "game";
    auto created = project::Project::create(dir, "Cookies", "3d");
    REQUIRE(created.ok());
    const Uuid missing = Uuid::generate();
    {
        scene::Scene s;
        auto path = created->startup_scene_path();
        REQUIRE(path.ok());
        REQUIRE(s.load_file(*path).ok());
        const auto light = [&](const char* name, scene::LightComponent::Type type) {
            auto& l = s.world().add<scene::LightComponent>(s.create(name));
            l.type = type;
            l.cookie = missing;
            return &l;
        };
        light("Torch", scene::LightComponent::Type::Spot);
        light("Bulb", scene::LightComponent::Type::Point);
        light("Panel", scene::LightComponent::Type::Area);
        REQUIRE(s.save_file(*path).ok());
    }
    auto r = player::validate_project(*created);
    REQUIRE(r.ok());
    for (const auto& d : r->diagnostics) MESSAGE(d.line());
    const auto* torch = find(*r, "'Torch' uses missing light cookie texture");
    REQUIRE(torch);
    CHECK(torch->severity == player::Severity::Error);
    CHECK(torch->asset == missing);
    CHECK(find(*r, "'Bulb' uses missing light cookie texture"));
    CHECK_FALSE(find(*r, "'Bulb' has a cookie but is an area light"));
    const auto* panel = find(*r, "'Panel' has a cookie but is an area light");
    REQUIRE(panel);
    CHECK(panel->severity == player::Severity::Warning);
}
