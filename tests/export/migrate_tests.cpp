// SPDX-License-Identifier: Apache-2.0
// `victus migrate` library (victus/player/migrate.hpp): scan a project for files written by an older
// VICTUS, and with write=true save them in the current format keeping the originals (ADR-0100).
#include "doctest.h"
#include "victus/core/fs.hpp"
#include "victus/player/migrate.hpp"
#include "victus/project/project.hpp"
#include "victus/scene/scene.hpp"

#include <algorithm>
#include <filesystem>

using namespace victus;
using json::Value;

namespace {

struct Beacon {
    float range{1.0f};
};

void register_beacon() {
    static bool done = false;
    if (done) return;
    done = true;
    auto& reg = scene::SerializerRegistry::instance();
    reg.add<Beacon>(
        "MigrateTestBeacon",
        [](const Beacon& b) {
            Value v = Value::object();
            v.set("range", b.range);
            return v;
        },
        [](Beacon& b, const Value& v) -> Status {
            b.range = v["range"].as_float(1.0f);
            return {};
        });
    REQUIRE(reg.set_version("MigrateTestBeacon", 2, {{1, "range doubles", [](Value& c) -> Status {
                                                          c.set("range", c["range"].as_number() * 2.0);
                                                          return {};
                                                      }}}));
}

std::string old_scene(const char* component_version_json) {
    return std::string(R"({"format":"victus.scene","version":1,"name":"Old","entities":[{"uuid":"6f1c2a4e-0000-4000-8000-0000000000aa","name":"B","components":{"MigrateTestBeacon":{"range":3)") +
           component_version_json + "}}}]}";
}

struct TempDir {
    fs::Path path;
    explicit TempDir(const char* name) {
        path = std::filesystem::temp_directory_path() / (std::string(name) + "_" + Uuid::generate().to_string().substr(0, 8));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

} // namespace

TEST_CASE("migrate: a fresh project needs nothing; old files are reported, then written with originals kept") {
    register_beacon();
    TempDir tmp("victus_migrate");
    const fs::Path dir = tmp.path / "game";
    REQUIRE(project::Project::create(dir, "Migrate", "3d").ok());

    auto clean = player::migrate_project(dir, false);
    REQUIRE(clean);
    CHECK(clean->scanned >= 2);
    CHECK(clean->entries.empty());

    const std::string original = old_scene("");
    REQUIRE(fs::write_text_atomic(dir / "scenes" / "old.vscene", original).ok());
    REQUIRE(fs::write_text_atomic(dir / "scenes" / "future.vscene", R"({"format":"victus.scene","version":99,"entities":[]})").ok());
    std::filesystem::create_directories(dir / ".victus");
    REQUIRE(fs::write_text_atomic(dir / ".victus" / "ignored.vscene", "{ not json").ok());

    auto report = player::migrate_project(dir, false);
    REQUIRE(report);
    CHECK(report->pending() == 1);
    CHECK(report->failures() == 1);
    const auto old_it = std::find_if(report->entries.begin(), report->entries.end(), [](const auto& e) { return e.file == "scenes/old.vscene"; });
    REQUIRE(old_it != report->entries.end());
    CHECK_FALSE(old_it->written);
    REQUIRE(old_it->notes.size() == 1);
    CHECK(old_it->notes[0] == "MigrateTestBeacon 1 -> 2: range doubles");
    const auto future_it = std::find_if(report->entries.begin(), report->entries.end(), [](const auto& e) { return e.file == "scenes/future.vscene"; });
    REQUIRE(future_it != report->entries.end());
    CHECK(future_it->error.find("newer VICTUS") != std::string::npos);
    CHECK(fs::read_text(dir / "scenes" / "old.vscene").value() == original); // report only: untouched

    std::filesystem::remove(dir / "scenes" / "future.vscene");
    auto written = player::migrate_project(dir, true);
    REQUIRE(written);
    CHECK(written->failures() == 0);
    CHECK(written->pending() == 0);
    REQUIRE(written->entries.size() == 1);
    CHECK(written->entries[0].written);
    CHECK(fs::read_text(fs::premigration_path(dir / "scenes" / "old.vscene")).value() == original);
    auto doc = json::parse(fs::read_text(dir / "scenes" / "old.vscene").value());
    REQUIRE(doc);
    const Value& beacon = (*doc)["entities"][0]["components"]["MigrateTestBeacon"];
    CHECK(beacon["$version"].as_int() == 2);
    CHECK(beacon["range"].as_float() == doctest::Approx(6.0f));

    auto after = player::migrate_project(dir, false);
    REQUIRE(after);
    CHECK(after->entries.empty());
}
