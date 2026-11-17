# SPDX-License-Identifier: Apache-2.0
victus_add_module(ai
    SOURCES src/behavior_tree.cpp src/ai.cpp
    DEPS VICTUS::Scene VICTUS::Gameplay VICTUS::Physics VICTUS::Navigation)
