# SPDX-License-Identifier: Apache-2.0
# Editor core is a library so tests and tools can drive it without a window.
add_library(victus_editor_core STATIC
    src/undo.cpp
    src/editor.cpp
    src/panels.cpp
    src/network_panel.cpp
    src/profiler_panel.cpp
    src/command_palette.cpp
    src/autosave.cpp
    src/selection.cpp
    src/material_panel.cpp
    src/graph_panel.cpp
    src/sprite_sheets.cpp
    src/tilemaps.cpp
    src/terrain_tools.cpp
    src/ai_debugger.cpp
    src/animation_panel.cpp
    src/sequencer_panel.cpp
    src/editor_clients.cpp
    src/gizmo.cpp
    src/gizmo_tool.cpp
    src/thumbnails.cpp
    src/widgets.cpp
    src/launcher.cpp
    src/workspace.cpp
    src/problems.cpp
    src/script_debugger.cpp
    src/export_dialog.cpp
    src/lighting_bake.cpp
    src/diagnostics_dialog.cpp
    src/console.cpp
    src/assets_panel.cpp
    src/import_files.cpp
    src/add_component.cpp
    src/roles.cpp
    src/lessons.cpp
    src/guide.cpp
    src/ragdoll_tools.cpp
    src/foot_tools.cpp
    src/safe_mode.cpp)
add_library(VICTUS::EditorCore ALIAS victus_editor_core)
target_include_directories(victus_editor_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(victus_editor_core PUBLIC VICTUS::Scene VICTUS::Project VICTUS::Runtime VICTUS::Ui VICTUS::Gameplay VICTUS::Player VICTUS::Script VICTUS::Physics VICTUS::Audio VICTUS::Net VICTUS::Tilemap)
victus_configure_target(victus_editor_core)

add_executable(victus_editor app/main.cpp)
target_link_libraries(victus_editor PRIVATE victus_editor_core VICTUS::Platform)
# Development builds find the bundled samples in the source tree (VICTUS_SAMPLES_DIR overrides).
target_compile_definitions(victus_editor PRIVATE VICTUS_SOURCE_SAMPLES_DIR="${PROJECT_SOURCE_DIR}/samples")
victus_configure_target(victus_editor)
