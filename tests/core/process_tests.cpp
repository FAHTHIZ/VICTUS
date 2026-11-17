// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/core/process.hpp"

using namespace victus;

#if !defined(_WIN32)
// POSIX-only for now: these use /bin/sh and cat. The Windows implementation compiles in CI but is
// not yet exercised by a test (needs a cmd.exe-based echo child).

TEST_CASE("process: line round trip through a child") {
    auto child = process::ChildProcess::spawn({{"cat"}});
    REQUIRE(child);
    auto& c = *child.value();
    CHECK(c.running());
    REQUIRE(c.write_line("hello"));
    REQUIRE(c.write("wor"));
    REQUIRE(c.write("ld\r\nsecond\n"));
    auto a = c.read_line(5000);
    REQUIRE(a);
    REQUIRE(a->has_value());
    CHECK(**a == "hello");
    auto b = c.read_line(5000);
    REQUIRE(b);
    CHECK(b->value() == "world"); // CR stripped
    CHECK(c.read_line(5000)->value() == "second");
    // Nothing more yet: a zero timeout polls.
    auto none = c.read_line(0);
    REQUIRE(none);
    CHECK_FALSE(none->has_value());
    c.close_stdin();
    auto end = c.read_line(5000);
    CHECK_FALSE(end); // EOF
    CHECK(c.wait(5000) == 0);
    CHECK_FALSE(c.running());
}

TEST_CASE("process: large lines are reassembled") {
    auto child = process::ChildProcess::spawn({{"cat"}});
    REQUIRE(child);
    std::string big(3u * 1024u * 1024u, 'x');
    big[12345] = 'y';
    // Write from this thread in pieces while reading (cat echoes as it goes; pipes are ~64 KiB).
    auto& c = *child.value();
    std::string got;
    std::size_t sent = 0;
    while (sent < big.size()) {
        const std::size_t n = std::min<std::size_t>(32768, big.size() - sent);
        REQUIRE(c.write(std::string_view(big).substr(sent, n)));
        sent += n;
        (void)c.read_line(0); // drain into the internal buffer without completing a line
    }
    REQUIRE(c.write("\n"));
    auto line = c.read_line(10000);
    REQUIRE(line);
    REQUIRE(line->has_value());
    CHECK(line->value().size() == big.size());
    CHECK(line->value() == big);
}

TEST_CASE("process: exit codes, partial output and missing programs") {
    auto child = process::ChildProcess::spawn({{"sh", "-c", "printf 'no newline'; exit 3"}});
    REQUIRE(child);
    auto& c = *child.value();
    auto r = c.read_line(5000);
    REQUIRE_FALSE(r);
    CHECK(r.error().message.find("no newline") != std::string::npos);
    CHECK(c.wait(5000) == 3);

    auto missing = process::ChildProcess::spawn({{"victus-definitely-not-a-program"}});
    CHECK_FALSE(missing);
    CHECK_FALSE(process::ChildProcess::spawn({}));
    CHECK(process::find_executable("sh"));
}

TEST_CASE("process: writing to a dead child is an error, not SIGPIPE") {
    auto child = process::ChildProcess::spawn({{"sh", "-c", "exit 0"}});
    REQUIRE(child);
    auto& c = *child.value();
    CHECK(c.wait(5000) == 0);
    std::string chunk(1u << 20, 'z');
    CHECK_FALSE(c.write(chunk));
}

TEST_CASE("process: kill and working directory") {
    auto child = process::ChildProcess::spawn({{"sh", "-c", "pwd; sleep 30"}, "/"});
    REQUIRE(child);
    auto& c = *child.value();
    auto line = c.read_line(5000);
    REQUIRE(line);
    CHECK(line->value() == "/");
    c.kill();
    CHECK_FALSE(c.running());
    CHECK(c.exit_code().value_or(0) == 128 + 9);
}

TEST_CASE("process: line length limit") {
    process::LaunchOptions o;
    o.args = {"sh", "-c", "head -c 5000 /dev/zero | tr '\\0' a; echo"};
    o.max_line_bytes = 1000;
    auto child = process::ChildProcess::spawn(o);
    REQUIRE(child);
    auto r = child.value()->read_line(5000);
    CHECK_FALSE(r);
}
#endif
