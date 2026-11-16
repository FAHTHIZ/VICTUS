# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.24)
project(VICTUS VERSION 0.2.0 DESCRIPTION "Independent cross-platform game engine" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

option(VICTUS_BUILD_TESTS "Build VICTUS automated tests" ON)
option(VICTUS_BUILD_BENCHMARKS "Build VICTUS benchmark executables" ON)
option(VICTUS_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)
set(VICTUS_SANITIZE "" CACHE STRING "Sanitizers for VICTUS targets: '', 'address,undefined', 'thread'")

include(VictusTargets)

if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

add_subdirectory(engine/core)

if(VICTUS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(VICTUS_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
