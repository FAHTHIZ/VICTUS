// SPDX-License-Identifier: Apache-2.0
// Material graphs (ADR-0062).
//
// A `.vmatgraph` asset is a node graph (the visual script document model with
// "domain": "material") that compiles to the GLSL surface function of a surface shader
// (ADR-0035):
//
//   void surface(SurfaceInput i, inout Surface s) { ... }
//
// so it runs through exactly the same path as a hand-written `.vshader`: materials reference it
// as their shader, GpuAssetCache compiles it with glslang/SPIRV-Cross, errors fall back to the
// built-in PBR shading, edits hot reload, and `victus check`/export compile it.
//
// - Input nodes read SurfaceInput (UVs, positions, time, the material's four parameters, vertex
//   colour) and the material's own surface values (textured base colour, roughness, ...).
// - Math nodes are generic: their "any" pins take the widest connected type (float < vec2 < vec3
//   < vec4); narrower inputs are splatted or padded, wider ones truncated.
// - One Surface Output node assigns the Surface fields whose inputs are linked; unlinked fields
//   keep the material's values.
//
// Every node used becomes one GLSL local in dependency order; nodes not connected to the output
// are reported as warnings. Generation is deterministic.
#pragma once
#include "victus/core/result.hpp"
#include "victus/graph/graph.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace victus::graph {

inline constexpr std::string_view kMaterialGraphExtension = ".vmatgraph";
inline constexpr std::string_view kMaterialOutputNode = "mat.output";

// Every material node type, in palette order.
[[nodiscard]] std::span<const NodeDesc> material_node_library();

struct CompiledMaterialGraph {
    std::string source; // the surface function (GLSL); empty when there are errors
    std::vector<GraphDiagnostic> diagnostics;
    [[nodiscard]] bool ok() const noexcept;
    // "node 12 (Multiply): message" lines for every error.
    [[nodiscard]] std::string error_text() const;
};

[[nodiscard]] CompiledMaterialGraph compile_material_graph(const ScriptGraph& graph, std::string_view source_name = {});
[[nodiscard]] std::vector<GraphDiagnostic> validate_material_graph(const ScriptGraph& graph);

// Parses a .vmatgraph document and compiles it; errors are prefixed with `name`.
[[nodiscard]] Result<std::string> material_graph_to_glsl(std::string_view text, std::string_view name);

// The graph a new material graph begins with: the material's base colour tinted by a colour.
[[nodiscard]] ScriptGraph starter_material_graph();

} // namespace victus::graph
