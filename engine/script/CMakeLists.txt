# SPDX-License-Identifier: Apache-2.0
victus_add_module(script
    SOURCES src/script.cpp
    DEPS VICTUS::Scene VICTUS::Gameplay VICTUS::Physics VICTUS::Audio VICTUS::Animation VICTUS::Particles VICTUS::Gameui VICTUS::Navigation
    PRIVATE_DEPS Luau.VM Luau.Compiler)
