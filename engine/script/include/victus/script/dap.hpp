// SPDX-License-Identifier: Apache-2.0
// Debug Adapter Protocol (https://microsoft.github.io/debug-adapter-protocol/) front end of the
// Luau source debugger (ADR-0099). DapSession answers the protocol's debugging requests against
// one ScriptHost; the program around it moves the messages (stdio for `victus debug-adapter`)
// and handles `launch`, since starting a game is up to the host application.
#pragma once
#include "victus/core/json.hpp"
#include "victus/script/debugger.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace victus::script {

class ScriptHost;

// Base protocol framing: "Content-Length: N\r\n\r\n" + N bytes of JSON.
[[nodiscard]] std::string dap_encode(const json::Value& message);

// Incremental decoder of the base protocol.
class DapDecoder {
public:
    void feed(std::string_view bytes);
    // The next complete message (nullopt: more bytes needed). Malformed content is an error.
    [[nodiscard]] Result<std::optional<json::Value>> next();

private:
    std::string buffer_;
};

class DapSession {
public:
    using Send = std::function<void(const json::Value& message)>;
    // Blocks for the next message from the client; nullopt when the channel closed.
    using Receive = std::function<std::optional<json::Value>()>;

    DapSession(Send send, Receive receive);
    ~DapSession();
    DapSession(const DapSession&) = delete;
    DapSession& operator=(const DapSession&) = delete;

    // Connects the debugger to `host` (created with HostOptions::debugger) whose scripts live in
    // `assets_root`; breakpoints set before are applied now.
    void attach(ScriptHost& host, std::filesystem::path assets_root);
    void detach();

    // Handles a request. False for requests the session leaves to the caller (launch, attach,
    // unknown commands); the caller then responds itself.
    bool handle(const json::Value& request);

    void respond(const json::Value& request, bool success, json::Value body = json::Value::object(),
                 std::string_view message = {});
    void event(std::string_view name, json::Value body = json::Value::object());
    void output(std::string_view category, std::string_view text);

    // Set once `configurationDone` arrived (the game may start).
    [[nodiscard]] bool configured() const noexcept { return configured_; }
    // Set by `disconnect`/`terminate` or when the channel closed while stopped.
    [[nodiscard]] bool disconnected() const noexcept { return disconnected_; }
    [[nodiscard]] std::uint64_t stops() const noexcept { return stops_; }

    // Sends `breakpoint` events for breakpoints whose verification changed (their script loaded
    // after they were set). Call once per frame while running; stops call it too.
    void poll();

private:
    StepAction on_stop(DebugStop& stop);
    void set_breakpoints(const json::Value& request);
    void apply_breakpoints(const std::string& script, const json::Value& list, json::Value* out);
    json::Value variables_of(int reference);
    json::Value variable_json(const DebugVariable& v);
    std::int64_t breakpoint_id(const std::string& script, std::size_t index);
    [[nodiscard]] std::optional<std::string> script_path(std::string_view file) const;

    Send send_;
    Receive receive_;
    ScriptHost* host_{nullptr};
    std::filesystem::path assets_root_;
    DebugStop* stop_{nullptr};
    std::optional<StepAction> resume_;
    std::int64_t seq_{1};
    bool configured_{false};
    bool disconnected_{false};
    bool break_on_error_{true};
    std::uint64_t stops_{0};
    // Breakpoint requests by script path (kept across attach so they can arrive first).
    std::unordered_map<std::string, json::Value> breakpoints_;
    std::unordered_map<std::string, std::string> files_;
    std::unordered_map<std::string, std::int64_t> script_ids_;
    std::unordered_map<std::string, std::vector<std::pair<bool, int>>> reported_; // script path -> client file path
};

} // namespace victus::script
