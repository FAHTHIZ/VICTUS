# SPDX-License-Identifier: Apache-2.0
victus_add_module(platform
    SOURCES src/platform.cpp
    DEPS VICTUS::Core
    PRIVATE_DEPS SDL3::SDL3)
