// SPDX-License-Identifier: Apache-2.0
// Luau source debugger (ADR-0099).
//
// Breakpoints patch BREAK instructions into the compiled functions (lua_breakpoint), so they cost
// nothing until hit. Stepping and pause requests run the next top-level script call in Luau's
// single-step interpreter (lua_singlestep + the debugstep callback), which reports a line event
// whenever the (function, line) of a stack depth changes. A step started from a breakpoint in a
// call that is not single-stepping cannot switch interpreters mid-call, so it temporarily
// "floods" every line of every module with breakpoints until the call returns.
#include "script_internal.hpp"

#include <charconv>

namespace victus::script {

namespace {

constexpr std::size_t kMaxChildren = 500;
constexpr std::size_t kMaxValueText = 240;

std::string source_path(const char* source) {
    if (!source) return {};
    if (*source == '@' || *source == '=') return source + 1;
    return source;
}

std::string quote(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (out.size() >= kMaxValueText) {
            out += "...";
            break;
        }
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default: out += c;
        }
    }
    return out + "\"";
}

std::string number_text(double v) {
    char buf[32];
    auto [end, ec] = std::to_chars(buf, buf + sizeof buf, v);
    return ec == std::errc{} ? std::string(buf, end) : std::string("nan");
}

// Text of a key or value without calling metamethods.
std::string plain_text(lua_State* L, int idx) {
    switch (lua_type(L, idx)) {
    case LUA_TNIL: return "nil";
    case LUA_TBOOLEAN: return lua_toboolean(L, idx) ? "true" : "false";
    case LUA_TNUMBER: return number_text(lua_tonumber(L, idx));
    case LUA_TSTRING: {
        std::size_t n = 0;
        const char* s = lua_tolstring(L, idx, &n);
        return quote(std::string_view(s, n));
    }
    case LUA_TVECTOR: {
        const float* v = lua_tovector(L, idx);
        return std::format("vector({}, {}, {})", number_text(v[0]), number_text(v[1]), number_text(v[2]));
    }
    default: return luaL_typename(L, idx);
    }
}

// Calls __tostring on userdata in protected mode.
int tostring_thunk(lua_State* L) {
    luaL_tolstring(L, 1, nullptr);
    return 1;
}

bool hit_condition_met(std::string_view cond, std::uint64_t hits) {
    std::size_t i = 0;
    while (i < cond.size() && cond[i] == ' ') ++i;
    std::string_view op;
    for (std::string_view candidate : {">=", "<=", "==", ">", "<", "%"})
        if (cond.substr(i).starts_with(candidate)) {
            op = candidate;
            i += candidate.size();
            break;
        }
    while (i < cond.size() && cond[i] == ' ') ++i;
    std::uint64_t n = 0;
    const auto [ptr, ec] = std::from_chars(cond.data() + i, cond.data() + cond.size(), n);
    if (ec != std::errc{} || ptr == cond.data() + i) return true; // unparsable: always stop
    if (op == ">=") return hits >= n;
    if (op == "<=") return hits <= n;
    if (op == ">") return hits > n;
    if (op == "<") return hits < n;
    if (op == "%") return n != 0 && hits % n == 0;
    return hits == n;
}

} // namespace

// ---- stop state ---------------------------------------------------------------------------------

struct DebugStop::Impl {
    ScriptHost::Impl* host{nullptr};
    lua_State* L{nullptr};
    StopReason reason{StopReason::Pause};
    std::string message;
    std::vector<DebugFrame> frames;
    std::vector<int> levels; // lua_getinfo level of each frame
    std::vector<int> refs;   // tables handed out as references (freed when the stop ends)

    ~Impl() {
        for (int r : refs) lua_unref(L, r);
    }

    DebugVariable describe(std::string name, int idx) {
        idx = lua_absindex(L, idx);
        DebugVariable v;
        v.name = std::move(name);
        v.type = luaL_typename(L, idx);
        switch (lua_type(L, idx)) {
        case LUA_TTABLE: {
            // Preview: up to four entries, then the count.
            std::string preview = "{";
            std::size_t count = 0;
            lua_pushnil(L);
            while (lua_next(L, idx) != 0) {
                if (count < 4) {
                    if (count) preview += ", ";
                    if (lua_type(L, -2) == LUA_TNUMBER && lua_tonumber(L, -2) == static_cast<double>(count + 1))
                        preview += plain_text(L, -1);
                    else if (lua_type(L, -2) == LUA_TSTRING)
                        preview += std::string(lua_tostring(L, -2)) + " = " + plain_text(L, -1);
                    else
                        preview += "[" + plain_text(L, -2) + "] = " + plain_text(L, -1);
                }
                ++count;
                lua_pop(L, 1);
            }
            if (count > 4) preview += std::format(", ... ({} entries)", count);
            v.value = preview + "}";
            lua_pushvalue(L, idx);
            refs.push_back(lua_ref(L, -1));
            lua_pop(L, 1);
            v.reference = static_cast<int>(refs.size());
            break;
        }
        case LUA_TUSERDATA:
        case LUA_TLIGHTUSERDATA:
            v.value = "userdata";
            if (luaL_getmetafield(L, idx, "__tostring")) {
                lua_pop(L, 1);
                lua_pushcfunction(L, &tostring_thunk, "tostring");
                lua_pushvalue(L, idx);
                if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_isstring(L, -1)) v.value = lua_tostring(L, -1);
                lua_pop(L, 1);
            }
            break;
        case LUA_TFUNCTION: {
            lua_Debug ar{};
            lua_pushvalue(L, idx);
            if (lua_getinfo(L, -1, "sn", &ar)) {
                const std::string fname = ar.name ? ar.name : "";
                if (ar.what && std::string_view(ar.what) == "C") v.value = std::format("function {} [C]", fname);
                else v.value = std::format("function {} ({}:{})", fname, source_path(ar.source), ar.linedefined);
            } else {
                v.value = "function";
            }
            lua_pop(L, 1);
            break;
        }
        default: v.value = plain_text(L, idx);
        }
        return v;
    }

    // Pushes the environment for evaluating code in a frame: the frame's upvalues and locals
    // (later locals shadow earlier ones) over the function's globals.
    void push_frame_env(std::size_t frame) {
        lua_newtable(L);
        const int env = lua_gettop(L);
        if (frame < levels.size()) {
            const int level = levels[frame];
            lua_Debug ar{};
            if (lua_getinfo(L, level, "f", &ar)) {
                const int fn = lua_gettop(L);
                for (int n = 1;; ++n) {
                    const char* name = lua_getupvalue(L, fn, n);
                    if (!name) break;
                    if (*name) lua_setfield(L, env, name);
                    else lua_pop(L, 1);
                }
                lua_newtable(L); // metatable: unknown names read the function's globals
                lua_getfenv(L, fn);
                lua_setfield(L, -2, "__index");
                lua_setmetatable(L, env);
                lua_pop(L, 1); // fn
            }
            for (int n = 1;; ++n) {
                const char* name = lua_getlocal(L, level, n);
                if (!name) break;
                if (*name && *name != '(') lua_setfield(L, env, name);
                else lua_pop(L, 1);
            }
        }
    }

    // Evaluates `expression` and leaves its first result on the stack, or returns an error.
    Status eval_push(std::string_view expression, std::size_t frame) {
        lua_rawcheckstack(L, 8);
        const int top = lua_gettop(L);
        push_frame_env(frame);
        const int env = lua_gettop(L);
        lua_CompileOptions opts{};
        opts.optimizationLevel = 1;
        opts.debugLevel = 1;
        std::string error;
        bool loaded = false;
        for (const std::string& code : {"return " + std::string(expression), std::string(expression)}) {
            std::size_t size = 0;
            char* bc = luau_compile(code.data(), code.size(), &opts, &size);
            if (!bc) return make_error(ErrorCode::OutOfMemory, "compiler out of memory");
            const int st = luau_load(L, "=debug", bc, size, env);
            std::free(bc);
            if (st == 0) {
                loaded = true;
                break;
            }
            if (error.empty()) error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "syntax error";
            lua_pop(L, 1);
        }
        if (!loaded) {
            lua_settop(L, top);
            return make_error(ErrorCode::ScriptError, error);
        }
        const double saved = host->deadline;
        host->deadline = host->options.call_budget_seconds > 0 ? now_seconds() + host->options.call_budget_seconds : 0;
        const int st = lua_pcall(L, 0, 1, 0);
        host->deadline = saved;
        if (st != LUA_OK) {
            std::string msg = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
            lua_settop(L, top);
            return make_error(ErrorCode::ScriptError, std::move(msg));
        }
        lua_remove(L, env);
        return {};
    }
};

StopReason DebugStop::reason() const noexcept { return impl_->reason; }
const std::string& DebugStop::message() const noexcept { return impl_->message; }
const std::vector<DebugFrame>& DebugStop::frames() const noexcept { return impl_->frames; }

std::vector<DebugVariable> DebugStop::locals(std::size_t frame) {
    std::vector<DebugVariable> out;
    if (frame >= impl_->levels.size()) return out;
    lua_State* L = impl_->L;
    lua_rawcheckstack(L, 4);
    for (int n = 1;; ++n) {
        const char* name = lua_getlocal(L, impl_->levels[frame], n);
        if (!name) break;
        if (*name && *name != '(') {
            // A later local of the same name shadows the earlier one.
            std::erase_if(out, [&](const DebugVariable& v) { return v.name == name; });
            out.push_back(impl_->describe(name, -1));
        }
        lua_pop(L, 1);
    }
    return out;
}

std::vector<DebugVariable> DebugStop::upvalues(std::size_t frame) {
    std::vector<DebugVariable> out;
    if (frame >= impl_->levels.size()) return out;
    lua_State* L = impl_->L;
    lua_rawcheckstack(L, 4);
    lua_Debug ar{};
    if (!lua_getinfo(L, impl_->levels[frame], "f", &ar)) return out;
    for (int n = 1;; ++n) {
        const char* name = lua_getupvalue(L, -1, n);
        if (!name) break;
        if (*name) out.push_back(impl_->describe(name, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return out;
}

std::vector<DebugVariable> DebugStop::children(int reference) {
    std::vector<DebugVariable> out;
    if (reference <= 0 || static_cast<std::size_t>(reference) > impl_->refs.size()) return out;
    lua_State* L = impl_->L;
    lua_rawcheckstack(L, 6);
    lua_getref(L, impl_->refs[static_cast<std::size_t>(reference - 1)]);
    const int t = lua_gettop(L);
    struct Entry {
        bool numeric;
        double number;
        DebugVariable var;
    };
    std::vector<Entry> entries;
    std::size_t total = 0;
    lua_pushnil(L);
    while (lua_next(L, t) != 0) {
        if (++total <= kMaxChildren) {
            Entry e{lua_type(L, -2) == LUA_TNUMBER, lua_type(L, -2) == LUA_TNUMBER ? lua_tonumber(L, -2) : 0.0, {}};
            std::string name = lua_type(L, -2) == LUA_TSTRING ? std::string(lua_tostring(L, -2))
                                                              : "[" + plain_text(L, -2) + "]";
            e.var = impl_->describe(std::move(name), -1);
            entries.push_back(std::move(e));
        }
        lua_pop(L, 1);
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.numeric != b.numeric) return a.numeric;
        if (a.numeric) return a.number < b.number;
        return a.var.name < b.var.name;
    });
    for (auto& e : entries) out.push_back(std::move(e.var));
    if (total > kMaxChildren)
        out.push_back({"...", std::format("{} more entries", total - kMaxChildren), "note", 0});
    if (lua_getmetatable(L, t)) {
        out.push_back(impl_->describe("[metatable]", -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return out;
}

Result<DebugVariable> DebugStop::evaluate(std::string_view expression, std::size_t frame) {
    if (auto st = impl_->eval_push(expression, frame); !st) return st.error();
    DebugVariable v = impl_->describe(std::string(expression), -1);
    lua_pop(impl_->L, 1);
    return v;
}

Result<DebugVariable> DebugStop::set_variable(std::size_t frame, std::string_view name, std::string_view expression) {
    if (frame >= impl_->levels.size()) return make_error(ErrorCode::InvalidArgument, "no such frame");
    lua_State* L = impl_->L;
    const int level = impl_->levels[frame];
    // The innermost local of that name, else an upvalue.
    int local = 0;
    for (int n = 1;; ++n) {
        const char* lname = lua_getlocal(L, level, n);
        if (!lname) break;
        if (name == lname) local = n;
        lua_pop(L, 1);
    }
    int upvalue = 0;
    lua_Debug ar{};
    if (!local && lua_getinfo(L, level, "f", &ar)) {
        for (int n = 1;; ++n) {
            const char* uname = lua_getupvalue(L, -1, n);
            if (!uname) break;
            lua_pop(L, 1);
            if (name == uname) {
                upvalue = n;
                break;
            }
        }
        lua_pop(L, 1);
    }
    if (!local && !upvalue) return make_error(ErrorCode::NotFound, std::format("no variable '{}' in this frame", name));
    if (auto st = impl_->eval_push(expression, frame); !st) return st.error();
    DebugVariable v = impl_->describe(std::string(name), -1);
    if (local) {
        lua_setlocal(L, level, local);
    } else {
        lua_getinfo(L, level, "f", &ar);
        lua_insert(L, -2);
        lua_setupvalue(L, -2, upvalue);
        lua_pop(L, 1);
    }
    return v;
}

// ---- hooks --------------------------------------------------------------------------------------

namespace {

using Impl = ScriptHost::Impl;

// Runs the handler for a stop on thread L and applies the action it returns.
void stop(Impl& h, lua_State* L, StopReason reason, std::string message) {
    auto& d = h.dbg;
    if (!d.handler || d.in_handler) return;
    d.in_handler = true;
    ++d.stats.stops;
    const double paused_at = now_seconds();
    DebugStop::Impl s;
    s.host = &h;
    s.L = L;
    s.reason = reason;
    s.message = std::move(message);
    lua_Debug ar{};
    for (int level = 0; lua_getinfo(L, level, "sln", &ar); ++level) {
        if (ar.what && std::string_view(ar.what) == "C") continue;
        DebugFrame f;
        f.source = source_path(ar.source);
        f.line = ar.currentline;
        if (ar.name) f.function = ar.name;
        else if (ar.what && std::string_view(ar.what) == "main") f.function = "<main>";
        else f.function = "<anonymous>";
        s.frames.push_back(std::move(f));
        s.levels.push_back(level);
    }
    if (s.message.empty() && !s.frames.empty())
        s.message = std::format("{} at {}:{}", reason == StopReason::Breakpoint ? "breakpoint" : reason == StopReason::Step ? "step" : "paused",
                                s.frames.front().source, s.frames.front().line);
    DebugStop view(s);
    StepAction action = StepAction::Continue;
    {
        const int top = lua_gettop(L);
        action = d.handler(view);
        lua_settop(L, top);
    }
    const int line = s.frames.empty() ? 0 : s.frames.front().line;
    d.in_handler = false;
    if (h.deadline > 0) h.deadline += now_seconds() - paused_at; // time spent stopped is not the script's
    d.step = action;
    if (action == StepAction::Continue) {
        d.step_thread = nullptr;
        return;
    }
    if (reason == StopReason::Exception) { // the call is unwinding: stop at the next line that runs
        d.step = StepAction::StepIn;
        d.step_thread = nullptr;
        d.step_line = -1;
        return;
    }
    d.step_thread = L;
    d.step_depth = lua_stackdepth(L);
    d.step_line = line;
}

bool step_done(const Impl::Debugger& d, lua_State* L, int depth, int line) {
    switch (d.step) {
    case StepAction::Continue: return false;
    case StepAction::StepIn: return d.step_line < 0 || L != d.step_thread || line != d.step_line || depth != d.step_depth;
    case StepAction::StepOver:
        return L == d.step_thread && (depth < d.step_depth || (depth == d.step_depth && line != d.step_line));
    case StepAction::StepOut: return L == d.step_thread && depth < d.step_depth;
    }
    return false;
}

// Evaluates a breakpoint's condition, hit count and log message; true when it should stop.
bool breakpoint_stops(Impl& h, lua_State* L, Impl::DebugBreakpoint& bp, std::string& error) {
    auto& d = h.dbg;
    ++d.stats.breakpoint_hits;
    if (!bp.request.condition.empty()) {
        DebugStop::Impl s;
        s.host = &h;
        s.L = L;
        s.levels.push_back(0);
        d.in_handler = true;
        auto st = s.eval_push(bp.request.condition, 0);
        d.in_handler = false;
        if (!st) {
            error = std::format("breakpoint condition '{}' failed: {}", bp.request.condition, st.error().message);
            return true;
        }
        const bool truthy = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        if (!truthy) return false;
    }
    ++bp.hits;
    if (!bp.request.hit_condition.empty() && !hit_condition_met(bp.request.hit_condition, bp.hits)) return false;
    if (bp.request.log_message.empty()) return true;
    // Logpoint: interpolate {expression} parts and continue.
    std::string out;
    const std::string& msg = bp.request.log_message;
    DebugStop::Impl s;
    s.host = &h;
    s.L = L;
    s.levels.push_back(0);
    d.in_handler = true;
    for (std::size_t i = 0; i < msg.size(); ++i) {
        if (msg[i] != '{') {
            out += msg[i];
            continue;
        }
        const std::size_t end = msg.find('}', i);
        if (end == std::string::npos) {
            out += msg.substr(i);
            break;
        }
        const std::string expr = msg.substr(i + 1, end - i - 1);
        if (auto st = s.eval_push(expr, 0); !st) {
            out += "{" + st.error().message + "}";
        } else {
            out += lua_type(L, -1) == LUA_TSTRING ? std::string(lua_tostring(L, -1)) : s.describe({}, -1).value;
            lua_pop(L, 1);
        }
        i = end;
    }
    d.in_handler = false;
    ++d.stats.log_messages;
    if (d.output) d.output(out);
    else VICTUS_LOG_INFO("script", "{}", out);
    return false;
}

// A line starts executing on L.
void line_event(Impl& h, lua_State* L, int line) {
    auto& d = h.dbg;
    if (d.in_handler || !d.handler) return;
    const int depth = lua_stackdepth(L);
    if (d.pause.exchange(false)) {
        stop(h, L, StopReason::Pause, {});
        return;
    }
    lua_Debug ar{};
    lua_getinfo(L, 0, "s", &ar);
    if (auto it = d.breakpoints.find(source_path(ar.source)); it != d.breakpoints.end()) {
        for (auto& bp : it->second) {
            if (!bp.verified || bp.line != line) continue;
            std::string error;
            if (breakpoint_stops(h, L, bp, error)) {
                stop(h, L, StopReason::Breakpoint, std::move(error));
                return;
            }
        }
    }
    if (step_done(d, L, depth, line)) {
        d.step = StepAction::Continue;
        stop(h, L, StopReason::Step, {});
    }
}

// Single step: before each instruction. Reports a line event when the line of the current frame
// changes (or a new frame starts).
void on_step(lua_State* L, lua_Debug* ar) {
    Impl& h = host_of(L);
    auto& d = h.dbg;
    if (d.in_handler) return;
    if (d.lines_thread != L) {
        d.lines_thread = L;
        d.lines.clear();
    }
    lua_Debug info{};
    lua_getinfo(L, 0, "p", &info);
    const auto depth = static_cast<std::size_t>(lua_stackdepth(L));
    if (d.lines.size() != depth + 1) d.lines.resize(depth + 1, {-1, -1});
    const std::pair<int, int> at{info.protoid, ar->currentline};
    if (d.lines[depth] == at) return;
    d.lines[depth] = at;
    line_event(h, L, ar->currentline);
}

// A BREAK instruction: a user breakpoint or a flooded line.
void on_break(lua_State* L, lua_Debug* ar) {
    Impl& h = host_of(L);
    if (h.dbg.singlestep) {
        on_step(L, ar); // the single-step callback skips BREAK, so report the line here
        return;
    }
    line_event(h, L, ar->currentline);
}

void set_lines(lua_State* L, int chunk, int lines, bool enable) {
    lua_rawcheckstack(L, 2);
    lua_getref(L, chunk);
    for (int line = 1; line <= lines;) {
        const int t = lua_breakpoint(L, -1, line, enable ? 1 : 0);
        if (t < 0) break;
        line = t + 1;
    }
    lua_pop(L, 1);
}

// (Re)installs the user breakpoints of one module.
void apply_breakpoints(Impl& h, lua_State* L, const std::string& path, int chunk) {
    auto it = h.dbg.breakpoints.find(path);
    if (it == h.dbg.breakpoints.end() || chunk == LUA_NOREF) return;
    lua_rawcheckstack(L, 2);
    lua_getref(L, chunk);
    for (auto& bp : it->second) {
        const int t = lua_breakpoint(L, -1, bp.request.line, 1);
        bp.verified = t > 0;
        bp.line = t > 0 ? t : bp.request.line;
        bp.message = t > 0 ? std::string() : "no code at or after this line";
    }
    lua_pop(L, 1);
}

void unflood(Impl& h, lua_State* L) {
    if (!h.dbg.flooded) return;
    h.dbg.flooded = false;
    for (auto& [path, m] : h.modules) {
        if (m.chunk == LUA_NOREF) continue;
        set_lines(L, m.chunk, m.lines, false);
        apply_breakpoints(h, L, path, m.chunk);
    }
}

void flood(Impl& h, lua_State* L) {
    if (h.dbg.flooded) return;
    h.dbg.flooded = true;
    for (auto& [path, m] : h.modules)
        if (m.chunk != LUA_NOREF) set_lines(L, m.chunk, m.lines, true);
}

} // namespace

void ScriptHost::Impl::debug_begin_call(lua_State* T) {
    auto& d = dbg;
    d.singlestep = d.handler && (d.pause.load() || d.step != StepAction::Continue);
    lua_singlestep(T, d.singlestep ? 1 : 0);
    d.lines_thread = nullptr;
    d.lines.clear();
}

void ScriptHost::Impl::debug_end_call(lua_State* T) {
    auto& d = dbg;
    unflood(*this, T);
    if (d.singlestep) lua_singlestep(T, 0);
    d.singlestep = false;
    // A step that ran off the end of the call stops at the next line any script runs.
    if (d.step != StepAction::Continue) {
        d.step = StepAction::StepIn;
        d.step_thread = nullptr;
        d.step_line = -1;
    }
}

void ScriptHost::Impl::debug_module_loaded(const std::string& path, int chunk, int /*lines*/) {
    apply_breakpoints(*this, L, path, chunk);
}

int ScriptHost::Impl::debug_error_handler(lua_State* L) {
    Impl& h = host_of(L);
    if (h.dbg.handler && !h.dbg.in_handler) {
        const char* msg = lua_tostring(L, 1);
        stop(h, L, StopReason::Exception, msg ? msg : "error");
    }
    lua_settop(L, 1);
    return 1;
}

// Called from the hooks after a stop's action: a step out of a call that is not single-stepping
// needs every line to report.
namespace {
void after_stop(Impl& h, lua_State* L) {
    if (h.dbg.step != StepAction::Continue && h.dbg.step_thread && !h.dbg.singlestep) flood(h, L);
    if (h.dbg.step == StepAction::Continue && h.dbg.flooded) unflood(h, L);
}

void on_break_hook(lua_State* L, lua_Debug* ar) {
    on_break(L, ar);
    after_stop(host_of(L), L);
}
} // namespace

// ---- ScriptHost API -----------------------------------------------------------------------------

void ScriptHost::set_debug_handler(DebugHandler handler, DebugOutput output) {
    auto& h = *impl_;
    h.dbg.handler = std::move(handler);
    h.dbg.output = std::move(output);
    if (!h.options.debugger) return;
    lua_Callbacks* cb = lua_callbacks(h.L);
    cb->debugbreak = h.dbg.handler ? &on_break_hook : nullptr;
    cb->debugstep = h.dbg.handler ? &on_step : nullptr;
    if (!h.dbg.handler) {
        h.dbg.step = StepAction::Continue;
        h.dbg.pause = false;
    }
}

std::vector<BreakpointState> ScriptHost::set_breakpoints(std::string_view path, std::vector<SourceBreakpoint> breakpoints) {
    auto& h = *impl_;
    const std::string key(path);
    // Remove the old patches first (lines shared by old and new breakpoints are set again below).
    auto mod = h.modules.find(key);
    const int chunk = mod != h.modules.end() ? mod->second.chunk : LUA_NOREF;
    if (auto old = h.dbg.breakpoints.find(key); old != h.dbg.breakpoints.end() && chunk != LUA_NOREF) {
        lua_getref(h.L, chunk);
        for (const auto& bp : old->second)
            if (bp.verified) lua_breakpoint(h.L, -1, bp.line, 0);
        lua_pop(h.L, 1);
    }
    auto& list = h.dbg.breakpoints[key];
    list.clear();
    for (auto& b : breakpoints) {
        Impl::DebugBreakpoint bp;
        bp.line = b.line;
        bp.request = std::move(b);
        bp.message = h.options.debugger ? "the script is not loaded yet" : "the debugger is off (HostOptions::debugger)";
        list.push_back(std::move(bp));
    }
    if (h.options.debugger) apply_breakpoints(h, h.L, key, chunk);
    if (list.empty()) h.dbg.breakpoints.erase(key);
    return this->breakpoints(path);
}

std::vector<BreakpointState> ScriptHost::breakpoints(std::string_view path) const {
    std::vector<BreakpointState> out;
    auto it = impl_->dbg.breakpoints.find(std::string(path));
    if (it == impl_->dbg.breakpoints.end()) return out;
    for (const auto& bp : it->second) out.push_back({bp.request.line, bp.line, bp.verified, bp.message});
    return out;
}

void ScriptHost::set_break_on_error(bool enabled) { impl_->dbg.break_on_error = enabled; }

void ScriptHost::request_pause() { impl_->dbg.pause = true; }

DebugStats ScriptHost::debug_stats() const { return impl_->dbg.stats; }

} // namespace victus::script
