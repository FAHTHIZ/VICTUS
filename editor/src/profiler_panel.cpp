// SPDX-License-Identifier: Apache-2.0
// Window > Profiler (ADR-0061): frame-time graph and percentiles, per-scope CPU timings (systems,
// scripts, render passes, GPU waits), memory and GPU resource counters, pause and trace recording.
#include "victus/editor/editor.hpp"

#include "victus/core/log.hpp"
#include "victus/core/memory.hpp"
#include "victus/render/gpu_context.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <chrono>
#include <format>

namespace victus::editor {

namespace {

std::string bytes_label(double b) {
    if (b < 10 * 1024) return std::format("{:.0f} B", b);
    if (b < 10 * 1024 * 1024) return std::format("{:.1f} KB", b / 1024.0);
    return std::format("{:.1f} MB", b / (1024.0 * 1024.0));
}

} // namespace

profiler::FrameReport Editor::profiler_report() const {
    return frame_profiler_ ? frame_profiler_->report() : profiler::FrameReport{};
}

void Editor::set_profiler_paused(bool paused) noexcept {
    if (frame_profiler_) frame_profiler_->set_paused(paused);
}

bool Editor::profiler_paused() const noexcept { return frame_profiler_ && frame_profiler_->paused(); }

void Editor::record_profile_trace(std::size_t frames) {
    if (!frame_profiler_) frame_profiler_ = std::make_unique<profiler::FrameProfiler>(300);
    frame_profiler_->record(std::max<std::size_t>(frames, 1));
}

bool Editor::recording_profile_trace() const noexcept { return frame_profiler_ && frame_profiler_->recording(); }

void Editor::update_profiler() {
    // The profiler runs continuously so the graph has history the moment the window opens; zones
    // are cheap (a timestamp pair) and drained every frame.
    if (!frame_profiler_) frame_profiler_ = std::make_unique<profiler::FrameProfiler>(300);
    frame_profiler_->frame();
    auto zones = frame_profiler_->take_recording();
    if (zones.empty()) return;
    const std::filesystem::path dir = project_ ? project_->root() / ".victus" / "profiles" : std::filesystem::temp_directory_path();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    auto path = dir / std::format("trace-{:%Y%m%d-%H%M%S}.json", now);
    for (int i = 2; std::filesystem::exists(path); ++i) path.replace_filename(std::format("trace-{:%Y%m%d-%H%M%S}-{}.json", now, i));
    if (auto st = profiler::write_chrome_trace(path, zones); st) {
        last_profile_trace_ = path;
        notify("profiler trace saved: " + path.string() + " (open in ui.perfetto.dev or chrome://tracing)");
    } else {
        notify("could not save the profiler trace: " + st.error().message, true);
    }
}

void Editor::draw_profiler() {
    if (!show_profiler_ || !frame_profiler_) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 580, vp->WorkPos.y + 60), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 600), ImGuiCond_FirstUseEver);
    if (profiler_focus_) {
        ImGui::SetNextWindowFocus();
        profiler_focus_ = false;
    }
    if (!ImGui::Begin("Profiler", &show_profiler_)) {
        ImGui::End();
        return;
    }
    const profiler::FrameReport r = frame_profiler_->report();
    bool paused = frame_profiler_->paused();
    if (ImGui::Checkbox("Pause", &paused)) frame_profiler_->set_paused(paused);
    ImGui::SameLine();
    if (ImGui::Button("Reset")) frame_profiler_->reset();
    ImGui::SameLine();
    ImGui::BeginDisabled(frame_profiler_->recording());
    if (ImGui::Button(frame_profiler_->recording() ? "Recording..." : "Record Trace (300 frames)")) record_profile_trace(300);
    ImGui::EndDisabled();
    if (!last_profile_trace_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("last: %s", last_profile_trace_.filename().string().c_str());
    }

    const auto samples = frame_profiler_->frame_times().ordered();
    std::vector<float> plot;
    plot.reserve(samples.size());
    for (const auto v : samples) plot.push_back(static_cast<float>(v));
    const float top = static_cast<float>(std::max(r.max_ms, 33.4));
    const std::string overlay = std::format("{:.2f} ms ({:.0f} fps)", r.last_ms, r.last_ms > 0 ? 1000.0 / r.last_ms : 0.0);
    ImGui::PlotLines("##frametimes", plot.data(), static_cast<int>(plot.size()), 0, overlay.c_str(), 0.0f, top,
                     ImVec2(-1, 90));
    ImGui::Text("p50 %.2f   p95 %.2f   p99 %.2f ms", r.p50_ms, r.p95_ms, r.p99_ms);
    ImGui::TextDisabled("mean %.2f   max %.2f ms over %zu frames", r.mean_ms, r.max_ms, r.frames);

    if (ImGui::CollapsingHeader("CPU Scopes", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("##scopes", 5,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable,
                              ImVec2(0, 220))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Mean ms", ImGuiTableColumnFlags_WidthFixed, 64);
            ImGui::TableSetupColumn("p95 ms", ImGuiTableColumnFlags_WidthFixed, 64);
            ImGui::TableSetupColumn("Max ms", ImGuiTableColumnFlags_WidthFixed, 64);
            ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, 48);
            ImGui::TableHeadersRow();
            const double frame_mean = std::max(r.mean_ms, 1e-6);
            for (const auto& s : r.scopes) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(s.name.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.1f%% of the mean frame", 100.0 * s.mean_ms / frame_mean);
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", s.mean_ms);
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", s.p95_ms);
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", s.max_ms);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", s.calls_per_frame);
            }
            ImGui::EndTable();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("gpu.* scopes are CPU time spent waiting on the GPU (swapchain, readbacks); pass.* scopes are CPU encode time per render pass.");
        ImGui::PopStyleColor();
    }

    if (ImGui::CollapsingHeader("Memory & GPU", ImGuiTreeNodeFlags_DefaultOpen)) {
        const rhi::DeviceStats& ds = gpu_.device().stats();
        ImGui::Text("GPU textures: %u (%s)", ds.live_textures, bytes_label(static_cast<double>(ds.texture_bytes)).c_str());
        ImGui::Text("GPU buffers:  %u (%s)", ds.live_buffers, bytes_label(static_cast<double>(ds.buffer_bytes)).c_str());
        ImGui::Text("Pipelines: %u graphics, %u compute   Frames submitted: %llu", ds.live_pipelines,
                    ds.live_compute_pipelines, static_cast<unsigned long long>(ds.frames_submitted));
        if (memory::allocation_tracking_enabled()) {
            const auto ms = memory::allocation_stats();
            ImGui::Text("CPU heap: %s live, %s peak, %llu allocations", bytes_label(static_cast<double>(ms.live_bytes)).c_str(),
                        bytes_label(static_cast<double>(ms.peak_live_bytes)).c_str(),
                        static_cast<unsigned long long>(ms.allocations));
        } else {
            ImGui::TextDisabled("CPU heap tracking is off in this build.");
        }
    }
    ImGui::End();
}

} // namespace victus::editor
