# SPDX-License-Identifier: Apache-2.0
victus_add_module(runtime
    SOURCES
        src/scene_rendering.cpp
        src/scene_view.cpp
    DEPS VICTUS::Assets VICTUS::Scene VICTUS::Gameplay VICTUS::Animation)
