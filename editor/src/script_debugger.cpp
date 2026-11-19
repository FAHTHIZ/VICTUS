// SPDX-License-Identifier: Apache-2.0
// Script Debugger panel and play-mode integration of the Luau source debugger (ADR-0106).
//
// A breakpoint stops the game synchronously inside the script call that hit it (ADR-0099). The
// Studio stays usable through a nested frame loop: the stop handler ends the UI frame that was
// being built, then runs complete Studio frames through the application's frame pump until the
// user resumes, and finally begins a new UI frame so the interrupted frame can finish. While
// stopped, Editor::frame() does not advance play mode or touch the assets, and every panel except
// this one is read-only, so nothing can change the scene under the stopped script.
#include "victus/editor/editor.hpp"

#include "victus/core/fs.hpp"
#include "victus/core/json.hpp"
#include "victus/core/log.hpp"
#include "victus/ui/style.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <format>
#include <functional>
#include <system_error>

namespace victus::editor {

namespace tk = ui::tokens;
using script::StepAction;

namespace {

const char* stop_reason_text(script::StopReason r) noexcept {
    switch (r) {
    case script::StopReason::Breakpoint:
        return "Breakpoint";
    case script::StopReason::Step:
        return "Step";
    case script::StopReason::Pause:
        return "Paused";
    case script::StopReason::Exception:
        return "Error";
    }
    return "";
}

fs::Path breakpoints_file(const project::Project& p) { return p.root() / ".victus" / "breakpoints.json"; }

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        for (char& c : line)
            if (c == '\t') c = ' ';
        lines.push_back(std::move(line));
        if (end == text.size()) break;
        start = end + 1;
    }
    return lines;
}

} // namespace

// ---- breakpoints ----------------------------------------------------------------------------

void Editor::set_breakpoints(const std::string& path, std::vector<script::SourceBreakpoint> breakpoints) {
    std::sort(breakpoints.begin(), breakpoints.end(), [](const auto& a, const auto& b) { return a.line < b.line; });
    breakpoints.erase(std::unique(breakpoints.begin(), breakpoints.end(), [](const auto& a, const auto& b) { return a.line == b.line; }),
                      breakpoints.end());
    if (play_scripts_) (void)play_scripts_->set_breakpoints(path, breakpoints);
    if (breakpoints.empty()) breakpoints_.erase(path);
    else breakpoints_[path] = std::move(breakpoints);
    save_breakpoints();
}

bool Editor::toggle_breakpoint(const std::string& path, int line) {
    if (path.empty() || line <= 0) return false;
    std::vector<script::SourceBreakpoint> list;
    if (const auto it = breakpoints_.find(path); it != breakpoints_.end()) list = it->second;
    const auto it = std::find_if(list.begin(), list.end(), [line](const auto& b) { return b.line == line; });
    const bool set = it == list.end();
    if (set) list.push_back({.line = line});
    else list.erase(it);
    set_breakpoints(path, std::move(list));
    return set;
}

std::vector<script::BreakpointState> Editor::breakpoint_states(const std::string& path) const {
    if (play_scripts_) return play_scripts_->breakpoints(path);
    std::vector<script::BreakpointState> out;
    if (const auto it = breakpoints_.find(path); it != breakpoints_.end())
        for (const auto& b : it->second)
            out.push_back({b.line, b.line, false, "applies when the game runs"});
    return out;
}

void Editor::set_break_on_error(bool enabled) {
    break_on_error_ = enabled;
    if (play_scripts_) play_scripts_->set_break_on_error(enabled);
    save_breakpoints();
}

void Editor::load_breakpoints() {
    breakpoints_.clear();
    break_on_error_ = true;
    if (!project_) return;
    auto text = fs::read_text(breakpoints_file(*project_));
    if (!text) return;
    auto doc = json::parse(*text);
    if (!doc) {
        notify("breakpoints.json: " + doc.error().message, true);
        return;
    }
    const json::Value& root = *doc;
    if (const json::Value* b = root.find("break_on_error")) break_on_error_ = b->as_bool(true);
    if (const json::Value* list = root.find("breakpoints"); list && list->is_array())
        for (const json::Value& v : list->as_array()) {
            const std::string path = v["path"].as_string_or("");
            const int line = static_cast<int>(v["line"].as_number(0));
            if (path.empty() || line <= 0) continue;
            breakpoints_[path].push_back({line, v["condition"].as_string_or(""), v["hit_condition"].as_string_or(""),
                                          v["log_message"].as_string_or("")});
        }
}

void Editor::save_breakpoints() const {
    if (!project_) return;
    json::Value list = json::Value::array();
    for (const auto& [path, bps] : breakpoints_)
        for (const auto& b : bps) {
            json::Value v = json::Value::object();
            v.set("path", path);
            v.set("line", b.line);
            if (!b.condition.empty()) v.set("condition", b.condition);
            if (!b.hit_condition.empty()) v.set("hit_condition", b.hit_condition);
            if (!b.log_message.empty()) v.set("log_message", b.log_message);
            list.push(std::move(v));
        }
    json::Value root = json::Value::object();
    root.set("format", "victus.breakpoints");
    root.set("version", 1);
    root.set("break_on_error", break_on_error_);
    root.set("breakpoints", std::move(list));
    std::error_code ec;
    std::filesystem::create_directories(breakpoints_file(*project_).parent_path(), ec);
    (void)fs::write_text_atomic(breakpoints_file(*project_), json::stringify(root, true) + "\n");
}

// ---- play session -----------------------------------------------------------------------------

void Editor::apply_debugger_to_play() {
    if (!play_scripts_) return;
    play_scripts_->set_debug_handler([this](script::DebugStop& stop) { return on_debug_stop(stop); },
                                     [](std::string_view text) { VICTUS_LOG_INFO("script", "{}", text); });
    play_scripts_->set_break_on_error(break_on_error_);
    for (const auto& [path, bps] : breakpoints_)
        (void)play_scripts_->set_breakpoints(path, bps);
}

StepAction Editor::on_debug_stop(script::DebugStop& stop) {
    ++debug_stop_count_;
    // Stop was pressed during an earlier stop of this call, or there is no UI to show the stop.
    if (debug_stop_after_resume_ || !frame_pump_.frame) return StepAction::Continue;
    debug_stop_ = &stop;
    debug_action_.reset();
    debug_frame_ = 0;
    debug_eval_result_.clear();
    if (!stop.frames().empty()) show_script_source(stop.frames().front().source, stop.frames().front().line);
    show_panel(Panel::ScriptDebugger);
    if (frame_pump_.suspend) frame_pump_.suspend();
    while (!debug_action_) {
        if (!frame_pump_.frame()) {
            // The application is closing: let the call finish without further stops.
            debug_stop_after_resume_ = true;
            debug_action_ = StepAction::Continue;
        }
    }
    if (frame_pump_.resume) frame_pump_.resume();
    debug_stop_ = nullptr;
    debug_source_line_ = 0;
    const StepAction action = *debug_action_;
    debug_action_.reset();
    return action;
}

void Editor::debug_resume(StepAction action) noexcept {
    if (debug_stop_) debug_action_ = action;
}

void Editor::debug_pause() {
    if (play_scripts_ && !debug_stop_) play_scripts_->request_pause();
}

void Editor::show_script_source(const std::string& path, int line) {
    if (path != debug_source_path_ || debug_source_lines_.empty()) {
        debug_source_path_ = path;
        debug_source_lines_.clear();
        if (db_ && !path.empty())
            if (auto text = fs::read_text(db_->assets_dir() / path)) debug_source_lines_ = split_lines(*text);
    }
    debug_source_line_ = line;
    debug_source_scroll_ = line > 0;
}

// ---- panel ----------------------------------------------------------------------------------

void Editor::draw_script_debugger() {
    if (!show_script_debugger_) return;
    if (!ImGui::Begin(panel_title(Panel::ScriptDebugger), &show_script_debugger_)) {
        ImGui::End();
        return;
    }
    script::DebugStop* stop = debug_stop_;
    const bool playing = play_state_ != PlayState::Editing && play_scripts_ != nullptr;

    // Keyboard: the usual debugger keys while stopped.
    if (stop) {
        const bool shift = ImGui::GetIO().KeyShift;
        if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) debug_resume(StepAction::Continue);
        if (ImGui::IsKeyPressed(ImGuiKey_F10, false)) debug_resume(StepAction::StepOver);
        if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) debug_resume(shift ? StepAction::StepOut : StepAction::StepIn);
    }

    // Status and controls.
    if (stop) {
        ImGui::TextColored(stop->reason() == script::StopReason::Exception ? tk::kDanger : tk::kWarning, "%s",
                           stop_reason_text(stop->reason()));
        ImGui::SameLine();
        ImGui::TextWrapped("%s", stop->message().c_str());
    } else if (playing) {
        ImGui::TextColored(tk::kSuccess, "Running");
        ImGui::SameLine();
        ImGui::TextColored(tk::kTextMuted, "breakpoints stop the game here");
    } else {
        ImGui::TextColored(tk::kTextMuted, "Not playing. Breakpoints apply when you press Play.");
    }
    ImGui::BeginDisabled(stop == nullptr);
    if (ImGui::Button("Continue (F5)")) debug_resume(StepAction::Continue);
    ImGui::SameLine();
    if (ImGui::Button("Step Over (F10)")) debug_resume(StepAction::StepOver);
    ImGui::SameLine();
    if (ImGui::Button("Step Into (F11)")) debug_resume(StepAction::StepIn);
    ImGui::SameLine();
    if (ImGui::Button("Step Out (Shift+F11)")) debug_resume(StepAction::StepOut);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!playing || stop != nullptr);
    if (ImGui::Button("Pause")) debug_pause();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!playing);
    if (ImGui::Button("Stop Game")) {
            if (stop) {
                debug_stop_after_resume_ = true;
                debug_resume(StepAction::Continue);
            } else {
                this->stop();
            }
        }
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool boe = break_on_error_;
    if (ImGui::Checkbox("Break on errors", &boe)) set_break_on_error(boe);

    const float avail = ImGui::GetContentRegionAvail().x;
    const float source_w = std::max(200.0f, avail * 0.58f);

    // Source view with a breakpoint gutter.
    ImGui::BeginChild("##dbg_source", ImVec2(source_w, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##dbg_script", debug_source_path_.empty() ? "Choose a script" : debug_source_path_.c_str())) {
            if (db_) {
                std::vector<std::string> scripts;
                std::error_code ec;
                const fs::Path root = db_->assets_dir();
                for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
                     !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
                    if (it->is_regular_file(ec) && it->path().extension() == ".luau")
                        scripts.push_back(std::filesystem::relative(it->path(), root, ec).generic_string());
                std::sort(scripts.begin(), scripts.end());
                for (const std::string& s : scripts)
                    if (ImGui::Selectable(s.c_str(), s == debug_source_path_)) show_script_source(s);
            }
            ImGui::EndCombo();
        }
        ImGui::BeginChild("##dbg_lines", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        std::vector<int> bp_lines, actual_lines;
        if (const auto it = breakpoints_.find(debug_source_path_); it != breakpoints_.end())
            for (const auto& b : it->second)
                bp_lines.push_back(b.line);
        if (playing)
            for (const auto& s : breakpoint_states(debug_source_path_))
                if (s.verified) actual_lines.push_back(s.line);
        const float line_h = ImGui::GetTextLineHeightWithSpacing();
        const float gutter = ImGui::CalcTextSize("00000").x + line_h;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(debug_source_lines_.size()), line_h);
        if (debug_source_scroll_ && debug_source_line_ > 0) {
            clip.IncludeItemByIndex(debug_source_line_ - 1);
        }
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const int line = i + 1;
                ImGui::PushID(i);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const bool current = line == debug_source_line_;
                if (current)
                    dl->AddRectFilled(p, ImVec2(p.x + ImGui::GetWindowWidth() + ImGui::GetScrollMaxX(), p.y + line_h),
                                      ImGui::GetColorU32(tk::kAccentSubtle));
                if (ImGui::InvisibleButton("##gutter", ImVec2(gutter, line_h))) (void)toggle_breakpoint(debug_source_path_, line);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to add or remove a breakpoint on line %d", line);
                const bool has_bp = std::find(bp_lines.begin(), bp_lines.end(), line) != bp_lines.end();
                const bool lands = std::find(actual_lines.begin(), actual_lines.end(), line) != actual_lines.end();
                const ImVec2 c(p.x + line_h * 0.5f, p.y + line_h * 0.5f);
                if (has_bp || lands)
                    dl->AddCircleFilled(c, line_h * 0.3f, ImGui::GetColorU32(lands || !playing ? tk::kDanger : tk::kTextDisabled));
                if (current) dl->AddTriangleFilled(ImVec2(c.x - 2, c.y - 5), ImVec2(c.x + 5, c.y), ImVec2(c.x - 2, c.y + 5),
                                                   ImGui::GetColorU32(tk::kWarning));
                dl->AddText(ImVec2(p.x + line_h, p.y), ImGui::GetColorU32(tk::kTextDisabled), std::format("{:>4}", line).c_str());
                ImGui::SameLine(0, 0);
                ImGui::TextUnformatted(debug_source_lines_[static_cast<std::size_t>(i)].c_str());
                if (current && debug_source_scroll_) {
                    ImGui::SetScrollHereY(0.4f);
                    debug_source_scroll_ = false;
                }
                ImGui::PopID();
            }
        if (debug_source_lines_.empty()) ImGui::TextColored(tk::kTextMuted, "No script selected.");
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Call stack, variables, watches and breakpoints.
    ImGui::BeginChild("##dbg_side", ImVec2(0, 0));
    if (ImGui::BeginTabBar("##dbg_tabs")) {
        if (ImGui::BeginTabItem("Variables")) {
            if (!stop) {
                ImGui::TextColored(tk::kTextMuted, "Variables show while the game is stopped.");
            } else {
                const auto& frames = stop->frames();
                if (ImGui::BeginListBox("##frames", ImVec2(-1, std::min<float>(6.0f, static_cast<float>(frames.size()) + 0.5f) *
                                                                    ImGui::GetTextLineHeightWithSpacing()))) {
                    for (std::size_t i = 0; i < frames.size(); ++i) {
                        const auto& f = frames[i];
                        const std::string label = std::format("{}  {}:{}##f{}", f.function, f.source, f.line, i);
                        if (ImGui::Selectable(label.c_str(), i == debug_frame_)) {
                            debug_frame_ = i;
                            show_script_source(f.source, f.line);
                        }
                    }
                    ImGui::EndListBox();
                }
                // Tables expand lazily through their references (depth-limited).
                const std::function<void(const script::DebugVariable&, int)> row = [&](const script::DebugVariable& v, int depth) {
                    const std::string text = std::format("{} = {}", v.name, v.value);
                    if (v.reference != 0 && depth < 8) {
                        if (ImGui::TreeNode(&v, "%s", text.c_str())) {
                            for (const auto& child : stop->children(v.reference))
                                row(child, depth + 1);
                            ImGui::TreePop();
                        }
                    } else {
                        ImGui::BulletText("%s", text.c_str());
                    }
                    if (ImGui::IsItemHovered() && !v.type.empty()) ImGui::SetTooltip("%s", v.type.c_str());
                };
                if (ImGui::TreeNodeEx("Locals", ImGuiTreeNodeFlags_DefaultOpen)) {
                    for (const auto& v : stop->locals(debug_frame_))
                        row(v, 0);
                    ImGui::TreePop();
                }
                if (ImGui::TreeNodeEx("Upvalues")) {
                    for (const auto& v : stop->upvalues(debug_frame_))
                        row(v, 0);
                    ImGui::TreePop();
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Watch")) {
            int remove = -1;
            for (std::size_t i = 0; i < debug_watches_.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::SmallButton("x")) remove = static_cast<int>(i);
                ImGui::SameLine();
                std::string value = "(not stopped)";
                if (stop) {
                    auto r = stop->evaluate(debug_watches_[i], debug_frame_);
                    value = r ? r->value : r.error().message;
                }
                ImGui::TextWrapped("%s = %s", debug_watches_[i].c_str(), value.c_str());
                ImGui::PopID();
            }
            if (remove >= 0) debug_watches_.erase(debug_watches_.begin() + remove);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputTextWithHint("##watch", "Add a watch expression (Enter)", &debug_watch_input_,
                                         ImGuiInputTextFlags_EnterReturnsTrue) &&
                !debug_watch_input_.empty()) {
                debug_watches_.push_back(debug_watch_input_);
                debug_watch_input_.clear();
            }
            ImGui::Separator();
            ImGui::BeginDisabled(stop == nullptr);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputTextWithHint("##eval", "Evaluate in the selected frame (Enter)", &debug_eval_input_,
                                         ImGuiInputTextFlags_EnterReturnsTrue) &&
                stop && !debug_eval_input_.empty()) {
                auto r = stop->evaluate(debug_eval_input_, debug_frame_);
                debug_eval_result_ = r ? std::format("{} ({})", r->value, r->type) : "error: " + r.error().message;
            }
            ImGui::EndDisabled();
            if (!debug_eval_result_.empty()) ImGui::TextWrapped("%s", debug_eval_result_.c_str());
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Breakpoints")) {
            if (breakpoints_.empty()) ImGui::TextColored(tk::kTextMuted, "Click a line number in the source view to add one.");
            std::optional<std::pair<std::string, std::vector<script::SourceBreakpoint>>> changed;
            for (const auto& [path, bps] : breakpoints_) {
                const auto states = breakpoint_states(path);
                for (std::size_t i = 0; i < bps.size(); ++i) {
                    const auto& b = bps[i];
                    ImGui::PushID(std::format("{}:{}", path, b.line).c_str());
                    if (ImGui::SmallButton("x")) {
                        auto list = bps;
                        list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
                        changed.emplace(path, std::move(list));
                    }
                    ImGui::SameLine();
                    std::string where = std::format("{}:{}", path, b.line);
                    for (const auto& s : states)
                        if (s.requested_line == b.line && s.verified && s.line != b.line) where += std::format(" (stops at {})", s.line);
                    if (ImGui::Selectable(where.c_str())) show_script_source(path, b.line);
                    std::string cond = b.condition, log = b.log_message;
                    ImGui::Indent();
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputTextWithHint("##cond", "Condition (Luau, e.g. self.hp < 10)", &cond, ImGuiInputTextFlags_EnterReturnsTrue)) {
                        auto list = bps;
                        list[i].condition = cond;
                        changed.emplace(path, std::move(list));
                    }
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputTextWithHint("##log", "Log message instead of stopping ({expr} allowed)", &log,
                                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                        auto list = bps;
                        list[i].log_message = log;
                        changed.emplace(path, std::move(list));
                    }
                    ImGui::Unindent();
                    ImGui::PopID();
                }
            }
            if (changed) set_breakpoints(changed->first, std::move(changed->second));
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace victus::editor
