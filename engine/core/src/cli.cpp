// SPDX-License-Identifier: Apache-2.0
#include "victus/core/cli.hpp"

#include <charconv>
#include <format>

namespace victus::cli {

Parser::Parser(std::string program, std::string summary) : program_(std::move(program)), summary_(std::move(summary)) {}

Parser& Parser::flag(std::string name, std::string help) {
    options_.push_back({std::move(name), std::move(help), {}, true, false});
    return *this;
}

Parser& Parser::option(std::string name, std::string help, std::string default_value) {
    options_.push_back({std::move(name), std::move(help), std::move(default_value), false, false});
    return *this;
}

Parser& Parser::positional(std::string name, std::string help, bool required) {
    positionals_.push_back({std::move(name), std::move(help), {}, false, required});
    return *this;
}

Status Parser::parse(int argc, const char* const* argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return parse(args);
}

Status Parser::parse(const std::vector<std::string>& args) {
    std::size_t positional_index = 0;
    bool only_positionals = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (!only_positionals && a == "--") {
            only_positionals = true;
            continue;
        }
        if (!only_positionals && a.size() > 2 && a.starts_with("--")) {
            std::string name = a.substr(2);
            std::string value;
            bool has_value = false;
            if (const auto eq = name.find('='); eq != std::string::npos) {
                value = name.substr(eq + 1);
                name = name.substr(0, eq);
                has_value = true;
            }
            const Spec* spec = nullptr;
            for (const auto& o : options_)
                if (o.name == name) spec = &o;
            if (!spec) return make_error(ErrorCode::InvalidArgument, std::format("unknown option --{}", name));
            if (spec->is_flag) {
                if (has_value) return make_error(ErrorCode::InvalidArgument, std::format("--{} takes no value", name));
                values_[name] = "1";
            } else {
                if (!has_value) {
                    if (i + 1 >= args.size()) return make_error(ErrorCode::InvalidArgument, std::format("--{} requires a value", name));
                    value = args[++i];
                }
                values_[name] = value;
            }
            continue;
        }
        if (positional_index < positionals_.size()) {
            values_[positionals_[positional_index++].name] = a;
        } else {
            extra_.push_back(a);
        }
    }
    for (std::size_t p = positional_index; p < positionals_.size(); ++p)
        if (positionals_[p].required)
            return make_error(ErrorCode::InvalidArgument, std::format("missing required argument <{}>", positionals_[p].name));
    return {};
}

bool Parser::has(std::string_view name) const { return values_.find(name) != values_.end(); }

std::string Parser::get(std::string_view name) const {
    if (auto it = values_.find(name); it != values_.end()) return it->second;
    for (const auto& o : options_)
        if (o.name == name) return o.default_value;
    return {};
}

double Parser::get_number(std::string_view name, double fallback) const {
    const std::string s = get(name);
    double d = fallback;
    if (s.empty()) return fallback;
    const auto res = std::from_chars(s.data(), s.data() + s.size(), d);
    return res.ec == std::errc() ? d : fallback;
}

std::string Parser::help() const {
    std::string out = std::format("Usage: {}", program_);
    if (!options_.empty()) out += " [options]";
    for (const auto& p : positionals_) out += p.required ? std::format(" <{}>", p.name) : std::format(" [{}]", p.name);
    out += '\n';
    if (!summary_.empty()) out += summary_ + "\n";
    if (!positionals_.empty()) {
        out += "\nArguments:\n";
        for (const auto& p : positionals_) out += std::format("  {:<22} {}\n", p.name, p.help);
    }
    if (!options_.empty()) {
        out += "\nOptions:\n";
        for (const auto& o : options_) {
            const std::string left = o.is_flag ? "--" + o.name : "--" + o.name + " <v>";
            out += std::format("  {:<22} {}", left, o.help);
            if (!o.default_value.empty()) out += std::format(" (default: {})", o.default_value);
            out += '\n';
        }
    }
    return out;
}

} // namespace victus::cli
