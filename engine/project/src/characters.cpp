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
extern const std::uint8_t kOrionGlb[];
extern const std::size_t kOrionGlbSize;
extern const std::uint8_t kNovaGlb[];
extern const std::size_t kNovaGlbSize;
} // namespace embedded

const std::vector<StarterCharacter>& starter_characters() {
    static const std::vector<StarterCharacter> list = {
        {"orion", "Orion",
         "Masculine-presenting VICTUS mannequin: athletic build, V-cut chest cuirass, cropped sculpted hair, cyan mark.",
         "assets/models/characters/victus_orion.glb", "8e84eb12-63c8-4aed-aae2-517c038d3071"},
        {"nova", "Nova",
         "Feminine-presenting VICTUS mannequin: slender build, tapered cuirass, swept sculpted hair, amber mark.",
         "assets/models/characters/victus_nova.glb", "44037d0d-1627-44a8-a225-633971f196b2"},
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
    if (index_of(c) == 0) return {embedded::kOrionGlb, embedded::kOrionGlbSize};
    return {embedded::kNovaGlb, embedded::kNovaGlbSize};
}

std::string starter_character_meta(const StarterCharacter& c) {
    json::Value m = json::Value::object();
    m.set("victus_meta", 1);
    m.set("uuid", std::string(c.uuid));
    m.set("importer", "gltf");
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
