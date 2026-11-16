// SPDX-License-Identifier: Apache-2.0
// Internal: an in-editor client of a hosted play session (see editor_clients.cpp).
#pragma once

#include "victus/animation/animation.hpp"
#include "victus/editor/editor.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/navigation/navigation.hpp"
#include "victus/net/net.hpp"
#include "victus/particles/particles.hpp"
#include "victus/physics/physics.hpp"
#include "victus/scene/scene.hpp"
#include "victus/script/script.hpp"

#include <memory>
#include <string>

namespace victus::editor {

struct EditorClient {
    std::string name;
    scene::Scene scene;
    std::unique_ptr<script::ScriptHost> scripts;
    std::unique_ptr<physics::PhysicsWorld> physics;
    std::unique_ptr<animation::AnimationSystem> animation;
    std::unique_ptr<particles::ParticleSystem> particles;
    std::unique_ptr<navigation::NavigationSystem> navigation;
    gameplay::ManualInput idle; // input while another peer is viewed: nothing held
    gameplay::Simulation sim;
    std::unique_ptr<net::NetClient> client;
};

} // namespace victus::editor
