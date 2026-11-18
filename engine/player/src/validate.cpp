// SPDX-License-Identifier: Apache-2.0
// Project validation (`victus check`, the editor's Problems panel). Moved from tools/cli so the
// messages, order and counts stay exactly what `victus check` has always printed.
#include "victus/player/validate.hpp"
#include "victus/ai/ai.hpp"
#include "victus/animation/animation.hpp"
#include "victus/animation/retarget.hpp"
#include "victus/animation/sprite.hpp"
#include "victus/assets/database.hpp"
#include "victus/platform/platform.hpp"
#include "victus/project/project.hpp"
#include "victus/runtime/surface_shader.hpp"
#include "victus/scene/scene.hpp"
#include "victus/script/script.hpp"
#include "victus/sequencer/sequencer.hpp"
#include "victus/terrain/terrain.hpp"
#include "victus/tilemap/tilemap.hpp"

#include <algorithm>
#include <filesystem>
#include <set>

namespace victus::player {

std::string_view to_string(Severity s) noexcept { return s == Severity::Error ? "error" : "warning"; }

std::string_view to_string(Category c) noexcept {
    switch (c) {
        case Category::Project: return "project";
        case Category::Script: return "script";
        case Category::Material: return "material";
        case Category::Shader: return "shader";
        case Category::SpriteSheet: return "sprite sheet";
        case Category::Tileset: return "tile set";
        case Category::Terrain: return "terrain";
        case Category::BehaviorTree: return "behaviour tree";
        case Category::Sequence: return "sequence";
        case Category::AnimController: return "animation controller";
        case Category::Prefab: return "prefab";
        case Category::Scene: return "scene";
        case Category::Entity: return "entity";
    }
    return "unknown";
}

std::string Diagnostic::line() const { return std::string(to_string(severity)) + ": " + message; }

std::size_t ValidationReport::errors() const noexcept {
    return static_cast<std::size_t>(std::count_if(diagnostics.begin(), diagnostics.end(),
                                                  [](const Diagnostic& d) { return d.severity == Severity::Error; }));
}

std::size_t ValidationReport::warnings() const noexcept { return diagnostics.size() - errors(); }

std::string ValidationReport::summary() const {
    return "checked " + std::to_string(scenes) + " scenes, " + std::to_string(prefabs) + " prefabs, " +
           std::to_string(materials) + " materials, " + std::to_string(shaders) + " shaders, " +
           std::to_string(sprite_sheets) + " sprite sheets, " + std::to_string(tilesets) + " tile sets, " +
           std::to_string(controllers) + " animation controllers and " + std::to_string(scripts) +
           " scripts: " + std::to_string(errors()) + " problems";
}

namespace {

std::string entity_name(const scene::Scene& s, scene::Entity e) {
    const auto* n = s.world().get<scene::Name>(e);
    return n ? n->value : std::string("<unnamed>");
}

// Prefixes `path: ` unless the message already starts with the path (loader errors usually do).
std::string with_path(const std::string& path, const std::string& message) {
    return message.starts_with(path) ? message : path + ": " + message;
}

struct Sink {
    ValidationReport& report;
    Diagnostic& add(Severity s, Category c, std::string path, std::string message, std::string fix = {}) {
        Diagnostic d;
        d.severity = s;
        d.category = c;
        d.path = std::move(path);
        d.message = std::move(message);
        d.suggested_fix = std::move(fix);
        report.diagnostics.push_back(std::move(d));
        return report.diagnostics.back();
    }
    Diagnostic& error(Category c, std::string path, std::string message, std::string fix = {}) {
        return add(Severity::Error, c, std::move(path), std::move(message), std::move(fix));
    }
};

} // namespace

Result<ValidationReport> validate_project(const project::Project& proj) {
    project::register_components();
    ValidationReport report;
    Sink sink{report};
    const fs::Path assets = proj.root() / "assets";
    const std::vector<std::string> project_scenes = proj.scenes(); // targets of scene.load
    for (const auto& msg : proj.settings().input.validate(
             [](std::string_view k) { return platform::key_from_name(k) != platform::Key::Unknown; }))
        sink.error(Category::Project, std::string(project::kProjectFileName), std::string(project::kProjectFileName) + ": " + msg,
                   "Fix the input action in Project Settings or victus.project; key names are listed in docs/USER_GUIDE.md.");

    std::vector<std::string> prefab_files;
    std::set<Uuid> prefab_uuids;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(assets, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        const std::string rel = std::filesystem::relative(it->path(), assets).generic_string();
        if (it->path().extension() == scene::kPrefabExtension) {
            prefab_files.push_back(rel);
            if (auto src = script::read_prefab(assets, rel); src && !src->asset.is_nil()) prefab_uuids.insert(src->asset);
            continue;
        }
        if (it->path().extension() != ".luau" && it->path().extension() != ".vgraph") continue;
        ++report.scripts;
        auto raw = fs::read_text(it->path());
        auto text = raw ? script::luau_source(rel, std::move(*raw)) : Result<std::string>(std::move(raw).error());
        if (!text) {
            sink.error(Category::Script, rel, rel + ": " + text.error().message, "Open the script and fix the reported error.");
        } else if (auto st = script::check_syntax(*text, rel); !st) {
            sink.error(Category::Script, rel, st.error().message, "Fix the syntax error at the reported line.");
        } else {
            for (const std::string& target : script::scene_references(*text))
                if (std::find(project_scenes.begin(), project_scenes.end(), target) == project_scenes.end())
                    sink.error(Category::Script, rel,
                               rel + ": scene.load(\"" + target + "\") names a scene that does not exist",
                               "Create the scene under scenes/ or correct the name passed to scene.load.");
        }
    }

    // Materials: each must parse and reference existing textures (resolved through the database).
    auto db = assets::AssetDatabase::open(proj.root());
    if (!db) return std::move(db).error();
    for (const assets::AssetRecord& r : (*db)->records()) {
        if (r.kind != assets::AssetKind::Material) continue;
        ++report.materials;
        auto m = (*db)->resolve_material(r.uuid); // also checks the parent chain
        if (!m) {
            sink.error(Category::Material, r.path, m.error().message, "Fix the material file or its parent material.").asset = r.uuid;
            continue;
        }
        const auto refs = m->textures();
        for (std::size_t i = 0; i < refs.size(); ++i) {
            if (refs[i]->is_nil()) continue;
            const assets::AssetRecord* t = (*db)->find(*refs[i]);
            if (!t || t->kind != assets::AssetKind::Texture)
                sink.error(Category::Material, r.path,
                           r.path + ": " + assets::kMaterialTextureSlotNames[i] + " texture " + refs[i]->to_string() +
                               " does not exist",
                           "Assign an existing texture in the Material Editor or clear the slot.")
                    .asset = *refs[i];
        }
        if (!m->shader.is_nil()) {
            const assets::AssetRecord* sh = (*db)->find(m->shader);
            if (!sh || sh->kind != assets::AssetKind::Shader)
                sink.error(Category::Material, r.path,
                           r.path + ": surface shader " + m->shader.to_string() + " does not exist",
                           "Assign an existing surface shader or clear the material's shader.")
                    .asset = m->shader;
        }
    }
    // Surface shaders: each must compile; errors name the file and line.
    for (const assets::AssetRecord& r : (*db)->records()) {
        if (r.kind != assets::AssetKind::Shader) continue;
        ++report.shaders;
        const std::string path = r.path;
        auto code = runtime::load_surface_shader_code(**db, r.uuid);
        if (!code)
            sink.error(Category::Shader, path, with_path(path, code.error().message), "Fix the shader source or graph.").asset = r.uuid;
        else if (auto c = runtime::compile_surface_shader(*code, path, false); !c)
            sink.error(Category::Shader, path, c.error().message, "Fix the compile error at the reported line.").asset = r.uuid;
    }
    // Sprite sheets and tile sets: each must parse and point at an existing texture.
    for (const assets::AssetRecord& r : (*db)->records()) {
        if (r.kind != assets::AssetKind::SpriteSheet) continue;
        ++report.sprite_sheets;
        if (auto sheet = (*db)->load_sprite_sheet(r.uuid); !sheet)
            sink.error(Category::SpriteSheet, r.path, sheet.error().message, "Fix the sprite sheet or point it at an existing texture.")
                .asset = r.uuid;
    }
    for (const assets::AssetRecord& r : (*db)->records()) {
        if (r.kind != assets::AssetKind::Tileset) continue;
        ++report.tilesets;
        if (auto ts = (*db)->load_tileset(r.uuid); !ts)
            sink.error(Category::Tileset, r.path, ts.error().message, "Fix the tile set or point it at an existing texture.").asset =
                r.uuid;
    }
    // Terrains: each heightfield must load.
    for (const assets::AssetRecord& r : (*db)->records()) {
        if (r.kind != assets::AssetKind::Terrain) continue;
        if (auto t = (*db)->load_terrain(r.uuid); !t)
            sink.error(Category::Terrain, r.path, t.error().message, "Restore the .vterrain file from version control or recreate it.")
                .asset = r.uuid;
    }
    // Behaviour trees: each must parse.
    for (const assets::AssetRecord& r : (*db)->records()) {
        if (r.kind != assets::AssetKind::BehaviorTree) continue;
        auto text = fs::read_text((*db)->source_path(r));
        auto t = text ? ai::BehaviorTree::parse(*text, r.path) : Result<std::shared_ptr<const ai::BehaviorTree>>(std::move(text).error());
        if (!t)
            sink.error(Category::BehaviorTree, r.path, r.path + ": " + t.error().message, "Fix the behaviour tree at the reported node.")
                .asset = r.uuid;
    }
    // Sequences: each must parse.
    for (const assets::AssetRecord& r : (*db)->records()) {
        if (r.kind != assets::AssetKind::Sequence) continue;
        auto text = fs::read_text((*db)->source_path(r));
        auto q = text ? sequencer::Sequence::parse(*text, r.path)
                      : Result<std::shared_ptr<const sequencer::Sequence>>(std::move(text).error());
        if (!q)
            sink.error(Category::Sequence, r.path, r.path + ": " + q.error().message, "Fix the sequence in the Sequencer panel or file.")
                .asset = r.uuid;
    }
    // Animation controllers: each must parse (states, clips, parameters and transitions).
    for (const assets::AssetRecord& r : (*db)->records()) {
        if (r.kind != assets::AssetKind::AnimController) continue;
        ++report.controllers;
        auto text = fs::read_text((*db)->source_path(r));
        auto c = text ? animation::Controller::parse(*text, r.path)
                      : Result<std::shared_ptr<const animation::Controller>>(std::move(text).error());
        if (!c)
            sink.error(Category::AnimController, r.path, r.path + ": " + c.error().message,
                       "Fix the controller's states, parameters or transitions.")
                .asset = r.uuid;
    }

    const animation::SetProvider anim_sets = assets::animation_provider(**db);
    const animation::ControllerLoader anim_loader = animation::directory_loader(assets);
    script::HostOptions so;
    so.assets_root = assets;
    so.input_map = proj.settings().input;
    auto host = script::ScriptHost::create(so);
    if (!host) return std::move(host).error();

    // Unknown components (warning), scripts that do not load and links to missing assets/prefabs.
    const auto check_entities = [&](const std::string& rel, scene::Scene& scene) {
        const auto entity = [&](Severity s, scene::Entity e, std::string message, std::string fix, Uuid asset = {}) {
            Diagnostic& d = sink.add(s, Category::Entity, rel, std::move(message), std::move(fix));
            d.entity_name = entity_name(scene, e);
            d.entity_uuid = scene.uuid(e);
            d.asset = asset;
        };
        const auto who = [&](scene::Entity e) { return rel + ": entity '" + entity_name(scene, e) + "'"; };
        scene.world().each<scene::UnknownComponents>([&](scene::Entity e, scene::UnknownComponents& u) {
            for (const auto& [name, value] : u.components) {
                entity(Severity::Warning, e, who(e) + " has unknown component '" + name + "'",
                       "Enable the module or plugin that defines it, or remove the component; it is kept as saved.");
                (void)value;
            }
        });
        scene.world().each<script::ScriptComponent>([&](scene::Entity e, script::ScriptComponent& c) {
            if (c.script.empty()) return;
            if (auto d = (*host)->describe(c.script); !d)
                entity(Severity::Error, e, who(e) + ": " + d.error().message, "Fix the script or assign an existing one.");
        });
        scene.world().each<scene::MeshRendererComponent>([&](scene::Entity e, scene::MeshRendererComponent& c) {
            if (c.material_asset.is_nil()) return;
            const assets::AssetRecord* m = (*db)->find(c.material_asset);
            if (!m || m->kind != assets::AssetKind::Material)
                entity(Severity::Error, e, who(e) + " uses missing material " + c.material_asset.to_string(),
                       "Assign an existing material in the Inspector.", c.material_asset);
        });
        // Animator controllers must load; clips they name should exist in the Animator's model.
        scene.world().each<animation::AnimatorComponent>([&](scene::Entity e, animation::AnimatorComponent& c) {
            std::shared_ptr<const animation::AnimationSet> retargeted;
            if (!c.clip_source.is_nil() && c.clip_source != c.model) {
                const assets::AssetRecord* m = (*db)->find(c.clip_source);
                if (!m || m->kind != assets::AssetKind::Model) {
                    entity(Severity::Error, e, who(e) + " retargets clips from missing model " + c.clip_source.to_string(),
                           "Assign an existing model to the Animator's clip_source, or clear it.", c.clip_source);
                } else if (const auto src = anim_sets(c.clip_source), tgt = c.model.is_nil() ? nullptr : anim_sets(c.model);
                           src && tgt) {
                    retargeted = animation::retarget_set(*src, *tgt);
                    if (!retargeted)
                        entity(Severity::Warning, e,
                               who(e) + ": no bone of clip_source " + c.clip_source.to_string() + " matches the model's bones",
                               "Use a clip source whose skeleton has the same bone names.", c.clip_source);
                }
            }
            if (c.controller.empty()) return;
            auto text = anim_loader(c.controller);
            auto ctrl = text ? animation::Controller::parse(*text, c.controller)
                             : Result<std::shared_ptr<const animation::Controller>>(std::move(text).error());
            if (!ctrl) {
                entity(Severity::Error, e, who(e) + ": controller " + c.controller + ": " + ctrl.error().message,
                       "Fix the controller or assign an existing one in the Animator.");
                return;
            }
            const auto set = retargeted ? retargeted : c.model.is_nil() ? nullptr : anim_sets(c.model);
            if (!set) return;
            for (const std::string& clip : (*ctrl)->clip_names())
                if (set->find_clip(clip) < 0)
                    entity(Severity::Warning, e,
                           who(e) + ": controller " + c.controller + " uses clip '" + clip + "' that its model lacks",
                           "Rename the clip in the controller or import a model that has it.", c.model);
            const auto& states = (*ctrl)->states;
            const std::string& root = (*ctrl)->root_node;
            if (!root.empty() && std::any_of(states.begin(), states.end(), [](const auto& st) { return st.root_motion; }) &&
                std::none_of(set->nodes.begin(), set->nodes.end(), [&](const auto& n) { return n.name == root; }))
                entity(Severity::Warning, e,
                       who(e) + ": controller " + c.controller + " takes root motion from node '" + root +
                           "' that its model lacks",
                       "Set the controller's root_node to a node of the model, or turn root motion off.", c.model);
        });
        scene.world().each<animation::SpriteAnimatorComponent>([&](scene::Entity e, animation::SpriteAnimatorComponent& c) {
            if (c.sheet.is_nil()) return;
            const assets::AssetRecord* s = (*db)->find(c.sheet);
            if (!s || s->kind != assets::AssetKind::SpriteSheet)
                entity(Severity::Error, e, who(e) + " uses missing sprite sheet " + c.sheet.to_string(),
                       "Assign an existing sprite sheet in the Inspector.", c.sheet);
        });
        scene.world().each<tilemap::TilemapComponent>([&](scene::Entity e, tilemap::TilemapComponent& c) {
            if (c.tileset.is_nil()) return;
            const assets::AssetRecord* t = (*db)->find(c.tileset);
            if (!t || t->kind != assets::AssetKind::Tileset)
                entity(Severity::Error, e, who(e) + " uses missing tile set " + c.tileset.to_string(),
                       "Assign an existing tile set in the Inspector.", c.tileset);
        });
        scene.world().each<terrain::TerrainComponent>([&](scene::Entity e, terrain::TerrainComponent& c) {
            const assets::AssetRecord* t = c.data.is_nil() ? nullptr : (*db)->find(c.data);
            if (!t || t->kind != assets::AssetKind::Terrain)
                entity(Severity::Error, e, who(e) + " uses missing terrain data " + c.data.to_string(),
                       "Assign existing .vterrain data or recreate the terrain.", c.data);
        });
        scene.world().each<ai::BehaviorTreeComponent>([&](scene::Entity e, ai::BehaviorTreeComponent& c) {
            const assets::AssetRecord* t = (*db)->find_path(c.tree);
            if (!t || t->kind != assets::AssetKind::BehaviorTree)
                entity(Severity::Error, e, who(e) + " uses missing behaviour tree '" + c.tree + "'",
                       "Assign an existing .vbt behaviour tree in the Inspector.");
        });
        scene.world().each<sequencer::SequencePlayerComponent>([&](scene::Entity e, sequencer::SequencePlayerComponent& c) {
            if (c.sequence.empty()) return; // assigned at runtime (Luau play_sequence)
            const assets::AssetRecord* t = (*db)->find_path(c.sequence);
            if (!t || t->kind != assets::AssetKind::Sequence)
                entity(Severity::Error, e, who(e) + " uses missing sequence '" + c.sequence + "'",
                       "Assign an existing .vseq sequence or clear the field.");
        });
        scene.world().each<scene::PrefabInstance>([&](scene::Entity e, scene::PrefabInstance& pi) {
            if (!prefab_uuids.contains(pi.prefab))
                entity(Severity::Warning, e,
                       who(e) + " is an instance of a prefab that no longer exists (" + pi.prefab.to_string() +
                           "); it keeps its saved content",
                       "Unpack the instance or restore the prefab file.", pi.prefab);
        });
    };
    for (const auto& rel : prefab_files) {
        ++report.prefabs;
        auto src = script::read_prefab(assets, rel);
        if (!src) {
            sink.error(Category::Prefab, rel, src.error().message, "Fix or restore the prefab file.");
            continue;
        }
        scene::Scene scene;
        if (auto e = scene::instantiate(scene, src->prefab, src->asset); !e) {
            sink.error(Category::Prefab, rel, rel + ": " + e.error().message, "Fix or restore the prefab file.").asset = src->asset;
            continue;
        }
        check_entities(rel, scene);
    }
    for (const auto& rel : proj.scenes()) {
        ++report.scenes;
        auto path = proj.resolve(rel);
        scene::Scene scene;
        if (!path) {
            sink.error(Category::Scene, rel, rel + ": " + path.error().message, "Move the scene under the project's scenes/ folder.");
            continue;
        }
        if (auto st = scene.load_file(*path); !st) {
            sink.error(Category::Scene, rel, rel + ": " + st.error().message, "Fix or restore the scene file from version control.");
            continue;
        }
        check_entities(rel, scene);
    }
    return report;
}

Result<ValidationReport> validate_project(const fs::Path& project_path) {
    project::register_components();
    auto proj = project::Project::open(project_path);
    if (!proj) return std::move(proj).error();
    return validate_project(*proj);
}

} // namespace victus::player
