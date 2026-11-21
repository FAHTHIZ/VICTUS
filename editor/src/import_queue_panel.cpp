// SPDX-License-Identifier: Apache-2.0
// Import Queue panel and the per-frame hand-off of background imports (ADR-0189): finished
// items evict their stale GPU copies, thumbnails and collider meshes on the main thread, failures
// are reported, and a batch summary is posted when the queue goes idle.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include "victus/runtime/hot_reload.hpp"
#include "victus/ui/style.hpp"

#include <imgui.h>

#include <algorithm>
#include <format>

namespace victus::editor {

namespace tk = ui::tokens;

std::size_t Editor::poll_imports() {
    if (!imports_) return 0;
    auto finished = imports_->take_finished();
    runtime::HotReloadReport changes;
    for (const auto& item : finished) {
        import_batch_.reported = false;
        switch (item.state) {
        case assets::ImportState::Imported:
            ++import_batch_.imported;
            changes.reimported.push_back(item.uuid);
            break;
        case assets::ImportState::UpToDate: ++import_batch_.up_to_date; break;
        case assets::ImportState::Failed:
            ++import_batch_.failed;
            notify("Import failed: " + item.error, true);
            break;
        case assets::ImportState::Cancelled: ++import_batch_.cancelled; break;
        default: break;
        }
        for (const auto& m : item.messages) notify(m, false);
    }
    if (!changes.reimported.empty()) {
        // The same steps as a hot reload of the changed sources: swap the new cooked data in.
        runtime::AssetHotReloader::apply(changes, cache_.get(), play_animation_.get(), tilemaps_.get(), terrains_.get(),
                                         play_physics_.get());
        for (const auto& uuid : changes.reimported) {
            collider_meshes_.invalidate(uuid);
            if (thumbnails_) thumbnails_->invalidate(uuid);
        }
        if (cache_) cache_->evict_materials(); // materials hold the textures they sample
        if (edit_animation_) edit_animation_->clear_cache();
    }
    if (!import_batch_.reported && !imports_->progress().busy()) {
        const auto& b = import_batch_;
        // Previews of models and materials sample the reimported textures: re-render them all.
        if (b.imported > 0 && thumbnails_) thumbnails_->clear();
        if (b.imported + b.failed + b.cancelled > 0)
            notify(std::format("Imports finished: {} imported, {} up to date, {} failed{}", b.imported, b.up_to_date,
                               b.failed, b.cancelled ? std::format(", {} cancelled", b.cancelled) : std::string{}),
                   b.failed > 0);
        import_batch_ = {};
    }
    return finished.size();
}

bool Editor::wait_for_imports(std::chrono::milliseconds timeout) {
    if (!imports_) return true;
    const bool idle = imports_->wait_idle(timeout);
    (void)poll_imports();
    return idle;
}

void Editor::draw_import_queue() {
    if (!show_import_queue_) return;
    if (!ImGui::Begin("Import Queue", &show_import_queue_)) {
        ImGui::End();
        return;
    }
    if (!imports_) {
        ImGui::TextDisabled(safe_mode_ ? "Imports are off in safe mode." : "Open a project to import its assets.");
        ImGui::End();
        return;
    }
    const assets::ImportProgress p = imports_->progress();
    const bool paused = imports_->paused();
    if (kit::icon_text_button("Reimport All", kit::Icon::Rotate)) (void)reimport_assets();
    kit::tooltip("Queue every texture and model again; ones whose source and settings did not change finish at once");
    ImGui::SameLine();
    if (kit::icon_text_button(paused ? "Resume" : "Pause", paused ? kit::Icon::Play : kit::Icon::Pause))
        imports_->set_paused(!paused);
    kit::tooltip("While paused no new import starts (one already running finishes)");
    ImGui::SameLine();
    if (kit::icon_text_button("Cancel", kit::Icon::Stop, false, p.queued > 0)) (void)imports_->cancel_all();
    kit::tooltip("Drop everything still waiting; an import already running finishes");
    ImGui::SameLine();
    if (kit::icon_text_button("Retry Failed", kit::Icon::Undo, false, p.failed + p.cancelled > 0)) (void)imports_->retry_failed();
    ImGui::SameLine();
    if (kit::icon_text_button("Clear Finished", kit::Icon::Close, false, p.total > p.queued + p.running))
        imports_->clear_finished();

    const std::size_t done = p.total - p.queued - p.running;
    const std::string overlay =
        p.busy() ? std::format("{} / {}{}", done, p.total, paused ? " (paused)" : "") : std::format("{} finished", p.total);
    ImGui::ProgressBar(p.fraction(), ImVec2(-1, 0), overlay.c_str());
    if (!p.current.empty()) {
        std::string now;
        for (const auto& c : p.current) now += (now.empty() ? "" : ", ") + c;
        ImGui::TextDisabled("Importing %s", now.c_str());
    } else {
        ImGui::TextDisabled("%zu imported, %zu up to date, %zu failed, %zu cancelled", p.imported, p.up_to_date, p.failed,
                            p.cancelled);
    }

    const auto items = imports_->items();
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##imports", 5, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Asset", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthStretch, 0.7f);
        ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch, 4.0f);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(items.size()));
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const auto& it = items[static_cast<std::size_t>(i)];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(it.path.empty() ? it.uuid.to_string().c_str() : it.path.c_str());
                ImGui::TableNextColumn();
                const ImVec4 color = it.state == assets::ImportState::Failed      ? tk::kDanger
                                     : it.state == assets::ImportState::Imported  ? tk::kSuccess
                                     : it.state == assets::ImportState::Running   ? tk::kAccent
                                                                                   : tk::kTextMuted;
                ImGui::TextColored(color, "%s", assets::import_state_name(it.state));
                ImGui::TableNextColumn();
                if (it.attempts > 0 && assets::import_finished(it.state)) ImGui::Text("%.2f s", it.seconds);
                ImGui::TableNextColumn();
                const std::string& msg = !it.error.empty() ? it.error : (it.messages.empty() ? std::string{} : it.messages.front());
                ImGui::TextUnformatted(msg.c_str());
                if (!msg.empty() && ImGui::IsItemHovered()) {
                    std::string all = it.error;
                    for (const auto& m : it.messages) all += (all.empty() ? "" : "\n") + m;
                    ImGui::SetTooltip("%s", all.c_str());
                }
                ImGui::TableNextColumn();
                if (it.state == assets::ImportState::Queued) {
                    if (ImGui::SmallButton("Cancel")) (void)imports_->cancel(it.uuid);
                } else if (it.state == assets::ImportState::Failed || it.state == assets::ImportState::Cancelled) {
                    if (ImGui::SmallButton("Retry")) (void)imports_->retry(it.uuid);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace victus::editor
