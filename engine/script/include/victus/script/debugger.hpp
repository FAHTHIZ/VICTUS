// SPDX-License-Identifier: Apache-2.0
// Luau source debugger of a ScriptHost (ADR-0099): line breakpoints (with conditions, hit counts
// and log messages), stepping, pause requests and stops on uncaught errors, with call stacks,
// locals, upvalues, table contents and expression evaluation while stopped.
//
// Stops are synchronous: the handler runs inside the script call that stopped and returns how to
// continue, so the game is frozen while it runs. Front ends (the `victus debug-adapter` Debug
// Adapter Protocol server, tests) inspect the stop through DebugStop and block in the handler
// until the user resumes.
#pragma once
#include "victus/core/result.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace victus::script {

// One breakpoint of a source file (the debugger's request).
struct SourceBreakpoint {
    int line{0};                // 1-based; moved forward to the next line with code
    std::string condition;      // Luau expression; stops only when it is truthy (empty = always)
    std::string hit_condition;  // "N"/"==N", ">N", ">=N", "<N", "<=N" or "%N" on the hit count
    std::string log_message;    // non-empty: a logpoint that prints this ({expr} interpolated)
};

// Where a breakpoint ended up.
struct BreakpointState {
    int requested_line{0};
    int line{0};           // actual line; equals requested_line while the script is not loaded
    bool verified{false};  // false until the script is loaded (or when no code follows the line)
    std::string message;   // why it is not verified
};

enum class StopReason { Breakpoint, Step, Pause, Exception };

enum class StepAction { Continue, StepIn, StepOver, StepOut };

struct DebugFrame {
    std::string function; // "update", "<main>", "<anonymous>"
    std::string source;   // module path relative to the assets folder ("scripts/player.luau")
    int line{0};
};

struct DebugVariable {
    std::string name;
    std::string value; // display text
    std::string type;  // Luau type name
    int reference{0};  // nonzero: a table whose entries children(reference) lists
};

// The state of a stopped script. Valid only while the handler runs.
class DebugStop {
public:
    [[nodiscard]] StopReason reason() const noexcept;
    // Exception: the error message; otherwise a description ("breakpoint at player.luau:12").
    [[nodiscard]] const std::string& message() const noexcept;
    // Luau frames, innermost first (C functions are omitted).
    [[nodiscard]] const std::vector<DebugFrame>& frames() const noexcept;
    [[nodiscard]] std::vector<DebugVariable> locals(std::size_t frame);
    [[nodiscard]] std::vector<DebugVariable> upvalues(std::size_t frame);
    // Entries of a table returned with a nonzero reference (sorted, at most 500).
    [[nodiscard]] std::vector<DebugVariable> children(int reference);
    // Evaluates a Luau expression (or statement) with the frame's locals and upvalues in scope.
    // Assignments to locals are not written back; use set_local for that.
    [[nodiscard]] Result<DebugVariable> evaluate(std::string_view expression, std::size_t frame = 0);
    // Assigns the value of `expression` to a local or upvalue of the frame.
    [[nodiscard]] Result<DebugVariable> set_variable(std::size_t frame, std::string_view name,
                                                     std::string_view expression);

    struct Impl;
    explicit DebugStop(Impl& impl) noexcept : impl_(&impl) {}

private:
    Impl* impl_;
};

// Called when a script stops; returns how to continue.
using DebugHandler = std::function<StepAction(DebugStop& stop)>;
// Logpoint output (one message per call, without a trailing newline).
using DebugOutput = std::function<void(std::string_view text)>;

struct DebugStats {
    std::uint64_t stops{0};
    std::uint64_t breakpoint_hits{0}; // including those whose condition was false
    std::uint64_t log_messages{0};
};

} // namespace victus::script
