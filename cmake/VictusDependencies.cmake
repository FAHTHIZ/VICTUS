# SPDX-License-Identifier: Apache-2.0
# Pinned third-party dependencies. Every entry is recorded in docs/THIRD_PARTY_AND_LICENSES.md.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

option(VICTUS_USE_SYSTEM_SDL "Use an installed SDL3 instead of the pinned source release" OFF)

function(victus_fetch_sdl3)
    if(TARGET SDL3::SDL3)
        return()
    endif()
    if(VICTUS_USE_SYSTEM_SDL)
        find_package(SDL3 3.2 REQUIRED CONFIG)
        return()
    endif()
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    # SDL3 3.4.20 (zlib). https://github.com/libsdl-org/SDL/releases/tag/release-3.4.20
    FetchContent_Declare(SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
        URL_HASH SHA256=f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        EXCLUDE_FROM_ALL SYSTEM)
    FetchContent_MakeAvailable(SDL3)
    if(NOT TARGET SDL3::SDL3 AND TARGET SDL3-static)
        add_library(SDL3::SDL3 ALIAS SDL3-static)
    endif()
endfunction()

# Luau 0.747 (MIT). https://github.com/luau-lang/luau/releases/tag/0.747
# Only the VM and compiler are linked; the CLI, analysis, code generation and tests are not built.
function(victus_fetch_luau)
    if(TARGET Luau.VM)
        return()
    endif()
    set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
    set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
    set(LUAU_WERROR OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(luau
        URL https://github.com/luau-lang/luau/archive/refs/tags/0.747.tar.gz
        URL_HASH SHA256=2c72471f91c42af7f2c07cc4cc602b27c215346d491d7ab3d80da63050ad43f2
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        EXCLUDE_FROM_ALL SYSTEM)
    FetchContent_MakeAvailable(luau)
    # Luau's sources are third-party: never apply VICTUS warnings-as-errors or sanitizer-only flags.
    foreach(t Luau.VM Luau.Compiler Luau.Ast Luau.Common Luau.Bytecode)
        if(TARGET ${t})
            set_target_properties(${t} PROPERTIES POSITION_INDEPENDENT_CODE ON)
            # Upstream builds warning-free with its own flags; GCC -O3 reports false-positive
            # -Warray-bounds in lgc.cpp. Silence third-party diagnostics rather than patch them.
            get_target_property(_type ${t} TYPE)
            if(NOT _type STREQUAL "INTERFACE_LIBRARY")
                target_compile_options(${t} PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)
            endif()
        endif()
    endforeach()
endfunction()
