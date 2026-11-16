# SPDX-License-Identifier: Apache-2.0
victus_add_module(navigation
    SOURCES src/navigation.cpp
    DEPS VICTUS::Scene VICTUS::Gameplay VICTUS::Physics
    PRIVATE_DEPS victus_recast)
