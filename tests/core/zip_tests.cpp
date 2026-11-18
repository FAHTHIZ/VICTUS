// SPDX-License-Identifier: Apache-2.0
// Stored-only ZIP writer/reader used by diagnostics bundles (victus/core/zip.hpp).
#include "doctest.h"
#include "victus/core/zip.hpp"

#include <string>

using namespace victus;

namespace {
std::span<const std::uint8_t> bytes(std::string_view s) { return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}; }
std::string text(const std::vector<std::uint8_t>& v) { return {v.begin(), v.end()}; }
} // namespace

TEST_CASE("zip: crc32 matches the standard check value") {
    CHECK(zip::crc32(bytes("123456789")) == 0xCBF43926u);
    CHECK(zip::crc32(bytes("")) == 0u);
    // Incremental use gives the same result as one call.
    CHECK(zip::crc32(bytes("6789"), zip::crc32(bytes("12345"))) == 0xCBF43926u);
}

TEST_CASE("zip: round trip keeps names, order and contents") {
    zip::Writer w;
    REQUIRE(w.add("README.txt", std::string_view("hello\n"), 1700000000).ok());
    REQUIRE(w.add("crashes/crash-1-2-victus.txt", std::string_view("reason: test\n"), 1700000000).ok());
    REQUIRE(w.add("empty.txt", std::string_view(""), 0).ok()); // before 1980: clamped, still valid
    std::string big(70000, 'x');
    for (std::size_t i = 0; i < big.size(); i += 97) big[i] = static_cast<char>('a' + i % 26);
    REQUIRE(w.add("logs/big.log", std::string_view(big), 1700000000).ok());
    CHECK(w.entries() == 4);
    CHECK_FALSE(w.add("README.txt", std::string_view("dup"), 0).ok()); // duplicate names refused
    const auto archive = w.finish();
    CHECK(archive.size() > big.size());
    // End of central directory signature, then a zero-length comment.
    REQUIRE(archive.size() >= 22);
    CHECK(archive[archive.size() - 22] == 0x50);
    CHECK(archive[archive.size() - 21] == 0x4b);
    CHECK(archive[archive.size() - 20] == 0x05);
    CHECK(archive[archive.size() - 19] == 0x06);

    auto entries = zip::read(archive);
    REQUIRE(entries.ok());
    REQUIRE(entries->size() == 4);
    CHECK((*entries)[0].name == "README.txt");
    CHECK(text((*entries)[0].data) == "hello\n");
    CHECK((*entries)[1].name == "crashes/crash-1-2-victus.txt");
    CHECK((*entries)[2].data.empty());
    CHECK(text((*entries)[3].data) == big);
}

TEST_CASE("zip: unsafe entry names are refused") {
    zip::Writer w;
    for (const char* bad : {"", "/etc/passwd", "../x", "a/../../x", "C:/x", "a\\b", "a//b", "dir/"}) {
        CAPTURE(bad);
        CHECK_FALSE(w.add(bad, std::string_view("x"), 0).ok());
    }
    CHECK(w.entries() == 0);
}

TEST_CASE("zip: corrupted or truncated archives are rejected") {
    zip::Writer w;
    REQUIRE(w.add("a.txt", std::string_view("some content here"), 1700000000).ok());
    const auto good = w.finish();
    REQUIRE(zip::read(good).ok());

    CHECK_FALSE(zip::read({}).ok());
    for (std::size_t cut : {std::size_t{1}, std::size_t{10}, std::size_t{22}, good.size() / 2}) {
        CAPTURE(cut);
        std::vector<std::uint8_t> t(good.begin(), good.end() - static_cast<std::ptrdiff_t>(cut));
        CHECK_FALSE(zip::read(t).ok());
    }
    // Flip one content byte: the CRC check fails.
    auto flipped = good;
    const std::string_view body = "some content here";
    const auto at = text(flipped).find(body);
    REQUIRE(at != std::string::npos);
    flipped[at] ^= 0x20;
    CHECK_FALSE(zip::read(flipped).ok());
    // Garbage the same size as a real archive.
    std::vector<std::uint8_t> junk(good.size(), 0xAB);
    CHECK_FALSE(zip::read(junk).ok());
}
