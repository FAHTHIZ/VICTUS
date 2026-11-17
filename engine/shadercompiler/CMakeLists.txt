# SPDX-License-Identifier: Apache-2.0
# Run-time GLSL -> SPIR-V (glslang) and SPIR-V -> MSL (SPIRV-Cross) for project shaders (ADR-0035).
victus_add_module(shadercompiler
    SOURCES src/shader_compiler.cpp
    DEPS VICTUS::Core
    PRIVATE_DEPS glslang glslang-default-resource-limits spirv-cross-msl spirv-cross-glsl spirv-cross-core)
