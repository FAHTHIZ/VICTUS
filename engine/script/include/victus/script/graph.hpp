// SPDX-License-Identifier: Apache-2.0
// Visual scripting graphs (ADR-0058).
//
// A `.vgraph` asset is a JSON document of typed nodes and links that compiles to an ordinary Luau
// script module, so graphs run in the same sandboxed ScriptHost as hand-written scripts (hot
// reload, budgets, per-entity properties, every scripting API).
//
// - Event nodes ("event.update", "event.collision_enter", ...) become module methods. Their exec
//   output starts a chain of statement nodes; flow nodes (branch, sequence, for range, do once)
//   become structured Luau control flow.
// - Pure nodes (math, logic, input queries, getters) are inlined as expressions where they are
//   used. Statement nodes with data outputs store them in locals of the event function.
// - Graph variables become fields of `self`: exposed ones are module fields (editable per entity
//   in the inspector, like a script's own fields), private ones are hidden from the editor.
//
// Serialization is deterministic: nodes sorted by id, links sorted, object keys in a fixed order.
#pragma once
#include "victus/core/json.hpp"
#include "victus/core/result.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace victus::script {

enum class PinType : std::uint8_t { Exec, Any, Number, Bool, String, Vector, Entity };
[[nodiscard]] std::string_view pin_type_name(PinType t) noexcept;
[[nodiscard]] bool parse_pin_type(std::string_view name, PinType& out) noexcept;
// Whether a link from an output of type `from` may feed an input of type `to`.
[[nodiscard]] bool pin_types_compatible(PinType from, PinType to) noexcept;

struct PinDesc {
    std::string name;
    PinType type{PinType::Any};
    json::Value default_value; // literal used when a data input is not linked
};

enum class NodeKind : std::uint8_t {
    Event,     // entry point: a module method
    Statement, // exec in/out, runs `code`
    Pure,      // no exec pins, inlined as an expression per output
    Branch,
    Sequence,
    ForRange,
    DoOnce,
    GetVariable,
    SetVariable,
};

struct NodeDesc {
    std::string type;     // "math.add"
    std::string category; // "Math"
    std::string title;    // "Add"
    std::string tooltip;
    NodeKind kind{NodeKind::Statement};
    std::vector<PinDesc> inputs;  // exec pins first for statements
    std::vector<PinDesc> outputs;
    // Event: Luau method name ("update"); parameters are the data outputs in order.
    std::string method;
    // Statement: Luau statement template; Pure: one expression template per output (same order).
    // `{pin}` is an input expression, `{=pin}` the local holding an output, `{self}` the instance.
    std::string code;
    std::vector<std::string> output_code;
};

// Every built-in node type, in palette order.
[[nodiscard]] std::span<const NodeDesc> node_library();
[[nodiscard]] const NodeDesc* find_node_desc(std::string_view type);

struct GraphVariable {
    std::string name; // Luau identifier, no leading underscore
    PinType type{PinType::Number};
    json::Value default_value;
    bool exposed{true}; // editable per entity in the inspector
};

struct GraphNode {
    std::uint32_t id{0};
    std::string type;
    float x{0.0f}, y{0.0f};                // editor position
    json::Value inputs = json::Value::object(); // pin -> literal for unlinked data inputs
    json::Value config = json::Value::object(); // e.g. {"variable": "speed"}
    std::string comment;
};

struct GraphLink {
    std::uint32_t from_node{0};
    std::string from_pin;
    std::uint32_t to_node{0};
    std::string to_pin;
    friend bool operator==(const GraphLink&, const GraphLink&) = default;
};

struct ScriptGraph {
    static constexpr std::uint32_t kVersion = 1;
    std::vector<GraphVariable> variables;
    std::vector<GraphNode> nodes;
    std::vector<GraphLink> links;
    std::uint32_t next_id{1};

    [[nodiscard]] GraphNode* node(std::uint32_t id) noexcept;
    [[nodiscard]] const GraphNode* node(std::uint32_t id) const noexcept;
    [[nodiscard]] const GraphVariable* variable(std::string_view name) const noexcept;
    // Adds a node of `type` (not checked against the library) and returns its id.
    std::uint32_t add_node(std::string type, float x = 0.0f, float y = 0.0f);
    // Removes a node and every link touching it.
    void remove_node(std::uint32_t id);
    // Adds a link; a data input or exec output keeps only its newest link. False when the
    // endpoints do not exist or the pin types do not match.
    bool connect(std::uint32_t from_node, std::string_view from_pin, std::uint32_t to_node, std::string_view to_pin);
    void disconnect_input(std::uint32_t node, std::string_view pin);
    // The pins of a node, resolved for variable nodes (their value pin takes the variable's type).
    [[nodiscard]] std::vector<PinDesc> input_pins(const GraphNode& n) const;
    [[nodiscard]] std::vector<PinDesc> output_pins(const GraphNode& n) const;
};

[[nodiscard]] json::Value graph_to_json(const ScriptGraph& graph);
[[nodiscard]] Result<ScriptGraph> graph_from_json(const json::Value& doc);
[[nodiscard]] Result<ScriptGraph> parse_graph(std::string_view text);
[[nodiscard]] std::string serialize_graph(const ScriptGraph& graph);

struct GraphDiagnostic {
    enum class Severity : std::uint8_t { Error, Warning };
    Severity severity{Severity::Error};
    std::uint32_t node{0}; // 0: the graph itself
    std::string pin;
    std::string message;
};

struct GraphCompileOptions {
    std::string source_name; // shown in the generated header, e.g. "scripts/door.vgraph"
    // Emits `__vg_trace(node)` at every event entry and before every statement (editor debugging,
    // ADR-0058/0060). The recorder comes from the host's `__vg_trace_for(source_name)` when present.
    bool trace{false};
};

struct CompiledGraph {
    std::string source;                    // Luau module; empty when there are errors
    std::vector<std::uint32_t> line_nodes; // node emitted on each line (index = line - 1, 0 = none)
    std::vector<GraphDiagnostic> diagnostics;
    [[nodiscard]] bool ok() const noexcept;
    // The node a runtime error at `line` belongs to (0 when unknown).
    [[nodiscard]] std::uint32_t node_at_line(std::uint32_t line) const noexcept;
    // "node 12 (Branch): message" lines for every error.
    [[nodiscard]] std::string error_text() const;
};

// Validates and compiles. Errors leave `source` empty; warnings (unreachable nodes) do not.
[[nodiscard]] CompiledGraph compile_graph(const ScriptGraph& graph, const GraphCompileOptions& options = {});
// Validation only (the diagnostics compile_graph would report).
[[nodiscard]] std::vector<GraphDiagnostic> validate_graph(const ScriptGraph& graph);

// Rewrites "<chunk>:LINE:" locations in a Luau error message to name the graph node.
[[nodiscard]] std::string annotate_graph_error(std::string_view message, const CompiledGraph& compiled,
                                               const ScriptGraph& graph);

} // namespace victus::script
