# SPDX-License-Identifier: Apache-2.0
victus_add_module(player
    SOURCES
        src/player.cpp
    DEPS VICTUS::Runtime VICTUS::Project VICTUS::Gameplay VICTUS::Platform)
