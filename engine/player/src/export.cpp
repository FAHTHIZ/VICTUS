// SPDX-License-Identifier: Apache-2.0
#include "victus/player/export.hpp"
#include "victus/plugin/plugin.hpp"
#include "victus/animation/sprite.hpp"

#include "victus/assets/database.hpp"
#include "victus/audio/audio.hpp"
#include "victus/core/build_info.hpp"
#include "victus/core/hash.hpp"
#include "victus/core/json.hpp"
#include "victus/core/log.hpp"
#include "victus/core/pack.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/project/project.hpp"
#include "victus/terrain/terrain.hpp"
#include "victus/runtime/surface_shader.hpp"
#include "victus/scene/scene.hpp"
#include "victus/ai/ai.hpp"
#include "victus/sequencer/sequencer.hpp"
#include "victus/script/script.hpp"
#include "victus/world/world.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <set>

namespace victus::player {
namespace {

namespace sfs = std::filesystem;

std::string entity_label(const scene::Scene& s, scene::Entity e) {
    const auto* n = s.world().get<scene::Name>(e);
    return n ? n->value : std::string("<unnamed>");
}

fs::Path default_player() {
#if defined(_WIN32)
    return fs::executable_dir() / "victus_player.exe";
#else
    return fs::executable_dir() / "victus_player";
#endif
}

} // namespace

std::string executable_base_name(std::string_view project_name) {
    // Letters, digits and '-' are kept; '.' only between two alphanumerics (so "v1.2" survives
    // but "../x" cannot form a relative path); every other run of characters becomes one '_'.
    const auto alnum = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
    std::string out;
    bool pending_sep = false;
    for (std::size_t i = 0; i < project_name.size(); ++i) {
        const char c = project_name[i];
        const bool keep = alnum(c) || c == '-' ||
                          (c == '.' && i > 0 && i + 1 < project_name.size() && alnum(project_name[i - 1]) && alnum(project_name[i + 1]));
        if (!keep) {
            pending_sep = true;
            continue;
        }
        if (pending_sep && !out.empty()) out.push_back('_');
        pending_sep = false;
        out.push_back(c);
    }
    if (out.size() > 64) out.resize(64);
    return out.empty() ? std::string("Game") : out;
}

Result<ExportReport> export_game(const ExportOptions& o) {
    project::register_components();
    auto proj = project::Project::open(o.project);
    if (!proj) return std::move(proj).error();
    if (o.output.empty()) return make_error(ErrorCode::InvalidArgument, "export needs an output folder");

    // Refuse to write into an unrelated non-empty folder; re-exporting over a previous export is fine.
    std::error_code ec;
    const fs::Path out = sfs::absolute(o.output, ec).lexically_normal();
    if (const std::string inside = out.lexically_relative(proj->root()).generic_string();
        !inside.empty() && !inside.starts_with(".."))
        return make_error(ErrorCode::InvalidArgument, "export folder must be outside the project folder");
    if (sfs::exists(out, ec)) {
        if (!sfs::is_directory(out, ec)) return make_error(ErrorCode::AlreadyExists, out.string() + " exists and is not a folder");
        if (!sfs::is_empty(out, ec) && !sfs::is_regular_file(out / "export.json", ec))
            return make_error(ErrorCode::AlreadyExists, "export folder is not empty and holds no previous export: " + out.string());
    }

    ExportReport report;
    auto db = assets::AssetDatabase::open(proj->root());
    if (!db) return std::move(db).error();
    const auto imported = (*db)->import_all();
    if (imported.failed) {
        std::string msg = std::format("{} asset(s) failed to import", imported.failed);
        for (const auto& m : imported.messages) msg += "\n  " + m;
        return make_error(ErrorCode::ParseError, msg);
    }
    for (const auto& m : imported.messages) report.warnings.push_back(m);

    pack::PackWriter pw;
    std::vector<std::string> errors;
    VICTUS_TRY_STATUS(pw.add_file("victus.project", proj->file()));

    // Assets: cooked form for imported kinds, verbatim bytes for everything else.
    std::set<std::string> asset_paths;
    const std::vector<std::string> project_scenes = proj->scenes(); // targets of scene.load
    for (const assets::AssetRecord& r : (*db)->records()) {
        asset_paths.insert(r.path);
        if (r.importer == "texture" || r.importer == "gltf") {
            const fs::Path cooked = (*db)->cooked_path(r.uuid);
            if (auto st = pw.add_file(std::format(".victus/cache/{}.vcooked", r.uuid.to_string()), cooked); !st) {
                errors.push_back(std::format("{}: cooked data unavailable: {}", r.path, st.error().message));
                continue;
            }
            ++report.cooked_assets;
            continue;
        }
        const fs::Path src = (*db)->source_path(r);
        if (r.kind == assets::AssetKind::Script) {
            auto raw = fs::read_text(src);
            if (!raw) {
                errors.push_back(std::format("{}: {}", r.path, raw.error().message));
                continue;
            }
            auto text = script::luau_source(r.path, std::move(*raw));
            if (!text) {
                errors.push_back(text.error().message);
                continue;
            }
            if (auto st = script::check_syntax(*text, r.path); !st) {
                errors.push_back(st.error().message);
                continue;
            }
            for (const std::string& target : script::scene_references(*text))
                if (std::find(project_scenes.begin(), project_scenes.end(), target) == project_scenes.end())
                    errors.push_back(std::format("{}: scene.load(\"{}\") names a scene that does not exist", r.path, target));
        }
        if (auto st = pw.add_file("assets/" + r.path, src); !st) errors.push_back(std::format("{}: {}", r.path, st.error().message));
    }
    VICTUS_TRY_STATUS(pw.add_text(".victus/assets.index.json", json::stringify((*db)->index_json(), false)));

    // Prefabs: every prefab must load; its scripts must exist.
    for (const assets::AssetRecord& r : (*db)->records()) {
        if (r.kind != assets::AssetKind::Prefab) continue;
        auto src = script::read_prefab((*db)->assets_dir(), r.path);
        if (!src) {
            errors.push_back(src.error().message);
            continue;
        }
        scene::Scene scene;
        if (auto e = scene::instantiate(scene, src->prefab, r.uuid); !e) {
            errors.push_back(std::format("{}: {}", r.path, e.error().message));
            continue;
        }
        scene.world().each<script::ScriptComponent>([&](scene::Entity e, script::ScriptComponent& c) {
            if (!c.script.empty() && !asset_paths.contains(c.script))
                errors.push_back(std::format("{}: entity '{}' uses missing script '{}'", r.path, entity_label(scene, e), c.script));
        });
    }

    // Materials: every material must parse; texture references must name texture assets.
    for (const assets::AssetRecord& r : (*db)->records()) {
        if (r.kind != assets::AssetKind::Material) continue;
        auto m = (*db)->resolve_material(r.uuid); // also checks the parent chain
        if (!m) {
            errors.push_back(m.error().message);
            continue;
        }
        const auto refs = m->textures();
        for (std::size_t i = 0; i < refs.size(); ++i) {
            if (refs[i]->is_nil()) continue;
            const assets::AssetRecord* t = (*db)->find(*refs[i]);
            if (!t || t->kind != assets::AssetKind::Texture)
                errors.push_back(std::format("{}: {} texture {} does not exist", r.path, assets::kMaterialTextureSlotNames[i],
                                             refs[i]->to_string()));
        }
        if (!m->shader.is_nil()) {
            const assets::AssetRecord* sh = (*db)->find(m->shader);
            if (!sh || sh->kind != assets::AssetKind::Shader)
                errors.push_back(std::format("{}: surface shader {} does not exist", r.path, m->shader.to_string()));
        }
    }

    // Surface shaders: every one must compile (for every backend the game may run on).
    for (const assets::AssetRecord& r : (*db)->records()) {
        if (r.kind != assets::AssetKind::Shader) continue;
        const std::string path = r.path;
        auto code = runtime::load_surface_shader_code(**db, r.uuid);
        if (!code) {
            const std::string& msg = code.error().message;
            errors.push_back(msg.starts_with(path) ? msg : std::format("{}: {}", path, msg));
            continue;
        }
        if (auto c = runtime::compile_surface_shader(*code, path, true); !c) errors.push_back(c.error().message);
    }

    // Sprite sheets: every one must parse and point at an existing texture.
    for (const assets::AssetRecord& r : (*db)->records()) {
        if (r.kind != assets::AssetKind::SpriteSheet) continue;
        if (auto sheet = (*db)->load_sprite_sheet(r.uuid); !sheet) errors.push_back(sheet.error().message);
    }

    // Tile sets: every one must parse and point at an existing texture.
    for (const assets::AssetRecord& r : (*db)->records()) {
        if (r.kind != assets::AssetKind::Tileset) continue;
        if (auto ts = (*db)->load_tileset(r.uuid); !ts) errors.push_back(ts.error().message);
    }

    // Terrains: every heightfield must load (header, size and checksum).
    for (const assets::AssetRecord& r : (*db)->records()) {
        if (r.kind != assets::AssetKind::Terrain) continue;
        if (auto t = (*db)->load_terrain(r.uuid); !t) errors.push_back(t.error().message);
    }

    // Behaviour trees: every one must parse.
    for (const assets::AssetRecord& r : (*db)->records()) {
        if (r.kind != assets::AssetKind::BehaviorTree) continue;
        auto text = fs::read_text((*db)->source_path(r));
        if (!text) errors.push_back(std::format("{}: {}", r.path, text.error().message));
        else if (auto t = ai::BehaviorTree::parse(*text, r.path); !t) errors.push_back(std::format("{}: {}", r.path, t.error().message));
    }

    // Sequences: every one must parse.
    for (const assets::AssetRecord& r : (*db)->records()) {
        if (r.kind != assets::AssetKind::Sequence) continue;
        auto text = fs::read_text((*db)->source_path(r));
        if (!text) errors.push_back(std::format("{}: {}", r.path, text.error().message));
        else if (auto q = sequencer::Sequence::parse(*text, r.path); !q) errors.push_back(std::format("{}: {}", r.path, q.error().message));
    }

    // Animation controllers: every one must parse.
    for (const assets::AssetRecord& r : (*db)->records()) {
        if (r.kind != assets::AssetKind::AnimController) continue;
        auto text = fs::read_text((*db)->source_path(r));
        if (!text) errors.push_back(std::format("{}: {}", r.path, text.error().message));
        else if (auto c = animation::Controller::parse(*text, r.path); !c) errors.push_back(std::format("{}: {}", r.path, c.error().message));
    }

    // Scenes: every scene must load; references to scripts and audio clips must resolve.
    std::vector<std::string> scenes = proj->scenes();
    if (std::find(scenes.begin(), scenes.end(), proj->settings().startup_scene) == scenes.end())
        scenes.push_back(proj->settings().startup_scene);
    std::set<std::string> packed_worlds;
    std::set<std::string> packed_lightmaps;
    for (std::size_t scene_index = 0; scene_index < scenes.size(); ++scene_index) {
        const std::string rel = scenes[scene_index]; // copy: streamed worlds append their cells
        auto path = proj->resolve(rel);
        if (!path) {
            errors.push_back(std::format("{}: {}", rel, path.error().message));
            continue;
        }
        scene::Scene scene;
        if (auto st = scene.load_file(*path); !st) {
            errors.push_back(std::format("{}: {}", rel, st.error().message));
            continue;
        }
        scene.world().each<scene::UnknownComponents>([&](scene::Entity e, scene::UnknownComponents& u) {
            for (const auto& [name, value] : u.components) {
                (void)value;
                report.warnings.push_back(std::format("{}: entity '{}' has unknown component '{}'", rel, entity_label(scene, e), name));
            }
        });
        scene.world().each<script::ScriptComponent>([&](scene::Entity e, script::ScriptComponent& c) {
            if (!c.script.empty() && !asset_paths.contains(c.script))
                errors.push_back(std::format("{}: entity '{}' uses missing script '{}'", rel, entity_label(scene, e), c.script));
        });
        scene.world().each<scene::MeshRendererComponent>([&](scene::Entity e, scene::MeshRendererComponent& c) {
            if (c.material_asset.is_nil()) return;
            const assets::AssetRecord* m = (*db)->find(c.material_asset);
            if (!m || m->kind != assets::AssetKind::Material)
                errors.push_back(std::format("{}: entity '{}' uses missing material {}", rel, entity_label(scene, e),
                                             c.material_asset.to_string()));
        });
        scene.world().each<animation::AnimatorComponent>([&](scene::Entity e, animation::AnimatorComponent& c) {
            if (!c.clip_source.is_nil()) {
                const assets::AssetRecord* m = (*db)->find(c.clip_source);
                if (!m || m->kind != assets::AssetKind::Model)
                    errors.push_back(std::format("{}: entity '{}' retargets clips from missing model {}", rel,
                                                 entity_label(scene, e), c.clip_source.to_string()));
            }
            if (c.controller.empty()) return;
            const assets::AssetRecord* r = (*db)->find_path(c.controller);
            if (!r || r->kind != assets::AssetKind::AnimController)
                errors.push_back(std::format("{}: entity '{}' uses missing animation controller '{}'", rel,
                                             entity_label(scene, e), c.controller));
        });
        scene.world().each<animation::SpriteAnimatorComponent>([&](scene::Entity e, animation::SpriteAnimatorComponent& c) {
            if (c.sheet.is_nil()) return;
            const assets::AssetRecord* s = (*db)->find(c.sheet);
            if (!s || s->kind != assets::AssetKind::SpriteSheet)
                errors.push_back(std::format("{}: entity '{}' uses missing sprite sheet {}", rel, entity_label(scene, e),
                                             c.sheet.to_string()));
        });
        scene.world().each<tilemap::TilemapComponent>([&](scene::Entity e, tilemap::TilemapComponent& c) {
            if (c.tileset.is_nil()) return;
            const assets::AssetRecord* t = (*db)->find(c.tileset);
            if (!t || t->kind != assets::AssetKind::Tileset)
                errors.push_back(std::format("{}: entity '{}' uses missing tile set {}", rel, entity_label(scene, e),
                                             c.tileset.to_string()));
        });
        scene.world().each<terrain::TerrainComponent>([&](scene::Entity e, terrain::TerrainComponent& c) {
            const assets::AssetRecord* t = c.data.is_nil() ? nullptr : (*db)->find(c.data);
            if (!t || t->kind != assets::AssetKind::Terrain)
                errors.push_back(std::format("{}: entity '{}' uses missing terrain data {}", rel, entity_label(scene, e),
                                             c.data.to_string()));
            for (std::uint32_t i = 0; i < terrain::kLayerCount; ++i) {
                const Uuid& tex = c.layer_texture(i);
                if (tex.is_nil()) continue;
                const assets::AssetRecord* x = (*db)->find(tex);
                if (!x || x->kind != assets::AssetKind::Texture)
                    errors.push_back(std::format("{}: entity '{}' terrain layer {} uses missing texture {}", rel,
                                                 entity_label(scene, e), i, tex.to_string()));
            }
        });
        scene.world().each<audio::AudioSourceComponent>([&](scene::Entity e, audio::AudioSourceComponent& c) {
            if (!c.clip.empty() && !asset_paths.contains(c.clip))
                report.warnings.push_back(std::format("{}: entity '{}' uses missing audio clip '{}'", rel, entity_label(scene, e), c.clip));
        });
        // Baked lighting (ADR-0125): the scene's lightmap ships with it; a missing bake only
        // costs the baked lighting, so it is a warning.
        scene.world().each<gameplay::EnvironmentComponent>([&](scene::Entity e, gameplay::EnvironmentComponent& c) {
            if (c.lightmap.empty() || !packed_lightmaps.insert(c.lightmap).second) return;
            auto lpath = proj->resolve(c.lightmap);
            if (!lpath || !sfs::is_regular_file(*lpath, ec)) {
                report.warnings.push_back(std::format("{}: entity '{}' uses lightmap '{}', which is missing (bake lighting)", rel,
                                                      entity_label(scene, e), c.lightmap));
                return;
            }
            if (auto st = pw.add_file(c.lightmap, *lpath); !st) errors.push_back(std::format("{}: {}", c.lightmap, st.error().message));
        });
        // Streamed worlds (ADR-0090): the descriptor ships and every cell is checked like a scene.
        scene.world().each<world::WorldStreamingComponent>([&](scene::Entity e, world::WorldStreamingComponent& c) {
            if (c.world.empty() || !packed_worlds.insert(c.world).second) return;
            auto wpath = proj->resolve(c.world);
            if (!wpath) {
                errors.push_back(std::format("{}: entity '{}' streams world '{}': {}", rel, entity_label(scene, e), c.world,
                                             wpath.error().message));
                return;
            }
            auto desc = world::load_world_file(*wpath);
            if (!desc) {
                errors.push_back(std::format("{}: entity '{}' streams world '{}': {}", rel, entity_label(scene, e), c.world,
                                             desc.error().message));
                return;
            }
            if (auto st = pw.add_file(c.world, *wpath); !st) errors.push_back(std::format("{}: {}", c.world, st.error().message));
            for (const world::CellDesc& cell : desc->cells)
                if (std::find(scenes.begin(), scenes.end(), cell.scene) == scenes.end()) scenes.push_back(cell.scene);
        });
        if (!rel.starts_with("assets/")) VICTUS_TRY_STATUS(pw.add_file(rel, *path));
    }
    if (!errors.empty()) {
        std::string msg = std::format("export failed with {} problem(s):", errors.size());
        for (const auto& e : errors) msg += "\n  " + e;
        return make_error(ErrorCode::InvalidArgument, msg);
    }

    const fs::Path game_dir = out / std::string(kGameFolder);
    sfs::create_directories(game_dir, ec);
    if (ec) return make_error(ErrorCode::IoError, std::format("cannot create {}: {}", game_dir.string(), ec.message()));
    report.pack = game_dir / std::string(kPackFile);
    VICTUS_TRY_STATUS(pw.write(report.pack));
    report.files = pw.size();
    report.payload_bytes = pw.payload_bytes();
    report.pack_bytes = sfs::file_size(report.pack, ec);

    // Validate the archive we just wrote before declaring success.
    auto written = pack::Pack::open(report.pack);
    if (!written) return std::move(written).error();
    for (const auto& e : (*written)->entries())
        if (auto bytes = (*written)->read(e.path); !bytes) return std::move(bytes).error();

    if (o.include_player) {
        const fs::Path src = o.player_executable.empty() ? default_player() : o.player_executable;
        if (!sfs::is_regular_file(src, ec)) return make_error(ErrorCode::NotFound, "player executable not found: " + src.string());
        std::string name = o.executable_name.empty() ? executable_base_name(proj->settings().name) : executable_base_name(o.executable_name);
#if defined(_WIN32)
        name += ".exe";
#endif
        report.executable = out / name;
        sfs::copy_file(src, report.executable, sfs::copy_options::overwrite_existing, ec);
        if (ec) return make_error(ErrorCode::IoError, std::format("cannot copy player to {}: {}", report.executable.string(), ec.message()));
        sfs::permissions(report.executable, sfs::perms::owner_exec | sfs::perms::group_exec | sfs::perms::others_exec,
                         sfs::perm_options::add, ec);
    }
    // Native plugins (ADR-0078) ship as loose files next to the pack: shared libraries cannot be
    // loaded from inside an archive. Only plugins the player loads, with a library for this
    // platform, are copied.
    json::Value plugin_names = json::Value::array();
    {
        std::vector<plugin::DiscoveryIssue> issues;
        const auto manifests = plugin::discover(proj->root() / "plugins", &issues);
        for (const auto& i : issues) report.warnings.push_back(std::format("plugin {}: {}", i.path.string(), i.message));
        const fs::Path plugins_out = game_dir / "plugins";
        sfs::remove_all(plugins_out, ec);
        for (const auto& m : manifests) {
            if (!m.enabled || std::find(m.targets.begin(), m.targets.end(), "player") == m.targets.end()) continue;
            const auto lib = m.libraries.find(std::string(plugin::platform_key()));
            if (lib == m.libraries.end() || !sfs::is_regular_file(m.dir / lib->second, ec)) {
                report.warnings.push_back(std::format("plugin '{}' has no built library for {}; not exported", m.name,
                                                      plugin::platform_key()));
                continue;
            }
            const fs::Path dst = plugins_out / m.name;
            sfs::create_directories((dst / lib->second).parent_path(), ec);
            sfs::copy_file(m.manifest_path, dst / m.manifest_path.filename(), sfs::copy_options::overwrite_existing, ec);
            if (!ec) sfs::copy_file(m.dir / lib->second, dst / lib->second, sfs::copy_options::overwrite_existing, ec);
            if (ec) return make_error(ErrorCode::IoError, std::format("cannot export plugin '{}': {}", m.name, ec.message()));
            plugin_names.push(m.name);
            ++report.plugins;
        }
    }
    VICTUS_TRY_STATUS(fs::write_text_atomic(out / "THIRD_PARTY_NOTICES.txt", third_party_notices()));

    json::Value files = json::Value::array();
    for (const auto& e : (*written)->entries()) {
        json::Value f = json::Value::object();
        f.set("path", e.path);
        f.set("bytes", static_cast<double>(e.size));
        f.set("fnv1a64", std::format("{:016x}", e.hash));
        files.push(std::move(f));
    }
    json::Value manifest = json::Value::object();
    manifest.set("victus_export", 1);
    manifest.set("name", proj->settings().name);
    manifest.set("startup_scene", proj->settings().startup_scene);
    manifest.set("engine_version", build::version());
    manifest.set("engine_revision", build::git_revision());
    manifest.set("platform", std::format("{}-{}", build::platform(), build::architecture()));
    manifest.set("executable", report.executable.empty() ? std::string() : report.executable.filename().string());
    manifest.set("pack", std::format("{}/{}", kGameFolder, kPackFile));
    manifest.set("pack_bytes", static_cast<double>(report.pack_bytes));
    manifest.set("files", std::move(files));
    manifest.set("plugins", std::move(plugin_names));
    VICTUS_TRY_STATUS(fs::write_text_atomic(out / "export.json", json::stringify(manifest, true) + "\n"));
    VICTUS_LOG_INFO("export", "exported '{}' to {} ({} files, {} bytes)", proj->settings().name, out.string(), report.files,
                    report.pack_bytes);
    return report;
}

} // namespace victus::player
