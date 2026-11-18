# SPDX-License-Identifier: Apache-2.0
victus_add_module(animation
    SOURCES src/clip.cpp src/animator.cpp src/controller.cpp src/sprite.cpp src/ik.cpp
    src/retarget.cpp
    DEPS VICTUS::Scene VICTUS::Gameplay)
