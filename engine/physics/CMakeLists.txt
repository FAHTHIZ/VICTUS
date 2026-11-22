# SPDX-License-Identifier: Apache-2.0
victus_add_module(physics
    SOURCES src/components.cpp src/physics.cpp src/polygon.cpp src/jolt_world.cpp src/box2d_world.cpp src/ragdoll.cpp src/destruction.cpp src/vehicle_setup.cpp src/cloth.cpp
    DEPS VICTUS::Scene VICTUS::Gameplay
    PRIVATE_DEPS Jolt box2d)
