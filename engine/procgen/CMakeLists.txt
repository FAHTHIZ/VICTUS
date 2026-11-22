# SPDX-License-Identifier: Apache-2.0
victus_add_module(procgen
    SOURCES src/city.cpp src/city_mesh.cpp src/city_scene.cpp
    DEPS VICTUS::Scene VICTUS::Gameplay VICTUS::Traffic VICTUS::Animation VICTUS::Physics VICTUS::Core)
