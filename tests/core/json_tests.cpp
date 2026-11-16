// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/core/json.hpp"

using namespace victus;

TEST_CASE("json: parse and access nested values") {
    auto r = json::parse(R"({"name":"Hero","hp":100,"pos":[1.5,-2,3e2],"tags":{"a":true,"b":null}})");
    REQUIRE(r.ok());
    const json::Value& v = *r;
    CHECK(v["name"].as_string() == "Hero");
    CHECK(v["hp"].as_int() == 100);
    CHECK(v["pos"][1].as_number() == -2.0);
    CHECK(v["pos"][2].as_number() == 300.0);
    CHECK(v["tags"]["a"].as_bool());
    CHECK(v["tags"]["b"].is_null());
    CHECK(v["missing"]["deeper"].is_null());
}

TEST_CASE("json: round trip preserves order and values") {
    json::Value v = json::Value::object();
    v.set("z", 1);
    v.set("a", "text \"quoted\"\n");
    v.set("f", 0.1);
    json::Value arr = json::Value::array();
    arr.push(1);
    arr.push(json::Value::object());
    v.set("arr", arr);
    const std::string text = json::stringify(v);
    auto back = json::parse(text);
    REQUIRE(back.ok());
    CHECK(*back == v);
    CHECK(back->as_object()[0].first == "z");
    CHECK(json::stringify(*back) == text);
}

TEST_CASE("json: unicode escapes decode to UTF-8") {
    auto r = json::parse(R"("\u00e9\ud83d\ude00")");
    REQUIRE(r.ok());
    CHECK(r->as_string() == "\xc3\xa9\xf0\x9f\x98\x80");
}

TEST_CASE("json: malformed input is rejected with location") {
    for (const char* bad : {"{", "[1,]", "{\"a\" 1}", "tru", "\"unterminated", "01x", "{} extra", "[\"\\q\"]", "\"\\ud800\""}) {
        auto r = json::parse(bad);
        CHECK_FALSE(r.ok());
        if (!r.ok()) {
            CHECK(r.error().code == ErrorCode::ParseError);
            CHECK(r.error().message.find("JSON 1:") == 0);
        }
    }
}

TEST_CASE("json: depth and size limits protect against hostile input") {
    std::string deep(1000, '[');
    deep += std::string(1000, ']');
    CHECK_FALSE(json::parse(deep).ok());
    json::ParseLimits limits;
    limits.max_bytes = 4;
    CHECK(json::parse("[1,2,3]", limits).error().code == ErrorCode::LimitExceeded);
}

TEST_CASE("json: copy-on-write isolates copies") {
    json::Value a = json::Value::object();
    a.set("x", 1);
    json::Value b = a;
    b.set("x", 2);
    CHECK(a["x"].as_int() == 1);
    CHECK(b["x"].as_int() == 2);
}

TEST_CASE("json: line comments tolerated") {
    auto r = json::parse("{\n // comment\n \"a\": 1 // trailing\n}");
    REQUIRE(r.ok());
    CHECK((*r)["a"].as_int() == 1);
}
