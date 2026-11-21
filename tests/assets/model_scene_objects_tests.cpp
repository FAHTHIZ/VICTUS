// SPDX-License-Identifier: Apache-2.0
// glTF KHR_lights_punctual lights and cameras: import, cooked round trip, instantiation, export.
#include "doctest.h"
#include "gltf_writer.hpp"
#include "victus/assets/importers.hpp"
#include "victus/assets/instantiate.hpp"

#include <cmath>
#include <span>

using namespace victus;
using namespace victus::assets;

namespace {
// The quad fixture plus a sun, a spot (range 12), a point light without range, a perspective
// camera and an orthographic one, each on its own node under "Root".
std::vector<std::uint8_t> lit_glb() {
    test::GltfFixture fx;
    auto ext = json::parse(R"({"KHR_lights_punctual":{"lights":[
        {"name":"Sun","type":"directional","color":[1,0.9,0.8],"intensity":2049},
        {"name":"Spot","type":"spot","intensity":683,"range":12,"spot":{"innerConeAngle":0.2,"outerConeAngle":0.5}},
        {"type":"point","intensity":6830}]}})");
    REQUIRE(ext.ok());
    fx.doc.set("extensions", *ext);
    fx.doc.set("extensionsUsed", json::Value(json::Array{"KHR_lights_punctual"}));
    auto cams = json::parse(R"([{"name":"Main","type":"perspective","perspective":{"yfov":0.6,"znear":0.05,"zfar":250,"aspectRatio":1.5}},
                                {"type":"orthographic","orthographic":{"xmag":8,"ymag":4,"znear":0,"zfar":50}}])");
    REQUIRE(cams.ok());
    fx.doc.set("cameras", *cams);
    json::Array nodes = fx.doc["nodes"].as_array();
    auto light_node = [](const char* name, int light) {
        auto n = json::parse(std::string(R"({"name":")") + name + R"(","translation":[0,3,0],"extensions":{"KHR_lights_punctual":{"light":)" +
                             std::to_string(light) + "}}}");
        return *n;
    };
    nodes.push_back(light_node("SunNode", 0));
    nodes.push_back(light_node("SpotNode", 1));
    nodes.push_back(light_node("LampNode", 2));
    nodes.push_back(*json::parse(R"({"name":"CamNode","camera":0,"translation":[0,1,5]})"));
    nodes.push_back(*json::parse(R"({"name":"TopNode","camera":1})"));
    json::Value root = nodes[0];
    root.set("children", json::Value(json::Array{1, 2, 3, 4, 5, 6}));
    nodes[0] = root;
    fx.doc.set("nodes", json::Value(nodes));
    return fx.glb();
}

const ModelNode* find(const CookedModel& m, std::string_view name) {
    for (const auto& n : m.nodes)
        if (n.name == name) return &n;
    return nullptr;
}
} // namespace

TEST_CASE("assets: glTF lights and cameras import, round-trip and instantiate") {
    const auto glb = lit_glb();
    ModelImportSettings settings;
    settings.scale = 2.0f;
    auto m = import_gltf_memory(glb, "lit.glb", settings);
    REQUIRE_MESSAGE(m.ok(), (m.ok() ? "" : m.error().message));
    REQUIRE(m->lights.size() == 3);
    REQUIRE(m->cameras.size() == 2);
    const ModelLight& sun = m->lights[0];
    CHECK(sun.type == ModelLight::Type::Directional);
    CHECK(sun.name == "Sun");
    CHECK(sun.intensity == doctest::Approx(3.0f));  // 2049 lux / 683
    CHECK(sun.color.g == doctest::Approx(0.9f));
    const ModelLight& spot = m->lights[1];
    CHECK(spot.type == ModelLight::Type::Spot);
    CHECK(spot.intensity == doctest::Approx(1.0f));
    CHECK(spot.range == doctest::Approx(24.0f)); // import scale 2
    CHECK(spot.inner_cone == doctest::Approx(0.2f));
    CHECK(spot.outer_cone == doctest::Approx(0.5f));
    CHECK(m->lights[2].type == ModelLight::Type::Point);
    CHECK(m->lights[2].range == 0.0f);
    CHECK(m->lights[2].name == "light2");
    const ModelCamera& cam = m->cameras[0];
    CHECK_FALSE(cam.orthographic);
    CHECK(cam.yfov == doctest::Approx(0.6f));
    CHECK(cam.znear == doctest::Approx(0.1f));
    CHECK(cam.zfar == doctest::Approx(500.0f));
    CHECK(cam.aspect == doctest::Approx(1.5f));
    CHECK(m->cameras[1].orthographic);
    CHECK(m->cameras[1].ymag == doctest::Approx(8.0f));
    CHECK(m->cameras[1].aspect == doctest::Approx(2.0f));
    REQUIRE(find(*m, "SpotNode"));
    CHECK(find(*m, "SpotNode")->light == 1);
    CHECK(find(*m, "CamNode")->camera == 0);
    CHECK(find(*m, "TopNode")->camera == 1);
    CHECK(find(*m, "Root")->light == -1);

    // Cooked format round trip.
    const auto bytes = serialize(CookedHeader{.kind = AssetKind::Model}, *m);
    auto back = deserialize_model(bytes);
    REQUIRE_MESSAGE(back.ok(), (back.ok() ? "" : back.error().message));
    REQUIRE(back->lights.size() == 3);
    CHECK(back->lights[1].range == doctest::Approx(24.0f));
    CHECK(back->cameras[1].orthographic);
    CHECK(find(*back, "LampNode")->light == 2);
    CookedModel broken = *m;
    broken.nodes[1].light = 7;
    CHECK_FALSE(validate_model(broken).ok());

    // Disabled by settings.
    ModelImportSettings off;
    off.import_lights = false;
    off.import_cameras = false;
    auto none = import_gltf_memory(glb, "lit.glb", off);
    REQUIRE(none.ok());
    CHECK(none->lights.empty());
    CHECK(none->cameras.empty());
    CHECK(ModelImportSettings::from_json(off.to_json()).import_lights == false);
    ModelImportSettings unitless;
    unitless.light_intensity_scale = 1.0f;
    CHECK(ModelImportSettings::from_json(unitless.to_json()).light_intensity_scale == 1.0f);
    CHECK(ModelImportSettings::from_json(ModelImportSettings{}.to_json()).light_intensity_scale == doctest::Approx(1.0f / 683.0f));

    // Instantiation: components on the light and camera nodes; imported cameras are not primary.
    scene::Scene s;
    const scene::Entity root = instantiate_model(s, Uuid::generate(), *m, "Lit");
    int lights = 0, cameras = 0;
    s.world().each<scene::LightComponent>([&](scene::Entity e, const scene::LightComponent& l) {
        ++lights;
        if (s.world().get<scene::Name>(e)->value == "SpotNode") {
            CHECK(l.type == scene::LightComponent::Type::Spot);
            CHECK(l.range == doctest::Approx(24.0f));
            CHECK(l.outer_angle_degrees == doctest::Approx(28.6479f));
            CHECK(l.inner_angle_degrees == doctest::Approx(11.4592f));
        } else if (s.world().get<scene::Name>(e)->value == "LampNode") {
            CHECK(l.range == doctest::Approx(std::sqrt(1000.0f))); // 10 x 100, derived
        } else {
            CHECK(l.type == scene::LightComponent::Type::Directional);
            CHECK(l.intensity == doctest::Approx(3.0f));
        }
    });
    s.world().each<scene::CameraComponent>([&](scene::Entity e, const scene::CameraComponent& c) {
        ++cameras;
        CHECK_FALSE(c.primary);
        if (s.world().get<scene::Name>(e)->value == "CamNode") {
            CHECK(c.fov_y_degrees == doctest::Approx(34.3775f));
            CHECK(c.z_far == doctest::Approx(500.0f));
        } else {
            CHECK(c.projection == scene::CameraComponent::Projection::Orthographic);
            CHECK(c.ortho_height == doctest::Approx(16.0f));
        }
    });
    CHECK(lights == 3);
    CHECK(cameras == 2);
    CHECK(s.children(root).size() == 1);

    // glTF export writes them back (photometric) and re-imports to the same values.
    auto json_text = export_gltf_json(*m);
    REQUIRE(json_text.ok());
    CHECK(json_text->find("KHR_lights_punctual") != std::string::npos);
    auto again = import_gltf_memory({reinterpret_cast<const std::uint8_t*>(json_text->data()), json_text->size()}, "again.gltf", {});
    REQUIRE_MESSAGE(again.ok(), (again.ok() ? "" : again.error().message));
    REQUIRE(again->lights.size() == 3);
    CHECK(again->lights[0].intensity == doctest::Approx(3.0f));
    CHECK(again->lights[1].range == doctest::Approx(24.0f));
    CHECK(again->lights[1].outer_cone == doctest::Approx(0.5f));
    REQUIRE(again->cameras.size() == 2);
    CHECK(again->cameras[0].zfar == doctest::Approx(500.0f));
    CHECK(again->cameras[1].ymag == doctest::Approx(8.0f));
    CHECK(find(*again, "TopNode")->camera == 1);
}
