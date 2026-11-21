# SPDX-License-Identifier: Apache-2.0
victus_add_module(net
    SOURCES src/transport.cpp src/secure.cpp src/session.cpp
    DEPS VICTUS::Scene VICTUS::Gameplay VICTUS::Physics
    PRIVATE_DEPS victus_enet victus_monocypher)
