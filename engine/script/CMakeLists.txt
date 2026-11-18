# SPDX-License-Identifier: Apache-2.0
victus_add_module(script
    SOURCES src/script.cpp
    DEPS VICTUS::Graph VICTUS::Scene VICTUS::Gameplay VICTUS::Physics VICTUS::Audio VICTUS::Animation VICTUS::Particles VICTUS::Gameui VICTUS::Navigation VICTUS::Ai VICTUS::Sequencer VICTUS::Net VICTUS::Tilemap VICTUS::Terrain
    PRIVATE_DEPS Luau.VM Luau.Compiler)
