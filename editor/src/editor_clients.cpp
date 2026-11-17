// SPDX-License-Identifier: Apache-2.0
// In-editor clients of a hosted play session (NetPlaySettings::editor_clients). Each one is a
// complete client machine: its own copy of the authored scene, script host, physics, animation,
// particles and navigation, joined to the editor's listen server over UDP loopback with the
// session's key and password, exactly like `victus run --connect`. The viewport, hierarchy and
// inspector show the viewed peer's world (set_view_peer) and only that peer gets the keyboard.
#include "editor_client.hpp"

#include "victus/animation/animation.hpp"
#include "victus/assets/database.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/gameui/gameui.hpp"
#include "victus/navigation/navigation.hpp"
#include "victus/particles/particles.hpp"
#include "victus/physics/physics.hpp"
#include "victus/project/project.hpp"
#include "victus/script/script.hpp"

#include <format>

namespace victus::editor {


std::size_t Editor::editor_client_count() const noexcept { return play_clients_.size(); }

const net::NetClient* Editor::editor_client(std::size_t index) const noexcept {
    return index >= 1 && index <= play_clients_.size() ? play_clients_[index - 1]->client.get() : nullptr;
}

scene::Scene* Editor::editor_client_scene(std::size_t index) noexcept {
    return index >= 1 && index <= play_clients_.size() ? &play_clients_[index - 1]->scene : nullptr;
}

scene::Scene& Editor::active_scene() noexcept {
    if (view_peer_ >= 1 && view_peer_ <= play_clients_.size()) return play_clients_[view_peer_ - 1]->scene;
    return play_scene_ ? *play_scene_ : *scene_;
}

std::string Editor::peer_label(std::size_t peer) const {
    if (peer == 0) return play_server_ ? std::format("Host ({})", net_settings_.player_name) : std::string("Game");
    if (peer > play_clients_.size()) return {};
    return std::format("Client {} ({})", peer, play_clients_[peer - 1]->name);
}

void Editor::set_view_peer(std::size_t peer) {
    if (peer > play_clients_.size() || peer == view_peer_) return;
    view_peer_ = peer;
    route_input();
    prune_selection(); // spawned entities differ between peers; authored ones keep their UUIDs
}

void Editor::route_input() {
    gameplay::InputSource* live = game_input_override_ ? game_input_override_ : input_.get();
    if (!recording_ && !replay_) sim_.input = view_peer_ == 0 ? live : &host_idle_input_;
    for (std::size_t i = 0; i < play_clients_.size(); ++i) {
        auto& c = *play_clients_[i];
        c.sim.input = view_peer_ == i + 1 ? live : &c.idle;
    }
}

void Editor::start_editor_clients() {
    if (!play_server_ || !project_ || net_settings_.editor_clients == 0) return;
    for (std::uint16_t n = 1; n <= net_settings_.editor_clients; ++n) {
        auto c = std::make_unique<EditorClient>();
        c->name = std::format("player {}", n + 1);
        if (auto st = c->scene.load(scene_->save()); !st.ok()) {
            notify(std::format("in-editor client {}: {}", n, st.error().message), true);
            break;
        }
        gameplay::add_builtin_systems(c->sim);
        gameui::add_ui_systems(c->sim);
        if (db_) {
            script::HostOptions so;
            so.assets_root = db_->assets_dir();
            so.watch_files = false; // the host's script host follows file edits
            so.input_map = project_->settings().input;
            so.save_version = project_->settings().save_version;
            so.save_directory = project_->dev_save_directory();
            if (auto host = script::ScriptHost::create(so)) {
                c->scripts = std::move(host).value();
                script::add_script_systems(c->sim, *c->scripts);
            }
        }
        physics::PhysicsSettings ps;
        ps.jobs = &workers();
        if (auto pw = physics::PhysicsWorld::create(ps)) {
            c->physics = std::move(pw).value();
            physics::add_physics_systems(c->sim, *c->physics);
        }
        c->animation = std::make_unique<animation::AnimationSystem>(
            animation::AnimationSettings{.provider = db_ ? assets::animation_provider(*db_) : animation::SetProvider{},
                                         .sheets = db_ ? assets::sprite_sheet_provider(*db_) : animation::SheetProvider{},
                                         .jobs = &workers()});
        animation::add_animation_systems(c->sim, *c->animation);
        if (!tilemaps_) tilemaps_ = std::make_unique<tilemap::TilemapSystem>();
        tilemap::add_tilemap_systems(c->sim, *tilemaps_);
        if (!terrains_) terrains_ = std::make_unique<terrain::TerrainSystem>();
        terrain::add_terrain_systems(c->sim, *terrains_);
        c->particles = std::make_unique<particles::ParticleSystem>(particles::ParticleSettings{.jobs = &workers()});
        particles::add_particle_systems(c->sim, *c->particles);
        c->navigation = std::make_unique<navigation::NavigationSystem>();
        navigation::add_navigation_systems(c->sim, *c->navigation);
        c->ai = std::make_unique<ai::AiSystem>(db_ ? ai::directory_loader(db_->assets_dir()) : ai::TreeLoader{});
        ai::add_ai_systems(c->sim, *c->ai);
        // No audio: only the viewed host plays sound (scripts' audio calls are no-ops here).

        auto transport = net::create_udp_transport({});
        if (!transport) {
            notify(std::format("in-editor client {}: {}", n, transport.error().message), true);
            break;
        }
        std::unique_ptr<net::Transport> t = std::move(*transport);
        if (!net_settings_.insecure) {
            net::SecureOptions sec;
            if (auto key = net::key_from_hex(server_key_)) sec.expected_server_key = *key;
            sec.password = net_settings_.password;
            t = net::with_encryption(std::move(t), std::move(sec));
        }
        net::ClientOptions co;
        co.host = "127.0.0.1";
        co.port = net_settings_.port;
        co.name = c->name;
        co.game_id = net::make_game_id(project_->settings().name, scene_path_);
        c->client = std::make_unique<net::NetClient>(std::move(t), co);
        c->client->install(c->sim);
        play_clients_.push_back(std::move(c));
    }
    route_input();
}

void Editor::tick_editor_clients(float dt) {
    for (auto& c : play_clients_) c->sim.tick(c->scene, dt);
}

void Editor::stop_editor_clients() {
    // Clients say goodbye before the server shuts down, so the host sees them leave cleanly.
    for (auto& c : play_clients_)
        if (c->client) c->client->disconnect(c->sim.time().time);
    play_clients_.clear();
    view_peer_ = 0;
}

} // namespace victus::editor
