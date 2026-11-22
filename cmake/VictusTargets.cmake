# SPDX-License-Identifier: Apache-2.0
# Shared target configuration for VICTUS-owned code (never applied to third-party code).
function(victus_configure_target target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
        # Portable C APIs (fopen/getenv) are used deliberately; silence MSVC's *_s deprecation.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
        if(VICTUS_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target} PRIVATE -Wshadow=local)
        endif()
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE -Wno-shadow-field-in-constructor -Wno-gnu-zero-variadic-macro-arguments)
        endif()
        if(VICTUS_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
    if(VICTUS_SANITIZE AND NOT MSVC)
        target_compile_options(${target} PRIVATE -fsanitize=${VICTUS_SANITIZE} -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=${VICTUS_SANITIZE})
    endif()
endfunction()

# Engine module layers (docs/ARCHITECTURE.md, "Module map"). A module may link only modules on a
# strictly lower layer, so dependencies always point down and cycles cannot form. Adding a module
# means adding it here; victus_add_module fails the configure for an unknown module or an upward
# or sideways dependency.
set(VICTUS_MODULE_LAYERS
    core=0
    platform=1 ecs=1 shadercompiler=1 graph=1 text=1
    rhi=2 scene=2
    render=3 gameplay=3 world=3
    ui=4 physics=4 plugin=4 animation=4 particles=4 audio=4 gameui=4
    navigation=5 traffic=5 tilemap=5 terrain=5 sequencer=5 net=5
    ai=6 assets=6 procgen=6
    script=7 library=7
    runtime=8 project=8
    player=9)

function(victus_module_layer name out)
    foreach(entry ${VICTUS_MODULE_LAYERS})
        string(REPLACE "=" ";" kv "${entry}")
        list(GET kv 0 key)
        if(key STREQUAL "${name}")
            list(GET kv 1 value)
            set(${out} ${value} PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out} "" PARENT_SCOPE)
endfunction()

function(victus_check_module_deps name)
    victus_module_layer(${name} own)
    if(own STREQUAL "")
        message(FATAL_ERROR "engine module '${name}' has no layer in VICTUS_MODULE_LAYERS (cmake/VictusTargets.cmake)")
    endif()
    foreach(dep ${ARGN})
        if(dep MATCHES "^VICTUS::(.+)$")
            string(TOLOWER "${CMAKE_MATCH_1}" dep_name)
            victus_module_layer(${dep_name} dep_layer)
            if(dep_layer STREQUAL "")
                message(FATAL_ERROR "engine module '${name}' links unknown module ${dep}")
            elseif(NOT dep_layer LESS own)
                message(FATAL_ERROR "engine module '${name}' (layer ${own}) must not link ${dep} (layer ${dep_layer}): "
                    "dependencies must point to a lower layer (docs/ARCHITECTURE.md, Module map)")
            endif()
        endif()
    endforeach()
endfunction()

# victus_add_module(<name> SOURCES ... [DEPS ...] [PRIVATE_DEPS ...])
# Creates victus_<name> with alias VICTUS::<Name>, public include dir engine/<name>/include.
function(victus_add_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;PRIVATE_DEPS" ${ARGN})
    victus_check_module_deps(${name} ${ARG_DEPS} ${ARG_PRIVATE_DEPS})
    add_library(victus_${name} STATIC ${ARG_SOURCES})
    string(SUBSTRING ${name} 0 1 first)
    string(TOUPPER ${first} first)
    string(SUBSTRING ${name} 1 -1 rest)
    add_library(VICTUS::${first}${rest} ALIAS victus_${name})
    target_include_directories(victus_${name} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    target_compile_features(victus_${name} PUBLIC cxx_std_20)
    if(ARG_DEPS)
        target_link_libraries(victus_${name} PUBLIC ${ARG_DEPS})
    endif()
    if(ARG_PRIVATE_DEPS)
        target_link_libraries(victus_${name} PRIVATE ${ARG_PRIVATE_DEPS})
    endif()
    victus_configure_target(victus_${name})
endfunction()

# victus_add_test(<name> SOURCES ... DEPS ...)
function(victus_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;LABELS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE victus_doctest_main ${ARG_DEPS})
    victus_configure_target(${name})
    add_test(NAME ${name} COMMAND ${name})
    if(ARG_LABELS)
        set_tests_properties(${name} PROPERTIES LABELS "${ARG_LABELS}")
    endif()
endfunction()
