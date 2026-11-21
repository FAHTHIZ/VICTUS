// SPDX-License-Identifier: Apache-2.0
// Assets panel helpers: plain type names, search / type / folder filters and the folder list.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include "victus/ui/style.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <format>
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
    if (db_)
        for (const auto& r : db_->records())
            for (std::string f = folder_of(r.path); !f.empty(); f = folder_of(f))
                out.insert(f);
    return {out.begin(), out.end()};
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
