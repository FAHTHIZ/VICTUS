# SPDX-License-Identifier: Apache-2.0
victus_add_module(rhi
    SOURCES src/rhi_sdl.cpp
    DEPS VICTUS::Core VICTUS::Platform
    PRIVATE_DEPS SDL3::SDL3)
