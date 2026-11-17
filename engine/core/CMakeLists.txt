# SPDX-License-Identifier: Apache-2.0
find_package(Git QUIET)
set(VICTUS_GIT_REVISION "unknown")
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} OUTPUT_VARIABLE rev OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE rev_result ERROR_QUIET)
    if(rev_result EQUAL 0)
        set(VICTUS_GIT_REVISION ${rev})
    endif()
endif()
if(CMAKE_CONFIGURATION_TYPES)
    set(VICTUS_BUILD_TYPE_NAME "multi-config")
else()
    set(VICTUS_BUILD_TYPE_NAME "${CMAKE_BUILD_TYPE}")
endif()
configure_file(src/build_info.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/build_info.cpp @ONLY)

find_package(Threads REQUIRED)
victus_add_module(core
    SOURCES
        src/cli.cpp
        src/diagnostics.cpp
        src/engine_clock.cpp
        src/fs.cpp
        src/hash.cpp
        src/jobs.cpp
        src/json.cpp
        src/log.cpp
        src/math.cpp
        src/pack.cpp
        src/memory.cpp
        src/process.cpp
        src/profiler.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/build_info.cpp
    DEPS Threads::Threads)
if(ANDROID)
    target_link_libraries(victus_core PUBLIC log)
endif()

# Opt-in global operator new/delete tracking, linked only into executables that want it.
add_library(victus_alloc_tracking OBJECT src/alloc_tracking.cpp)
target_link_libraries(victus_alloc_tracking PRIVATE victus_core)
victus_configure_target(victus_alloc_tracking)
