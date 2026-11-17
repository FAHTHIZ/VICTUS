# SPDX-License-Identifier: Apache-2.0
victus_add_module(project
    SOURCES src/project.cpp src/templates.cpp src/robot.cpp
    DEPS VICTUS::Assets VICTUS::Scene VICTUS::Gameplay VICTUS::Script VICTUS::Physics VICTUS::Audio VICTUS::Animation VICTUS::Particles VICTUS::Gameui VICTUS::Navigation VICTUS::Net VICTUS::Tilemap)
