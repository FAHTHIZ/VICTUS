# SPDX-License-Identifier: Apache-2.0
victus_add_module(script
    SOURCES
        src/script.cpp
        src/script_lua.cpp
        src/bind_scene.cpp
        src/bind_physics.cpp
        src/bind_render.cpp
        src/bind_mesh.cpp
        src/bind_net.cpp
        src/bind_world.cpp
        src/bind_audio.cpp
        src/bind_save.cpp
        src/bind_bridge.cpp
        src/bind_gameplay.cpp
    DEPS VICTUS::Graph VICTUS::Scene VICTUS::Gameplay VICTUS::Physics VICTUS::Audio VICTUS::Animation VICTUS::Particles VICTUS::Gameui VICTUS::Navigation VICTUS::Ai VICTUS::Sequencer VICTUS::Net VICTUS::Tilemap VICTUS::Terrain
    PRIVATE_DEPS Luau.VM Luau.Compiler)
