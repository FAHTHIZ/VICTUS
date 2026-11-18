// SPDX-License-Identifier: Apache-2.0
// Console panel: engine and script log output with severity toggles, search, merged repeats,
// copy, and a "jump to latest" control when the user has scrolled up.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include "victus/platform/platform.hpp"
#include "victus/ui/style.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <chrono>
#include <ctime>
#include <format>

namespace victus::editor {

namespace tk = ui::tokens;

namespace {

constexpr const char* kConsoleWindow = "Console";

int severity_slot(log::Level level) noexcept {
    if (level >= log::Level::Error) return 2;
    if (level == log::Level::Warn) return 1;
    return 0;
}

std::string clock_time(std::chrono::system_clock::time_point t) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    return std::format("{:02}:{:02}:{:02}", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

std::string line_text(const log::Record& r, std::size_t repeats) {
    std::string out = std::format("[{}] {}: {}", log::to_string(r.level), r.category, r.message);
    if (repeats > 1) out += std::format(" (x{})", repeats);
    return out;
}

} // namespace

Editor::ConsoleCounts Editor::console_counts() const {
    if (!log_sink_) return {};
    const std::uint64_t total = log_sink_->total_written();
    if (total != console_counted_) {
        ConsoleCounts c;
        for (const auto& r : log_sink_->snapshot()) {
            if (r.level >= log::Level::Error)
                ++c.errors;
            else if (r.level == log::Level::Warn)
                ++c.warnings;
            else
                ++c.info;
        }
        console_counts_ = c;
        console_counted_ = total;
    }
    return console_counts_;
}

void Editor::clear_console() {
    if (!log_sink_) return;
    log_sink_->clear();
    console_counts_ = {};
    console_counted_ = log_sink_->total_written();
    console_selected_ = -1;
}

std::vector<Editor::ConsoleRow> Editor::console_filtered(const std::vector<log::Record>& records) const {
    std::vector<ConsoleRow> rows;
    rows.reserve(records.size());
    for (std::size_t i = 0; i < records.size(); ++i) {
        const log::Record& r = records[i];
        if (!console_show_[severity_slot(r.level)]) continue;
        if (!console_search_.empty() && !kit::contains_icase(r.message, console_search_) &&
            !kit::contains_icase(r.category, console_search_))
            continue;
        if (console_collapse_ && !rows.empty()) {
            const log::Record& prev = records[rows.back().record];
            if (prev.level == r.level && prev.category == r.category && prev.message == r.message) {
                ++rows.back().repeats;
                continue;
            }
        }
        rows.push_back({i, 1});
    }
    return rows;
}

std::size_t Editor::console_rows() const {
    if (!log_sink_) return 0;
    return console_filtered(log_sink_->snapshot()).size();
}

std::string Editor::console_text() const {
    if (!log_sink_) return {};
    const auto records = log_sink_->snapshot();
    std::string out;
    for (const ConsoleRow& row : console_filtered(records)) {
        out += line_text(records[row.record], row.repeats);
        out += '\n';
    }
    return out;
}

void Editor::draw_console() {
    if (focus_console_) core_open_[4] = true;
    if (!core_open_[4]) return;
    if (focus_console_) {
        ImGui::SetNextWindowFocus();
        --focus_console_; // a few frames, so it also wins over a layout being docked
    }
    if (!ImGui::Begin(kConsoleWindow, &core_open_[4])) {
        ImGui::End();
        return;
    }
    const ConsoleCounts counts = console_counts();
    const auto records = log_sink_ ? log_sink_->snapshot() : std::vector<log::Record>{};
    const std::uint64_t first_global =
        log_sink_ ? log_sink_->total_written() - static_cast<std::uint64_t>(records.size()) : 0;

    // ---- toolbar: severity toggles, search, collapse, copy, clear ----
    const auto toggle = [&](const char* id, std::size_t n, std::string_view one, std::string_view many, ImVec4 tone,
                            bool& on, const char* tip) {
        const std::string label = std::format("{} {}##{}", n, n == 1 ? one : many, id);
        ImGui::PushID(id);
        ImGui::PushStyleColor(ImGuiCol_Text, on ? (n ? tone : tk::kTextMuted) : tk::kTextDisabled);
        ImGui::Checkbox(label.c_str(), &on);
        ImGui::PopStyleColor();
        kit::tooltip(tip);
        ImGui::PopID();
        ImGui::SameLine(0, tk::kSpace4);
    };
    toggle("errors", counts.errors, "error", "errors", tk::kDanger, console_show_[2],
           "Show errors: something failed, for example a script stopped with an error");
    toggle("warnings", counts.warnings, "warning", "warnings", tk::kWarning, console_show_[1],
           "Show warnings: things that still work but are probably not what you meant");
    toggle("messages", counts.info, "message", "messages", tk::kText, console_show_[0],
           "Show messages: print() output from scripts and normal engine notes");
    const float right = ImGui::GetFrameHeight() * 3 + tk::kSpace2 * 2;
    (void)kit::search_input("##console_search", "Search messages", console_search_,
                            std::max(120.0f, ImGui::GetContentRegionAvail().x - right - tk::kSpace4));
    ImGui::SameLine(0, tk::kSpace4);
    if (kit::icon_button("##collapse", kit::Icon::Layout,
                         console_collapse_
                             ? "Repeated lines are merged into one row with a count (click to list every line)"
                             : "Every line is listed (click to merge repeated lines)",
                         console_collapse_))
        console_collapse_ = !console_collapse_;
    ImGui::SameLine(0, tk::kSpace2);
    const std::vector<ConsoleRow> rows = console_filtered(records);
    if (kit::icon_button("##copy", kit::Icon::File, "Copy the lines shown below", false, !rows.empty()) && platform_)
        platform_->set_clipboard(console_text());
    ImGui::SameLine(0, tk::kSpace2);
    if (kit::icon_button("##clear", kit::Icon::Close, "Clear the Console", false, !records.empty())) clear_console();
    ImGui::Separator();

    if (records.empty()) {
        (void)kit::empty_state(kit::Icon::Info, "No messages yet",
                               "print() output from your scripts, warnings and errors show up here while you work "
                               "and play.");
        ImGui::End();
        return;
    }
    if (rows.empty()) {
        if (kit::empty_state(kit::Icon::Search, "Nothing to show",
                             std::format("{} line{} hidden by the filters above.", records.size(),
                                         records.size() == 1 ? " is" : "s are"),
                             "Show everything")) {
            console_search_.clear();
            console_show_[0] = console_show_[1] = console_show_[2] = true;
        }
        ImGui::End();
        return;
    }

    // ---- rows ----
    const float jump_h = console_follow_ ? 0.0f : ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##log", ImVec2(0, -jump_h), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(rows.size()));
    std::string copy_line;
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            const ConsoleRow& row = rows[static_cast<std::size_t>(i)];
            const log::Record& r = records[row.record];
            const long long global = static_cast<long long>(first_global) + static_cast<long long>(row.record);
            const int slot = severity_slot(r.level);
            const ImVec4 col = slot == 2 ? tk::kDanger : slot == 1 ? tk::kWarning : tk::kText;
            ImGui::PushID(static_cast<int>(global));
            const ImVec2 start = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##row", console_selected_ == global,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                console_selected_ = global;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s  %s  (%s)", clock_time(r.time).c_str(), log::to_string(r.level),
                                  r.category.c_str());
            if (ImGui::BeginPopupContextItem("##row_menu")) {
                console_selected_ = global;
                if (ImGui::MenuItem("Copy Line")) copy_line = line_text(r, row.repeats);
                if (ImGui::MenuItem("Copy Message")) copy_line = r.message;
                if (ImGui::MenuItem(std::format("Show Only \"{}\"", r.category).c_str())) console_search_ = r.category;
                ImGui::EndPopup();
            }
            ImGui::SetCursorScreenPos(start);
            kit::icon(slot == 2   ? kit::Icon::Error
                      : slot == 1 ? kit::Icon::Warning
                                  : kit::Icon::Info,
                      slot ? col : tk::kTextDisabled);
            ImGui::SameLine(0, tk::kSpace2);
            ImGui::TextColored(tk::kTextMuted, "%s", r.category.c_str());
            ImGui::SameLine(0, tk::kSpace2);
            ImGui::TextColored(col, "%s", r.message.c_str());
            if (row.repeats > 1) {
                ImGui::SameLine(0, tk::kSpace2);
                kit::pill(std::format("x{}", row.repeats), ui::Tone::Neutral);
            }
            ImGui::PopID();
        }
    }
    if (ImGui::IsWindowFocused() && console_selected_ >= 0 && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) {
        for (const ConsoleRow& row : rows)
            if (static_cast<long long>(first_global) + static_cast<long long>(row.record) == console_selected_)
                copy_line = line_text(records[row.record], row.repeats);
    }
    if (!copy_line.empty() && platform_) platform_->set_clipboard(copy_line);
    // Follow new output while the view is at the bottom; scrolling up pauses it.
    const bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4;
    if (at_bottom)
        console_follow_ = true;
    else if (rows.size() == console_last_rows_)
        console_follow_ = false; // the user scrolled, not new output
    console_last_rows_ = rows.size();
    if (console_follow_) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    if (!console_follow_) {
        if (kit::icon_text_button("Jump to latest", kit::Icon::Step)) console_follow_ = true;
    }
    ImGui::End();
}

} // namespace victus::editor
