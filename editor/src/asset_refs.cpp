// SPDX-License-Identifier: Apache-2.0
// Studio asset references (ADR-0175): "Find References", reference-safe Rename/Move and Delete
// with the impact shown first. The index itself lives in victus/assets/references.hpp.
#include "victus/editor/editor.hpp"
#include "victus/ui/style.hpp"
#include "widgets.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>

namespace victus::editor {

namespace tok = ui::tokens;

namespace {
constexpr const char* kRefsWindow = "References###asset_refs";
constexpr const char* kRenamePopup = "Rename Asset";
constexpr const char* kFolderPopup = "Rename Folder";
constexpr const char* kDeletePopup = "Delete Asset";

std::string file_name(std::string_view rel) {
    const auto slash = rel.rfind('/');
    return std::string(slash == std::string_view::npos ? rel : rel.substr(slash + 1));
}
} // namespace

std::vector<assets::AssetReference> Editor::asset_users(const Uuid& asset) const {
    if (!db_) return {};
    return assets::ReferenceIndex::build(*db_).users(asset);
}

std::vector<assets::AssetReference> Editor::asset_dependencies(const Uuid& asset) const {
    if (!db_) return {};
    const auto* r = db_->find(asset);
    if (!r) return {};
    return assets::ReferenceIndex::build(*db_).dependencies(assets::project_relative(*r));
}

Result<assets::RenameReport> Editor::rename_asset(const Uuid& asset, std::string new_relative) {
    if (!db_ || !project_) return make_error(ErrorCode::InvalidArgument, "no project is open");
    if (play_state_ != PlayState::Editing) return make_error(ErrorCode::InvalidArgument, "stop play mode before renaming assets");
    // Path references are rewritten on disk; the open scene is saved first and reloaded after,
    // so its in-memory copy never writes the old path back.
    const bool reload = !scene_path_.empty();
    if (reload && dirty()) VICTUS_TRY_STATUS(save_scene());
    auto r = assets::rename_asset(*db_, asset, new_relative);
    if (!r) return r;
    if (reload && std::find(r->rewritten.begin(), r->rewritten.end(), scene_path_) != r->rewritten.end()) {
        const std::string path = scene_path_;
        if (auto st = open_scene(path); !st) notify("Could not reload " + path + ": " + st.error().message, true);
    }
    cache_->clear();
    if (thumbnails_) thumbnails_->clear();
    if (asset_store_) asset_store_->invalidate_installed();
    refs_stale_ = true;
    notify(r->rewritten.empty() ? std::format("Renamed to {}", r->to)
                                : std::format("Renamed to {} and updated {} file{}", r->to, r->rewritten.size(), r->rewritten.size() == 1 ? "" : "s"));
    return r;
}

Result<assets::FolderRenameReport> Editor::rename_asset_folder(std::string from, std::string to) {
    if (!db_ || !project_) return make_error(ErrorCode::InvalidArgument, "no project is open");
    if (play_state_ != PlayState::Editing) return make_error(ErrorCode::InvalidArgument, "stop play mode before moving folders");
    const bool reload = !scene_path_.empty();
    if (reload && dirty()) VICTUS_TRY_STATUS(save_scene());
    auto r = assets::rename_folder(*db_, from, to);
    if (!r) return r;
    if (reload && std::find(r->rewritten.begin(), r->rewritten.end(), scene_path_) != r->rewritten.end()) {
        const std::string path = scene_path_;
        if (auto st = open_scene(path); !st) notify("Could not reload " + path + ": " + st.error().message, true);
    }
    // Keep the folder view on the moved folder (or the matching sub-folder).
    if (asset_folder_ == r->from) asset_folder_ = r->to;
    else if (asset_folder_.starts_with(r->from + "/")) asset_folder_ = r->to + asset_folder_.substr(r->from.size());
    cache_->clear();
    if (thumbnails_) thumbnails_->clear();
    if (asset_store_) asset_store_->invalidate_installed();
    refs_stale_ = true;
    notify(std::format("Moved {} to {} ({} asset{}{})", r->from, r->to, r->moved, r->moved == 1 ? "" : "s",
                       r->rewritten.empty() ? std::string()
                                            : std::format(", updated {} file{}", r->rewritten.size(), r->rewritten.size() == 1 ? "" : "s")));
    return r;
}

Status Editor::move_asset_to_folder(const Uuid& asset, std::string_view folder) {
    const auto* r = db_ ? db_->find(asset) : nullptr;
    if (!r) return make_error(ErrorCode::NotFound, "asset not found");
    const std::string name = std::filesystem::path(r->path).filename().generic_string();
    const std::string target = folder.empty() ? name : std::string(folder) + "/" + name;
    if (target == r->path) return {};
    if (auto res = rename_asset(asset, target); !res) return make_error(res.error().code, res.error().message);
    asset_focus_ = asset;
    return {};
}

void Editor::begin_rename_folder(std::string folder) {
    folder_rename_text_ = folder;
    folder_rename_from_ = std::move(folder);
    folder_rename_error_.clear();
}

Status Editor::delete_asset(const Uuid& asset) {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project is open");
    if (play_state_ != PlayState::Editing) return make_error(ErrorCode::InvalidArgument, "stop play mode before deleting assets");
    const auto* r = db_->find(asset);
    const std::string name = r ? r->path : std::string();
    auto trashed = assets::trash_asset(*db_, asset);
    if (!trashed) return std::move(trashed).error();
    cache_->clear();
    if (thumbnails_) thumbnails_->clear();
    if (asset_store_) asset_store_->invalidate_installed();
    refs_stale_ = true;
    if (asset_focus_ == asset) asset_focus_ = {};
    notify("Moved " + name + " to the Trash (Assets > Trash... restores it)");
    return {};
}

std::vector<assets::TrashEntry> Editor::trash_entries() const {
    return db_ ? assets::list_trash(*db_) : std::vector<assets::TrashEntry>{};
}

Status Editor::restore_trashed(std::string_view id) {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project is open");
    if (play_state_ != PlayState::Editing) return make_error(ErrorCode::InvalidArgument, "stop play mode before restoring assets");
    std::string path;
    for (const auto& e : assets::list_trash(*db_))
        if (e.id == id) path = e.original_path;
    VICTUS_TRY_STATUS(assets::restore_from_trash(*db_, id));
    cache_->clear();
    if (thumbnails_) thumbnails_->clear();
    if (asset_store_) asset_store_->invalidate_installed();
    refs_stale_ = true;
    if (const auto* r = db_->find_path(path)) asset_focus_ = r->uuid;
    notify("Restored " + path);
    return {};
}

Status Editor::purge_trashed(std::string_view id) {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project is open");
    VICTUS_TRY_STATUS(assets::purge_trash(*db_, id));
    notify(id.empty() ? std::string("Emptied the Trash") : std::string("Deleted for good"));
    return {};
}

void Editor::handle_asset_shortcuts() {
    if (!db_ || asset_focus_.is_nil() || !db_->find(asset_focus_)) return;
    if (ImGui::GetIO().WantTextInput || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) return;
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) begin_rename_asset(asset_focus_);
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) begin_delete_asset(asset_focus_);
    if (ImGui::GetIO().KeyShift && ImGui::GetIO().KeyAlt && ImGui::IsKeyPressed(ImGuiKey_F, false)) open_asset_references(asset_focus_);
}

void Editor::draw_trash() {
    if (!show_trash_) return;
    ImGui::SetNextWindowSize(ImVec2(620, 380), ImGuiCond_FirstUseEver);
    const ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Trash", &show_trash_)) {
        ImGui::End();
        return;
    }
    const auto entries = trash_entries();
    if (entries.empty()) {
        kit::empty_state(kit::Icon::Folder, "The Trash is empty",
                         "Assets you delete land here, with their IDs, so you can bring them back. The Trash lives in "
                         "this project's .victus folder on this computer.");
        ImGui::End();
        return;
    }
    ImGui::TextColored(tok::kTextMuted, "%zu deleted asset%s. Restore puts an asset back where it was, with the same ID, so "
                                        "everything that used it works again.",
                       entries.size(), entries.size() == 1 ? "" : "s");
    std::string restore, purge;
    if (ImGui::BeginTable("##trash", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                          ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.4f))) {
        ImGui::TableSetupColumn("Asset", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Deleted", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        for (const auto& e : entries) {
            ImGui::PushID(e.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.original_path.c_str());
            ImGui::TableNextColumn();
            const std::int64_t ago = std::max<std::int64_t>(now - e.deleted_unix, 0);
            const std::string when = ago < 60      ? std::string("just now")
                                     : ago < 3600  ? std::format("{} min ago", ago / 60)
                                     : ago < 86400 ? std::format("{} h ago", ago / 3600)
                                                   : std::format("{} days ago", ago / 86400);
            ImGui::TextColored(tok::kTextMuted, "%s", when.c_str());
            ImGui::TableNextColumn();
            const std::string size = e.bytes >= (1u << 20) ? std::format("{:.1f} MB", static_cast<double>(e.bytes) / (1 << 20))
                                                           : std::format("{:.0f} KB", std::ceil(static_cast<double>(e.bytes) / 1024.0));
            ImGui::TextColored(tok::kTextMuted, "%s", size.c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Restore")) restore = e.id;
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete for good")) purge = e.id;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!restore.empty())
        if (auto st = restore_trashed(restore); !st) notify(st.error().message, true);
    if (!purge.empty())
        if (auto st = purge_trashed(purge); !st) notify(st.error().message, true);
    if (!confirm_empty_trash_) {
        if (ImGui::Button("Empty Trash")) confirm_empty_trash_ = true;
    } else {
        ImGui::TextColored(tok::kWarning, "Delete %zu asset%s for good?", entries.size(), entries.size() == 1 ? "" : "s");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, tok::kDanger);
        ImGui::PushStyleColor(ImGuiCol_Text, tok::kSurfaceBase);
        const bool yes = ImGui::Button("Empty");
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (yes) {
            if (auto st = purge_trashed(); !st) notify(st.error().message, true);
            confirm_empty_trash_ = false;
        } else if (ImGui::Button("Cancel")) {
            confirm_empty_trash_ = false;
        }
    }
    ImGui::End();
}

void Editor::open_asset_references(const Uuid& asset) {
    refs_asset_ = asset;
    refs_stale_ = true;
    show_asset_refs_ = true;
}

void Editor::begin_rename_asset(const Uuid& asset) {
    const auto* r = db_ ? db_->find(asset) : nullptr;
    if (!r) return;
    rename_target_ = asset;
    rename_text_ = r->path;
    rename_error_.clear();
    rename_users_ = asset_users(asset);
}

void Editor::begin_delete_asset(const Uuid& asset) {
    if (!db_ || !db_->find(asset)) return;
    delete_target_ = asset;
    rename_users_ = asset_users(asset);
}

void Editor::draw_asset_context_items(const assets::AssetRecord& r) {
    if (ImGui::MenuItem("Find References")) open_asset_references(r.uuid);
    if (ImGui::MenuItem("Rename or Move...", "F2")) begin_rename_asset(r.uuid);
    if (ImGui::MenuItem("Delete...", "Delete")) begin_delete_asset(r.uuid);
    ImGui::Separator();
    if (ImGui::MenuItem("Copy Path")) ImGui::SetClipboardText(r.path.c_str());
    if (ImGui::MenuItem("Copy ID")) ImGui::SetClipboardText(r.uuid.to_string().c_str());
}

namespace {
void reference_rows(const std::vector<assets::AssetReference>& refs, bool show_target, const assets::AssetDatabase& db,
                    const std::function<void(const assets::AssetReference&)>& on_open) {
    if (!ImGui::BeginTable("##refs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("How", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##open", ImGuiTableColumnFlags_WidthFixed);
    for (std::size_t i = 0; i < refs.size(); ++i) {
        const auto& ref = refs[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        std::string label = ref.file;
        if (show_target) {
            const auto* t = db.find(ref.target);
            label = t ? t->path : ref.target.to_string();
        }
        ImGui::TextUnformatted(label.c_str());
        ImGui::TableNextColumn();
        ImGui::TextColored(tok::kTextMuted, "%s%s", ref.by_path ? "by name" : "by ID",
                           ref.count > 1 ? std::format(" x{}", ref.count).c_str() : "");
        if (ImGui::IsItemHovered())
            kit::tooltip(ref.by_path ? "Refers to the file by its path. Rename or Move updates it for you."
                                     : "Refers to the asset by its ID, which never changes when you rename or move it.");
        ImGui::TableNextColumn();
        if (on_open && ImGui::SmallButton("Show")) on_open(ref);
        ImGui::PopID();
    }
    ImGui::EndTable();
}
} // namespace

void Editor::draw_asset_dialogs() {
    // ---- References window ----
    if (show_asset_refs_ && db_) {
        const auto* r = db_->find(refs_asset_);
        if (!r) {
            show_asset_refs_ = false;
        } else {
            if (refs_stale_ || refs_cache_asset_ != refs_asset_) {
                const auto idx = assets::ReferenceIndex::build(*db_);
                refs_users_ = idx.users(refs_asset_);
                refs_deps_ = idx.dependencies(assets::project_relative(*r));
                refs_cache_asset_ = refs_asset_;
                refs_stale_ = false;
            }
            ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
            if (ImGui::Begin(kRefsWindow, &show_asset_refs_)) {
                kit::icon(kit::icon_for(r->kind), kit::tint_for(r->kind));
                ImGui::SameLine();
                kit::heading(file_name(r->path));
                ImGui::TextColored(tok::kTextMuted, "assets/%s", r->path.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Refresh")) refs_stale_ = true;
                ImGui::Spacing();
                const auto open_file = [this](const assets::AssetReference& ref) {
                    if (ref.file.starts_with("scenes/") && ref.file.ends_with(".vscene")) {
                        if (dirty()) {
                            notify("Save the current scene first, then open " + ref.file, true);
                            return;
                        }
                        if (auto st = open_scene(ref.file); !st) notify(st.error().message, true);
                    } else if (ref.file.starts_with("assets/")) {
                        const std::string rel = ref.file.substr(7);
                        set_asset_folder(rel.rfind('/') == std::string::npos ? std::string() : rel.substr(0, rel.rfind('/')));
                        set_asset_filter(file_name(rel));
                        show_panel(Panel::Assets);
                    }
                };
                kit::heading(std::format("Used by ({})", refs_users_.size()), tok::kTypeBody);
                if (refs_users_.empty())
                    ImGui::TextColored(tok::kTextMuted, "Nothing in this project uses it. Deleting it is safe.");
                else
                    reference_rows(refs_users_, false, *db_, open_file);
                ImGui::Spacing();
                kit::heading(std::format("Uses ({})", refs_deps_.size()), tok::kTypeBody);
                if (refs_deps_.empty())
                    ImGui::TextColored(tok::kTextMuted, "It does not refer to other assets.");
                else
                    reference_rows(refs_deps_, true, *db_, [this](const assets::AssetReference& ref) { open_asset_references(ref.target); });
            }
            ImGui::End();
        }
    }

    // ---- Rename / Move ----
    if (!rename_target_.is_nil()) ImGui::OpenPopup(kRenamePopup);
    if (ImGui::BeginPopupModal(kRenamePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto* r = db_ ? db_->find(rename_target_) : nullptr;
        if (!r) {
            rename_target_ = {};
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::TextUnformatted("New name (folders with /, inside assets/):");
            ImGui::SetNextItemWidth(420);
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            char buf[512];
            const auto n = std::min(rename_text_.size(), sizeof buf - 1);
            std::copy_n(rename_text_.data(), n, buf);
            buf[n] = 0;
            const bool enter = ImGui::InputText("##rename", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue);
            rename_text_ = buf;
            const auto by_path = std::count_if(rename_users_.begin(), rename_users_.end(), [](const auto& u) { return u.by_path; });
            if (rename_users_.empty())
                ImGui::TextColored(tok::kTextMuted, "Nothing uses this asset yet.");
            else if (by_path > 0)
                ImGui::TextColored(tok::kInfo, "%zu file%s refer to it; %td by name will be updated.", rename_users_.size(),
                                   rename_users_.size() == 1 ? "" : "s", by_path);
            else
                ImGui::TextColored(tok::kTextMuted, "%zu file%s refer to it by ID; they keep working.", rename_users_.size(),
                                   rename_users_.size() == 1 ? "" : "s");
            if (dirty() && by_path > 0) ImGui::TextColored(tok::kWarning, "The open scene is saved first.");
            if (!rename_error_.empty()) {
                ImGui::PushTextWrapPos(420);
                ImGui::TextColored(tok::kDanger, "%s", rename_error_.c_str());
                ImGui::PopTextWrapPos();
            }
            const bool changed = rename_text_ != r->path && !rename_text_.empty();
            ImGui::BeginDisabled(!changed);
            if (ImGui::Button("Rename", ImVec2(120, 0)) || (enter && changed)) {
                if (auto res = rename_asset(rename_target_, rename_text_); res) {
                    set_asset_folder(res->to.rfind('/') == std::string::npos ? std::string() : res->to.substr(0, res->to.rfind('/')));
                    rename_target_ = {};
                    ImGui::CloseCurrentPopup();
                } else {
                    rename_error_ = res.error().message;
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                rename_target_ = {};
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    // ---- Rename / Move Folder ----
    if (!folder_rename_from_.empty()) ImGui::OpenPopup(kFolderPopup);
    if (ImGui::BeginPopupModal(kFolderPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Rename or move assets/%s", folder_rename_from_.c_str());
        ImGui::TextColored(tok::kTextMuted, "New folder path inside assets/ (use / for sub-folders):");
        ImGui::SetNextItemWidth(420);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##folder_rename", &folder_rename_text_, ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string prefix = folder_rename_from_ + "/";
        const auto recs = db_ ? db_->records() : std::vector<assets::AssetRecord>{};
        const auto inside = std::count_if(recs.begin(), recs.end(), [&](const assets::AssetRecord& r) { return r.path.starts_with(prefix); });
        ImGui::TextColored(tok::kInfo, "%td asset%s move with it and keep their IDs; files that name them by path are updated.",
                           inside, inside == 1 ? "" : "s");
        if (dirty()) ImGui::TextColored(tok::kWarning, "The open scene is saved first.");
        if (!folder_rename_error_.empty()) {
            ImGui::PushTextWrapPos(420);
            ImGui::TextColored(tok::kDanger, "%s", folder_rename_error_.c_str());
            ImGui::PopTextWrapPos();
        }
        const bool changed = !folder_rename_text_.empty() && folder_rename_text_ != folder_rename_from_;
        ImGui::BeginDisabled(!changed);
        if (ImGui::Button("Rename", ImVec2(120, 0)) || (enter && changed)) {
            if (auto res = rename_asset_folder(folder_rename_from_, folder_rename_text_); res) {
                folder_rename_from_.clear();
                ImGui::CloseCurrentPopup();
            } else {
                folder_rename_error_ = res.error().message;
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            folder_rename_from_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- Delete ----
    if (!delete_target_.is_nil()) ImGui::OpenPopup(kDeletePopup);
    if (ImGui::BeginPopupModal(kDeletePopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto* r = db_ ? db_->find(delete_target_) : nullptr;
        if (!r) {
            delete_target_ = {};
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::Text("Delete assets/%s?", r->path.c_str());
            if (rename_users_.empty()) {
                ImGui::TextColored(tok::kTextMuted, "Nothing in this project uses it.");
            } else {
                kit::icon(kit::Icon::Warning, tok::kWarning);
                ImGui::SameLine();
                ImGui::TextColored(tok::kWarning, "%zu file%s still use%s it and will show it as missing:", rename_users_.size(),
                                   rename_users_.size() == 1 ? "" : "s", rename_users_.size() == 1 ? "s" : "");
                for (std::size_t i = 0; i < rename_users_.size() && i < 8; ++i) ImGui::BulletText("%s", rename_users_[i].file.c_str());
                if (rename_users_.size() > 8) ImGui::TextColored(tok::kTextMuted, "and %zu more", rename_users_.size() - 8);
            }
            ImGui::TextColored(tok::kTextMuted, "It goes to the Trash (Assets > Trash...), where you can restore it.");
            ImGui::PushStyleColor(ImGuiCol_Button, tok::kDanger);
            ImGui::PushStyleColor(ImGuiCol_Text, tok::kSurfaceBase);
            const bool del = ImGui::Button("Move to Trash", ImVec2(140, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false);
            ImGui::PopStyleColor(2);
            if (del) {
                if (auto st = delete_asset(delete_target_); !st) notify(st.error().message, true);
                delete_target_ = {};
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                delete_target_ = {};
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

} // namespace victus::editor
