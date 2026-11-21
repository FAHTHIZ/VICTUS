# SPDX-License-Identifier: Apache-2.0
victus_add_module(sequencer
    SOURCES src/property.cpp src/sequence.cpp src/sequencer.cpp
    DEPS VICTUS::Scene VICTUS::Gameplay VICTUS::Animation VICTUS::Audio)
