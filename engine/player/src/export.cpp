// SPDX-License-Identifier: Apache-2.0
#include "victus/player/export.hpp"

#include "victus/assets/database.hpp"
#include "victus/audio/audio.hpp"
#include "victus/core/build_info.hpp"
#include "victus/core/hash.hpp"
#include "victus/core/json.hpp"
#include "victus/core/log.hpp"
#include "victus/core/pack.hpp"
#include "victus/project/project.hpp"
#include "victus/runtime/surface_shader.hpp"
#include "victus/scene/scene.hpp"
#include "victus/script/script.hpp"

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
            auto text = fs::read_text(src);
            if (!text) {
                errors.push_back(std::format("{}: {}", r.path, text.error().message));
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
        auto code = (*db)->load_shader_source(r.uuid);
        if (!code) {
            errors.push_back(std::format("{}: {}", path, code.error().message));
            continue;
        }
        if (auto c = runtime::compile_surface_shader(*code, path, true); !c) errors.push_back(c.error().message);
    }

    // Scenes: every scene must load; references to scripts and audio clips must resolve.
    std::vector<std::string> scenes = proj->scenes();
    if (std::find(scenes.begin(), scenes.end(), proj->settings().startup_scene) == scenes.end())
        scenes.push_back(proj->settings().startup_scene);
    for (const std::string& rel : scenes) {
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
        scene.world().each<audio::AudioSourceComponent>([&](scene::Entity e, audio::AudioSourceComponent& c) {
            if (!c.clip.empty() && !asset_paths.contains(c.clip))
                report.warnings.push_back(std::format("{}: entity '{}' uses missing audio clip '{}'", rel, entity_label(scene, e), c.clip));
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
    VICTUS_TRY_STATUS(fs::write_text_atomic(out / "export.json", json::stringify(manifest, true) + "\n"));
    VICTUS_LOG_INFO("export", "exported '{}' to {} ({} files, {} bytes)", proj->settings().name, out.string(), report.files,
                    report.pack_bytes);
    return report;
}

} // namespace victus::player
