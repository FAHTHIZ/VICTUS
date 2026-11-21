// SPDX-License-Identifier: Apache-2.0
// Behaviour Tree Editor window (ADR-0176): a top-down tree canvas for .vbt assets with a node
// palette, a details pane for the selected node, blackboard defaults, live problems, its own undo
// history, Save (which hot-reloads the tree in a running game) and, during Play, the selected
// entity's live node statuses drawn on the canvas.
//
// The model lives in BehaviorTreeDocument (bt_document.cpp) so tests drive edits without a window;
// this file only draws and routes input.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include "victus/core/fs.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>

namespace victus::editor {

using ai::Node;
using ai::NodeArity;
using ai::NodeType;
namespace sfs = std::filesystem;
namespace tk = ui::tokens;

namespace {

constexpr float kBoxW = 176.0f;
constexpr float kBoxH = 56.0f;
constexpr float kGapX = 22.0f;
constexpr float kGapY = 46.0f;

ImVec4 category_tint(const char* category) {
    const std::string_view c(category);
    if (c == "Composite") return tk::kInfo;
    if (c == "Decorator") return ImVec4(0.72f, 0.55f, 0.98f, 1.0f);
    if (c == "Condition") return tk::kWarning;
    return tk::kSuccess;
}

ImU32 u32(ImVec4 c, float alpha = 1.0f) { return ImGui::ColorConvertFloat4ToU32(ui::with_alpha(c, c.w * alpha)); }

// Tidy-tree layout: leaves get consecutive columns, parents sit centred over their children.
struct TreeLayout {
    std::vector<ImVec2> pos; // top-left in canvas units
    float width{0}, height{0};
};

TreeLayout layout_tree(const std::vector<Node>& nodes) {
    TreeLayout l;
    l.pos.assign(nodes.size(), ImVec2(0, 0));
    float next = 0;
    std::function<float(std::size_t)> place = [&](std::size_t i) -> float {
        const Node& n = nodes[i];
        float x = 0;
        if (n.children.empty()) {
            x = next;
            next += kBoxW + kGapX;
        } else {
            float first = 0, last = 0;
            for (std::size_t k = 0; k < n.children.size(); ++k) {
                const float cx = place(n.children[k]);
                if (k == 0) first = cx;
                last = cx;
            }
            x = (first + last) * 0.5f;
        }
        const float y = static_cast<float>(n.depth) * (kBoxH + kGapY);
        l.pos[i] = ImVec2(x, y);
        l.width = std::max(l.width, x + kBoxW);
        l.height = std::max(l.height, y + kBoxH);
        return x;
    };
    if (!nodes.empty()) place(0);
    return l;
}

ImVec4 status_tint(ai::NodeStatus s) {
    switch (s) {
    case ai::NodeStatus::Success: return tk::kSuccess;
    case ai::NodeStatus::Failure: return tk::kDanger;
    case ai::NodeStatus::Running: return tk::kAccent;
    default: return tk::kTextDisabled;
    }
}

const char* value_kind(const ai::Value& v) {
    if (std::holds_alternative<bool>(v)) return "Yes/No";
    if (std::holds_alternative<double>(v)) return "Number";
    if (std::holds_alternative<std::string>(v)) return "Text";
    if (std::holds_alternative<math::Vec3>(v)) return "Position";
    return "Nothing";
}

// Edits a blackboard value with a kind picker; returns true when changed.
bool value_editor(const char* id, ai::Value& v, bool allow_nothing) {
    bool changed = false;
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(96);
    if (ImGui::BeginCombo("##kind", value_kind(v))) {
        if (allow_nothing && ImGui::Selectable("Nothing", std::holds_alternative<std::monostate>(v))) v = std::monostate{}, changed = true;
        if (ImGui::Selectable("Yes/No", std::holds_alternative<bool>(v))) v = true, changed = true;
        if (ImGui::Selectable("Number", std::holds_alternative<double>(v))) v = 0.0, changed = true;
        if (ImGui::Selectable("Text", std::holds_alternative<std::string>(v))) v = std::string(), changed = true;
        if (ImGui::Selectable("Position", std::holds_alternative<math::Vec3>(v))) v = math::Vec3{}, changed = true;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (auto* b = std::get_if<bool>(&v)) {
        changed |= ImGui::Checkbox("##b", b);
    } else if (auto* d = std::get_if<double>(&v)) {
        changed |= ImGui::InputDouble("##d", d, 0, 0, "%g");
    } else if (auto* s = std::get_if<std::string>(&v)) {
        changed |= ImGui::InputText("##s", s);
    } else if (auto* p = std::get_if<math::Vec3>(&v)) {
        changed |= ImGui::DragFloat3("##p", &p->x, 0.1f);
    } else {
        ImGui::TextDisabled("(no value)");
    }
    ImGui::PopID();
    return changed;
}

} // namespace

// ---- opening and creating -------------------------------------------------------------------------

Result<Uuid> Editor::create_behavior_tree(std::string_view relative_path) {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project open");
    std::string rel(relative_path);
    if (rel.empty()) {
        rel = "ai/BehaviorTree.vbt";
        for (int i = 2; sfs::exists(db_->assets_dir() / rel); ++i) rel = std::format("ai/BehaviorTree_{}.vbt", i);
    } else if (!rel.ends_with(".vbt")) {
        rel += ".vbt";
    }
    auto clean = fs::sanitize_relative(rel);
    if (!clean) return std::move(clean).error();
    const sfs::path file = db_->assets_dir() / *clean;
    if (sfs::exists(file)) return make_error(ErrorCode::AlreadyExists, *clean + " already exists");
    std::error_code ec;
    sfs::create_directories(file.parent_path(), ec);
    BehaviorTreeDocument doc;
    VICTUS_TRY_STATUS(doc.save_as(file, *clean));
    (void)db_->refresh();
    const assets::AssetRecord* rec = db_->find_path(*clean);
    if (!rec) return make_error(ErrorCode::IoError, "behaviour tree was written but not registered: " + *clean);
    notify("Created behaviour tree " + *clean);
    VICTUS_TRY_STATUS(open_behavior_tree(rec->uuid));
    return rec->uuid;
}

Status Editor::open_behavior_tree(const Uuid& asset) {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project open");
    const assets::AssetRecord* rec = db_->find(asset);
    if (!rec || !rec->path.ends_with(".vbt")) return make_error(ErrorCode::NotFound, "behaviour tree " + asset.to_string() + " not found");
    auto doc = BehaviorTreeDocument::load(db_->source_path(*rec), rec->path);
    if (!doc) return make_error(doc.error().code, rec->path + ": " + doc.error().message);
    bt_doc_ = std::move(*doc);
    bt_selected_ = 0;
    bt_frame_ = true;
    bt_message_.clear();
    show_panel(Panel::BehaviorTreeEditor);
    return {};
}

Status Editor::open_behavior_tree_path(std::string_view asset_path) {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project open");
    const assets::AssetRecord* rec = db_->find_path(asset_path);
    if (!rec) return make_error(ErrorCode::NotFound, std::string(asset_path) + " is not in the project");
    return open_behavior_tree(rec->uuid);
}

Status Editor::save_behavior_tree() {
    if (!bt_doc_) return make_error(ErrorCode::InvalidArgument, "no behaviour tree open");
    VICTUS_TRY_STATUS(bt_doc_->save());
    if (play_ai_) play_ai_->reload(bt_doc_->asset_path());
    if (const std::string problem = bt_doc_->validate(); !problem.empty())
        notify("Saved " + bt_doc_->asset_path() + ", but it will not run until fixed: " + problem, true);
    else
        notify("Saved " + bt_doc_->asset_path());
    return {};
}

void Editor::bt_apply(Result<int> r) {
    if (r) {
        bt_selected_ = *r;
        bt_message_.clear();
    } else {
        bt_message_ = r.error().message;
    }
}

// ---- drawing ------------------------------------------------------------------------------------

void Editor::draw_bt_palette() {
    BehaviorTreeDocument& doc = *bt_doc_;
    kit::heading("Nodes", tk::kTypeBody);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled(bt_selected_ >= 0 ? "Click to add under the selected node" : "Select a node first");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    const char* last = "";
    for (const BtNodeInfo& info : bt_node_catalog()) {
        if (std::string_view(info.category) != last) {
            last = info.category;
            ImGui::Spacing();
            ImGui::TextColored(category_tint(info.category), "%ss", info.category);
        }
        ImGui::PushID(static_cast<int>(info.type));
        const bool can = doc.can_add_child(bt_selected_);
        ImGui::BeginDisabled(!can);
        if (ImGui::Selectable(info.label)) bt_apply(doc.add_child(bt_selected_, info.type));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", info.description);
        ImGui::PopID();
    }
}

void Editor::draw_bt_node_menu(int index) {
    BehaviorTreeDocument& doc = *bt_doc_;
    const Node& n = doc.nodes()[static_cast<std::size_t>(index)];
    const BtNodeInfo& info = bt_node_info(n.type);
    ImGui::TextDisabled("%s", info.label);
    ImGui::Separator();
    const auto type_menu = [&](const char* label, auto&& filter, auto&& act) {
        if (!ImGui::BeginMenu(label)) return;
        const char* cat = "";
        for (const BtNodeInfo& i : bt_node_catalog()) {
            if (!filter(i)) continue;
            if (std::string_view(i.category) != cat) {
                if (*cat) ImGui::Separator();
                cat = i.category;
            }
            if (ImGui::MenuItem(i.label)) act(i.type);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i.description);
        }
        ImGui::EndMenu();
    };
    ImGui::BeginDisabled(!doc.can_add_child(index));
    type_menu("Add Child", [](const BtNodeInfo&) { return true; }, [&](NodeType t) { bt_apply(doc.add_child(index, t)); });
    ImGui::EndDisabled();
    type_menu("Wrap In", [](const BtNodeInfo& i) { return ai::node_arity(i.type) == NodeArity::One; },
              [&](NodeType t) { bt_apply(doc.wrap(index, t)); });
    type_menu(
        "Change To",
        [&](const BtNodeInfo& i) {
            const NodeArity a = ai::node_arity(i.type);
            return i.type != n.type && !(a == NodeArity::Leaf && !n.children.empty()) && !(a == NodeArity::One && n.children.size() > 1);
        },
        [&](NodeType t) { bt_apply(doc.change_type(index, t)); });
    ImGui::Separator();
    const int parent = doc.parent_of(index);
    if (ImGui::MenuItem("Move Left", "Alt+Left", false, parent >= 0)) bt_apply(doc.move(index, -1));
    if (ImGui::MenuItem("Move Right", "Alt+Right", false, parent >= 0)) bt_apply(doc.move(index, +1));
    if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, parent >= 0 && doc.can_add_child(parent))) bt_apply(doc.duplicate(index));
    if (ImGui::MenuItem("Delete", "Delete", false, index > 0)) bt_apply(doc.remove(index));
}

void Editor::draw_bt_canvas(const std::optional<ai::TreeDebug>& live) {
    BehaviorTreeDocument& doc = *bt_doc_;
    const auto& nodes = doc.nodes();
    const TreeLayout lay = layout_tree(nodes);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = std::max(size.x, 120.0f);
    size.y = std::max(size.y, 120.0f);
    ImGui::InvisibleButton("##btcanvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                                   ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImGuiIO& io = ImGui::GetIO();

    if (bt_frame_) {
        // Fit the whole tree, centred, at a zoom between 40% and 100%.
        const float zx = (size.x - 40.0f) / std::max(lay.width, 1.0f);
        const float zy = (size.y - 40.0f) / std::max(lay.height, 1.0f);
        bt_zoom_ = std::clamp(std::min(zx, zy), 0.4f, 1.0f);
        bt_pan_x_ = (size.x - lay.width * bt_zoom_) * 0.5f;
        bt_pan_y_ = 24.0f;
        bt_frame_ = false;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        const float old = bt_zoom_;
        bt_zoom_ = std::clamp(bt_zoom_ * (io.MouseWheel > 0 ? 1.12f : 1.0f / 1.12f), 0.3f, 2.0f);
        const float mx = io.MousePos.x - origin.x, my = io.MousePos.y - origin.y;
        bt_pan_x_ = mx - (mx - bt_pan_x_) * (bt_zoom_ / old);
        bt_pan_y_ = my - (my - bt_pan_y_) * (bt_zoom_ / old);
    }
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || ImGui::IsMouseDragging(ImGuiMouseButton_Left) ||
                   ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
        bt_pan_x_ += io.MouseDelta.x;
        bt_pan_y_ += io.MouseDelta.y;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x + size.x, origin.y + size.y);
    dl->PushClipRect(origin, end, true);
    dl->AddRectFilled(origin, end, u32(tk::kSurfaceBase));
    // Dot grid so panning reads as movement.
    const float step = 24.0f * bt_zoom_;
    if (step > 6.0f) {
        const float ox = std::fmod(bt_pan_x_, step), oy = std::fmod(bt_pan_y_, step);
        const int cols = static_cast<int>((end.x - origin.x - ox) / step) + 1;
        const int rows = static_cast<int>((end.y - origin.y - oy) / step) + 1;
        for (int cx = 0; cx < cols; ++cx)
            for (int cy = 0; cy < rows; ++cy)
                dl->AddCircleFilled(ImVec2(origin.x + ox + static_cast<float>(cx) * step, origin.y + oy + static_cast<float>(cy) * step),
                                    1.0f, u32(tk::kBorder));
    }
    const auto to_screen = [&](ImVec2 p) {
        return ImVec2(origin.x + bt_pan_x_ + p.x * bt_zoom_, origin.y + bt_pan_y_ + p.y * bt_zoom_);
    };
    const float w = kBoxW * bt_zoom_, h = kBoxH * bt_zoom_;
    const bool live_ok = live && live->error.empty() && live->nodes.size() == nodes.size() && !doc.dirty();

    // Links first (elbow connectors from the bottom of a parent to the top of each child).
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const ImVec2 a = to_screen(lay.pos[i]);
        const ImVec2 from(a.x + w * 0.5f, a.y + h);
        for (const std::uint32_t c : nodes[i].children) {
            const ImVec2 b = to_screen(lay.pos[c]);
            const ImVec2 to(b.x + w * 0.5f, b.y);
            const float mid = from.y + (to.y - from.y) * 0.5f;
            const bool hot = live_ok && live->nodes[c].active;
            const ImU32 col = hot ? u32(tk::kAccent) : u32(tk::kTextDisabled, 0.8f);
            const float th = (hot ? 2.5f : 1.5f) * std::max(bt_zoom_, 0.6f);
            dl->AddLine(from, ImVec2(from.x, mid), col, th);
            dl->AddLine(ImVec2(from.x, mid), ImVec2(to.x, mid), col, th);
            dl->AddLine(ImVec2(to.x, mid), to, col, th);
        }
    }

    int hovered_node = -1;
    const float font = ImGui::GetFontSize();
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const Node& n = nodes[i];
        const BtNodeInfo& info = bt_node_info(n.type);
        const ImVec2 a = to_screen(lay.pos[i]);
        const ImVec2 b(a.x + w, a.y + h);
        if (b.x < origin.x || a.x > end.x || b.y < origin.y || a.y > end.y) continue;
        const bool over = hovered && io.MousePos.x >= a.x && io.MousePos.x < b.x && io.MousePos.y >= a.y && io.MousePos.y < b.y;
        if (over) hovered_node = static_cast<int>(i);
        const bool sel = static_cast<int>(i) == bt_selected_;
        const std::string problem = doc.node_problem(static_cast<int>(i));
        const ImVec4 tint = category_tint(info.category);
        const float r = tk::kRadius * bt_zoom_;
        dl->AddRectFilled(a, b, u32(over ? tk::kSurfaceHover : tk::kSurfaceRaised), r);
        // Category stripe on the left edge.
        dl->AddRectFilled(a, ImVec2(a.x + 4.0f * bt_zoom_, b.y), u32(tint), r, ImDrawFlags_RoundTopLeft | ImDrawFlags_RoundBottomLeft);
        ImVec4 border = sel ? tk::kAccent : tk::kBorder;
        if (!problem.empty()) border = tk::kDanger;
        if (live_ok && live->nodes[i].active) border = status_tint(live->nodes[i].status);
        dl->AddRect(a, b, u32(border), r, sel || !problem.empty() || (live_ok && live->nodes[i].active) ? 2.0f : 1.0f);
        if (bt_zoom_ >= 0.45f) {
            const float fs = font * std::clamp(bt_zoom_, 0.75f, 1.15f);
            const float pad = 10.0f * bt_zoom_;
            const std::string title = n.name.empty() ? std::string(info.label) : n.name;
            dl->PushClipRect(a, ImVec2(b.x - pad * 0.5f, b.y), true);
            dl->AddText(ImGui::GetFont(), fs, ImVec2(a.x + pad, a.y + 7.0f * bt_zoom_), u32(tk::kText), title.c_str());
            std::string sub = problem.empty() ? bt_node_summary(n) : problem;
            if (sub.empty() || (!n.name.empty() && problem.empty() && sub.empty())) sub = n.name.empty() ? info.category : info.label;
            dl->AddText(ImGui::GetFont(), fs * 0.9f, ImVec2(a.x + pad, a.y + h * 0.55f),
                        u32(problem.empty() ? tk::kTextMuted : tk::kDanger), sub.c_str());
            dl->PopClipRect();
            if (live_ok && live->nodes[i].status != ai::NodeStatus::Idle) {
                const ImVec2 dot(b.x - pad, a.y + pad);
                dl->AddCircleFilled(dot, 4.0f * bt_zoom_, u32(status_tint(live->nodes[i].status)));
            }
        }
    }
    dl->PopClipRect();

    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x == 0.0f &&
        ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y == 0.0f)
        bt_selected_ = hovered_node;
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && std::abs(ImGui::GetMouseDragDelta(ImGuiMouseButton_Right, 0).x) < 3 &&
        std::abs(ImGui::GetMouseDragDelta(ImGuiMouseButton_Right, 0).y) < 3) {
        if (hovered_node >= 0) {
            bt_selected_ = hovered_node;
            ImGui::OpenPopup("##btnode");
        } else {
            ImGui::OpenPopup("##btcanvasmenu");
        }
    }
    if (ImGui::BeginPopup("##btnode")) {
        if (bt_selected_ >= 0 && bt_selected_ < static_cast<int>(doc.nodes().size())) draw_bt_node_menu(bt_selected_);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##btcanvasmenu")) {
        if (ImGui::MenuItem("Frame Tree", "F")) bt_frame_ = true;
        if (ImGui::MenuItem("Reset Zoom")) bt_zoom_ = 1.0f;
        ImGui::EndPopup();
    }
    if (hovered && hovered_node >= 0 && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        const BtNodeInfo& info = bt_node_info(nodes[static_cast<std::size_t>(hovered_node)].type);
        ImGui::SetTooltip("%s — %s%s", info.label, info.description, "\nRight-click for options");
    }
    if (nodes.size() <= 2 && !live_ok) {
        const char* hint = "Right-click a node to add children, wrap it or change its type. Drag to pan, scroll to zoom.";
        const ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(origin.x + (size.x - ts.x) * 0.5f, end.y - ts.y - 12.0f), u32(tk::kTextDisabled), hint);
    }
}

void Editor::draw_bt_details(const std::optional<ai::TreeDebug>& live) {
    BehaviorTreeDocument& doc = *bt_doc_;
    ImGui::PushTextWrapPos(0.0f);
    const bool valid_sel = bt_selected_ >= 0 && bt_selected_ < static_cast<int>(doc.nodes().size());
    if (!valid_sel) {
        kit::heading("Tree", tk::kTypeBody);
        ImGui::TextDisabled("Click a node to edit it.");
    } else {
        const int i = bt_selected_;
        const Node snapshot = doc.nodes()[static_cast<std::size_t>(i)];
        const BtNodeInfo& info = bt_node_info(snapshot.type);
        ImGui::TextColored(category_tint(info.category), "%s", info.category);
        kit::heading(info.label, tk::kTypeHeading);
        ImGui::TextDisabled("%s", info.description);
        ImGui::Spacing();
        Node edit = snapshot;
        bool changed = false;
        const auto field = [&](const char* label) {
            ImGui::TextUnformatted(label);
            ImGui::SetNextItemWidth(-1);
        };
        field("Name (optional)");
        changed |= ImGui::InputTextWithHint("##name", info.label, &edit.name);
        const NodeType t = edit.type;
        if (t == NodeType::Sequence || t == NodeType::Selector) {
            changed |= ImGui::Checkbox("Reactive", &edit.reactive);
            ImGui::TextDisabled("Re-checks earlier children every tick.");
            kit::tooltip("Reactive: a running child is interrupted as soon as an earlier child's result changes "
                         "(use it to react to a target appearing).");
        }
        if (t == NodeType::Parallel) {
            field("Successes needed (0 = all)");
            int th = static_cast<int>(edit.threshold);
            if (ImGui::InputInt("##th", &th)) edit.threshold = static_cast<std::uint32_t>(std::max(th, 0)), changed = true;
        }
        if (t == NodeType::Repeat) {
            field("Times (0 = forever)");
            int c = static_cast<int>(edit.count);
            if (ImGui::InputInt("##count", &c)) edit.count = static_cast<std::uint32_t>(std::max(c, 0)), changed = true;
        }
        if (t == NodeType::Wait || t == NodeType::Cooldown || t == NodeType::Timeout) {
            field("Seconds");
            changed |= ImGui::DragFloat("##sec", &edit.seconds, 0.05f, 0.0f, 3600.0f, "%.2f s");
        }
        if (t == NodeType::Wait) {
            field("Random variation (±)");
            changed |= ImGui::DragFloat("##dev", &edit.deviation, 0.05f, 0.0f, 3600.0f, "%.2f s");
        }
        const bool keyed = t == NodeType::Check || t == NodeType::Guard || t == NodeType::Set || t == NodeType::Clear ||
                           t == NodeType::MoveTo || t == NodeType::RandomPoint;
        if (keyed) {
            field(t == NodeType::RandomPoint ? "Store the point in" : "Blackboard key");
            changed |= ImGui::InputText("##key", &edit.key);
            // Offer keys the tree already uses or defines.
            if (ImGui::BeginPopupContextItem("##keys")) {
                for (const auto& [k, v] : doc.defaults().entries())
                    if (ImGui::MenuItem(k.c_str())) edit.key = k, changed = true;
                ImGui::EndPopup();
            }
        }
        if (t == NodeType::Check || t == NodeType::Guard) {
            field("Condition");
            static constexpr ai::CompareOp kOps[] = {ai::CompareOp::IsSet,   ai::CompareOp::NotSet, ai::CompareOp::True,
                                                     ai::CompareOp::False,   ai::CompareOp::Equal,  ai::CompareOp::NotEqual,
                                                     ai::CompareOp::Less,    ai::CompareOp::LessEqual, ai::CompareOp::Greater,
                                                     ai::CompareOp::GreaterEqual};
            static constexpr const char* kOpWords[] = {"is set", "is not set", "is true", "is false", "equals",
                                                       "does not equal", "is less than", "is at most", "is greater than", "is at least"};
            int cur = 0;
            for (int k = 0; k < 10; ++k)
                if (kOps[k] == edit.op) cur = k;
            if (ImGui::Combo("##op", &cur, kOpWords, 10)) edit.op = kOps[cur], changed = true;
            const bool needs_value = edit.op != ai::CompareOp::IsSet && edit.op != ai::CompareOp::NotSet &&
                                     edit.op != ai::CompareOp::True && edit.op != ai::CompareOp::False;
            if (needs_value) {
                field("Compared with");
                if (std::holds_alternative<std::monostate>(edit.value)) edit.value = 0.0, changed = true;
                changed |= value_editor("cmp", edit.value, false);
            }
        }
        if (t == NodeType::Set) {
            field("Value");
            changed |= value_editor("setv", edit.value, false);
        }
        if (t == NodeType::MoveTo) {
            field("Arrive within");
            changed |= ImGui::DragFloat("##acc", &edit.acceptance, 0.05f, 0.0f, 100.0f, "%.2f m");
            field("Speed without a NavAgent");
            changed |= ImGui::DragFloat("##spd", &edit.speed, 0.05f, 0.0f, 100.0f, "%.2f m/s");
        }
        if (t == NodeType::RandomPoint) {
            field("Search radius");
            changed |= ImGui::DragFloat("##rad", &edit.radius, 0.1f, 0.0f, 1000.0f, "%.1f m");
        }
        if (t == NodeType::Action) {
            field("Action name");
            changed |= ImGui::InputText("##act", &edit.action);
            ImGui::TextDisabled("Calls self:<name>(dt, first) on the entity's script (return \"running\", \"success\" or \"failure\"), or a C++ action registered with that name.");
        }
        if (t == NodeType::Log) {
            field("Message");
            changed |= ImGui::InputText("##msg", &edit.action);
        }
        if (changed) {
            // One undo step per widget gesture: open a step when the edit starts.
            const std::string gesture = std::format("{}:{}", i, ImGui::GetItemID());
            if (bt_edit_gesture_ != gesture) {
                doc.begin_edit(std::format("Edit {}", info.label));
                bt_edit_gesture_ = gesture;
            }
            edit.children = snapshot.children;
            edit.depth = snapshot.depth;
            doc.mutable_node(i) = std::move(edit);
            doc.end_edit();
        }
        if (ImGui::IsItemDeactivated()) bt_edit_gesture_.clear();
        if (const std::string p = doc.node_problem(i); !p.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(tk::kDanger, "This node %s.", p.c_str());
        }
        if (live && live->nodes.size() == doc.nodes().size() && !doc.dirty()) {
            const auto& d = live->nodes[static_cast<std::size_t>(i)];
            ImGui::Spacing();
            ImGui::TextColored(status_tint(d.status), "Live: %s%s", std::string(ai::status_name(d.status)).c_str(),
                               d.active ? " (on the active path)" : "");
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    kit::heading("Blackboard defaults", tk::kTypeBody);
    ImGui::TextDisabled("Starting values every entity running this tree gets.");
    int erase = -1;
    const auto entries = doc.defaults().entries();
    for (int k = 0; k < static_cast<int>(entries.size()); ++k) {
        ImGui::PushID(k);
        ai::Value v = entries[static_cast<std::size_t>(k)].second;
        ImGui::TextUnformatted(entries[static_cast<std::size_t>(k)].first.c_str());
        ImGui::SameLine();
        if (kit::icon_button("##rm", kit::Icon::Close, "Remove this default")) erase = k;
        if (value_editor("bbv", v, false)) {
            const std::string gesture = std::format("bb:{}", entries[static_cast<std::size_t>(k)].first);
            if (bt_edit_gesture_ != gesture) {
                doc.begin_edit("Edit blackboard default");
                bt_edit_gesture_ = gesture;
            }
            doc.mutable_defaults().set(entries[static_cast<std::size_t>(k)].first, std::move(v));
            doc.end_edit();
        }
        ImGui::PopID();
    }
    if (erase >= 0) {
        doc.begin_edit("Remove blackboard default");
        (void)doc.mutable_defaults().erase(entries[static_cast<std::size_t>(erase)].first);
        doc.end_edit();
    }
    ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() * 2.5f);
    const bool enter = ImGui::InputTextWithHint("##newkey", "new key", &bt_new_key_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Add") || enter) && !bt_new_key_.empty() && !doc.defaults().has(bt_new_key_)) {
        doc.begin_edit("Add blackboard default");
        doc.mutable_defaults().set(bt_new_key_, 0.0);
        doc.end_edit();
        bt_new_key_.clear();
    }
    if (live && live_bb_) {
        ImGui::Spacing();
        ImGui::Separator();
        kit::heading("Live blackboard", tk::kTypeBody);
        for (const auto& [k, v] : live_bb_->entries()) ImGui::Text("%s = %s", k.c_str(), ai::to_string(v).c_str());
    }
    ImGui::PopTextWrapPos();
}

void Editor::draw_behavior_tree_editor() {
    if (!show_bt_editor_) return;
    std::string title = "Behaviour Tree";
    if (bt_doc_) title += std::string(" - ") + bt_doc_->asset_path() + (bt_doc_->dirty() ? " *" : "");
    title += "###BehaviourTree";
    ImGui::SetNextWindowSize(ImVec2(1000, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title.c_str(), &show_bt_editor_)) {
        ImGui::End();
        return;
    }
    if (!bt_doc_) {
        if (kit::empty_state(kit::Icon::Tree, "No behaviour tree open",
                             "Behaviour trees decide what AI characters do: patrol, chase, flee. Double-click a .vbt file "
                             "in Assets, or create a new one.",
                             db_ ? "New Behaviour Tree" : nullptr)) {
            if (auto id = create_behavior_tree(); !id) notify("cannot create behaviour tree: " + id.error().message, true);
        }
        ImGui::End();
        return;
    }
    BehaviorTreeDocument& doc = *bt_doc_;
    if (bt_selected_ >= static_cast<int>(doc.nodes().size())) bt_selected_ = 0;

    // Live debugging: the selected entity's runtime state, when it runs this tree.
    std::optional<ai::TreeDebug> live;
    live_bb_ = nullptr;
    if (play_state_ != PlayState::Editing && play_ai_ && !selection_.empty()) {
        scene::Scene& s = active_scene();
        const scene::Entity e = s.find(selection_.front());
        if (!e.is_null()) {
            live = play_ai_->debug(s, e);
            if (live && live->tree != doc.asset_path()) live.reset();
            if (live) live_bb_ = play_ai_->blackboard(s, e);
        }
    }

    // Toolbar.
    if (kit::icon_text_button("Save", kit::Icon::Save, doc.dirty())) {
        if (auto st = save_behavior_tree(); !st.ok()) notify("cannot save: " + st.error().message, true);
    }
    ImGui::SameLine();
    if (kit::icon_button("##btundo", kit::Icon::Undo, "Undo (Ctrl+Z)", false, doc.can_undo())) {
        doc.undo();
        bt_selected_ = std::min(bt_selected_, static_cast<int>(doc.nodes().size()) - 1);
    }
    ImGui::SameLine();
    if (kit::icon_button("##btredo", kit::Icon::Redo, "Redo (Ctrl+Y)", false, doc.can_redo())) {
        doc.redo();
        bt_selected_ = std::min(bt_selected_, static_cast<int>(doc.nodes().size()) - 1);
    }
    ImGui::SameLine();
    if (kit::icon_button("##btframe", kit::Icon::Focus, "Frame the whole tree (F)")) bt_frame_ = true;
    ImGui::SameLine();
    const std::string problem = doc.validate();
    if (problem.empty())
        kit::pill("Ready to run", ui::Tone::Success);
    else
        kit::pill("Will not run", ui::Tone::Danger);
    if (!problem.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", problem.c_str());
    if (live) {
        ImGui::SameLine();
        kit::pill(doc.dirty() ? "Live (save to see statuses)" : "Live", ui::Tone::Accent);
    }
    if (!bt_message_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(tk::kWarning, "%s", bt_message_.c_str());
    }

    // Keyboard shortcuts while the window is focused (and no text field is being edited).
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
        const bool ctrl = ImGui::GetIO().KeyCtrl;
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            if (auto st = save_behavior_tree(); !st.ok()) notify("cannot save: " + st.error().message, true);
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z) && doc.undo()) bt_selected_ = std::min(bt_selected_, static_cast<int>(doc.nodes().size()) - 1);
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y) && doc.redo()) bt_selected_ = std::min(bt_selected_, static_cast<int>(doc.nodes().size()) - 1);
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && bt_selected_ > 0) bt_apply(doc.duplicate(bt_selected_));
        if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_Delete, false) && bt_selected_ > 0) bt_apply(doc.remove(bt_selected_));
        if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) bt_frame_ = true;
        if (ImGui::GetIO().KeyAlt && ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && bt_selected_ > 0) bt_apply(doc.move(bt_selected_, -1));
        if (ImGui::GetIO().KeyAlt && ImGui::IsKeyPressed(ImGuiKey_RightArrow) && bt_selected_ > 0) bt_apply(doc.move(bt_selected_, +1));
    }

    const float avail = ImGui::GetContentRegionAvail().x;
    const float palette_w = std::clamp(avail * 0.17f, 150.0f, 220.0f);
    const float details_w = std::clamp(avail * 0.26f, 220.0f, 340.0f);
    ImGui::BeginChild("##btpalette", ImVec2(palette_w, 0), ImGuiChildFlags_Borders);
    draw_bt_palette();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##btcanvaswrap", ImVec2(avail - palette_w - details_w - ImGui::GetStyle().ItemSpacing.x * 2, 0),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    draw_bt_canvas(live);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##btdetails", ImVec2(0, 0), ImGuiChildFlags_Borders);
    draw_bt_details(live);
    ImGui::EndChild();
    ImGui::End();
}

} // namespace victus::editor
