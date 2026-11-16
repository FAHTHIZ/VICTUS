# SPDX-License-Identifier: Apache-2.0
# Dear ImGui is third-party code: built without VICTUS warning flags.
set(IMGUI_DIR ${PROJECT_SOURCE_DIR}/third_party/imgui)
add_library(victus_imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/misc/imgui_stdlib.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp)
target_include_directories(victus_imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/misc ${IMGUI_DIR}/backends)
target_compile_definitions(victus_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(victus_imgui PRIVATE SDL3::SDL3)
set_target_properties(victus_imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

victus_add_module(ui
    SOURCES
        src/ui_context.cpp
        src/theme.cpp
    DEPS VICTUS::Render VICTUS::Platform victus_imgui
    PRIVATE_DEPS SDL3::SDL3)
