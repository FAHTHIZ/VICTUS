# SPDX-License-Identifier: Apache-2.0
victus_add_module(render
    SOURCES
        src/builtin_shaders_lookup.cpp
        src/gpu_context.cpp
        src/image.cpp
        src/mesh.cpp
        src/render_graph.cpp
        src/scene_renderer.cpp
        src/sprite_batch.cpp
        generated/builtin_shaders.cpp
    DEPS VICTUS::Core VICTUS::Rhi)
# stb implementations are third-party code: compiled without VICTUS warning flags.
add_library(victus_stb STATIC src/stb_impl.cpp)
target_include_directories(victus_stb PUBLIC ${PROJECT_SOURCE_DIR}/third_party/stb)
target_link_libraries(victus_render PUBLIC victus_stb)
