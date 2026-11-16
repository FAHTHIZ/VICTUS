// SPDX-License-Identifier: Apache-2.0
// Editor network tooling: Window > Network (play-mode session settings, the hosted session's
// state, joined clients, bandwidth and a per-entity replication table) and the inspector's
// Network section for networked entities in play mode.
#include "victus/editor/editor.hpp"

#include "editor_client.hpp"

#include "victus/platform/platform.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <format>

namespace victus::editor {

using namespace scene;

namespace {

std::string format_bytes(std::uint64_t b) {
    if (b < 10 * 1024) return std::format("{} B", b);
    if (b < 10 * 1024 * 1024) return std::format("{:.1f} KB", static_cast<double>(b) / 1024.0);
    return std::format("{:.1f} MB", static_cast<double>(b) / (1024.0 * 1024.0));
}

bool contains_ci(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    const auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != hay.end();
}

std::string owner_label(const net::NetContext& ctx, std::uint16_t owner) {
    for (const auto& c : ctx.clients())
        if (c.id == owner) return std::format("{} ({})", c.name, owner);
    return owner == net::kServerId ? std::string("server") : std::format("client {}", owner);
}

} // namespace

void Editor::draw_network() {
    if (!show_network_ || !project_) return;
    ImGui::SetNextWindowSize(ImVec2(900, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Network", &show_network_)) {
        ImGui::End();
        return;
    }
    const bool editing = play_state_ == PlayState::Editing;

    // ---- settings (next Play) ----
    if (ImGui::CollapsingHeader("Play Mode Session", editing ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        ImGui::BeginDisabled(!editing);
        NetPlaySettings s = net_settings_;
        bool changed = false;
        int mode = s.mode == NetPlaySettings::Mode::Host ? 1 : 0;
        changed |= ImGui::RadioButton("Offline", &mode, 0);
        ImGui::SameLine();
        changed |= ImGui::RadioButton("Host (listen server)", &mode, 1);
        s.mode = mode ? NetPlaySettings::Mode::Host : NetPlaySettings::Mode::Offline;
        int port = s.port, max_clients = s.max_clients;
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("UDP port", &port)) {
            s.port = static_cast<std::uint16_t>(std::clamp(port, 1, 65535));
            changed = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Max clients", &max_clients)) {
            s.max_clients = static_cast<std::uint16_t>(std::clamp(max_clients, 1, 64));
            changed = true;
        }
        int extra = s.editor_clients;
        ImGui::SetNextItemWidth(120);
        if (ImGui::SliderInt("In-editor clients", &extra, 0, 3)) {
            s.editor_clients = static_cast<std::uint16_t>(extra);
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Extra players simulated inside the editor, each with its own world; switch with View or Ctrl+1..4");
        ImGui::SetNextItemWidth(240);
        changed |= ImGui::InputText("Host player name", &s.player_name);
        ImGui::SetNextItemWidth(240);
        changed |= ImGui::InputText("Password", &s.password, ImGuiInputTextFlags_Password);
        changed |= ImGui::Checkbox("Insecure (no encryption)", &s.insecure);
        ImGui::SetNextItemWidth(320);
        changed |= ImGui::InputTextWithHint("Link conditions", "latency=80,jitter=10,loss=0.05", &s.conditions);
        if (changed) {
            if (auto st = set_net_settings(s); !st.ok()) net_error_ = st.error().message;
            else net_error_.clear();
        }
        if (!net_error_.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", net_error_.c_str());
        if (ImGui::Button("Play as Host")) play_host();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Others join with victus run <project> --connect <ip>:%u", static_cast<unsigned>(net_settings_.port));
    }

    if (!play_server_) {
        ImGui::Separator();
        if (editing) ImGui::TextWrapped("Choose Host and press Play (or Play as Host) to run this scene as a listen server.");
        else ImGui::TextWrapped("This play session is offline: every script has authority. Stop, then Play as Host to test multiplayer.");
        ImGui::End();
        return;
    }

    const net::NetServer& srv = *play_server_;
    const net::NetDiagnostics d = srv.diagnostics();

    // ---- session summary ----
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.45f, 1), "Hosting");
    ImGui::SameLine();
    ImGui::Text("UDP %u, %s, tick %u, %u/%u clients%s", static_cast<unsigned>(net_settings_.port),
                d.encrypted ? "encrypted" : "insecure", d.tick, d.clients, static_cast<unsigned>(net_settings_.max_clients),
                d.held_players ? std::format(", {} held", d.held_players).c_str() : "");
    if (!server_key_.empty()) {
        ImGui::TextDisabled("Server key %s", server_key_.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy Key") && platform_) platform_->set_clipboard(server_key_);
    }
    if (ImGui::SmallButton("Copy Join Command") && platform_) platform_->set_clipboard(join_command());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", join_command().c_str());
    ImGui::Text("Send %.1f kbps   Receive %.1f kbps   Sent %s   Received %s", static_cast<double>(d.send_kbps),
                static_cast<double>(d.receive_kbps), format_bytes(d.bytes_sent).c_str(), format_bytes(d.bytes_received).c_str());
    const double delta_share = d.snapshots_sent ? 100.0 * static_cast<double>(d.delta_snapshots) / static_cast<double>(d.snapshots_sent) : 0.0;
    ImGui::Text("Snapshots %llu (%.0f%% delta), last %u B, deferred %u, rejected messages %llu",
                static_cast<unsigned long long>(d.snapshots_sent), delta_share, d.snapshot_bytes, d.entities_deferred,
                static_cast<unsigned long long>(d.rejected_messages));
    if (!net_send_history_.empty()) {
        const auto peak = [](const std::vector<float>& v) { return v.empty() ? 1.0f : std::max(1.0f, *std::max_element(v.begin(), v.end())); };
        const float top = std::max(peak(net_send_history_), peak(net_receive_history_)) * 1.15f;
        const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::PlotLines("##send", net_send_history_.data(), static_cast<int>(net_send_history_.size()), 0, "send kbps", 0.0f, top, ImVec2(w, 48));
        ImGui::SameLine();
        ImGui::PlotLines("##recv", net_receive_history_.data(), static_cast<int>(net_receive_history_.size()), 0, "receive kbps", 0.0f, top, ImVec2(w, 48));
    }

    // ---- in-editor players ----
    if (!play_clients_.empty() &&
        ImGui::CollapsingHeader(std::format("In-Editor Players ({})###editorpeers", play_clients_.size() + 1).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("##peers", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
            for (const char* h : {"View", "Player", "State", "RTT", "Prediction error", "Received"}) ImGui::TableSetupColumn(h);
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i <= play_clients_.size(); ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::RadioButton(std::format("Ctrl+{}", i + 1).c_str(), view_peer_ == i)) set_view_peer(i);
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(peer_label(i).c_str());
                if (i == 0) {
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted("server (no network delay)");
                    for (int k = 0; k < 3; ++k) ImGui::TableNextColumn();
                    continue;
                }
                const auto cd = play_clients_[i - 1]->client->diagnostics();
                ImGui::TableNextColumn();
                if (cd.connected) ImGui::TextUnformatted(cd.status.c_str());
                else ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "%s", cd.status.c_str());
                ImGui::TableNextColumn();
                if (cd.connected) ImGui::Text("%.1f ms", static_cast<double>(cd.rtt_ms));
                ImGui::TableNextColumn();
                if (cd.connected) ImGui::Text("%.3f m (%llu corrections)", static_cast<double>(cd.prediction_error),
                                              static_cast<unsigned long long>(cd.corrections));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(format_bytes(cd.bytes_received).c_str());
            }
            ImGui::EndTable();
        }
    }

    // ---- clients ----
    const auto clients = srv.client_diagnostics();
    if (ImGui::CollapsingHeader(std::format("Clients ({})###clients", clients.size()).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (clients.empty()) {
            ImGui::TextDisabled("No one has joined yet. Copy the join command and run it in a terminal or on another machine.");
        } else if (ImGui::BeginTable("##clients", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
            for (const char* h : {"ID", "Name", "State", "RTT", "Sent", "Received", "Snapshot ack", "Input queue"}) ImGui::TableSetupColumn(h);
            ImGui::TableHeadersRow();
            for (const auto& c : clients) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u", static_cast<unsigned>(c.id));
                ImGui::TableNextColumn();
                ImGui::PushID(c.id);
                if (ImGui::Selectable(c.name.c_str(), !c.player.is_null() && view_peer_ == 0 && is_selected(c.player), ImGuiSelectableFlags_SpanAllColumns) &&
                    !c.player.is_null() && play_scene_ && play_scene_->world().alive(c.player)) {
                    set_view_peer(0); // server-side entities live in the host's world
                    select(c.player);
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                if (c.held) ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "held (reconnecting)");
                else ImGui::TextUnformatted(c.encrypted ? "connected (encrypted)" : "connected");
                ImGui::TableNextColumn();
                if (!c.held) ImGui::Text("%.1f ms", static_cast<double>(c.rtt_ms));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(format_bytes(c.bytes_sent).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(format_bytes(c.bytes_received).c_str());
                ImGui::TableNextColumn();
                if (!c.held) ImGui::Text("%u / %u", c.snapshot_ack, c.snapshot_seq);
                ImGui::TableNextColumn();
                if (!c.held) ImGui::Text("%u", c.input_queue);
            }
            ImGui::EndTable();
        }
    }

    // ---- replication ----
    const auto rows = srv.replication();
    if (ImGui::CollapsingHeader(std::format("Replicated Entities ({})###replication", rows.size()).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(260);
        ImGui::InputTextWithHint("##netfilter", "filter by name or owner", &net_filter_);
        const double now = sim_.time().time;
        constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                                           ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable("##replication", 9, kFlags, ImVec2(0, std::max(120.0f, ImGui::GetContentRegionAvail().y)))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            for (const char* h : {"Net ID", "Entity", "Owner", "Updates", "Bytes", "Vars", "Relevant", "Deferred", "Last update"})
                ImGui::TableSetupColumn(h);
            ImGui::TableHeadersRow();
            for (const auto& r : rows) {
                const std::string owner = owner_label(srv, r.owner);
                if (!contains_ci(r.name, net_filter_) && !contains_ci(owner, net_filter_)) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u", r.net_id);
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(r.net_id));
                const std::string label = r.player ? r.name + "  [player]" : r.name;
                const bool alive = play_scene_ && play_scene_->world().alive(r.entity);
                if (ImGui::Selectable(label.c_str(), alive && view_peer_ == 0 && is_selected(r.entity), ImGuiSelectableFlags_SpanAllColumns) && alive) {
                    set_view_peer(0);
                    select(r.entity);
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(owner.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(r.updates));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(format_bytes(r.bytes + r.var_bytes).c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("snapshots %s, variables %s (%llu updates)", format_bytes(r.bytes).c_str(),
                                      format_bytes(r.var_bytes).c_str(), static_cast<unsigned long long>(r.var_updates));
                ImGui::TableNextColumn();
                ImGui::Text("%u", r.vars);
                ImGui::TableNextColumn();
                ImGui::Text("%u/%u", r.relevant_clients, d.clients);
                ImGui::TableNextColumn();
                if (r.deferred) ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "%llu", static_cast<unsigned long long>(r.deferred));
                else ImGui::TextDisabled("0");
                ImGui::TableNextColumn();
                if (r.last_update < 0) ImGui::TextDisabled("never");
                else ImGui::Text("%.2f s ago", std::max(0.0, now - r.last_update));
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void Editor::draw_network_section(Entity e) {
    if (!play_scene_ || play_state_ == PlayState::Editing) return;
    Scene& viewed = active_scene(); // the host's world or an in-editor client's
    auto& w = viewed.world();
    const auto* st = w.get<net::NetState>(e);
    if (!st) return;
    if (!ImGui::CollapsingHeader("Network (live)", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const net::NetContext* ctx = net::context_of(viewed);
    if (view_peer_ > 0) ImGui::TextDisabled("As seen by %s", peer_label(view_peer_).c_str());
    if (!ctx) {
        ImGui::TextDisabled("Offline: this machine has authority.");
    } else {
        ImGui::Text("Net ID %u, owner %s%s", st->net_id, owner_label(*ctx, st->owner).c_str(),
                    st->owner == net::kServerId && ctx->local_player() == e ? " (host)" : "");
        for (const auto& r : ctx->replication()) {
            if (r.entity != e) continue;
            ImGui::Text("Snapshot updates %llu (%s), variable updates %llu (%s)", static_cast<unsigned long long>(r.updates),
                        format_bytes(r.bytes).c_str(), static_cast<unsigned long long>(r.var_updates), format_bytes(r.var_bytes).c_str());
            if (r.deferred) ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "Deferred by the snapshot budget %llu times",
                                               static_cast<unsigned long long>(r.deferred));
        }
    }
    if (st->vars.is_object() && st->vars.size()) {
        ImGui::TextDisabled("Network variables");
        for (const auto& [k, v] : st->vars.as_object()) ImGui::BulletText("%s = %s", k.c_str(), json::stringify(v, false).c_str());
    } else {
        ImGui::TextDisabled("No network variables");
    }
}

} // namespace victus::editor
