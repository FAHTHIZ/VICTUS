// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "victus/core/result.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace victus::cli {

// Declarative command-line parser: `--name value`, `--name=value`, `--flag`, positionals.
class Parser {
public:
    explicit Parser(std::string program, std::string summary = {});
    Parser& flag(std::string name, std::string help);
    Parser& option(std::string name, std::string help, std::string default_value = {});
    Parser& positional(std::string name, std::string help, bool required = false);

    [[nodiscard]] Status parse(int argc, const char* const* argv);
    [[nodiscard]] Status parse(const std::vector<std::string>& args);

    [[nodiscard]] bool has(std::string_view name) const;
    [[nodiscard]] std::string get(std::string_view name) const;
    [[nodiscard]] double get_number(std::string_view name, double fallback) const;
    [[nodiscard]] const std::vector<std::string>& extra() const noexcept { return extra_; }
    [[nodiscard]] std::string help() const;

private:
    struct Spec {
        std::string name, help, default_value;
        bool is_flag{false};
        bool required{false};
    };
    std::string program_, summary_;
    std::vector<Spec> options_;
    std::vector<Spec> positionals_;
    std::map<std::string, std::string, std::less<>> values_;
    std::vector<std::string> extra_;
};

} // namespace victus::cli
