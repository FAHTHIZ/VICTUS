# SPDX-License-Identifier: Apache-2.0
add_library(victus_doctest_main STATIC doctest_main.cpp)
target_include_directories(victus_doctest_main PUBLIC ${PROJECT_SOURCE_DIR}/third_party/doctest)
target_compile_features(victus_doctest_main PUBLIC cxx_std_20)

victus_add_test(victus_core_tests
    SOURCES
        core/cli_tests.cpp
        core/engine_clock_tests.cpp
        core/fs_tests.cpp
        core/hash_tests.cpp
        core/jobs_tests.cpp
        core/json_tests.cpp
        core/log_tests.cpp
        core/math_tests.cpp
        core/memory_tests.cpp
        core/profiler_tests.cpp
    DEPS VICTUS::Core
    LABELS core)

if(VICTUS_WITH_GRAPHICS)
victus_add_test(victus_render_tests
    SOURCES
        render/render_graph_tests.cpp
        render/image_mesh_tests.cpp
    DEPS VICTUS::Render
    LABELS render)

victus_add_test(victus_gpu_tests
    SOURCES
        render/gpu_fixture.cpp
        render/gpu_render_tests.cpp
    DEPS VICTUS::Render VICTUS::Platform
    LABELS gpu)
target_compile_definitions(victus_gpu_tests PRIVATE VICTUS_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/test_output")
endif()
