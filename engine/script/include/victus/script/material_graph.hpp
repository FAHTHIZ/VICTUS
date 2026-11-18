// SPDX-License-Identifier: Apache-2.0
// Deprecated forwarding header. The graph model and its compilers moved to the Luau-free
// `engine/graph` module (VICTUS::Graph, namespace victus::graph) so the runtime can compile
// material graphs without linking the scripting VM. Include "victus/graph/material_graph.hpp" and use
// victus::graph:: instead; this header and the victus::script names it re-exports will be removed
// after one release.
#pragma once
#include "victus/graph/material_graph.hpp"

namespace victus::script {
using namespace ::victus::graph; // migration shim
} // namespace victus::script
