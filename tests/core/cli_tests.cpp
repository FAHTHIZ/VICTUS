// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/core/cli.hpp"

using namespace victus;

TEST_CASE("cli: options, flags and positionals") {
    cli::Parser p("victus", "test");
    p.flag("headless", "no window").option("frames", "frame count", "10").positional("project", "path", true);
    REQUIRE(p.parse(std::vector<std::string>{"--headless", "game/project.victus", "--frames=42", "rest"}).ok());
    CHECK(p.has("headless"));
    CHECK(p.get("project") == "game/project.victus");
    CHECK(p.get_number("frames", 0) == 42);
    CHECK(p.extra().size() == 1);
    CHECK(p.help().find("--frames") != std::string::npos);
}

TEST_CASE("cli: defaults and errors") {
    cli::Parser p("victus");
    p.option("frames", "count", "10").positional("project", "path", true);
    CHECK_FALSE(p.parse(std::vector<std::string>{}).ok());
    cli::Parser q("victus");
    q.option("frames", "count", "10");
    REQUIRE(q.parse(std::vector<std::string>{}).ok());
    CHECK(q.get("frames") == "10");
    cli::Parser r("victus");
    r.option("frames", "count");
    CHECK_FALSE(r.parse(std::vector<std::string>{"--unknown"}).ok());
    CHECK_FALSE(r.parse(std::vector<std::string>{"--frames"}).ok());
}
