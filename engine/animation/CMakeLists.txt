# SPDX-License-Identifier: Apache-2.0
victus_add_module(animation
    SOURCES src/clip.cpp src/animator.cpp src/sprite.cpp
    DEPS VICTUS::Scene VICTUS::Gameplay)
