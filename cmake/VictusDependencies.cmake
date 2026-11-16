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

# Jolt Physics 5.7.0 (MIT). https://github.com/jrouwe/JoltPhysics/releases/tag/v5.7.0
# Built as a static library without samples, tests, debug renderer, profiler or GPU compute.
# AVX/AVX2/FMA/F16C/LZCNT/TZCNT are disabled so binaries run on every x86-64 CPU with SSE4.2;
# ARM builds use NEON automatically.
# ThreadSanitizer only understands synchronization it can see: third-party libraries that run on
# VICTUS worker threads (Jolt's job graph, Box2D tasks, miniaudio's mixer) must be instrumented
# too, or their atomics look like data races. Only TSan is propagated; warnings stay disabled.
function(victus_third_party_tsan target)
    if(VICTUS_SANITIZE MATCHES "thread" AND NOT MSVC)
        target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${target} INTERFACE -fsanitize=thread)
    endif()
endfunction()

function(victus_fetch_jolt)
    if(TARGET Jolt)
        return()
    endif()
    foreach(opt TARGET_UNIT_TESTS TARGET_HELLO_WORLD TARGET_PERFORMANCE_TEST TARGET_SAMPLES TARGET_VIEWER
                ENABLE_ALL_WARNINGS INTERPROCEDURAL_OPTIMIZATION FLOATING_POINT_EXCEPTIONS_ENABLED ENABLE_INSTALL
                OVERRIDE_CXX_FLAGS JPH_USE_DX12 JPH_USE_VK JPH_USE_MTL JPH_USE_CPU_COMPUTE USE_AVX USE_AVX2 USE_AVX512
                USE_F16C USE_FMADD USE_LZCNT USE_TZCNT DEBUG_RENDERER_IN_DEBUG_AND_RELEASE PROFILER_IN_DEBUG_AND_RELEASE
                ENABLE_OBJECT_STREAM USE_STATIC_MSVC_RUNTIME_LIBRARY GENERATE_DEBUG_SYMBOLS JPH_BUILD_SHARED_LIBS)
        set(${opt} OFF CACHE BOOL "" FORCE)
    endforeach()
    # VICTUS subclasses Jolt interfaces (job system, listeners) from RTTI-enabled code.
    set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
    set(OBJECT_LAYER_BITS 32 CACHE STRING "" FORCE)
    FetchContent_Declare(JoltPhysics
        URL https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.7.0.tar.gz
        URL_HASH SHA256=9a0a405f2375dc4bd9f72486c56e1cbc7794c6a62e42fc48fadea45dfc93dacc
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        SOURCE_SUBDIR Build
        EXCLUDE_FROM_ALL SYSTEM)
    FetchContent_MakeAvailable(JoltPhysics)
    set_target_properties(Jolt PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(Jolt PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)
    victus_third_party_tsan(Jolt)
endfunction()

# Box2D 3.2.0 (MIT). https://github.com/erincatto/box2d/releases/tag/v3.2.0
function(victus_fetch_box2d)
    if(TARGET box2d)
        return()
    endif()
    foreach(opt BOX2D_SAMPLES BOX2D_BENCHMARKS BOX2D_DOCS BOX2D_UNIT_TESTS BOX2D_VALIDATE BOX2D_PROFILE
                BOX2D_SANITIZE BOX2D_COMPILE_WARNING_AS_ERROR BOX2D_DOUBLE_PRECISION)
        set(${opt} OFF CACHE BOOL "" FORCE)
    endforeach()
    FetchContent_Declare(box2d
        URL https://github.com/erincatto/box2d/archive/refs/tags/v3.2.0.tar.gz
        URL_HASH SHA256=ffec747b27a2267016f8d0240d658c82d187fb2271fa0f69fcbe53fbc067180d
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        EXCLUDE_FROM_ALL SYSTEM)
    FetchContent_MakeAvailable(box2d)
    set_target_properties(box2d PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(box2d PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)
    victus_third_party_tsan(box2d)
endfunction()

# miniaudio 0.11.26 (public domain / MIT-0). Single-file library: VICTUS compiles the
# implementation in its own translation unit (engine/audio/third_party/miniaudio_impl.c) with
# third-party warning settings; stb_vorbis from miniaudio's extras provides Ogg Vorbis.
function(victus_fetch_miniaudio)
    if(TARGET victus_miniaudio)
        return()
    endif()
    FetchContent_Declare(miniaudio
        URL https://github.com/mackron/miniaudio/archive/refs/tags/0.11.26.tar.gz
        URL_HASH SHA256=838ba55a21c9142adf7b76bdc4351e5cd8b7ae128f46b25ec25f94ae9969c600
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        SOURCE_SUBDIR victus_no_cmake) # use the sources only, not miniaudio's CMake project
    FetchContent_MakeAvailable(miniaudio)
    set(VICTUS_MINIAUDIO_DIR ${miniaudio_SOURCE_DIR} CACHE INTERNAL "miniaudio source directory")
    add_library(victus_miniaudio STATIC ${PROJECT_SOURCE_DIR}/engine/audio/third_party/miniaudio_impl.c)
    target_include_directories(victus_miniaudio SYSTEM PUBLIC ${miniaudio_SOURCE_DIR})
    target_compile_definitions(victus_miniaudio PUBLIC MA_NO_ENCODING)
    set_target_properties(victus_miniaudio PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
    target_compile_options(victus_miniaudio PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)
    victus_third_party_tsan(victus_miniaudio)
    if(APPLE)
        target_compile_definitions(victus_miniaudio PUBLIC MA_NO_RUNTIME_LINKING)
        target_link_libraries(victus_miniaudio PUBLIC "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
    elseif(UNIX)
        find_package(Threads REQUIRED)
        target_link_libraries(victus_miniaudio PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
    endif()
endfunction()
