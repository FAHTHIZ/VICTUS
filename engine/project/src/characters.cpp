// SPDX-License-Identifier: Apache-2.0
#include "victus/project/characters.hpp"

#include "victus/animation/animation.hpp"
#include "victus/assets/importers.hpp"
#include "victus/assets/instantiate.hpp"
#include "victus/core/json.hpp"

#include <array>
#include <mutex>
#include <optional>

namespace victus::project {

namespace embedded {
std::span<const std::uint8_t> kOrionGlb();
std::span<const std::uint8_t> kNovaGlb();
} // namespace embedded

const std::vector<StarterCharacter>& starter_characters() {
    static const std::vector<StarterCharacter> list = {
        {"orion", "Orion",
         "Masculine-presenting human pilot: athletic 1.77 m build, cropped hair and stubble, graphite flight suit with "
         "cyan piping; 60-joint skeleton, 44 facial blend shapes, full PBR texture set.",
         "assets/models/characters/victus_orion.glb", "8e84eb12-63c8-4aed-aae2-517c038d3071", 1.7697f},
        {"nova", "Nova",
         "Feminine-presenting human pilot: slender 1.68 m build, hair in a low bun, freckles, ivory flight suit with "
         "amber piping; 60-joint skeleton, 44 facial blend shapes, full PBR texture set.",
         "assets/models/characters/victus_nova.glb", "44037d0d-1627-44a8-a225-633971f196b2", 1.6827f},
    };
    return list;
}

const StarterCharacter* find_starter_character(std::string_view id) {
    for (const auto& c : starter_characters())
        if (c.id == id) return &c;
    return nullptr;
}

namespace {

std::size_t index_of(const StarterCharacter& c) {
    const auto& list = starter_characters();
    return static_cast<std::size_t>(&c - list.data());
}

} // namespace

std::span<const std::uint8_t> starter_character_glb(const StarterCharacter& c) {
    if (index_of(c) == 0) return embedded::kOrionGlb();
    return embedded::kNovaGlb();
}

std::string starter_character_meta(const StarterCharacter& c) {
    json::Value m = json::Value::object();
    m.set("victus_meta", 1);
    m.set("uuid", std::string(c.uuid));
    m.set("importer", "gltf");
    // Block-compressed textures (BC7 for desktop, ASTC 4x4 for mobile): each character carries
    // sixteen PBR maps up to 2048 px, about 245 MB as RGBA8 mip chains on the GPU, a quarter of
    // that compressed. RGBA8 copies are kept only up to 256 px (a device without BC or ASTC
    // decodes the larger levels at load). The cooked result is shared between projects through
    // the machine-wide cooked cache (ADR-0180), so only the first project pays for encoding.
    json::Value settings = json::Value::object();
    settings.set("texture_compression", "auto");
    settings.set("rgba8_max_size", 256);
    m.set("settings", std::move(settings));
    return json::stringify(m, true) + "\n";
}

std::string starter_character_controller() {
    // Locomotion blends Idle/Walk/Run by ground speed (m/s), phase-synced. A jump plays the
    // one-shot Jump, then Fall loops until "grounded"; walking off a ledge goes straight to Fall.
    // Land absorbs the impact and hands back to Locomotion. Interact is a short hand reach.
    return R"({
  "version": 1,
  "parameters": [
    { "name": "speed", "type": "float" },
    { "name": "grounded", "type": "bool", "default": true },
    { "name": "jump", "type": "trigger" },
    { "name": "interact", "type": "trigger" }
  ],
  "default_state": "Locomotion",
  "states": [
    { "name": "Locomotion", "blend1d": { "parameter": "speed", "points": [
        { "clip": "Idle", "at": 0 }, { "clip": "Walk", "at": 2.5 }, { "clip": "Run", "at": 5 } ] } },
    { "name": "Jump", "clip": "Jump", "loop": false },
    { "name": "Fall", "clip": "Fall" },
    { "name": "Land", "clip": "Land", "loop": false },
    { "name": "Interact", "clip": "Interact", "loop": false }
  ],
  "transitions": [
    { "from": "Locomotion", "to": "Jump", "duration": 0.08, "conditions": [ { "parameter": "jump" } ] },
    { "from": "Land", "to": "Jump", "duration": 0.08, "conditions": [ { "parameter": "jump" } ] },
    { "from": "Interact", "to": "Jump", "duration": 0.08, "conditions": [ { "parameter": "jump" } ] },
    { "from": "Locomotion", "to": "Fall", "duration": 0.25, "conditions": [ { "parameter": "grounded", "op": "false" } ] },
    { "from": "Interact", "to": "Fall", "duration": 0.25, "conditions": [ { "parameter": "grounded", "op": "false" } ] },
    { "from": "Jump", "to": "Land", "duration": 0.08, "exit_time": 0.35,
      "conditions": [ { "parameter": "grounded", "op": "true" } ] },
    { "from": "Jump", "to": "Fall", "duration": 0.3, "exit_time": 1.0,
      "conditions": [ { "parameter": "grounded", "op": "false" } ] },
    { "from": "Fall", "to": "Land", "duration": 0.06, "conditions": [ { "parameter": "grounded", "op": "true" } ] },
    { "from": "Land", "to": "Locomotion", "duration": 0.2, "exit_time": 0.6 },
    { "from": "Locomotion", "to": "Interact", "duration": 0.15, "conditions": [ { "parameter": "interact" } ] },
    { "from": "Interact", "to": "Locomotion", "duration": 0.25, "exit_time": 0.92 }
  ],
  "twist": [
    { "joint": "LowerArmTwist.L", "source": "Hand.L", "weight": 0.5 },
    { "joint": "LowerArmTwist.R", "source": "Hand.R", "weight": 0.5 }
  ]
}
)";
}

Result<scene::Entity> instantiate_starter_character(scene::Scene& s, const StarterCharacter& c, std::string_view name,
                                                    scene::Entity parent) {
    static std::mutex mutex;
    static std::array<std::optional<assets::CookedModel>, 2> cache;
    const std::size_t i = index_of(c);
    std::lock_guard lock(mutex);
    if (!cache[i]) {
        // Only the node graph matters here: skip mesh optimisation, LODs and tangents.
        assets::ModelImportSettings settings;
        settings.optimize_meshes = false;
        settings.generate_tangents = false;
        settings.import_textures = false;
        auto model = assets::import_gltf_memory(starter_character_glb(c), c.model_path, settings);
        if (!model) return std::move(model).error();
        cache[i] = std::move(model).value();
    }
    Uuid uuid;
    if (!Uuid::parse(c.uuid, uuid)) return make_error(ErrorCode::InvalidArgument, "bad starter character uuid");
    const scene::Entity root = assets::instantiate_model(s, uuid, *cache[i], name, parent);
    if (auto* a = s.world().get<animation::AnimatorComponent>(root)) {
        a->clip = "Idle";
        a->controller = kCharacterControllerAsset;
    }
    return root;
}

} // namespace victus::project
