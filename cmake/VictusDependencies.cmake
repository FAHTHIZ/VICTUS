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
    # Android: a shared libSDL3.so, which SDLActivity loads before the game's libmain.so and
    # whose JNI entry points must stay exported (a static archive would drop them). Elsewhere static.
    if(ANDROID)
        set(SDL_SHARED ON CACHE BOOL "" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    else()
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
    endif()
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

# Recast & Detour 1.6.0 (zlib). https://github.com/recastnavigation/recastnavigation/releases/tag/v1.6.0
# VICTUS compiles Recast (navmesh generation), Detour (navmesh + queries) and DetourCrowd (agent
# steering/avoidance) sources into one static library instead of using the upstream CMake project
# (which adds install/export rules and demo targets).
function(victus_fetch_recast)
    if(TARGET victus_recast)
        return()
    endif()
    FetchContent_Declare(recastnavigation
        URL https://github.com/recastnavigation/recastnavigation/archive/refs/tags/v1.6.0.tar.gz
        URL_HASH SHA256=d48ca0121962fa0639502c0f56c4e3ae72f98e55d88727225444f500775c0074
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        SOURCE_SUBDIR victus_no_cmake)
    FetchContent_MakeAvailable(recastnavigation)
    set(dir ${recastnavigation_SOURCE_DIR})
    file(GLOB sources ${dir}/Recast/Source/*.cpp ${dir}/Detour/Source/*.cpp ${dir}/DetourCrowd/Source/*.cpp)
    add_library(victus_recast STATIC ${sources})
    target_include_directories(victus_recast SYSTEM PUBLIC ${dir}/Recast/Include ${dir}/Detour/Include ${dir}/DetourCrowd/Include)
    set_target_properties(victus_recast PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(victus_recast PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)
    victus_third_party_tsan(victus_recast)
endfunction()

# ENet 1.3.18 (MIT). https://github.com/lsalzman/enet/releases/tag/v1.3.18
# Reliable/unreliable UDP transport used by engine/net. Compiled from source into a static library
# with the POSIX feature macros its configure step would detect (all present on Linux/macOS).
function(victus_fetch_enet)
    if(TARGET victus_enet)
        return()
    endif()
    FetchContent_Declare(enet
        URL https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.tar.gz
        URL_HASH SHA256=28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        SOURCE_SUBDIR victus_no_cmake)
    FetchContent_MakeAvailable(enet)
    set(dir ${enet_SOURCE_DIR})
    add_library(victus_enet STATIC
        ${dir}/callbacks.c ${dir}/compress.c ${dir}/host.c ${dir}/list.c ${dir}/packet.c
        ${dir}/peer.c ${dir}/protocol.c ${dir}/unix.c ${dir}/win32.c)
    target_include_directories(victus_enet SYSTEM PUBLIC ${dir}/include)
    set_target_properties(victus_enet PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
    target_compile_options(victus_enet PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)
    if(WIN32)
        target_compile_definitions(victus_enet PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(victus_enet PUBLIC ws2_32 winmm)
    elseif(UNIX)
        target_compile_definitions(victus_enet PRIVATE HAS_FCNTL=1 HAS_POLL=1 HAS_GETADDRINFO=1 HAS_GETNAMEINFO=1
            HAS_INET_PTON=1 HAS_INET_NTOP=1 HAS_MSGHDR_FLAGS=1 HAS_SOCKLEN_T=1)
    endif()
    victus_third_party_tsan(victus_enet)
endfunction()

# Monocypher 4.0.2 (CC0-1.0 or BSD-2-Clause). https://monocypher.org/
# X25519, BLAKE2b and XChaCha20-Poly1305 for encrypted, authenticated multiplayer sessions
# (engine/net/src/secure.cpp, ADR-0028). Two C files compiled into a static library.
function(victus_fetch_monocypher)
    if(TARGET victus_monocypher)
        return()
    endif()
    FetchContent_Declare(monocypher
        URL https://github.com/LoupVaillant/Monocypher/archive/refs/tags/4.0.2.tar.gz
        URL_HASH SHA256=bc1ca30b1b2654e4e7daf2492c0d204200e55137f23fda6b7142fd7d523bd6b4
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        SOURCE_SUBDIR victus_no_cmake)
    FetchContent_MakeAvailable(monocypher)
    add_library(victus_monocypher STATIC ${monocypher_SOURCE_DIR}/src/monocypher.c)
    target_include_directories(victus_monocypher SYSTEM PUBLIC ${monocypher_SOURCE_DIR}/src)
    set_target_properties(victus_monocypher PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
    target_compile_options(victus_monocypher PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)
    victus_third_party_tsan(victus_monocypher)
endfunction()

# glslang 16.6.0 (BSD-3-Clause, BSD-2-Clause, MIT, Apache-2.0; see its LICENSE.txt).
# https://github.com/KhronosGroup/glslang/releases/tag/16.6.0
# GLSL -> SPIR-V for project surface shaders compiled at run time (engine/shadercompiler,
# ADR-0035). Only the GLSL front end and SPIR-V back end are built (no HLSL, optimizer or tools).
function(victus_fetch_glslang)
    if(TARGET glslang)
        return()
    endif()
    foreach(opt ENABLE_GLSLANG_BINARIES ENABLE_HLSL ENABLE_OPT GLSLANG_TESTS GLSLANG_ENABLE_INSTALL BUILD_EXTERNAL
                ENABLE_PCH ENABLE_GLSLANG_JS BUILD_WERROR)
        set(${opt} OFF CACHE BOOL "" FORCE)
    endforeach()
    # RTTI on: the engine is built with RTTI and UBSan's vptr check needs glslang's type info.
    set(ENABLE_RTTI ON CACHE BOOL "" FORCE)
    FetchContent_Declare(glslang
        URL https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz
        URL_HASH SHA256=9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        EXCLUDE_FROM_ALL SYSTEM)
    FetchContent_MakeAvailable(glslang)
    foreach(t glslang SPIRV glslang-default-resource-limits GenericCodeGen MachineIndependent OSDependent)
        if(TARGET ${t})
            get_target_property(_type ${t} TYPE)
            if(NOT _type STREQUAL "INTERFACE_LIBRARY" AND NOT _type STREQUAL "ALIAS")
                set_target_properties(${t} PROPERTIES POSITION_INDEPENDENT_CODE ON)
                target_compile_options(${t} PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)
                victus_third_party_tsan(${t})
            endif()
        endif()
    endforeach()
endfunction()

# SPIRV-Cross vulkan-sdk-1.4.363.0 (Apache-2.0).
# https://github.com/KhronosGroup/SPIRV-Cross/releases/tag/vulkan-sdk-1.4.363.0
# SPIR-V -> MSL for surface shaders on Metal, and resource reflection. Core, GLSL and MSL only.
function(victus_fetch_spirv_cross)
    if(TARGET spirv-cross-msl)
        return()
    endif()
    foreach(opt SPIRV_CROSS_CLI SPIRV_CROSS_ENABLE_TESTS SPIRV_CROSS_ENABLE_HLSL SPIRV_CROSS_ENABLE_CPP
                SPIRV_CROSS_ENABLE_REFLECT SPIRV_CROSS_ENABLE_C_API SPIRV_CROSS_ENABLE_UTIL SPIRV_CROSS_SHARED)
        set(${opt} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_FORCE_PIC ON CACHE BOOL "" FORCE)
    FetchContent_Declare(spirv_cross
        URL https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
        URL_HASH SHA256=f708982e88b763ef5b0394ed468fc2c3628f68cdab89f2e7a946cd628c04e721
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        EXCLUDE_FROM_ALL SYSTEM)
    FetchContent_MakeAvailable(spirv_cross)
    foreach(t spirv-cross-core spirv-cross-glsl spirv-cross-msl)
        target_compile_options(${t} PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)
        victus_third_party_tsan(${t})
    endforeach()
endfunction()

# bc7enc_rdo, commit b9438627eef73a1157e84201b6fa6eb2ffd6d9f0 (MIT, alternatively public domain
# (Unlicense)). https://github.com/richgel999/bc7enc_rdo
# BC1/BC3 (rgbcx) and BC7 (bc7enc) block encoders plus the BC7 decoder for the texture cooker
# (engine/assets texture_compression.cpp, ADR-0047). No releases exist, so a commit is pinned.
function(victus_fetch_bc7enc)
    if(TARGET victus_bc7enc)
        return()
    endif()
    FetchContent_Declare(bc7enc_rdo
        URL https://github.com/richgel999/bc7enc_rdo/archive/b9438627eef73a1157e84201b6fa6eb2ffd6d9f0.tar.gz
        URL_HASH SHA256=bbb33d1dbb6178a3a2c044956c3cc0b8ea9fc2f2903824379dd86f87d53b64c6
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        SOURCE_SUBDIR victus_no_cmake)
    FetchContent_MakeAvailable(bc7enc_rdo)
    add_library(victus_bc7enc STATIC
        ${bc7enc_rdo_SOURCE_DIR}/rgbcx.cpp
        ${bc7enc_rdo_SOURCE_DIR}/bc7enc.cpp
        ${bc7enc_rdo_SOURCE_DIR}/bc7decomp.cpp)
    target_include_directories(victus_bc7enc SYSTEM PUBLIC ${bc7enc_rdo_SOURCE_DIR})
    set_target_properties(victus_bc7enc PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(victus_bc7enc PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)
    victus_third_party_tsan(victus_bc7enc)
endfunction()

# Arm ASTC Encoder 5.7.1 (Apache-2.0). https://github.com/ARM-software/astc-encoder/releases/tag/5.7.1
# ASTC LDR block encoder/decoder for the texture cooker's mobile variants (ADR-0047). Only the
# codec library sources (Source/astcenc_*.cpp) are built; the SIMD path follows the compiler's
# baseline ISA (SSE2 on x86-64, NEON on arm64).
function(victus_fetch_astcenc)
    if(TARGET victus_astcenc)
        return()
    endif()
    FetchContent_Declare(astcenc
        URL https://github.com/ARM-software/astc-encoder/archive/refs/tags/5.7.1.tar.gz
        URL_HASH SHA256=37ff3928c4c156fb3ac0d1adb5df144ff9c5cdbc8a8e1ffeb20229cc7f35c767
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        SOURCE_SUBDIR victus_no_cmake)
    FetchContent_MakeAvailable(astcenc)
    file(GLOB _astc_sources ${astcenc_SOURCE_DIR}/Source/astcenc_*.cpp)
    add_library(victus_astcenc STATIC ${_astc_sources})
    target_include_directories(victus_astcenc SYSTEM PUBLIC ${astcenc_SOURCE_DIR}/Source)
    set_target_properties(victus_astcenc PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(victus_astcenc PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)
    victus_third_party_tsan(victus_astcenc)
endfunction()
