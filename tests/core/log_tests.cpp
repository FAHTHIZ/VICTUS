// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/core/log.hpp"

#include <thread>

using namespace victus;

TEST_CASE("log: ring sink captures records and filters by level") {
    log::Logger logger;
    auto ring = std::make_shared<log::RingSink>(4);
    logger.add_sink(ring);
    logger.set_level(log::Level::Info);
    logger.write(log::Level::Debug, "test", "hidden");
    CHECK_FALSE(logger.enabled(log::Level::Debug));
    for (int i = 0; i < 6; ++i) logger.write(log::Level::Info, "test", std::to_string(i));
    const auto records = ring->snapshot();
    REQUIRE(records.size() == 4);
    CHECK(records.front().message == "2");
    CHECK(records.back().message == "5");
    CHECK(ring->total_written() == 7);
    CHECK(logger.count(log::Level::Info) == 6);
}

TEST_CASE("log: concurrent writes are safe") {
    log::Logger logger;
    auto ring = std::make_shared<log::RingSink>(100000);
    logger.add_sink(ring);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 1000; ++i) logger.write(log::Level::Warn, "mt", "x");
        });
    for (auto& t : threads) t.join();
    CHECK(ring->snapshot().size() == 4000);
}

TEST_CASE("log: level parsing") {
    log::Level l{};
    CHECK(log::parse_level("warn", l));
    CHECK(l == log::Level::Warn);
    CHECK_FALSE(log::parse_level("loud", l));
}
