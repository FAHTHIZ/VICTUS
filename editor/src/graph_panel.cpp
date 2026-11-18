// SPDX-License-Identifier: Apache-2.0
// Visual script graphs in the editor (ADR-0059): creating .vgraph assets and the Graph Editor
// window, a node canvas (pan, drag nodes, drag pins to link, right-click palette) with a side panel
// for variables, the selected node's literals and the graph's diagnostics.
//
// Graph files are project assets, not scene data: their edits have their own undo history (JSON
// snapshots of the graph) and are written to disk by Save, which hot-reloads every ScriptHost that
// runs the graph. The edit API is public so tests drive it without a window.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"
#include "victus/runtime/surface_shader.hpp"
#include "victus/graph/material_graph.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>

namespace victus::editor {

using namespace scene;
namespace sfs = std::filesystem;
using graph::GraphDiagnostic;
using graph::NodeKind;
using graph::PinType;

namespace {

constexpr float kNodeWidth = 190.0f;
constexpr float kTitleHeight = 22.0f;
constexpr float kRowHeight = 20.0f;
constexpr float kPinRadius = 5.0f;
constexpr std::size_t kMaxGraphUndo = 200;

ImU32 pin_color(PinType t) {
    switch (t) {
    case PinType::Exec: return IM_COL32(235, 235, 235, 255);
    case PinType::Number: return IM_COL32(120, 200, 120, 255);
    case PinType::Bool: return IM_COL32(220, 80, 80, 255);
    case PinType::String: return IM_COL32(230, 120, 220, 255);
    case PinType::Vector: return IM_COL32(240, 200, 70, 255);
    case PinType::Entity: return IM_COL32(80, 170, 240, 255);
    case PinType::Vec2: return IM_COL32(110, 215, 215, 255);
    case PinType::Vec4: return IM_COL32(225, 140, 245, 255);
    case PinType::Any: break;
    }
    return IM_COL32(160, 160, 160, 255);
}

ImU32 category_color(std::string_view category) {
    if (category == "Events") return IM_COL32(150, 40, 40, 255);
    if (category == "Flow") return IM_COL32(90, 90, 100, 255);
    if (category == "Variables") return IM_COL32(40, 110, 70, 255);
    if (category == "Math" || category == "Vector") return IM_COL32(50, 100, 60, 255);
    if (category == "Logic") return IM_COL32(110, 40, 40, 255);
    if (category == "Input") return IM_COL32(120, 80, 30, 255);
    if (category == "Physics") return IM_COL32(40, 80, 130, 255);
    if (category == "Output") return IM_COL32(150, 40, 40, 255);
    if (category == "Constant" || category == "UV") return IM_COL32(110, 80, 30, 255);
    if (category == "Texture" || category == "Effects") return IM_COL32(100, 50, 120, 255);
    return IM_COL32(45, 75, 125, 255);
}

// The default literal of a variable or pin of type `t`.
json::Value type_default(PinType t) {
    switch (t) {
    case PinType::Number: return json::Value(0.0);
    case PinType::Bool: return json::Value(false);
    case PinType::String: return json::Value(std::string());
    case PinType::Vector: return json::Value(json::Array{json::Value(0.0), json::Value(0.0), json::Value(0.0)});
    case PinType::Vec2: return json::Value(json::Array{json::Value(0.0), json::Value(0.0)});
    case PinType::Vec4: return json::Value(json::Array{json::Value(1.0), json::Value(1.0), json::Value(1.0), json::Value(1.0)});
    default: break;
    }
    return {};
}

std::string short_literal(const json::Value& v) {
    if (v.is_number()) return std::format("{:g}", v.as_number());
    if (v.is_bool()) return v.as_bool() ? "true" : "false";
    if (v.is_string() && v.as_string().starts_with("=")) return v.as_string().substr(1); // material graph default expression
    if (v.is_string()) {
        std::string s = v.as_string();
        if (s.size() > 14) s = s.substr(0, 12) + "..";
        return "\"" + s + "\"";
    }
    if (v.is_array() && v.size() == 2) return std::format("({:g}, {:g})", v[0].as_number(), v[1].as_number());
    if (v.is_array() && v.size() == 3)
        return std::format("({:g}, {:g}, {:g})", v[0].as_number(), v[1].as_number(), v[2].as_number());
    if (v.is_array() && v.size() == 4)
        return std::format("({:g}, {:g}, {:g}, {:g})", v[0].as_number(), v[1].as_number(), v[2].as_number(), v[3].as_number());
    return {};
}

bool pin_linked_in(const graph::ScriptGraph& g, std::uint32_t node, std::string_view pin) {
    return std::ranges::any_of(g.links, [&](const graph::GraphLink& l) { return l.to_node == node && l.to_pin == pin; });
}
bool pin_linked_out(const graph::ScriptGraph& g, std::uint32_t node, std::string_view pin) {
    return std::ranges::any_of(g.links, [&](const graph::GraphLink& l) { return l.from_node == node && l.from_pin == pin; });
}

std::string node_title(const graph::GraphNode& n) {
    const graph::NodeDesc* d = graph::find_node_desc(n.type);
    std::string title = d ? d->title : n.type;
    if (d && (d->kind == NodeKind::GetVariable || d->kind == NodeKind::SetVariable)) {
        const std::string var = n.config["variable"].as_string_or("");
        title = (d->kind == NodeKind::GetVariable ? "Get " : "Set ") + (var.empty() ? std::string("?") : var);
    }
    return title;
}

bool edit_floats(const char* label, int n, json::Value& v, bool color) {
    float f[4] = {0, 0, 0, 1};
    if (v.is_array())
        for (std::size_t i = 0; i < static_cast<std::size_t>(n) && i < v.size(); ++i) f[i] = v[i].as_float(0.0f);
    constexpr ImGuiColorEditFlags kHdr = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR;
    bool changed = false;
    if (color && n == 3) changed = ImGui::ColorEdit3(label, f, kHdr);
    else if (color && n == 4) changed = ImGui::ColorEdit4(label, f, kHdr);
    else if (n == 2) changed = ImGui::DragFloat2(label, f, 0.05f);
    else if (n == 3) changed = ImGui::DragFloat3(label, f, 0.05f);
    else changed = ImGui::DragFloat4(label, f, 0.05f);
    if (!changed) return false;
    json::Array a;
    for (int i = 0; i < n; ++i) a.emplace_back(static_cast<double>(f[i]));
    v = json::Value(std::move(a));
    return true;
}

// Literal editor for an unlinked input or a variable default. Returns true when `v` changed.
bool edit_literal(const char* label, PinType type, json::Value& v) {
    if (type != PinType::String && type != PinType::Any && v.is_string() && v.as_string().starts_with("=")) {
        // A material graph input whose default is an expression (e.g. the mesh UVs).
        ImGui::TextDisabled("%s: %s", label, v.as_string().c_str() + 1);
        ImGui::SameLine();
        if (ImGui::SmallButton("Set")) {
            v = type_default(type);
            if (type == PinType::Number) v = json::Value(0.5);
            return true;
        }
        ImGui::SetItemTooltip("Replace with a constant value");
        return false;
    }
    const bool color = std::string_view(label).find("color") != std::string_view::npos ||
                       std::string_view(label).find("emissive") != std::string_view::npos;
    switch (type) {
    case PinType::Vec2: return edit_floats(label, 2, v, false);
    case PinType::Vec4: return edit_floats(label, 4, v, color);
    case PinType::Number: {
        float f = v.as_float(0.0f);
        if (ImGui::DragFloat(label, &f, 0.05f)) {
            v = json::Value(static_cast<double>(f));
            return true;
        }
        return false;
    }
    case PinType::Bool: {
        bool b = v.as_bool(false);
        if (ImGui::Checkbox(label, &b)) {
            v = json::Value(b);
            return true;
        }
        return false;
    }
    case PinType::Vector: {
        if (color) return edit_floats(label, 3, v, true);
        float f[3] = {0, 0, 0};
        if (v.is_array())
            for (std::size_t i = 0; i < 3 && i < v.size(); ++i) f[i] = v[i].as_float(0.0f);
        if (ImGui::DragFloat3(label, f, 0.05f)) {
            v = json::Value(json::Array{json::Value(static_cast<double>(f[0])), json::Value(static_cast<double>(f[1])),
                                        json::Value(static_cast<double>(f[2]))});
            return true;
        }
        return false;
    }
    case PinType::String: {
        std::string s = v.as_string_or("");
        if (ImGui::InputText(label, &s)) {
            v = json::Value(s);
            return true;
        }
        return false;
    }
    case PinType::Any: {
        // Numbers stay numbers, anything else is text.
        std::string s = v.is_number() ? std::format("{:g}", v.as_number()) : v.as_string_or(short_literal(v));
        if (ImGui::InputText(label, &s)) {
            char* end = nullptr;
            const double num = std::strtod(s.c_str(), &end);
            v = (!s.empty() && end && *end == '\0') ? json::Value(num) : json::Value(s);
            return true;
        }
        return false;
    }
    case PinType::Entity:
    case PinType::Exec: break;
    }
    ImGui::TextDisabled("%s: link a value", label);
    return false;
}

constexpr PinType kVariableTypes[] = {PinType::Number, PinType::Bool, PinType::String, PinType::Vector, PinType::Entity,
                                      PinType::Any};

// On Start -> Log, the graph a new visual script begins with.
graph::ScriptGraph starter_graph() {
    graph::ScriptGraph g;
    const auto start = g.add_node("event.start", 0, 0);
    const auto log = g.add_node("debug.log", 260, 0);
    g.node(log)->inputs.set("message", json::Value("Hello from a visual script"));
    (void)g.connect(start, "out", log, "in");
    return g;
}

} // namespace

// ---- assets ---------------------------------------------------------------------------------------

Result<Uuid> Editor::create_graph(std::string_view relative_path) {
    return create_graph_file(relative_path, graph::GraphDomain::Script);
}

Result<Uuid> Editor::create_material_graph(std::string_view relative_path) {
    return create_graph_file(relative_path, graph::GraphDomain::Material);
}

Result<Uuid> Editor::create_graph_file(std::string_view relative_path, graph::GraphDomain domain) {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project open");
    const bool material = domain == graph::GraphDomain::Material;
    const std::string ext = material ? std::string(graph::kMaterialGraphExtension) : ".vgraph";
    const std::string stem = material ? "shaders/MaterialGraph" : "scripts/Graph";
    std::string rel(relative_path);
    if (rel.empty()) {
        rel = stem + ext;
        for (int i = 2; sfs::exists(db_->assets_dir() / rel); ++i) rel = std::format("{}_{}{}", stem, i, ext);
    } else if (!rel.ends_with(ext)) {
        rel += ext;
    }
    auto clean = fs::sanitize_relative(rel);
    if (!clean) return std::move(clean).error();
    if (sfs::exists(db_->assets_dir() / *clean)) return make_error(ErrorCode::AlreadyExists, *clean + " already exists");
    std::error_code ec;
    sfs::create_directories((db_->assets_dir() / *clean).parent_path(), ec);
    VICTUS_TRY_STATUS(fs::write_text_atomic(db_->assets_dir() / *clean,
                                            graph::serialize_graph(material ? graph::starter_material_graph() : starter_graph())));
    (void)db_->refresh();
    const assets::AssetRecord* rec = db_->find_path(*clean);
    if (!rec) return make_error(ErrorCode::IoError, "graph was written but not registered: " + *clean);
    notify((material ? "Created material graph " : "Created visual script ") + *clean);
    return rec->uuid;
}

Status Editor::open_graph_editor(const Uuid& asset) {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project open");
    const assets::AssetRecord* rec = db_->find(asset);
    if (!rec || !(rec->path.ends_with(".vgraph") || rec->path.ends_with(graph::kMaterialGraphExtension)))
        return make_error(ErrorCode::NotFound, "graph asset " + asset.to_string() + " not found");
    auto text = fs::read_text(db_->source_path(*rec));
    if (!text) return std::move(text).error();
    auto g = graph::parse_graph(*text);
    if (!g) return make_error(g.error().code, rec->path + ": " + g.error().message);
    // The extension decides what the graph compiles to.
    g->domain = rec->path.ends_with(graph::kMaterialGraphExtension) ? graph::GraphDomain::Material : graph::GraphDomain::Script;
    graph_ = std::move(*g);
    graph_asset_ = asset;
    graph_show_code_ = false;
    graph_path_ = rec->path;
    graph_disk_hash_ = fnv1a64(std::string_view(*text));
    graph_undo_.clear();
    graph_redo_.clear();
    graph_edit_key_.clear();
    graph_dirty_ = false;
    graph_selected_ = 0;
    graph_link_drag_.reset();
    graph_palette_link_.reset();
    graph_node_drag_ = false;
    graph_diags_ = graph::validate_graph(graph_);
    graph_heat_.clear();
    graph_hits_seen_.clear();
    graph_error_.reset();
    graph_live_ = false;
    // Frame the graph: its top-left node near the canvas corner.
    if (!graph_.nodes.empty()) {
        float min_x = graph_.nodes.front().x, min_y = graph_.nodes.front().y;
        for (const auto& n : graph_.nodes) {
            min_x = std::min(min_x, n.x);
            min_y = std::min(min_y, n.y);
        }
        graph_scroll_ = {40.0f - min_x, 40.0f - min_y};
    }
    show_graph_editor_ = true;
    return {};
}

Status Editor::save_graph() {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project open");
    const assets::AssetRecord* rec = graph_asset_.is_nil() ? nullptr : db_->find(graph_asset_);
    if (!rec) return make_error(ErrorCode::NotFound, "no graph open");
    const std::string path = rec->path; // records move when the hot reloader refreshes the database
    const std::string text = graph::serialize_graph(graph_);
    VICTUS_TRY_STATUS(fs::write_text_atomic(db_->source_path(*rec), text));
    graph_disk_hash_ = fnv1a64(std::string_view(text));
    graph_dirty_ = false;
    std::size_t errors = 0;
    for (const auto& d : graph_diags_) errors += d.severity == GraphDiagnostic::Severity::Error;
    if (graph_.domain == graph::GraphDomain::Material) {
        // The asset hot reloader recompiles the shader for the viewport; report GLSL errors now.
        if (!errors) {
            const auto compiled = graph::compile_material_graph(graph_, path);
            if (auto c = runtime::compile_surface_shader(compiled.source, path, false); !c) {
                notify("Saved " + path + ", but the shader does not compile: " + c.error().message, true);
                return {};
            }
        }
        (void)check_asset_changes();
    } else {
        // The inspector's property discovery and a running play session pick up the new module.
        if (edit_scripts_) (void)edit_scripts_->reload_changed();
        if (play_scripts_) (void)play_scripts_->reload_changed();
    }
    if (errors) notify(std::format("Saved {} ({} error{})", path, errors, errors == 1 ? "" : "s"), true);
    else notify("Saved " + path);
    return {};
}

void Editor::sync_graph_from_disk() {
    if (graph_asset_.is_nil() || graph_dirty_ || !db_) return;
    const assets::AssetRecord* rec = db_->find(graph_asset_);
    if (!rec) return;
    auto text = fs::read_text(db_->source_path(*rec));
    if (!text || fnv1a64(std::string_view(*text)) == graph_disk_hash_) return;
    auto g = graph::parse_graph(*text);
    if (!g) return; // keep the last good graph while the file is being written or is broken
    g->domain = graph_.domain;
    graph_begin_edit();
    graph_ = std::move(*g);
    graph_disk_hash_ = fnv1a64(std::string_view(*text));
    graph_diags_ = graph::validate_graph(graph_);
    graph_dirty_ = false;
}

bool Editor::assign_graph_to(Entity e) {
    if (e.is_null() || graph_path_.empty()) return false;
    auto before = capture(active_scene(), {active_scene().uuid(e)});
    auto& w = active_scene().world();
    if (!w.has<script::ScriptComponent>(e)) w.add<script::ScriptComponent>(e);
    w.get<script::ScriptComponent>(e)->script = graph_path_;
    record_edit("Assign visual script", std::move(before));
    return true;
}

// ---- debugging ----------------------------------------------------------------------------------

void Editor::update_graph_debug(float dt) {
    for (auto& [id, heat] : graph_heat_) heat = std::max(0.0f, heat - dt * 1.5f);
    if (!play_scripts_ || graph_path_.empty()) {
        graph_live_ = false;
        return;
    }
    if (!graph_live_) { // a new play session: counts start over
        graph_live_ = true;
        graph_hits_seen_.clear();
        graph_heat_.clear();
    }
    auto hits = play_scripts_->graph_node_hits(graph_path_);
    for (const auto& [id, runs] : hits) {
        const auto it = graph_hits_seen_.find(id);
        if (it == graph_hits_seen_.end() || it->second != runs) graph_heat_[id] = 1.0f;
    }
    graph_hits_seen_ = std::move(hits);
    graph_error_ = play_scripts_->last_graph_error(graph_path_);
}

float Editor::graph_node_heat(std::uint32_t node) const {
    const auto it = graph_heat_.find(node);
    return it == graph_heat_.end() ? 0.0f : it->second;
}

std::uint64_t Editor::graph_node_runs(std::uint32_t node) const {
    const auto it = graph_hits_seen_.find(node);
    return it == graph_hits_seen_.end() ? 0 : it->second;
}

// ---- edits ----------------------------------------------------------------------------------------

void Editor::graph_begin_edit(std::string_view coalesce_key) {
    if (!coalesce_key.empty() && coalesce_key == graph_edit_key_) return;
    graph_edit_key_ = std::string(coalesce_key);
    graph_undo_.push_back(graph::serialize_graph(graph_));
    if (graph_undo_.size() > kMaxGraphUndo) graph_undo_.erase(graph_undo_.begin());
    graph_redo_.clear();
}

void Editor::graph_end_edit() {
    graph_dirty_ = true;
    graph_diags_ = graph::validate_graph(graph_);
}

std::uint32_t Editor::graph_add_node(std::string_view type, float x, float y) {
    if (graph_asset_.is_nil() || !graph::find_node_desc(type, graph_.domain)) return 0;
    graph_begin_edit();
    const std::uint32_t id = graph_.add_node(std::string(type), x, y);
    graph_end_edit();
    graph_selected_ = id;
    return id;
}

bool Editor::graph_remove_node(std::uint32_t id) {
    if (!graph_.node(id)) return false;
    graph_begin_edit();
    graph_.remove_node(id);
    graph_end_edit();
    if (graph_selected_ == id) graph_selected_ = 0;
    return true;
}

bool Editor::graph_move_node(std::uint32_t id, float x, float y) {
    if (!graph_.node(id)) return false;
    graph_begin_edit();
    graph_.node(id)->x = x;
    graph_.node(id)->y = y;
    graph_end_edit();
    return true;
}

bool Editor::graph_connect(std::uint32_t from, std::string_view from_pin, std::uint32_t to, std::string_view to_pin) {
    if (from == to) return false;
    graph::ScriptGraph trial = graph_;
    if (!trial.connect(from, from_pin, to, to_pin)) return false;
    graph_begin_edit();
    graph_ = std::move(trial);
    graph_end_edit();
    return true;
}

bool Editor::graph_disconnect(std::uint32_t node, std::string_view input_pin) {
    if (!pin_linked_in(graph_, node, input_pin)) return false;
    graph_begin_edit();
    graph_.disconnect_input(node, input_pin);
    graph_end_edit();
    return true;
}

bool Editor::graph_set_literal(std::uint32_t node, std::string_view input_pin, json::Value value) {
    graph::GraphNode* n = graph_.node(node);
    if (!n) return false;
    const auto pins = graph_.input_pins(*n);
    const auto it = std::ranges::find_if(pins, [&](const graph::PinDesc& p) { return p.name == input_pin; });
    if (it == pins.end() || it->type == PinType::Exec) return false;
    graph_begin_edit(std::format("literal:{}:{}", node, input_pin));
    graph_.node(node)->inputs.set(input_pin, std::move(value));
    graph_end_edit();
    return true;
}

bool Editor::graph_set_config(std::uint32_t node, std::string_view key, json::Value value) {
    if (!graph_.node(node)) return false;
    graph_begin_edit(std::format("config:{}:{}", node, key));
    graph_.node(node)->config.set(key, std::move(value));
    graph_end_edit();
    return true;
}

bool Editor::graph_add_variable(graph::GraphVariable variable) {
    if (graph_asset_.is_nil() || variable.name.empty() || graph_.variable(variable.name)) return false;
    if (graph_.domain == graph::GraphDomain::Material) return false; // material graphs use Parameter nodes
    if (variable.default_value.is_null()) variable.default_value = type_default(variable.type);
    graph_begin_edit();
    graph_.variables.push_back(std::move(variable));
    graph_end_edit();
    return true;
}

bool Editor::graph_update_variable(std::string_view name, graph::GraphVariable variable) {
    const auto it = std::ranges::find_if(graph_.variables, [&](const graph::GraphVariable& v) { return v.name == name; });
    if (it == graph_.variables.end() || variable.name.empty()) return false;
    if (variable.name != name && graph_.variable(variable.name)) return false;
    const auto index = static_cast<std::size_t>(it - graph_.variables.begin());
    graph_begin_edit(std::format("variable:{}", index));
    const std::string old_name(name);
    if (variable.name != old_name)
        for (auto& n : graph_.nodes)
            if (n.config["variable"].as_string_or("") == old_name) n.config.set("variable", json::Value(variable.name));
    graph_.variables[index] = std::move(variable);
    graph_end_edit();
    return true;
}

bool Editor::graph_remove_variable(std::string_view name) {
    const auto it = std::ranges::find_if(graph_.variables, [&](const graph::GraphVariable& v) { return v.name == name; });
    if (it == graph_.variables.end()) return false;
    graph_begin_edit();
    graph_.variables.erase(it);
    graph_end_edit();
    return true;
}

bool Editor::graph_undo() {
    if (graph_undo_.empty()) return false;
    auto prev = graph::parse_graph(graph_undo_.back());
    if (!prev) return false;
    graph_redo_.push_back(graph::serialize_graph(graph_));
    graph_undo_.pop_back();
    prev->domain = graph_.domain;
    graph_ = std::move(*prev);
    graph_edit_key_.clear();
    graph_end_edit();
    if (graph_selected_ && !graph_.node(graph_selected_)) graph_selected_ = 0;
    return true;
}

bool Editor::graph_redo() {
    if (graph_redo_.empty()) return false;
    auto next = graph::parse_graph(graph_redo_.back());
    if (!next) return false;
    graph_undo_.push_back(graph::serialize_graph(graph_));
    graph_redo_.pop_back();
    next->domain = graph_.domain;
    graph_ = std::move(*next);
    graph_edit_key_.clear();
    graph_end_edit();
    if (graph_selected_ && !graph_.node(graph_selected_)) graph_selected_ = 0;
    return true;
}

// ---- window ---------------------------------------------------------------------------------------

void Editor::draw_graph_editor() {
    graph_focused_ = false;
    if (!show_graph_editor_) return;
    ImGui::SetNextWindowSize(ImVec2(1000, 600), ImGuiCond_FirstUseEver);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    const std::string title = std::format("Graph Editor{}###GraphEditor", graph_dirty_ ? " *" : "");
    if (!ImGui::Begin(title.c_str(), &show_graph_editor_)) {
        ImGui::End();
        return;
    }
    graph_focused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (graph_asset_.is_nil() || !db_ || !db_->find(graph_asset_)) {
        ImGui::TextDisabled("Double-click a .vgraph or .vmatgraph in Assets, or create one:");
        if (ImGui::Button("New Visual Script") && db_) {
            if (auto id = create_graph(); !id) notify("cannot create visual script: " + id.error().message, true);
            else if (auto st = open_graph_editor(*id); !st.ok()) notify(st.error().message, true);
        }
        ImGui::SameLine();
        if (ImGui::Button("New Material Graph") && db_) {
            if (auto id = create_material_graph(); !id) notify("cannot create material graph: " + id.error().message, true);
            else if (auto st = open_graph_editor(*id); !st.ok()) notify(st.error().message, true);
        }
        ImGui::End();
        return;
    }

    // Toolbar.
    ImGui::TextUnformatted(graph_path_.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        if (auto st = save_graph(); !st.ok()) notify(st.error().message, true);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(graph_undo_.empty());
    if (ImGui::Button("Undo")) (void)graph_undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(graph_redo_.empty());
    if (ImGui::Button("Redo")) (void)graph_redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (graph_.domain == graph::GraphDomain::Material) {
        ImGui::BeginDisabled(!show_material_editor_ || material_editing_.is_nil());
        if (ImGui::Button("Use in Material")) (void)set_editing_material_shader(graph_asset_);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Makes this graph the surface shader of the material open in the Material Editor");
        ImGui::SameLine();
        ImGui::Checkbox("GLSL", &graph_show_code_);
        ImGui::SetItemTooltip("Show the surface shader this graph compiles to");
    } else {
        ImGui::BeginDisabled(selected().is_null());
        if (ImGui::Button("Use on Selected")) (void)assign_graph_to(selected());
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    std::size_t errors = 0, warnings = 0;
    for (const auto& d : graph_diags_) (d.severity == GraphDiagnostic::Severity::Error ? errors : warnings)++;
    if (errors) ImGui::TextColored(ui::tokens::kDanger, "%zu error%s", errors, errors == 1 ? "" : "s");
    else ImGui::TextColored(ui::tokens::kSuccess, "Compiles");
    if (warnings) {
        ImGui::SameLine();
        ImGui::TextColored(ui::tokens::kWarning, "%zu warning%s", warnings, warnings == 1 ? "" : "s");
    }
    if (graph_live_) {
        ImGui::SameLine();
        ImGui::TextColored(ui::tokens::kWarning, "Live");
        ImGui::SetItemTooltip("Playing: nodes light up as they run");
    }
    if (graph_error_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::tokens::kDanger);
        ImGui::PushTextWrapPos(0);
        const std::string label = graph_error_->node ? std::format("Runtime error in node #{}: {}", graph_error_->node, graph_error_->message)
                                                     : "Runtime error: " + graph_error_->message;
        if (ImGui::Selectable(label.c_str()) && graph_error_->node) {
            graph_selected_ = graph_error_->node;
            if (const auto* target = graph_.node(graph_error_->node)) graph_scroll_ = {120.0f - target->x, 80.0f - target->y};
        }
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    ImGui::BeginChild("##graph_side", ImVec2(260, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    draw_graph_side_panel();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##graph_canvas", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    draw_graph_canvas();
    ImGui::EndChild();

    // Graph shortcuts (the scene's are suppressed while this window has focus).
    if (graph_focused_ && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
            if (auto st = save_graph(); !st.ok()) notify(st.error().message, true);
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) (void)graph_undo();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))
            (void)graph_redo();
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && graph_selected_) (void)graph_remove_node(graph_selected_);
    }
    ImGui::End();
}

void Editor::draw_graph_side_panel() {
    const bool material = graph_.domain == graph::GraphDomain::Material;
    if (material && graph_show_code_) {
        ImGui::SeparatorText("Generated GLSL");
        const auto compiled = graph::compile_material_graph(graph_, graph_path_);
        std::string code = compiled.ok() ? compiled.source : "// fix the graph's errors first\n";
        ImGui::InputTextMultiline("##glsl", &code, ImVec2(-1, 220), ImGuiInputTextFlags_ReadOnly);
    }
    if (material) {
        ImGui::SeparatorText("Material Graph");
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Right-click the canvas to add nodes. Linked Surface Output inputs (or ones given a value) "
                            "replace the material's values. Parameter nodes read the material's shader parameters.");
        ImGui::PopTextWrapPos();
    }
    // Variables.
    if (!material) ImGui::SeparatorText("Variables");
    if (!material) {
        std::optional<std::string> remove;
        for (std::size_t i = 0; i < graph_.variables.size(); ++i) {
            const graph::GraphVariable var = graph_.variables[i];
            ImGui::PushID(static_cast<int>(i));
            graph::GraphVariable next = var;
            bool edited = false;
            ImGui::SetNextItemWidth(110);
            if (ImGui::InputText("##name", &next.name)) edited = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            const std::string type_name(graph::pin_type_name(var.type));
            if (ImGui::BeginCombo("##type", type_name.c_str())) {
                for (PinType t : kVariableTypes) {
                    const std::string n(graph::pin_type_name(t));
                    if (ImGui::Selectable(n.c_str(), t == var.type) && t != var.type) {
                        next.type = t;
                        next.default_value = type_default(t);
                        edited = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove = var.name;
            if (ImGui::Checkbox("Exposed", &next.exposed)) edited = true;
            ImGui::SetItemTooltip("Exposed variables are editable per entity in the Inspector");
            ImGui::SameLine();
            if (ImGui::SmallButton("Get")) {
                if (const auto id = graph_add_node("variable.get", -graph_scroll_.x + 60, -graph_scroll_.y + 60))
                    (void)graph_set_config(id, "variable", json::Value(var.name));
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Set")) {
                if (const auto id = graph_add_node("variable.set", -graph_scroll_.x + 60, -graph_scroll_.y + 60))
                    (void)graph_set_config(id, "variable", json::Value(var.name));
            }
            if (var.type != PinType::Entity && var.type != PinType::Any) {
                ImGui::SetNextItemWidth(-1);
                if (edit_literal("##default", var.type, next.default_value)) edited = true;
            }
            if (edited && !(next.name != var.name && graph_.variable(next.name))) (void)graph_update_variable(var.name, next);
            ImGui::Separator();
            ImGui::PopID();
        }
        if (remove) (void)graph_remove_variable(*remove);
        if (ImGui::Button("+ Variable")) {
            std::string name = "value";
            for (int i = 2; graph_.variable(name); ++i) name = std::format("value{}", i);
            (void)graph_add_variable({name, PinType::Number, json::Value(0.0), true});
        }
    }

    // Selected node.
    if (graph::GraphNode* n = graph_selected_ ? graph_.node(graph_selected_) : nullptr) {
        ImGui::SeparatorText("Node");
        const graph::NodeDesc* d = graph::find_node_desc(n->type);
        ImGui::Text("%s  #%u", node_title(*n).c_str(), n->id);
        ImGui::TextDisabled("%s", n->type.c_str());
        if (d && !d->tooltip.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s", d->tooltip.c_str());
            ImGui::PopTextWrapPos();
        }
        const std::uint32_t id = n->id;
        if (d && (d->kind == NodeKind::GetVariable || d->kind == NodeKind::SetVariable)) {
            const std::string cur = n->config["variable"].as_string_or("");
            if (ImGui::BeginCombo("Variable", cur.empty() ? "(none)" : cur.c_str())) {
                for (const auto& v : graph_.variables)
                    if (ImGui::Selectable(v.name.c_str(), v.name == cur)) (void)graph_set_config(id, "variable", json::Value(v.name));
                ImGui::EndCombo();
            }
        }
        for (const auto& pin : graph_.input_pins(*graph_.node(id))) {
            if (pin.type == PinType::Exec || pin_linked_in(graph_, id, pin.name)) continue;
            const json::Value* lit = graph_.node(id)->inputs.find(pin.name);
            json::Value v = lit ? *lit : pin.default_value;
            ImGui::PushID(pin.name.c_str());
            if (edit_literal(pin.name.c_str(), pin.type, v)) (void)graph_set_literal(id, pin.name, std::move(v));
            ImGui::PopID();
        }
        std::string comment = graph_.node(id)->comment;
        if (ImGui::InputTextWithHint("Comment", "notes", &comment)) {
            graph_begin_edit(std::format("comment:{}", id));
            graph_.node(id)->comment = comment;
            graph_end_edit();
        }
        if (ImGui::Button("Delete Node")) (void)graph_remove_node(id);
    }

    // Diagnostics.
    if (!graph_diags_.empty()) {
        ImGui::SeparatorText("Problems");
        for (std::size_t i = 0; i < graph_diags_.size(); ++i) {
            const GraphDiagnostic& d = graph_diags_[i];
            const ImVec4 col = d.severity == GraphDiagnostic::Severity::Error ? ui::tokens::kDanger : ui::tokens::kWarning;
            ImGui::PushID(static_cast<int>(i));
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            const std::string where = d.node ? std::format("#{} ", d.node) : std::string();
            ImGui::PushTextWrapPos(0);
            if (ImGui::Selectable((where + d.message).c_str(), d.node && d.node == graph_selected_) && d.node) {
                graph_selected_ = d.node;
                if (const auto* target = graph_.node(d.node)) graph_scroll_ = {120.0f - target->x, 80.0f - target->y};
            }
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
}

void Editor::draw_graph_canvas() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = std::max(size.x, 50.0f);
    size.y = std::max(size.y, 50.0f);
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    ImGui::InvisibleButton("##canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 origin(p0.x + graph_scroll_.x, p0.y + graph_scroll_.y);
    const ImVec2 mouse = io.MousePos;

    dl->PushClipRect(p0, p1, true);
    dl->AddRectFilled(p0, p1, IM_COL32(28, 29, 33, 255));
    constexpr float kGrid = 32.0f;
    for (float x = std::fmod(graph_scroll_.x, kGrid); x < size.x; x += kGrid)
        dl->AddLine(ImVec2(p0.x + x, p0.y), ImVec2(p0.x + x, p1.y), IM_COL32(45, 46, 52, 255));
    for (float y = std::fmod(graph_scroll_.y, kGrid); y < size.y; y += kGrid)
        dl->AddLine(ImVec2(p0.x, p0.y + y), ImVec2(p1.x, p0.y + y), IM_COL32(45, 46, 52, 255));

    // Layout: node rectangles and pin positions in screen space.
    struct PinPos {
        std::string name;
        PinType type;
        ImVec2 pos;
        json::Value def;
    };
    struct Layout {
        std::uint32_t id;
        ImVec2 min, max;
        std::vector<PinPos> ins, outs;
    };
    std::vector<Layout> layouts;
    layouts.reserve(graph_.nodes.size());
    for (const auto& n : graph_.nodes) {
        Layout l{n.id, {}, {}, {}, {}};
        const auto ins = graph_.input_pins(n);
        const auto outs = graph_.output_pins(n);
        const std::size_t rows = std::max<std::size_t>(1, std::max(ins.size(), outs.size()));
        l.min = ImVec2(origin.x + n.x, origin.y + n.y);
        l.max = ImVec2(l.min.x + kNodeWidth, l.min.y + kTitleHeight + kRowHeight * static_cast<float>(rows) + 6.0f);
        for (std::size_t i = 0; i < ins.size(); ++i)
            l.ins.push_back({ins[i].name, ins[i].type,
                             ImVec2(l.min.x, l.min.y + kTitleHeight + kRowHeight * (static_cast<float>(i) + 0.5f) + 3.0f),
                             ins[i].default_value});
        for (std::size_t i = 0; i < outs.size(); ++i)
            l.outs.push_back({outs[i].name, outs[i].type,
                              ImVec2(l.max.x, l.min.y + kTitleHeight + kRowHeight * (static_cast<float>(i) + 0.5f) + 3.0f), {}});
        layouts.push_back(std::move(l));
    }
    const auto find_layout = [&](std::uint32_t id) -> const Layout* {
        for (const auto& l : layouts)
            if (l.id == id) return &l;
        return nullptr;
    };
    const auto find_pin = [](const std::vector<PinPos>& pins, std::string_view name) -> const PinPos* {
        for (const auto& p : pins)
            if (p.name == name) return &p;
        return nullptr;
    };
    const auto bezier = [&](ImVec2 a, ImVec2 b, ImU32 col, float thickness) {
        const float dx = std::max(40.0f, std::abs(b.x - a.x) * 0.5f);
        dl->AddBezierCubic(a, ImVec2(a.x + dx, a.y), ImVec2(b.x - dx, b.y), b, col, thickness);
    };

    // Links.
    for (const auto& link : graph_.links) {
        const Layout* a = find_layout(link.from_node);
        const Layout* b = find_layout(link.to_node);
        const PinPos* pa = a ? find_pin(a->outs, link.from_pin) : nullptr;
        const PinPos* pb = b ? find_pin(b->ins, link.to_pin) : nullptr;
        if (!pa || !pb) continue;
        bezier(pa->pos, pb->pos, pin_color(pa->type), pa->type == PinType::Exec ? 3.0f : 2.0f);
        // Exec links between nodes that just ran glow with the flow of execution.
        if (pa->type == PinType::Exec) {
            const float heat = std::min(graph_node_heat(link.from_node), graph_node_heat(link.to_node));
            if (heat > 0.01f) bezier(pa->pos, pb->pos, IM_COL32(255, 170, 40, static_cast<int>(heat * 230)), 4.0f);
        }
    }

    // Diagnostics per node (worst severity).
    std::map<std::uint32_t, GraphDiagnostic::Severity> node_problem;
    for (const auto& d : graph_diags_) {
        if (!d.node) continue;
        auto [it, inserted] = node_problem.emplace(d.node, d.severity);
        if (!inserted && d.severity == GraphDiagnostic::Severity::Error) it->second = d.severity;
    }

    // Nodes.
    struct Hit {
        std::uint32_t node{0};
        const PinPos* pin{nullptr};
        bool output{false};
    } hit;
    for (const auto& l : layouts) {
        const graph::GraphNode* n = graph_.node(l.id);
        const graph::NodeDesc* d = graph::find_node_desc(n->type);
        if (const float heat = graph_node_heat(l.id); heat > 0.01f)
            dl->AddRect(ImVec2(l.min.x - 3, l.min.y - 3), ImVec2(l.max.x + 3, l.max.y + 3),
                        IM_COL32(255, 170, 40, static_cast<int>(heat * 255)), 8.0f, 3.0f);
        dl->AddRectFilled(l.min, l.max, IM_COL32(48, 50, 56, 240), 6.0f);
        dl->AddRectFilled(l.min, ImVec2(l.max.x, l.min.y + kTitleHeight), d ? category_color(d->category) : IM_COL32(120, 30, 30, 255),
                          6.0f, ImDrawFlags_RoundTop);
        dl->AddText(ImVec2(l.min.x + 8, l.min.y + 4), IM_COL32(245, 245, 245, 255), node_title(*n).c_str());
        ImU32 border = IM_COL32(20, 20, 24, 255);
        float border_w = 1.0f;
        if (const auto it = node_problem.find(l.id); it != node_problem.end()) {
            border = it->second == GraphDiagnostic::Severity::Error ? IM_COL32(240, 70, 60, 255) : IM_COL32(240, 190, 60, 255);
            border_w = 2.0f;
        }
        if (l.id == graph_selected_) {
            border = IM_COL32(255, 255, 255, 255);
            border_w = 2.0f;
        }
        if (graph_error_ && graph_error_->node == l.id) {
            border = IM_COL32(255, 60, 50, 255);
            border_w = 3.0f;
        }
        dl->AddRect(l.min, l.max, border, 6.0f, border_w);
        for (std::size_t row = 0; row < l.ins.size(); ++row) {
            const PinPos& p = l.ins[row];
            const bool linked = pin_linked_in(graph_, l.id, p.name);
            if (linked) dl->AddCircleFilled(p.pos, kPinRadius, pin_color(p.type));
            else dl->AddCircle(p.pos, kPinRadius, pin_color(p.type), 0, 1.5f);
            std::string label = p.name;
            if (!linked && p.type != PinType::Exec) {
                const json::Value* lit = n->inputs.find(p.name);
                const std::string text = short_literal(lit ? *lit : p.def);
                if (!text.empty()) label += " " + text;
            }
            // Colour literals show a swatch; labels sharing a row with an output stay in their half.
            const json::Value* lit = linked ? nullptr : n->inputs.find(p.name);
            const json::Value& value = lit ? *lit : p.def;
            float swatch = 0.0f;
            if (!linked && (p.type == PinType::Vec4 || p.type == PinType::Vector) && value.is_array() &&
                (p.name.find("color") != std::string::npos || p.name == "emissive")) {
                label = p.name;
                swatch = 14.0f;
            }
            float max_w = kNodeWidth - 24.0f - swatch;
            if (row < l.outs.size()) max_w = kNodeWidth * 0.5f - 14.0f - swatch;
            while (label.size() > 3 && ImGui::CalcTextSize(label.c_str()).x > max_w) label = label.substr(0, label.size() - 3) + "..";
            dl->AddText(ImVec2(p.pos.x + 9, p.pos.y - 7), IM_COL32(210, 210, 215, 255), label.c_str());
            if (swatch > 0.0f) {
                const float x = p.pos.x + 13 + ImGui::CalcTextSize(label.c_str()).x;
                const auto c = [&](std::size_t i) {
                    return static_cast<int>(std::clamp(i < value.size() ? value[i].as_float(0.0f) : 1.0f, 0.0f, 1.0f) * 255.0f);
                };
                dl->AddRectFilled(ImVec2(x, p.pos.y - 5), ImVec2(x + 12, p.pos.y + 5), IM_COL32(c(0), c(1), c(2), 255), 2.0f);
                dl->AddRect(ImVec2(x, p.pos.y - 5), ImVec2(x + 12, p.pos.y + 5), IM_COL32(20, 20, 24, 255), 2.0f);
            }
            if (std::hypot(mouse.x - p.pos.x, mouse.y - p.pos.y) <= kPinRadius + 4) hit = {l.id, &p, false};
        }
        for (const auto& p : l.outs) {
            if (pin_linked_out(graph_, l.id, p.name)) dl->AddCircleFilled(p.pos, kPinRadius, pin_color(p.type));
            else dl->AddCircle(p.pos, kPinRadius, pin_color(p.type), 0, 1.5f);
            const ImVec2 ts = ImGui::CalcTextSize(p.name.c_str());
            dl->AddText(ImVec2(p.pos.x - 9 - ts.x, p.pos.y - 7), IM_COL32(210, 210, 215, 255), p.name.c_str());
            if (std::hypot(mouse.x - p.pos.x, mouse.y - p.pos.y) <= kPinRadius + 4) hit = {l.id, &p, true};
        }
        if (!hit.pin && mouse.x >= l.min.x && mouse.x <= l.max.x && mouse.y >= l.min.y && mouse.y <= l.max.y) hit.node = l.id;
    }

    // Link being dragged.
    if (graph_link_drag_) {
        const Layout* l = find_layout(graph_link_drag_->node);
        const PinPos* p = l ? find_pin(graph_link_drag_->output ? l->outs : l->ins, graph_link_drag_->pin) : nullptr;
        if (p) {
            if (graph_link_drag_->output) bezier(p->pos, mouse, pin_color(p->type), 2.0f);
            else bezier(mouse, p->pos, pin_color(p->type), 2.0f);
        }
    }
    dl->PopClipRect();

    // Tooltip: node description and its problems.
    if (hovered && !graph_link_drag_ && !graph_node_drag_ && hit.node && !hit.pin) {
        const graph::GraphNode* n = graph_.node(hit.node);
        const graph::NodeDesc* d = graph::find_node_desc(n->type);
        std::string tip = d ? d->tooltip : "unknown node type " + n->type;
        if (!n->comment.empty()) tip += "\n" + n->comment;
        if (graph_live_ || graph_node_runs(hit.node)) tip += std::format("\nRuns: {}", graph_node_runs(hit.node));
        if (graph_error_ && graph_error_->node == hit.node) tip += "\nRuntime error: " + graph_error_->message;
        for (const auto& diag : graph_diags_)
            if (diag.node == hit.node) tip += "\n- " + diag.message;
        if (!tip.empty()) ImGui::SetTooltip("%s", tip.c_str());
    } else if (hovered && hit.pin) {
        ImGui::SetTooltip("%s : %s", hit.pin->name.c_str(), std::string(graph::pin_type_name(hit.pin->type)).c_str());
    }

    // Interaction.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hit.pin) {
            graph_link_drag_ = GraphPinDrag{hit.node, hit.pin->name, hit.output};
        } else if (hit.node) {
            graph_selected_ = hit.node;
            graph_begin_edit();
            graph_node_drag_ = true;
            graph_node_moved_ = false;
        } else {
            graph_selected_ = 0;
        }
    }
    if (graph_node_drag_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (graph::GraphNode* n = graph_.node(graph_selected_); n && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
            n->x += io.MouseDelta.x;
            n->y += io.MouseDelta.y;
            graph_node_moved_ = true;
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (graph_node_drag_) {
            graph_node_drag_ = false;
            if (graph_node_moved_) {
                // Snap to the grid's quarter cells so layouts stay tidy.
                if (graph::GraphNode* n = graph_.node(graph_selected_)) {
                    n->x = std::round(n->x / 8.0f) * 8.0f;
                    n->y = std::round(n->y / 8.0f) * 8.0f;
                }
                graph_end_edit();
            } else if (!graph_undo_.empty()) {
                graph_undo_.pop_back(); // a click without a move is not an edit
            }
            graph_edit_key_.clear();
        }
        if (graph_link_drag_) {
            const GraphPinDrag from = *graph_link_drag_;
            graph_link_drag_.reset();
            if (hit.pin && hit.output != from.output) {
                if (from.output) (void)graph_connect(from.node, from.pin, hit.node, hit.pin->name);
                else (void)graph_connect(hit.node, hit.pin->name, from.node, from.pin);
            } else if (!hit.pin && !hit.node && hovered) {
                // Dropped on empty canvas: pick a node to link to.
                graph_palette_link_ = from;
                graph_palette_pos_ = {mouse.x - origin.x, mouse.y - origin.y};
                graph_palette_search_.clear();
                ImGui::OpenPopup("##graph_palette");
            }
        }
    }
    // Pan with the right or middle mouse button.
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 2.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 2.0f))) {
        graph_scroll_.x += io.MouseDelta.x;
        graph_scroll_.y += io.MouseDelta.y;
    }
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right, 0.0f);
        if (std::abs(drag.x) + std::abs(drag.y) < 4.0f) {
            if (hit.pin && !hit.output && pin_linked_in(graph_, hit.node, hit.pin->name)) {
                (void)graph_disconnect(hit.node, hit.pin->name);
            } else if (hit.node) {
                graph_selected_ = hit.node;
                ImGui::OpenPopup("##graph_node_menu");
            } else {
                graph_palette_link_.reset();
                graph_palette_pos_ = {mouse.x - origin.x, mouse.y - origin.y};
                graph_palette_search_.clear();
                ImGui::OpenPopup("##graph_palette");
            }
        }
    }

    if (ImGui::BeginPopup("##graph_node_menu")) {
        if (ImGui::MenuItem("Delete", "Del")) (void)graph_remove_node(graph_selected_);
        if (ImGui::MenuItem("Duplicate")) {
            if (const graph::GraphNode* n = graph_.node(graph_selected_)) {
                const graph::GraphNode copy = *n;
                if (const auto id = graph_add_node(copy.type, copy.x + 24, copy.y + 24)) {
                    graph_.node(id)->inputs = copy.inputs;
                    graph_.node(id)->config = copy.config;
                    graph_.node(id)->comment = copy.comment;
                    graph_end_edit();
                }
            }
        }
        if (ImGui::MenuItem("Disconnect All")) {
            const std::uint32_t id = graph_selected_;
            graph_begin_edit();
            std::erase_if(graph_.links, [&](const graph::GraphLink& l) { return l.from_node == id || l.to_node == id; });
            graph_end_edit();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##graph_palette")) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(220);
        ImGui::InputTextWithHint("##search", "search nodes", &graph_palette_search_);
        std::string needle = graph_palette_search_;
        for (char& ch : needle) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        // With a pending link, only nodes with a compatible pin are offered.
        PinType link_type = PinType::Any;
        if (graph_palette_link_) {
            if (const graph::GraphNode* src = graph_.node(graph_palette_link_->node)) {
                const auto pins = graph_palette_link_->output ? graph_.output_pins(*src) : graph_.input_pins(*src);
                for (const auto& p : pins)
                    if (p.name == graph_palette_link_->pin) link_type = p.type;
            }
        }
        const auto compatible_pin = [&](const graph::NodeDesc& d) -> std::string {
            if (!graph_palette_link_) return {};
            const auto& pins = graph_palette_link_->output ? d.inputs : d.outputs;
            for (const auto& p : pins) {
                const bool ok = graph_palette_link_->output ? graph::pin_types_compatible(link_type, p.type, graph_.domain)
                                                            : graph::pin_types_compatible(p.type, link_type, graph_.domain);
                if (ok) return p.name;
            }
            return {};
        };
        std::string picked;
        std::string current_category;
        bool category_open = false;
        int shown = 0;
        for (const auto& d : graph::node_library(graph_.domain)) {
            if (graph_palette_link_ && compatible_pin(d).empty()) continue;
            if (!needle.empty()) {
                std::string hay = d.title + " " + d.type + " " + d.category;
                for (char& ch : hay) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (hay.find(needle) == std::string::npos) continue;
                if (ImGui::Selectable(std::format("{}  ({})", d.title, d.category).c_str())) picked = d.type;
                if (!d.tooltip.empty()) ImGui::SetItemTooltip("%s", d.tooltip.c_str());
                ++shown;
                continue;
            }
            if (d.category != current_category) {
                if (category_open) ImGui::EndMenu();
                current_category = d.category;
                category_open = ImGui::BeginMenu(d.category.c_str());
            }
            if (category_open) {
                if (ImGui::MenuItem(d.title.c_str())) picked = d.type;
                if (!d.tooltip.empty()) ImGui::SetItemTooltip("%s", d.tooltip.c_str());
            }
            ++shown;
        }
        if (category_open) ImGui::EndMenu();
        if (!shown) ImGui::TextDisabled("no matching nodes");
        if (!picked.empty()) {
            const graph::NodeDesc* d = graph::find_node_desc(picked);
            if (const auto id = graph_add_node(picked, std::round(graph_palette_pos_.x / 8.0f) * 8.0f,
                                               std::round(graph_palette_pos_.y / 8.0f) * 8.0f)) {
                if (graph_palette_link_ && d) {
                    const std::string pin = compatible_pin(*d);
                    if (graph_palette_link_->output) (void)graph_connect(graph_palette_link_->node, graph_palette_link_->pin, id, pin);
                    else (void)graph_connect(id, pin, graph_palette_link_->node, graph_palette_link_->pin);
                }
            }
            graph_palette_link_.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace victus::editor
