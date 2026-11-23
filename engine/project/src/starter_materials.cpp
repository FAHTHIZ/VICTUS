// SPDX-License-Identifier: Apache-2.0
#include "victus/project/starter_materials.hpp"

#include "victus/assets/importers.hpp"
#include "victus/assets/material.hpp"
#include "victus/core/json.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace victus::project {

namespace embedded {
#define VICTUS_STARTER_SET(name)                         \
    std::span<const std::uint8_t> kStarter_##name##_color_jpg();  \
    std::span<const std::uint8_t> kStarter_##name##_normal_jpg(); \
    std::span<const std::uint8_t> kStarter_##name##_orm_jpg();
VICTUS_STARTER_SET(grass)
VICTUS_STARTER_SET(rock)
VICTUS_STARTER_SET(planks)
VICTUS_STARTER_SET(metal)
VICTUS_STARTER_SET(paving)
#undef VICTUS_STARTER_SET
std::span<const std::uint8_t> kStarter_partly_cloudy_hdr();
} // namespace embedded

namespace {

using Bytes = std::span<const std::uint8_t> (*)();

struct SetData {
    StarterMaterialInfo info;
    std::array<std::string_view, 3> texture_uuids; // color, normal, orm
    std::array<Bytes, 3> bytes;
    float repeats_per_metre;
    float metallic;
    float sharpness;   // triplanar blend sharpness; 0 = mesh UVs (uv_scale repeats per face)
    int textures_from; // index of the set whose textures this material reuses, -1 = its own
    float anti_tiling; // ADR-0240: organic surfaces 1, regular patterns (paving, planks) macro variation only
};

#define VICTUS_STARTER_BYTES(name) \
    {embedded::kStarter_##name##_color_jpg, embedded::kStarter_##name##_normal_jpg, embedded::kStarter_##name##_orm_jpg}

// UUIDs are fixed so scenes built by the templates can reference the files before they exist.
const std::array<SetData, static_cast<std::size_t>(StarterMaterial::Count)>& sets() {
    static const std::array<SetData, static_cast<std::size_t>(StarterMaterial::Count)> list = {{
        {{"grass", "assets/materials/starter/grass.vmat", "0ff34adc-f86d-4dbb-b002-8fca7737a368"},
         {"89bf7840-f653-4a02-8a4b-5d25f024f8ad", "85c17507-08f7-4d5c-95db-8b077e147c6f", "16dfa65e-2b3b-443e-be45-e766f997f2ac"},
         VICTUS_STARTER_BYTES(grass), 0.5f, 0.0f, 8.0f, -1, 1.0f},
        {{"rock", "assets/materials/starter/rock.vmat", "41d1b30b-56c2-4579-aa04-8db688955042"},
         {"f8c194c6-3aa4-465d-a97a-63cb67181eed", "86df8765-b674-45ea-9073-8ee93a3cd2df", "c728c44b-255b-4243-a0f1-f568f50789f7"},
         VICTUS_STARTER_BYTES(rock), 0.35f, 0.0f, 6.0f, -1, 1.0f},
        {{"planks", "assets/materials/starter/planks.vmat", "d2db09c1-abda-4fb5-9ab7-65355bad2577"},
         {"dd414392-69f6-41c6-a485-e422625ee8b0", "f19edca1-36c8-459f-9da4-ed9405718742", "2c259177-28b9-4410-9b2d-77da20d490e6"},
         VICTUS_STARTER_BYTES(planks), 0.5f, 0.0f, 12.0f, -1, 0.3f},
        {{"metal", "assets/materials/starter/metal.vmat", "9dd94911-8f3c-4fb8-87e3-c255a5b46baf"},
         {"f2b6b65a-34f8-4f2a-b0fe-8e04bd47f927", "57391cec-bb62-41c4-8c29-7aef5cbea650", "ab75b2fc-b2c3-4a3b-a603-0af2c84b01a9"},
         VICTUS_STARTER_BYTES(metal), 0.75f, 1.0f, 8.0f, -1, 0.0f},
        {{"paving", "assets/materials/starter/paving.vmat", "126386bf-b6d7-49b8-a925-f68faa031016"},
         {"2393ff81-64aa-4388-8792-ea7cf7c7809b", "8d3b50d4-950d-4450-b312-7a5ef6a5b596", "76be045c-c999-4d2c-8b07-ce690d6d4ebc"},
         VICTUS_STARTER_BYTES(paving), 0.35f, 0.0f, 12.0f, -1, 0.5f},
        {{"crate", "assets/materials/starter/crate.vmat", "c7a1e0b2-4d8f-4a5e-9b61-3f2d7c8e9a14"},
         {},
         {},
         1.0f, 0.0f, 0.0f, static_cast<int>(StarterMaterial::Planks), 0.0f},
    }};
    return list;
}
#undef VICTUS_STARTER_BYTES

constexpr std::string_view kSkyUuid = "79c5cde0-59c5-42b1-8c8f-69f900975ee9";
constexpr std::string_view kWindShaderUuid = "a3e1f0c2-6b7d-4e58-9c14-2f8b5d7e0a31";
constexpr std::string_view kLeavesUuid = "a3e1f0c2-6b7d-4e58-9c14-2f8b5d7e0a32";
constexpr std::string_view kBarkUuid = "a3e1f0c2-6b7d-4e58-9c14-2f8b5d7e0a33";

// Wind for vertex-coloured foliage (ADR-0243), after the usual real-time recipe (a height-weighted
// bend of the whole plant plus a small per-vertex flutter): the bend grows with the square of the
// height above the object's origin so the base stays planted, and its phase comes from the
// object's position so neighbouring trees do not sway in lockstep. Runs in the camera and shadow
// passes alike, so shadows move with the leaves.
constexpr std::string_view kWindShader = R"(// Foliage wind (used by materials/starter/leaves.vmat and bark.vmat, ADR-0243).
// params[0] = wind direction (xyz, world) + bend strength (w, m of sway at 10 m height)
// params[1] = x gust speed, y leaf flutter (m), z flutter speed
void vertex(VertexInput i, inout Vertex v) {
    float h = max(i.position.y, 0.0);
    vec3 dir = i.params[0].xyz;
    dir = normalize(vec3(dir.x, 0.0, dir.z) + vec3(1e-4, 0.0, 0.0));
    float phase = dot(i.object_origin.xz, vec2(0.21, 0.17));
    float t = i.time * i.params[1].x + phase;
    // Slow gusts that never die out completely, plus a quicker back-and-forth sway.
    float gust = 0.55 + 0.45 * sin(t * 0.43 + 1.1) * sin(t * 0.17 + 2.3);
    float sway = 0.35 * sin(t * 1.9) + 0.15 * sin(t * 3.1 + 0.7);
    float bend = i.params[0].w * (h * h) * 0.01 * (gust + sway * gust);
    v.world_position += dir * bend;
    v.world_position.y -= 0.15 * abs(bend) * bend / max(h, 0.5); // keep the length while bending
    // Leaf flutter: small, fast, varying across the crown, scaled up away from the trunk base.
    vec3 w = v.world_position;
    float f = sin(i.time * i.params[1].z + dot(w, vec3(1.7, 2.3, 1.3))) * sin(i.time * i.params[1].z * 0.63 + dot(w, vec3(-2.1, 0.9, 1.9)));
    v.world_position += v.normal * (f * i.params[1].y * clamp(h * 0.3, 0.0, 1.0));
}
)";

Uuid parse(std::string_view text) {
    Uuid u;
    (void)Uuid::parse(text, u);
    return u;
}

std::string to_string(std::span<const std::uint8_t> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string texture_meta(std::string_view uuid, bool srgb) {
    assets::TextureImportSettings settings;
    settings.srgb = srgb;
    json::Value m = json::Value::object();
    m.set("victus_meta", 1);
    m.set("uuid", std::string(uuid));
    m.set("importer", "texture");
    m.set("settings", settings.to_json());
    return json::stringify(m, true) + "\n";
}

std::string plain_meta(std::string_view uuid) {
    json::Value m = json::Value::object();
    m.set("victus_meta", 1);
    m.set("uuid", std::string(uuid));
    m.set("importer", "");
    return json::stringify(m, true) + "\n";
}

} // namespace

const StarterMaterialInfo& starter_material(StarterMaterial m) noexcept {
    return sets()[static_cast<std::size_t>(m)].info;
}

Uuid starter_material_uuid(StarterMaterial m) noexcept { return parse(starter_material(m).material_uuid); }

Uuid starter_sky_uuid() noexcept { return parse(kSkyUuid); }
Uuid starter_leaves_uuid() noexcept { return parse(kLeavesUuid); }
Uuid starter_bark_uuid() noexcept { return parse(kBarkUuid); }

void add_starter_material_files(std::vector<TemplateFile>& files) {
    static constexpr std::array<std::string_view, 3> kSuffix = {"_color.jpg", "_normal.jpg", "_orm.jpg"};
    for (const SetData& set : sets()) {
        const SetData& tex = set.textures_from < 0 ? set : sets()[static_cast<std::size_t>(set.textures_from)];
        if (set.textures_from < 0) {
            const std::string base = "assets/textures/starter/" + std::string(set.info.id);
            for (std::size_t i = 0; i < 3; ++i) {
                const std::string path = base + std::string(kSuffix[i]);
                files.push_back({path, to_string(set.bytes[i]())});
                files.push_back({path + ".meta", texture_meta(set.texture_uuids[i], i == 0)});
            }
        }
        assets::MaterialAsset m;
        m.base_color_texture = parse(tex.texture_uuids[0]);
        m.normal_texture = parse(tex.texture_uuids[1]);
        m.metallic_roughness_texture = parse(tex.texture_uuids[2]);
        m.metallic = set.metallic;
        m.roughness = 1.0f; // the ORM texture's green channel carries the roughness
        m.occlusion_strength = 1.0f;
        m.uv_scale = {set.repeats_per_metre, set.repeats_per_metre};
        m.triplanar = set.sharpness > 0.0f;
        if (m.triplanar) m.triplanar_sharpness = set.sharpness;
        m.anti_tiling = set.anti_tiling;
        files.push_back({std::string(set.info.material_path), json::stringify(m.to_json(), true) + "\n"});
        files.push_back({std::string(set.info.material_path) + ".meta", plain_meta(set.info.material_uuid)});
    }
    // Foliage (ADR-0243): white base colours so the tree meshes' vertex colours show through.
    files.push_back({std::string(kFoliageWindShaderPath), std::string(kWindShader)});
    files.push_back({std::string(kFoliageWindShaderPath) + ".meta", plain_meta(kWindShaderUuid)});
    const auto foliage = [&](std::string_view name, std::string_view uuid, float roughness, float flutter, bool leaves) {
        assets::MaterialAsset m;
        m.base_color = {1, 1, 1, 1};
        m.roughness = roughness;
        m.shader = parse(kWindShaderUuid);
        m.shader_params[0] = {1.0f, 0.0f, 0.35f, 0.35f};
        m.shader_params[1] = {0.9f, flutter, 5.0f, 0.0f};
        if (leaves) {
            m.subsurface_color = {0.32f, 0.42f, 0.08f}; // sunlight glowing through the leaves
            m.subsurface_thickness = 0.35f;
        }
        const std::string path = "assets/materials/starter/" + std::string(name) + ".vmat";
        files.push_back({path, json::stringify(m.to_json(), true) + "\n"});
        files.push_back({path + ".meta", plain_meta(uuid)});
    };
    foliage("leaves", kLeavesUuid, 0.7f, 0.035f, true);
    foliage("bark", kBarkUuid, 0.92f, 0.0f, false);
    files.push_back({std::string(kStarterSkyPath), to_string(embedded::kStarter_partly_cloudy_hdr())});
    files.push_back({std::string(kStarterSkyPath) + ".meta", texture_meta(kSkyUuid, false)});
}

} // namespace victus::project
