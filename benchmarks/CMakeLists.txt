# SPDX-License-Identifier: Apache-2.0
add_executable(victus_bench_core core_bench.cpp bench_harness.cpp $<TARGET_OBJECTS:victus_alloc_tracking>)
target_link_libraries(victus_bench_core PRIVATE VICTUS::Core)
victus_configure_target(victus_bench_core)
if(VICTUS_WITH_GRAPHICS)
    add_executable(victus_bench_render render_bench.cpp bench_harness.cpp)
    target_link_libraries(victus_bench_render PRIVATE VICTUS::Render VICTUS::Platform)
    victus_configure_target(victus_bench_render)
endif()
