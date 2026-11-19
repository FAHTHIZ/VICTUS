// SPDX-License-Identifier: Apache-2.0
#include "victus/player/migrate.hpp"

#include "victus/project/project.hpp"
#include "victus/scene/prefab.hpp"
#include "victus/scene/scene.hpp"
#include "victus/world/world.hpp"

#include <algorithm>
#include <filesystem>

namespace victus::player {

std::size_t MigrationScan::pending() const noexcept {
    return static_cast<std::size_t>(std::count_if(entries.begin(), entries.end(), [](const MigrationEntry& e) {
        return e.error.empty() && !e.written && !e.notes.empty();
    }));
}

std::size_t MigrationScan::failures() const noexcept {
    return static_cast<std::size_t>(std::count_if(entries.begin(), entries.end(), [](const MigrationEntry& e) { return !e.error.empty(); }));
}

namespace {

// Whole-document formats without component data: upgrade the JSON and write it back as is, so
// fields this build does not know survive.
void migrate_document(const fs::Path& path, const migration::Format& format, bool write, MigrationEntry& out) {
    auto text = fs::read_text(path, 64u << 20);
    if (!text) {
        out.error = text.error().message;
        return;
    }
    auto doc = json::parse(*text);
    if (!doc) {
        out.error = doc.error().message;
        return;
    }
    auto report = format.upgrade(*doc);
    if (!report) {
        out.error = report.error().message;
        return;
    }
    out.from = report->from;
    out.to = report->to;
    out.notes = report->applied;
    if (!write || out.notes.empty()) return;
    if (auto st = fs::keep_premigration_copy(path); !st) {
        out.error = st.error().message;
        return;
    }
    if (auto st = fs::write_safely(path, json::stringify(*doc, true) + "\n", {.validate = json::document_validator(format.name())}); !st) {
        out.error = st.error().message;
        return;
    }
    out.written = true;
}

std::uint32_t stored_version(const fs::Path& path) {
    auto text = fs::read_text(path, 256u << 20);
    if (!text) return 0;
    auto doc = json::parse(*text);
    return doc ? static_cast<std::uint32_t>(std::max<std::int64_t>(0, (*doc)["version"].as_int(0))) : 0;
}

} // namespace

Result<MigrationScan> migrate_project(const fs::Path& project_dir, bool write) {
    const fs::Path& root = project_dir;
    const fs::Path project_file = root / project::kProjectFileName;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(project_file, ec))
        return make_error(ErrorCode::NotFound, project_file.string() + " not found");
    project::register_components();
    MigrationScan scan;
    auto record = [&](MigrationEntry e) {
        ++scan.scanned;
        if (!e.notes.empty() || !e.error.empty()) scan.entries.push_back(std::move(e));
    };
    {
        MigrationEntry e{std::string(project::kProjectFileName), project::project_format().name()};
        migrate_document(project_file, project::project_format(), write, e);
        record(std::move(e));
    }
    std::vector<fs::Path> files;
    for (auto it = std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec) && (name.starts_with(".") || name == "build" || name == "node_modules")) {
            it.disable_recursion_pending();
            continue;
        }
        const std::string ext = it->path().extension().string();
        if (ext == ".vscene" || ext == ".vprefab" || ext == ".vworld") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    for (const fs::Path& path : files) {
        const std::string ext = path.extension().string();
        MigrationEntry e;
        e.file = path.lexically_relative(root).generic_string();
        if (ext == ".vworld") {
            e.format = world::world_format().name();
            migrate_document(path, world::world_format(), write, e);
        } else if (ext == ".vscene") {
            e.format = scene::scene_format().name();
            e.from = stored_version(path);
            e.to = scene::scene_format().current();
            scene::Scene s;
            if (auto st = s.load_file(path); !st) {
                e.error = st.error().message;
            } else {
                e.notes = s.migrations();
                if (write && !e.notes.empty()) {
                    if (auto w = s.save_file(path); !w) e.error = w.error().message;
                    else e.written = true;
                }
            }
        } else {
            e.format = scene::prefab_format().name();
            e.from = stored_version(path);
            e.to = scene::prefab_format().current();
            auto prefab = scene::load_prefab_file(path);
            if (!prefab) {
                e.error = prefab.error().message;
            } else {
                e.notes = prefab->migrations;
                if (write && !e.notes.empty()) {
                    if (auto w = scene::save_prefab_file(*prefab, path); !w) e.error = w.error().message;
                    else e.written = true;
                }
            }
        }
        record(std::move(e));
    }
    return scan;
}

} // namespace victus::player
