// SPDX-License-Identifier: Apache-2.0
// Assets panel helpers: plain type names, search / type / folder filters and the folder list.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include "victus/core/fs.hpp"
#include "victus/ui/style.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <ranges>
#include <set>

namespace victus::editor {

namespace tk = ui::tokens;

constexpr const char* kAssetPayload = "VICTUS_ASSET"; // matches the asset browser's drag source

namespace {

std::string folder_of(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool in_folder(const std::string& path, const std::string& folder) {
    if (folder.empty()) return true;
    return path.size() > folder.size() && path.compare(0, folder.size(), folder) == 0 && path[folder.size()] == '/';
}

} // namespace

const char* Editor::asset_kind_label(assets::AssetKind kind) noexcept {
    switch (kind) {
    case assets::AssetKind::Texture:
        return "Image";
    case assets::AssetKind::Model:
        return "3D Model";
    case assets::AssetKind::Scene:
        return "Scene";
    case assets::AssetKind::Script:
        return "Script";
    case assets::AssetKind::Audio:
        return "Sound";
    case assets::AssetKind::Prefab:
        return "Prefab";
    case assets::AssetKind::Material:
        return "Material";
    case assets::AssetKind::Shader:
        return "Shader";
    case assets::AssetKind::SpriteSheet:
        return "Sprite Sheet";
    case assets::AssetKind::Tileset:
        return "Tile Set";
    case assets::AssetKind::Terrain:
        return "Terrain";
    case assets::AssetKind::BehaviorTree:
        return "Behavior Tree";
    case assets::AssetKind::Sequence:
        return "Sequence";
    case assets::AssetKind::StringTable:
        return "Translations";
    case assets::AssetKind::AnimController:
        return "Animation Controller";
    default:
        return "Other";
    }
}

bool Editor::asset_visible(const assets::AssetRecord& r) const {
    if (asset_kind_filter_ && r.kind != *asset_kind_filter_) return false;
    if (!in_folder(r.path, asset_folder_)) return false;
    return asset_search_.empty() || kit::contains_icase(r.path, asset_search_) ||
           kit::contains_icase(asset_kind_label(r.kind), asset_search_);
}

std::size_t Editor::asset_rows() const {
    if (!db_) return 0;
    const auto recs = db_->records();
    return static_cast<std::size_t>(
        std::count_if(recs.begin(), recs.end(), [&](const auto& r) { return asset_visible(r); }));
}

std::vector<std::string> Editor::asset_folders() const {
    std::set<std::string> out;
    if (db_) {
        for (const auto& r : db_->records())
            for (std::string f = folder_of(r.path); !f.empty(); f = folder_of(f))
                out.insert(f);
        // Empty folders exist only on disk; rescan them every couple of seconds (and after
        // folder changes, which reset the timer).
        const double now = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
        if (disk_folders_time_ < 0.0 || now - disk_folders_time_ > 2.0 || now < disk_folders_time_) {
            disk_folders_.clear();
            std::error_code ec;
            const std::filesystem::path root = db_->assets_dir();
            for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                const std::string name = it->path().filename().string();
                if (name.starts_with('.')) {
                    if (it->is_directory(ec)) it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_directory(ec) || disk_folders_.size() >= 4096) continue;
                disk_folders_.push_back(std::filesystem::relative(it->path(), root, ec).generic_string());
            }
            disk_folders_time_ = now;
        }
        out.insert(disk_folders_.begin(), disk_folders_.end());
    }
    return {out.begin(), out.end()};
}

Status Editor::create_asset_folder(std::string_view relative) {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project is open");
    std::string rel(relative);
    while (rel.ends_with('/')) rel.pop_back();
    auto clean = fs::sanitize_relative(rel);
    if (rel.empty() || !clean || clean->empty())
        return make_error(ErrorCode::InvalidArgument, std::format("'{}' is not a valid folder name", relative));
    for (std::string_view part : std::views::split(std::string_view(*clean), '/') | std::views::transform([](auto r) {
             return std::string_view(r.begin(), r.end());
         }))
        if (part.empty() || part.starts_with('.') || part.find_first_of("\\:*?\"<>|") != std::string_view::npos)
            return make_error(ErrorCode::InvalidArgument, std::format("'{}' is not a valid folder name (no hidden names or special characters)", relative));
    const std::filesystem::path dir = db_->assets_dir() / *clean;
    std::error_code ec;
    if (std::filesystem::exists(dir, ec)) return make_error(ErrorCode::AlreadyExists, std::format("assets/{} already exists", *clean));
    std::filesystem::create_directories(dir, ec);
    if (ec) return make_error(ErrorCode::IoError, std::format("cannot create assets/{}: {}", *clean, ec.message()));
    disk_folders_time_ = -1.0;
    asset_folder_ = *clean;
    notify("Created folder " + *clean + ". Drag assets onto it in the folder list to move them in.");
    return {};
}

Status Editor::delete_asset_folder(std::string_view relative) {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project is open");
    auto clean = fs::sanitize_relative(relative);
    if (!clean || clean->empty()) return make_error(ErrorCode::InvalidArgument, std::format("'{}' is not a folder in assets/", relative));
    const std::filesystem::path dir = db_->assets_dir() / *clean;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return make_error(ErrorCode::NotFound, std::format("assets/{} is not a folder", *clean));
    std::size_t files = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec))
        if (!it->is_directory(ec)) ++files;
    if (files > 0)
        return make_error(ErrorCode::InvalidArgument,
                          std::format("assets/{} still holds {} file{}; move or delete them first", *clean, files, files == 1 ? "" : "s"));
    std::filesystem::remove_all(dir, ec);
    if (ec) return make_error(ErrorCode::IoError, std::format("cannot remove assets/{}: {}", *clean, ec.message()));
    disk_folders_time_ = -1.0;
    if (asset_folder_ == *clean || in_folder(asset_folder_, *clean)) asset_folder_ = folder_of(*clean);
    notify("Removed empty folder " + *clean);
    return {};
}

void Editor::begin_new_folder(std::string parent) {
    new_folder_parent_ = std::move(parent);
    new_folder_text_ = "New Folder";
    new_folder_error_.clear();
}

void Editor::draw_asset_folders(const std::vector<std::string>& folders) {
    const auto recs = db_ ? db_->records() : std::vector<assets::AssetRecord>{};
    const auto count = [&](const std::string& folder) {
        return std::count_if(recs.begin(), recs.end(), [&](const auto& r) { return in_folder(r.path, folder); });
    };
    const auto row = [&](const std::string& folder, std::string_view label, int depth, bool open_icon) {
        ImGui::PushID(folder.c_str());
        const bool selected = asset_folder_ == folder;
        const ImVec2 start = ImGui::GetCursorPos();
        if (ImGui::Selectable("##folder", selected)) asset_folder_ = folder;
        // Drop an asset on a folder to move it there (same as Rename or Move with a new folder).
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                Uuid id;
                std::memcpy(&id, p->Data, sizeof(Uuid));
                if (auto st = move_asset_to_folder(id, folder); !st.ok()) notify(st.error().message, true);
            }
            ImGui::EndDragDropTarget();
        }
        if (!folder.empty() && ImGui::BeginPopupContextItem("##folder_menu")) {
            ImGui::TextDisabled("%s", folder.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Rename or Move Folder...")) begin_rename_folder(folder);
            if (ImGui::MenuItem("New Folder Inside...")) begin_new_folder(folder);
            const bool empty = count(folder) == 0;
            if (ImGui::MenuItem("Delete Folder", nullptr, false, empty))
                if (auto st = delete_asset_folder(folder); !st.ok()) notify(st.error().message, true);
            if (!empty && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Only empty folders can be deleted. Move or delete the assets inside first.");
            if (ImGui::MenuItem("Copy Path") && platform_) platform_->set_clipboard(folder);
            ImGui::EndPopup();
        }
        if (ImGui::IsItemHovered() && !folder.empty())
            ImGui::SetTooltip("Drop assets here to move them into %s. Right-click to rename the folder.", folder.c_str());
        ImGui::SetCursorPos(ImVec2(start.x + static_cast<float>(depth) * tk::kSpace4 * 2, start.y));
        kit::icon(open_icon ? kit::Icon::FolderOpen : kit::Icon::Folder, selected ? tk::kAccent : tk::kTextMuted);
        ImGui::SameLine(0, tk::kSpace2);
        ImGui::TextUnformatted(label.data(), label.data() + label.size());
        ImGui::SameLine(0, tk::kSpace2);
        ImGui::TextColored(tk::kTextDisabled, "%zu", static_cast<std::size_t>(count(folder)));
        ImGui::PopID();
    };
    row("", "All assets", 0, asset_folder_.empty());
    for (const std::string& f : folders) {
        const int depth = static_cast<int>(std::count(f.begin(), f.end(), '/')) + 1;
        const auto slash = f.rfind('/');
        const std::string name = slash == std::string::npos ? f : f.substr(slash + 1);
        // Nested folders show only while their parent (or something inside it) is selected.
        const std::string parent = folder_of(f);
        if (!parent.empty() && asset_folder_ != parent && !in_folder(asset_folder_, parent)) continue;
        row(f, name, depth, asset_folder_ == f || in_folder(asset_folder_, f));
    }
}

} // namespace victus::editor
