// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/core/hash.hpp"

#include <set>

using namespace victus;

TEST_CASE("hash: base64 RFC 4648 test vectors") {
    auto enc = [](std::string_view t) {
        return victus::base64_encode({reinterpret_cast<const std::uint8_t*>(t.data()), t.size()});
    };
    CHECK(enc("") == "");
    CHECK(enc("f") == "Zg==");
    CHECK(enc("fo") == "Zm8=");
    CHECK(enc("foo") == "Zm9v");
    CHECK(enc("foob") == "Zm9vYg==");
    CHECK(enc("fooba") == "Zm9vYmE=");
    CHECK(enc("foobar") == "Zm9vYmFy");
}

TEST_CASE("hash: fnv1a64 reference values") {
    CHECK(fnv1a64("") == 0xcbf29ce484222325ull);
    CHECK(fnv1a64("a") == 0xaf63dc4c8601ec8cull);
    static_assert(fnv1a64("foobar") == 0x85944171f73967e8ull);
}

TEST_CASE("random: PCG32 matches the reference sequence") {
    // pcg32-demo with seed 42, stream 54 (pcg-c-basic reference output).
    Random rng(42u, 54u);
    const std::uint32_t expected[] = {0xa15c02b7, 0x7b47f409, 0xba1d3330, 0x83d2f293, 0xbfa4784b, 0xcbed606e};
    for (auto e : expected) CHECK(rng.next_u32() == e);
}

TEST_CASE("random: uniform bounds and determinism") {
    Random a(7), b(7);
    for (int i = 0; i < 1000; ++i) {
        const auto v = a.uniform(10);
        CHECK(v < 10);
        CHECK(v == b.uniform(10));
        const float f = a.next_float();
        b.next_float();
        CHECK(f >= 0.0f);
        CHECK(f < 1.0f);
        const int r = a.range(-3, 3);
        b.range(-3, 3);
        CHECK(r >= -3);
        CHECK(r <= 3);
    }
    Random c(1);
    c.restore(a.state(), a.increment());
    CHECK(c.next_u64() == a.next_u64());
}

TEST_CASE("uuid: generate, format and parse") {
    std::set<Uuid> seen;
    for (int i = 0; i < 1000; ++i) {
        const Uuid u = Uuid::generate();
        CHECK(seen.insert(u).second);
        const std::string s = u.to_string();
        CHECK(s.size() == 36);
        CHECK(s[14] == '4');
        Uuid parsed;
        REQUIRE(Uuid::parse(s, parsed));
        CHECK(parsed == u);
    }
    Uuid out;
    CHECK_FALSE(Uuid::parse("not-a-uuid", out));
    CHECK_FALSE(Uuid::parse("zzzzzzzz-0000-0000-0000-000000000000", out));
}
