// SPDX-License-Identifier: Apache-2.0
// Versioned document migration (ADR-0100).
#include "doctest.h"
#include "victus/core/migration.hpp"

using namespace victus;
using json::Value;

namespace {
Value doc_v(int version) {
    Value d = Value::object();
    d.set("format", "test.doc");
    d.set("version", version);
    Value inner = Value::object();
    inner.set("hp", 10);
    d.set("stats", inner);
    d.set("keep", "unknown field survives");
    return d;
}

migration::Format three() {
    return migration::Format("test.doc", 3,
                             {{2, "double hp",
                               [](Value& d) -> Status {
                                   Value s = d["stats"];
                                   s.set("health", s["health"].as_int() * 2);
                                   d.set("stats", s);
                                   return {};
                               }},
                              {1, "rename hp to health", [](Value& d) -> Status {
                                   Value s = d["stats"];
                                   Value out = Value::object();
                                   out.set("health", s["hp"]);
                                   d.set("stats", out);
                                   return {};
                               }}});
}
} // namespace

TEST_CASE("migration: steps run in order from the stored version and report what ran") {
    const auto f = three();
    Value d = doc_v(1);
    auto r = f.upgrade(d);
    REQUIRE(r);
    CHECK(r->from == 1);
    CHECK(r->to == 3);
    CHECK(r->migrated());
    REQUIRE(r->applied.size() == 2);
    CHECK(r->applied[0] == "1 -> 2: rename hp to health");
    CHECK(d["version"].as_int() == 3);
    CHECK(d["stats"]["health"].as_int() == 20);
    CHECK(d["stats"].find("hp") == nullptr);
    CHECK(d["keep"].as_string_or("") == "unknown field survives");

    Value d2 = doc_v(2);
    Value s = Value::object();
    s.set("health", 7);
    d2.set("stats", s);
    auto r2 = f.upgrade(d2);
    REQUIRE(r2);
    CHECK(r2->applied.size() == 1);
    CHECK(d2["stats"]["health"].as_int() == 14);

    Value current = doc_v(3);
    const Value before = current;
    auto r3 = f.upgrade(current);
    REQUIRE(r3);
    CHECK_FALSE(r3->migrated());
    CHECK(current == before);
}

TEST_CASE("migration: newer, missing and invalid versions are refused; failures leave the document unchanged") {
    const auto f = three();
    Value newer = doc_v(4);
    auto r = f.upgrade(newer);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::VersionMismatch);
    CHECK(r.error().message.find("newer VICTUS") != std::string::npos);
    for (const Value& bad : {Value(Value::object()), [] {
             Value d = doc_v(1);
             d.set("version", 1.5);
             return d;
         }(), [] {
             Value d = doc_v(1);
             d.set("version", "2");
             return d;
         }(), doc_v(0)}) {
        Value d = bad;
        CHECK_FALSE(f.upgrade(d));
        CHECK(d == bad);
    }

    // A failing step: the caller's document (and values shared with it) are untouched.
    migration::Format failing("test.doc", 3,
                              {{1, "mutate then succeed", [](Value& d) -> Status {
                                    Value s = d["stats"];
                                    s.set("hp", 999);
                                    d.set("stats", s);
                                    return {};
                                }},
                               {2, "fail", [](Value&) -> Status { return make_error(ErrorCode::ParseError, "bad data"); }}});
    Value d = doc_v(1);
    const Value original = d;
    auto fr = failing.upgrade(d);
    REQUIRE_FALSE(fr);
    CHECK(fr.error().message.find("from version 2 (fail)") != std::string::npos);
    CHECK(d == original);
    CHECK(d["stats"]["hp"].as_int() == 10);
}

TEST_CASE("migration: an incomplete chain cannot upgrade but still reads current documents") {
#ifdef NDEBUG
    migration::Format gap("test.doc", 3, {{1, "only one", [](Value&) -> Status { return {}; }}});
    Value old = doc_v(1);
    CHECK_FALSE(gap.upgrade(old));
    Value cur = doc_v(3);
    CHECK(gap.upgrade(cur));
#endif
    Value c = Value::object();
    CHECK(migration::component_version(c) == 1);
    c.set("$version", 4);
    CHECK(migration::component_version(c) == 4);
    c.set("$version", -2);
    CHECK(migration::component_version(c) == 1);
}
