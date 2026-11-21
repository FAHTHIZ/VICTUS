// SPDX-License-Identifier: Apache-2.0
// Bringing outside files into a project: drag and drop onto the Studio window, or Assets > Import.
#include "victus/editor/editor.hpp"

#include "victus/assets/database.hpp"
#include "victus/core/json.hpp"
#include "victus/core/log.hpp"
#include "victus/platform/platform.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <sstream>

namespace victus::editor {

namespace sfs = std::filesystem;

namespace {

constexpr std::uintmax_t kMaxGltfBytes = 64ull << 20; // only the JSON is read, to find companions

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const char* default_folder(assets::AssetKind kind) {
    switch (kind) {
    case assets::AssetKind::Texture:
        return "textures";
    case assets::AssetKind::Model:
        return "models";
    case assets::AssetKind::Audio:
        return "audio";
    case assets::AssetKind::Script:
        return "scripts";
    case assets::AssetKind::Material:
        return "materials";
    case assets::AssetKind::Shader:
        return "shaders";
    case assets::AssetKind::Scene:
        return "scenes";
    case assets::AssetKind::Prefab:
        return "prefabs";
    default:
        return "";
    }
}

std::string unsupported_reason(const sfs::path& p) {
    const std::string ext = lower(p.extension().string());
    if (ext == ".blend" || ext == ".dae" || ext == ".3ds" || ext == ".max" || ext == ".ma" || ext == ".mb")
        return std::format("{}: {} models are not supported yet. Export it as glTF (.glb) or FBX - Blender: File > "
                           "Export > glTF 2.0.",
                           p.filename().string(), ext.substr(1));
    if (ext == ".psd" || ext == ".gif" || ext == ".webp" || ext == ".tif" || ext == ".tiff" || ext == ".exr")
        return std::format("{}: save it as PNG or JPG first.", p.filename().string());
    if (ext == ".aiff" || ext == ".aif" || ext == ".m4a" || ext == ".aac" || ext == ".wma")
        return std::format("{}: save it as WAV, OGG, MP3 or FLAC first.", p.filename().string());
    if (ext.empty()) return std::format("{}: files without an extension cannot be imported.", p.filename().string());
    return std::format("{}: VICTUS does not use {} files.", p.filename().string(), ext);
}

// A free name in `dir`: "robot.glb", then "robot 2.glb", "robot 3.glb"...
sfs::path free_name(const sfs::path& dir, const sfs::path& name) {
    std::error_code ec;
    sfs::path out = dir / name;
    for (int n = 2; sfs::exists(out, ec); ++n)
        out = dir / std::format("{} {}{}", name.stem().string(), n, name.extension().string());
    return out;
}

// Relative files a .gltf points at (buffers and images), ignoring data: URIs and anything
// leaving the model's folder.
std::vector<std::string> gltf_companions(const sfs::path& gltf) {
    std::vector<std::string> out;
    std::error_code ec;
    if (sfs::file_size(gltf, ec) > kMaxGltfBytes || ec) return out;
    std::ifstream in(gltf, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    auto doc = json::parse(ss.str());
    if (!doc) return out;
    for (const char* key : {"buffers", "images"}) {
        const json::Value* list = doc->find(key);
        if (!list || !list->is_array()) continue;
        for (std::size_t i = 0; i < list->size(); ++i) {
            const json::Value* uri = (*list)[i].find("uri");
            if (!uri || !uri->is_string()) continue;
            std::string u = uri->as_string();
            if (u.starts_with("data:") || u.find("://") != std::string::npos) continue;
            // %20 is the only escape exporters commonly write.
            for (std::size_t p; (p = u.find("%20")) != std::string::npos;)
                u.replace(p, 3, " ");
            const sfs::path rel = sfs::path(u).lexically_normal();
            if (rel.is_absolute() || rel.empty() || *rel.begin() == "..") continue;
            out.push_back(rel.generic_string());
        }
    }
    return out;
}

// Relative files a Wavefront .obj needs: its `mtllib` material libraries and the texture maps
// (`map_*`, `bump`, `disp`, `norm`) those reference. The map's file name is the last token.
std::vector<std::string> obj_companions(const sfs::path& obj) {
    std::vector<std::string> out;
    const auto relative = [](std::string u) -> std::string {
        std::replace(u.begin(), u.end(), '\\', '/');
        const sfs::path rel = sfs::path(u).lexically_normal();
        if (rel.is_absolute() || rel.empty() || *rel.begin() == "..") return {};
        return rel.generic_string();
    };
    const auto lines = [](const sfs::path& p, auto&& fn) {
        std::error_code ec;
        if (sfs::file_size(p, ec) > kMaxGltfBytes || ec) return;
        std::ifstream in(p, std::ios::binary);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto b = line.find_first_not_of(" \t");
            if (b == std::string::npos) continue;
            fn(std::string_view(line).substr(b));
        }
    };
    std::vector<std::string> libs;
    lines(obj, [&](std::string_view l) {
        if (!l.starts_with("mtllib ")) return;
        const auto b = l.find_first_not_of(" \t", 7);
        if (b == std::string_view::npos) return;
        if (std::string r = relative(std::string(l.substr(b))); !r.empty()) libs.push_back(std::move(r));
    });
    for (const std::string& lib : libs) {
        out.push_back(lib);
        lines(obj.parent_path() / lib, [&](std::string_view l) {
            const std::string key = lower(std::string(l.substr(0, l.find_first_of(" \t"))));
            if (!(key.starts_with("map_") || key == "bump" || key == "disp" || key == "norm" || key == "refl")) return;
            const auto e = l.find_last_not_of(" \t");
            const auto b = l.find_last_of(" \t", e);
            if (b == std::string_view::npos) return;
            if (std::string r = relative(std::string(l.substr(b + 1, e - b))); !r.empty()) out.push_back(std::move(r));
        });
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    std::error_code ec;
    std::erase_if(out, [&](const std::string& r) { return !sfs::is_regular_file(obj.parent_path() / r, ec); });
    return out;
}

bool inside(const sfs::path& child, const sfs::path& parent) {
    std::error_code ec;
    const sfs::path c = sfs::weakly_canonical(child, ec), p = sfs::weakly_canonical(parent, ec);
    const auto rel = c.lexically_relative(p);
    return !rel.empty() && *rel.begin() != "..";
}

} // namespace

Editor::ImportedFiles Editor::import_files(const std::vector<sfs::path>& sources) {
    ImportedFiles result;
    if (!project_ || !db_) {
        result.skipped.push_back("Open a project first, then drop files on it.");
        return result;
    }
    const sfs::path assets = project_->assets_dir();
    std::vector<sfs::path> written;
    std::error_code ec;
    for (const sfs::path& src : sources) {
        if (!sfs::exists(src, ec)) {
            result.skipped.push_back(std::format("{}: not found.", src.string()));
            continue;
        }
        if (inside(src, assets)) {
            result.skipped.push_back(std::format("{}: already in this project.", src.filename().string()));
            continue;
        }
        if (sfs::is_directory(src, ec)) {
            // A whole folder (e.g. a model with its textures): copied as it is, as one folder.
            const sfs::path base = asset_folder_.empty() ? assets : assets / asset_folder_;
            sfs::create_directories(base, ec);
            const sfs::path dest = free_name(base, src.filename());
            sfs::copy(src, dest, sfs::copy_options::recursive, ec);
            if (ec) {
                result.skipped.push_back(
                    std::format("{}: could not copy ({}).", src.filename().string(), ec.message()));
                continue;
            }
            written.push_back(dest);
            continue;
        }
        const assets::AssetKind kind = assets::kind_for_extension(src.extension().string());
        if (kind == assets::AssetKind::Unknown) {
            result.skipped.push_back(unsupported_reason(src));
            continue;
        }
        const sfs::path dir = asset_folder_.empty() ? assets / default_folder(kind) : assets / asset_folder_;
        sfs::create_directories(dir, ec);
        sfs::path dest = free_name(dir, src.filename());
        const std::string ext = lower(src.extension().string());
        const auto companions = ext == ".gltf" ? gltf_companions(src)
                                : ext == ".obj" ? obj_companions(src)
                                                : std::vector<std::string>{};
        if (!companions.empty()) {
            // The .gltf/.obj finds its files by relative path: keep them together in their own folder.
            const sfs::path folder = free_name(dir, src.stem());
            sfs::create_directories(folder, ec);
            dest = folder / src.filename();
            for (const std::string& c : companions) {
                const sfs::path from = src.parent_path() / c;
                const sfs::path to = folder / c;
                sfs::create_directories(to.parent_path(), ec);
                sfs::copy_file(from, to, sfs::copy_options::skip_existing, ec);
                if (ec)
                    result.skipped.push_back(std::format("{}: the model needs {}, which could not be copied ({}).",
                                                         src.filename().string(), c, ec.message()));
            }
        }
        sfs::copy_file(src, dest, ec);
        if (ec) {
            result.skipped.push_back(std::format("{}: could not copy ({}).", src.filename().string(), ec.message()));
            continue;
        }
        written.push_back(dest);
    }
    if (written.empty()) {
        for (const auto& s : result.skipped)
            notify(s, true);
        return result;
    }

    const auto refresh = db_->refresh();
    for (const auto& e : refresh.errors)
        notify("asset metadata: " + e, true);
    for (const auto& rec : db_->records()) {
        const sfs::path full = assets / rec.path;
        const bool ours = std::any_of(written.begin(), written.end(),
                                      [&](const sfs::path& w) { return full == w || inside(full, w); });
        if (!ours) continue;
        if (!rec.importer.empty()) {
            std::vector<std::string> messages;
            if (auto r = db_->import(rec.uuid, &messages); !r)
                result.skipped.push_back(
                    std::format("{}: copied, but the import failed: {}", rec.path, r.error().message));
        }
        result.added.push_back(rec.path);
    }
    std::sort(result.added.begin(), result.added.end());
    if (!result.added.empty()) {
        // Show where they went: clear filters that would hide them.
        asset_search_.clear();
        asset_kind_filter_.reset();
        show_panel(Panel::Assets);
        notify(result.added.size() == 1 ? std::format("Added {}", result.added.front())
                                        : std::format("Added {} assets", result.added.size()));
    }
    for (const auto& s : result.skipped)
        notify(s, true);
    return result;
}

void Editor::open_import_dialog() {
    if (!project_ || !platform_) return;
    platform_->show_open_files_dialog(window_, {}, [this](std::vector<std::string> files) {
        if (files.empty()) return;
        std::vector<sfs::path> paths(files.begin(), files.end());
        (void)import_files(paths);
    });
}

} // namespace victus::editor
