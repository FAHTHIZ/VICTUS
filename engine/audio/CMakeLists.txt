# SPDX-License-Identifier: Apache-2.0
victus_add_module(audio
    SOURCES src/audio.cpp
    DEPS VICTUS::Scene VICTUS::Gameplay
    PRIVATE_DEPS victus_miniaudio)
