// SPDX-License-Identifier: Apache-2.0
// Debug Adapter Protocol session over the Luau source debugger (ADR-0099).
#include "victus/script/dap.hpp"

#include "victus/script/script.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <system_error>

namespace victus::script {

namespace {

constexpr int kThread = 1;
constexpr int kTableBase = 1 << 20; // variable references above this are tables of the stop

std::string_view reason_name(StopReason r) {
    switch (r) {
    case StopReason::Breakpoint: return "breakpoint";
    case StopReason::Step: return "step";
    case StopReason::Pause: return "pause";
    case StopReason::Exception: return "exception";
    }
    return "pause";
}

std::filesystem::path normalized(const std::filesystem::path& p) {
    std::error_code ec;
    auto abs = std::filesystem::absolute(p, ec);
    if (ec) abs = p;
    auto canon = std::filesystem::weakly_canonical(abs, ec);
    return (ec ? abs : canon).lexically_normal();
}

} // namespace

// ---- framing ------------------------------------------------------------------------------------

std::string dap_encode(const json::Value& message) {
    const std::string body = json::stringify(message, false);
    return std::format("Content-Length: {}\r\n\r\n{}", body.size(), body);
}

void DapDecoder::feed(std::string_view bytes) { buffer_.append(bytes); }

Result<std::optional<json::Value>> DapDecoder::next() {
    const std::size_t end = buffer_.find("\r\n\r\n");
    if (end == std::string::npos) {
        if (buffer_.size() > 4096) return make_error(ErrorCode::ParseError, "DAP header too long");
        return std::optional<json::Value>{};
    }
    std::size_t length = 0;
    bool found = false;
    std::string_view headers(buffer_.data(), end);
    while (!headers.empty()) {
        const std::size_t eol = headers.find("\r\n");
        std::string_view line = headers.substr(0, eol);
        headers = eol == std::string_view::npos ? std::string_view{} : headers.substr(eol + 2);
        constexpr std::string_view kKey = "Content-Length:";
        if (line.starts_with(kKey)) {
            line.remove_prefix(kKey.size());
            while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
            const auto [p, ec] = std::from_chars(line.data(), line.data() + line.size(), length);
            found = ec == std::errc{} && p != line.data();
        }
    }
    if (!found) return make_error(ErrorCode::ParseError, "DAP message without Content-Length");
    if (length > (64u << 20)) return make_error(ErrorCode::ParseError, "DAP message too large");
    if (buffer_.size() < end + 4 + length) return std::optional<json::Value>{};
    auto v = json::parse(std::string_view(buffer_).substr(end + 4, length));
    buffer_.erase(0, end + 4 + length);
    if (!v) return v.error();
    return std::optional<json::Value>(std::move(*v));
}

// ---- session ------------------------------------------------------------------------------------

DapSession::DapSession(Send send, Receive receive) : send_(std::move(send)), receive_(std::move(receive)) {}

DapSession::~DapSession() { detach(); }

void DapSession::attach(ScriptHost& host, std::filesystem::path assets_root) {
    detach();
    host_ = &host;
    assets_root_ = normalized(assets_root);
    host.set_debug_handler([this](DebugStop& s) { return on_stop(s); },
                           [this](std::string_view text) { output("console", std::string(text) + "\n"); });
    host.set_break_on_error(break_on_error_);
    for (const auto& [script, list] : breakpoints_) apply_breakpoints(script, list, nullptr);
}

void DapSession::detach() {
    if (!host_) return;
    for (const auto& [script, list] : breakpoints_) (void)host_->set_breakpoints(script, {});
    host_->set_break_on_error(false);
    host_->set_debug_handler({});
    host_ = nullptr;
}

void DapSession::respond(const json::Value& request, bool success, json::Value body, std::string_view message) {
    json::Value r = json::Value::object();
    r.set("seq", seq_++);
    r.set("type", "response");
    r.set("request_seq", request["seq"].as_int());
    r.set("success", success);
    r.set("command", request["command"].as_string_or(""));
    if (!message.empty()) r.set("message", message);
    r.set("body", std::move(body));
    send_(r);
}

void DapSession::event(std::string_view name, json::Value body) {
    json::Value e = json::Value::object();
    e.set("seq", seq_++);
    e.set("type", "event");
    e.set("event", name);
    e.set("body", std::move(body));
    send_(e);
}

void DapSession::output(std::string_view category, std::string_view text) {
    json::Value b = json::Value::object();
    b.set("category", category);
    b.set("output", text);
    event("output", std::move(b));
}

std::optional<std::string> DapSession::script_path(std::string_view file) const {
    if (file.empty()) return std::nullopt;
    const std::filesystem::path p(file);
    if (assets_root_.empty() || p.is_relative()) {
        std::string s = p.generic_string();
        return s.starts_with("../") ? std::nullopt : std::optional<std::string>(s);
    }
    const auto rel = normalized(p).lexically_relative(assets_root_);
    const std::string s = rel.generic_string();
    if (s.empty() || s.starts_with("..")) return std::nullopt;
    return s;
}

void DapSession::apply_breakpoints(const std::string& script, const json::Value& list, json::Value* out) {
    std::vector<SourceBreakpoint> bps;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const json::Value& b = list[i];
        bps.push_back({static_cast<int>(b["line"].as_int()), b["condition"].as_string_or(""),
                       b["hitCondition"].as_string_or(""), b["logMessage"].as_string_or("")});
    }
    std::vector<BreakpointState> states;
    if (host_) states = host_->set_breakpoints(script, std::move(bps));
    auto& last = reported_[script];
    last.assign(list.size(), {false, 0});
    for (std::size_t i = 0; i < states.size() && i < last.size(); ++i) last[i] = {states[i].verified, states[i].line};
    if (!out) return;
    for (std::size_t i = 0; i < list.size(); ++i) {
        json::Value b = json::Value::object();
        b.set("id", breakpoint_id(script, i));
        if (i < states.size()) {
            b.set("verified", states[i].verified);
            b.set("line", states[i].line);
            if (!states[i].message.empty()) b.set("message", states[i].message);
        } else {
            b.set("verified", false);
            b.set("line", list[i]["line"].as_int());
            b.set("message", "the game has not started");
        }
        out->push(std::move(b));
    }
}

void DapSession::set_breakpoints(const json::Value& request) {
    const json::Value& args = request["arguments"];
    const std::string file = args["source"]["path"].as_string_or("");
    const json::Value& list = args["breakpoints"].is_array() ? args["breakpoints"] : json::Value::array();
    json::Value out = json::Value::array();
    if (auto script = script_path(file)) {
        files_[*script] = file;
        if (list.size() == 0) breakpoints_.erase(*script);
        else breakpoints_[*script] = list;
        apply_breakpoints(*script, list, &out);
    } else {
        for (std::size_t i = 0; i < list.size(); ++i) {
            json::Value b = json::Value::object();
            b.set("verified", false);
            b.set("line", list[i]["line"].as_int());
            b.set("message", "not a script of this project's assets folder");
            out.push(std::move(b));
        }
    }
    json::Value body = json::Value::object();
    body.set("breakpoints", std::move(out));
    respond(request, true, std::move(body));
}

json::Value DapSession::variable_json(const DebugVariable& v) {
    json::Value j = json::Value::object();
    j.set("name", v.name);
    j.set("value", v.value);
    j.set("type", v.type);
    j.set("variablesReference", v.reference ? kTableBase + v.reference : 0);
    return j;
}

json::Value DapSession::variables_of(int reference) {
    json::Value out = json::Value::array();
    if (!stop_ || reference <= 0) return out;
    std::vector<DebugVariable> vars;
    if (reference > kTableBase) vars = stop_->children(reference - kTableBase);
    else if (reference % 2 == 1) vars = stop_->locals(static_cast<std::size_t>((reference - 1) / 2));
    else vars = stop_->upvalues(static_cast<std::size_t>((reference - 2) / 2));
    for (const auto& v : vars) out.push(variable_json(v));
    return out;
}

std::int64_t DapSession::breakpoint_id(const std::string& script, std::size_t index) {
    auto [it, inserted] = script_ids_.try_emplace(script, static_cast<std::int64_t>(script_ids_.size() + 1));
    return it->second * 10000 + static_cast<std::int64_t>(index);
}

void DapSession::poll() {
    if (!host_) return;
    for (const auto& [script, list] : breakpoints_) {
        const auto states = host_->breakpoints(script);
        auto& last = reported_[script];
        last.resize(states.size(), {});
        for (std::size_t i = 0; i < states.size(); ++i) {
            const std::pair<bool, int> now{states[i].verified, states[i].line};
            if (last[i] == now) continue;
            last[i] = now;
            json::Value b = json::Value::object();
            b.set("id", breakpoint_id(script, i));
            b.set("verified", states[i].verified);
            b.set("line", states[i].line);
            if (!states[i].message.empty()) b.set("message", states[i].message);
            json::Value body = json::Value::object();
            body.set("reason", "changed");
            body.set("breakpoint", std::move(b));
            event("breakpoint", std::move(body));
        }
    }
}

bool DapSession::handle(const json::Value& request) {
    const std::string command = request["command"].as_string_or("");
    const json::Value& args = request["arguments"];
    if (command == "initialize") {
        json::Value caps = json::Value::object();
        caps.set("supportsConfigurationDoneRequest", true);
        caps.set("supportsConditionalBreakpoints", true);
        caps.set("supportsHitConditionalBreakpoints", true);
        caps.set("supportsLogPoints", true);
        caps.set("supportsEvaluateForHovers", true);
        caps.set("supportsSetVariable", true);
        caps.set("supportsTerminateRequest", true);
        json::Value filter = json::Value::object();
        filter.set("filter", "uncaught");
        filter.set("label", "Uncaught Errors");
        filter.set("default", true);
        json::Value filters = json::Value::array();
        filters.push(std::move(filter));
        caps.set("exceptionBreakpointFilters", std::move(filters));
        respond(request, true, std::move(caps));
        return true;
    }
    if (command == "setBreakpoints") {
        set_breakpoints(request);
        return true;
    }
    if (command == "setExceptionBreakpoints") {
        break_on_error_ = false;
        const json::Value& f = args["filters"];
        for (std::size_t i = 0; i < f.size(); ++i)
            if (f[i].as_string_or("") == "uncaught") break_on_error_ = true;
        if (host_) host_->set_break_on_error(break_on_error_);
        respond(request, true);
        return true;
    }
    if (command == "configurationDone") {
        configured_ = true;
        respond(request, true);
        return true;
    }
    if (command == "threads") {
        json::Value t = json::Value::object();
        t.set("id", kThread);
        t.set("name", "Scripts");
        json::Value list = json::Value::array();
        list.push(std::move(t));
        json::Value body = json::Value::object();
        body.set("threads", std::move(list));
        respond(request, true, std::move(body));
        return true;
    }
    if (command == "stackTrace") {
        json::Value frames = json::Value::array();
        std::size_t total = 0;
        if (stop_) {
            const auto& f = stop_->frames();
            total = f.size();
            const auto start = static_cast<std::size_t>(std::max<std::int64_t>(0, args["startFrame"].as_int()));
            const auto levels = static_cast<std::size_t>(std::max<std::int64_t>(0, args["levels"].as_int()));
            for (std::size_t i = start; i < f.size() && (levels == 0 || i < start + levels); ++i) {
                json::Value j = json::Value::object();
                j.set("id", static_cast<std::int64_t>(i + 1));
                j.set("name", f[i].function);
                json::Value src = json::Value::object();
                src.set("name", std::filesystem::path(f[i].source).filename().string());
                if (auto it = files_.find(f[i].source); it != files_.end()) src.set("path", it->second);
                else if (!assets_root_.empty() && f[i].source.find('.') != std::string::npos)
                    src.set("path", (assets_root_ / f[i].source).lexically_normal().string());
                j.set("source", std::move(src));
                j.set("line", f[i].line);
                j.set("column", 1);
                frames.push(std::move(j));
            }
        }
        json::Value body = json::Value::object();
        body.set("stackFrames", std::move(frames));
        body.set("totalFrames", static_cast<std::uint64_t>(total));
        respond(request, true, std::move(body));
        return true;
    }
    if (command == "scopes") {
        const std::int64_t frame = std::max<std::int64_t>(1, args["frameId"].as_int(1)) - 1;
        json::Value scopes = json::Value::array();
        const char* names[2] = {"Locals", "Upvalues"};
        for (int k = 0; k < 2; ++k) {
            json::Value s = json::Value::object();
            s.set("name", names[k]);
            if (k == 0) s.set("presentationHint", "locals");
            s.set("variablesReference", frame * 2 + 1 + k);
            s.set("expensive", false);
            scopes.push(std::move(s));
        }
        json::Value body = json::Value::object();
        body.set("scopes", std::move(scopes));
        respond(request, true, std::move(body));
        return true;
    }
    if (command == "variables") {
        json::Value body = json::Value::object();
        body.set("variables", variables_of(static_cast<int>(args["variablesReference"].as_int())));
        respond(request, true, std::move(body));
        return true;
    }
    if (command == "evaluate") {
        const std::string expr = args["expression"].as_string_or("");
        if (stop_) {
            const std::int64_t frame = std::max<std::int64_t>(1, args["frameId"].as_int(1)) - 1;
            auto v = stop_->evaluate(expr, static_cast<std::size_t>(frame));
            if (!v) {
                respond(request, false, json::Value::object(), v.error().message);
                return true;
            }
            json::Value body = json::Value::object();
            body.set("result", v->value);
            body.set("type", v->type);
            body.set("variablesReference", v->reference ? kTableBase + v->reference : 0);
            respond(request, true, std::move(body));
            return true;
        }
        if (!host_) {
            respond(request, false, json::Value::object(), "the game has not started");
            return true;
        }
        // Running: evaluate in the scripts' global environment.
        const bool statement = expr.find('=') != std::string::npos && !expr.starts_with("return");
        auto r = host_->eval(statement ? expr : "return " + expr, nullptr, "repl");
        if (!r) {
            respond(request, false, json::Value::object(), r.error().message);
            return true;
        }
        json::Value body = json::Value::object();
        body.set("result", json::stringify(*r, false));
        body.set("variablesReference", 0);
        respond(request, true, std::move(body));
        return true;
    }
    if (command == "setVariable") {
        const int ref = static_cast<int>(args["variablesReference"].as_int());
        if (!stop_ || ref <= 0 || ref > kTableBase) {
            respond(request, false, json::Value::object(), "only locals and upvalues of a stopped frame can be set");
            return true;
        }
        const auto frame = static_cast<std::size_t>((ref - 1) / 2);
        auto v = stop_->set_variable(frame, args["name"].as_string_or(""), args["value"].as_string_or("nil"));
        if (!v) {
            respond(request, false, json::Value::object(), v.error().message);
            return true;
        }
        json::Value body = json::Value::object();
        body.set("value", v->value);
        body.set("type", v->type);
        body.set("variablesReference", v->reference ? kTableBase + v->reference : 0);
        respond(request, true, std::move(body));
        return true;
    }
    if (command == "continue" || command == "next" || command == "stepIn" || command == "stepOut") {
        if (stop_) {
            resume_ = command == "continue" ? StepAction::Continue
                      : command == "next"   ? StepAction::StepOver
                      : command == "stepIn" ? StepAction::StepIn
                                            : StepAction::StepOut;
        }
        json::Value body = json::Value::object();
        if (command == "continue") body.set("allThreadsContinued", true);
        respond(request, true, std::move(body));
        return true;
    }
    if (command == "pause") {
        if (host_) host_->request_pause();
        respond(request, true);
        return true;
    }
    if (command == "disconnect" || command == "terminate") {
        disconnected_ = true;
        if (stop_) resume_ = StepAction::Continue;
        respond(request, true);
        return true;
    }
    return false;
}

StepAction DapSession::on_stop(DebugStop& stop) {
    if (disconnected_) return StepAction::Continue;
    ++stops_;
    stop_ = &stop;
    resume_.reset();
    poll();
    json::Value body = json::Value::object();
    body.set("reason", reason_name(stop.reason()));
    body.set("description", stop.message());
    if (stop.reason() == StopReason::Exception) body.set("text", stop.message());
    body.set("threadId", kThread);
    body.set("allThreadsStopped", true);
    event("stopped", std::move(body));
    while (!resume_) {
        auto msg = receive_();
        if (!msg) {
            disconnected_ = true;
            break;
        }
        if (!handle(*msg)) respond(*msg, false, json::Value::object(), "not available while stopped");
    }
    stop_ = nullptr;
    const StepAction action = resume_.value_or(StepAction::Continue);
    resume_.reset();
    if (disconnected_) {
        detach();
        return StepAction::Continue;
    }
    return action;
}

} // namespace victus::script
