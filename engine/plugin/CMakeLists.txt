# SPDX-License-Identifier: Apache-2.0
# Native plugin host (ADR-0078). Plugin authors use only include/victus/plugin/victus_plugin.h.
victus_add_module(plugin
    SOURCES
        src/plugin.cpp
    DEPS VICTUS::Gameplay VICTUS::Scene
    PRIVATE_DEPS ${CMAKE_DL_LIBS})

# Header-only C SDK target for out-of-tree plugin libraries (no engine code is linked).
add_library(victus_plugin_sdk INTERFACE)
add_library(VICTUS::PluginSdk ALIAS victus_plugin_sdk)
target_include_directories(victus_plugin_sdk INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
