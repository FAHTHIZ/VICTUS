# SPDX-License-Identifier: Apache-2.0
# cgltf is third-party C code: built without VICTUS warning flags.
add_library(victus_cgltf STATIC src/cgltf_impl.c)
target_include_directories(victus_cgltf PUBLIC ${PROJECT_SOURCE_DIR}/third_party/cgltf)
set_target_properties(victus_cgltf PROPERTIES POSITION_INDEPENDENT_CODE ON)

victus_add_module(assets
    SOURCES
        src/cooked.cpp
        src/database.cpp
        src/importers.cpp
        src/gltf_export.cpp
        src/material.cpp
        src/texture_compression.cpp
    DEPS VICTUS::Render VICTUS::Animation VICTUS::Tilemap VICTUS::Terrain
    PRIVATE_DEPS victus_cgltf victus_bc7enc victus_astcenc)
