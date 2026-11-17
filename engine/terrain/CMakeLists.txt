# SPDX-License-Identifier: Apache-2.0
victus_add_module(terrain
    SOURCES src/heightfield.cpp src/terrain.cpp src/brush.cpp src/foliage.cpp
    DEPS VICTUS::Scene VICTUS::Gameplay VICTUS::Physics)
