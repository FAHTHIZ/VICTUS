# SPDX-License-Identifier: Apache-2.0
# Editor core is a library so tests and tools can drive it without a window.
add_library(victus_editor_core STATIC
    src/undo.cpp
    src/editor.cpp
    src/panels.cpp)
add_library(VICTUS::EditorCore ALIAS victus_editor_core)
target_include_directories(victus_editor_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(victus_editor_core PUBLIC VICTUS::Scene VICTUS::Project VICTUS::Runtime VICTUS::Ui VICTUS::Gameplay VICTUS::Player VICTUS::Script VICTUS::Physics VICTUS::Audio)
victus_configure_target(victus_editor_core)

add_executable(victus_editor app/main.cpp)
target_link_libraries(victus_editor PRIVATE victus_editor_core VICTUS::Platform)
victus_configure_target(victus_editor)
