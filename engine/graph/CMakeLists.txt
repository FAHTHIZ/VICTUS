# SPDX-License-Identifier: Apache-2.0
# Node-graph documents shared by visual scripts (.vgraph -> Luau source) and material graphs
# (.vmatgraph -> GLSL surface function). Pure data and code generation: no Luau, no GPU.
victus_add_module(graph
    SOURCES src/graph.cpp src/material_graph.cpp
    DEPS VICTUS::Core)
