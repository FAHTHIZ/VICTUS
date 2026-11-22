# SPDX-License-Identifier: Apache-2.0
victus_add_module(animation
    SOURCES src/clip.cpp src/animator.cpp src/controller.cpp src/sprite.cpp src/ik.cpp src/foot_placement.cpp
    src/retarget.cpp src/crowd.cpp
    DEPS VICTUS::Scene VICTUS::Gameplay)
