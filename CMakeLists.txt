cmake_minimum_required(VERSION 3.24)
project(VICTUS VERSION 0.1.0 DESCRIPTION "Simulation-first cross-platform real-time engine" LANGUAGES CXX)

option(VICTUS_BUILD_TESTS "Build VICTUS automated tests" ON)
option(VICTUS_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)

add_library(victus_core src/core/engine_clock.cpp)
add_library(VICTUS::Core ALIAS victus_core)
target_include_directories(victus_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(victus_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(victus_core PRIVATE /W4 /permissive-)
    if(VICTUS_WARNINGS_AS_ERRORS)
        target_compile_options(victus_core PRIVATE /WX)
    endif()
else()
    target_compile_options(victus_core PRIVATE -Wall -Wextra -Wpedantic)
    if(VICTUS_WARNINGS_AS_ERRORS)
        target_compile_options(victus_core PRIVATE -Werror)
    endif()
endif()

add_executable(victus_runtime src/runtime/main.cpp)
target_link_libraries(victus_runtime PRIVATE VICTUS::Core)

if(VICTUS_BUILD_TESTS)
    enable_testing()
    add_executable(victus_core_tests tests/core/engine_clock_tests.cpp)
    target_link_libraries(victus_core_tests PRIVATE VICTUS::Core)
    add_test(NAME victus_core_tests COMMAND victus_core_tests)
endif()
