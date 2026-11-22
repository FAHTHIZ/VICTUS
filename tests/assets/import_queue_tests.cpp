// SPDX-License-Identifier: Apache-2.0
// Background import queue (ADR-0189): imports run on worker threads with their own database,
// report per-file state and errors, can be cancelled and retried, and leave cooked files the
// caller's database treats as up to date.
#include "doctest.h"
#include "victus/assets/database.hpp"
#include "victus/assets/import_queue.hpp"
#include "victus/render/image.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>

using namespace victus;
using namespace victus::assets;

namespace {

struct Project {
    fs::Path path;
    explicit Project(const char* name) {
        path = std::filesystem::temp_directory_path() / (std::string(name) + "_" + Uuid::generate().to_string().substr(0, 8));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path / "assets" / "textures");
    }
    ~Project() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    void texture(const std::string& name, std::uint32_t size, std::uint32_t colour) const {
        auto bytes = render::encode_png(render::make_checker(size, 4, colour, 0xff000000u));
        REQUIRE(bytes.ok());
        REQUIRE(fs::write_atomic(path / "assets" / "textures" / name, *bytes).ok());
    }
    void broken(const std::string& name) const {
        REQUIRE(fs::write_text_atomic(path / "assets" / "textures" / name, "not a png").ok());
    }
};

const ImportItem* item(const std::vector<ImportItem>& items, std::string_view path) {
    const auto it = std::find_if(items.begin(), items.end(), [&](const ImportItem& i) { return i.path == path; });
    return it == items.end() ? nullptr : &*it;
}

} // namespace

TEST_CASE("import queue: imports in the background, reports per-file results and errors") {
    Project p("victus_import_queue");
    for (int i = 0; i < 6; ++i) p.texture(std::format("t{}.png", i), 64, 0xff0000ffu + static_cast<std::uint32_t>(i) * 0x100u);
    p.broken("bad.png");
    auto db = AssetDatabase::open(p.path);
    REQUIRE(db.ok());
    (void)(*db)->refresh();

    auto q = ImportQueue::open(p.path, 2);
    REQUIRE(q.ok());
    CHECK((*q)->worker_count() == 2);
    // Paused, so the second call deterministically sees queued items (a finished item is queued again by design).
    (*q)->set_paused(true);
    CHECK((*q)->enqueue_all() == 7);
    (void)(*q)->enqueue_all();
    CHECK((*q)->progress().total == 7); // not listed twice
    (*q)->set_paused(false);
    REQUIRE((*q)->wait_idle());
    const ImportProgress pr = (*q)->progress();
    CHECK(pr.total == 7);
    CHECK(pr.imported == 6);
    CHECK(pr.failed == 1);
    CHECK_FALSE(pr.busy());
    CHECK(pr.fraction() == doctest::Approx(1.0f));
    const auto items = (*q)->items();
    const ImportItem* bad = item(items, "textures/bad.png");
    REQUIRE(bad != nullptr);
    CHECK(bad->state == ImportState::Failed);
    CHECK_FALSE(bad->error.empty());
    CHECK(bad->attempts == 1);
    const ImportItem* good = item(items, "textures/t3.png");
    REQUIRE(good != nullptr);
    CHECK(good->state == ImportState::Imported);
    CHECK(good->seconds >= 0.0);

    // The caller's database sees the cooked results as current.
    for (const auto& r : (*db)->records())
        if (r.path != "textures/bad.png") CHECK((*db)->is_up_to_date(r.uuid).value());
    // Each finished item is reported once.
    const auto finished = (*q)->take_finished();
    CHECK(finished.size() == 7);
    CHECK((*q)->take_finished().empty());

    // Fix the broken file and retry only it; a second pass over everything is up to date.
    p.texture("bad.png", 32, 0xff00ff00u);
    CHECK((*q)->retry_failed() == 1);
    REQUIRE((*q)->wait_idle());
    CHECK(item((*q)->items(), "textures/bad.png")->state == ImportState::Imported);
    CHECK(item((*q)->items(), "textures/bad.png")->attempts == 2);
    (*q)->clear_finished();
    CHECK((*q)->progress().total == 0);
    CHECK((*q)->enqueue_all() == 7);
    REQUIRE((*q)->wait_idle());
    CHECK((*q)->progress().up_to_date == 7);
}

TEST_CASE("import queue: cancelling drops queued work, pausing holds it, settings changes are seen") {
    Project p("victus_import_cancel");
    for (int i = 0; i < 8; ++i) p.texture(std::format("c{}.png", i), 256, 0xff112233u + static_cast<std::uint32_t>(i));
    auto db = AssetDatabase::open(p.path);
    REQUIRE(db.ok());
    (void)(*db)->refresh();
    auto q = ImportQueue::open(p.path, 1);
    REQUIRE(q.ok());

    (*q)->set_paused(true);
    CHECK((*q)->paused());
    CHECK((*q)->enqueue_all() == 8);
    CHECK_FALSE((*q)->wait_idle(std::chrono::milliseconds(50))); // paused with work left
    CHECK((*q)->progress().queued == 8);
    const Uuid first = (*q)->items().front().uuid;
    CHECK((*q)->cancel(first));
    CHECK_FALSE((*q)->cancel(first)); // already cancelled
    CHECK((*q)->cancel_all() == 7);
    CHECK((*q)->progress().cancelled == 8);
    (*q)->set_paused(false);
    REQUIRE((*q)->wait_idle());
    CHECK((*q)->progress().imported == 0);
    for (const auto& r : (*db)->records()) CHECK_FALSE((*db)->is_up_to_date(r.uuid).value());

    // Retry one cancelled item, then everything.
    CHECK((*q)->retry(first));
    REQUIRE((*q)->wait_idle());
    CHECK((*q)->progress().imported == 1);
    CHECK((*q)->retry_failed() == 7);
    REQUIRE((*q)->wait_idle());
    CHECK((*q)->progress().imported == 8);

    // Import settings changed through the caller's database are picked up by the queue.
    const AssetRecord* rec = (*db)->find(first);
    REQUIRE(rec != nullptr);
    json::Value settings = rec->settings.is_object() ? rec->settings : json::Value::object();
    settings.set("max_size", json::Value(64.0));
    REQUIRE((*db)->set_settings(first, settings).ok());
    CHECK_FALSE((*db)->is_up_to_date(first).value());
    CHECK((*q)->enqueue({{first, ""}}) == 1);
    REQUIRE((*q)->wait_idle());
    CHECK(item((*q)->items(), rec->path)->state == ImportState::Imported);
    CHECK((*db)->is_up_to_date(first).value());
    auto tex = (*db)->load_texture(first);
    REQUIRE(tex.ok());
    REQUIRE_FALSE(tex->mips.empty());
    CHECK(tex->mips[0].width <= 64);

    // A destroyed queue with pending work joins cleanly.
    (*q)->set_paused(true);
    (void)(*q)->retry_failed();
    (*q)->clear_finished();
    CHECK((*q)->enqueue_all() == 8);
    q->reset();
}

TEST_CASE("import queue: a deleted asset fails with a plain reason") {
    Project p("victus_import_gone");
    p.texture("gone.png", 16, 0xffffffffu);
    auto db = AssetDatabase::open(p.path);
    REQUIRE(db.ok());
    (void)(*db)->refresh();
    const Uuid id = (*db)->find_path("textures/gone.png")->uuid;
    std::filesystem::remove(p.path / "assets" / "textures" / "gone.png");
    std::filesystem::remove(p.path / "assets" / "textures" / "gone.png.meta");
    auto q = ImportQueue::open(p.path);
    REQUIRE(q.ok());
    CHECK((*q)->enqueue({{id, "textures/gone.png"}}) == 1);
    REQUIRE((*q)->wait_idle());
    const auto items = (*q)->items();
    REQUIRE(items.size() == 1);
    CHECK(items[0].state == ImportState::Failed);
    CHECK(items[0].error == "the asset is no longer in the project");
    CHECK(std::string(import_state_name(ImportState::UpToDate)) == "up to date");
}

TEST_CASE("asset database: cached source hashes notice edits to files with old timestamps") {
    // Source hashes are cached per (path, size, mtime); files edited in the last few seconds are
    // never cached. Back-date the source so the cache is used, then change it.
    Project p("victus_hash_cache");
    p.texture("t.png", 64, 0xff0000ffu);
    const fs::Path file = p.path / "assets" / "textures" / "t.png";
    const auto old = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
    std::filesystem::last_write_time(file, old);
    auto db = AssetDatabase::open(p.path);
    REQUIRE(db.ok());
    (void)(*db)->refresh();
    const AssetRecord* rec = (*db)->find_path("textures/t.png");
    REQUIRE(rec != nullptr);
    const Uuid id = rec->uuid;
    REQUIRE((*db)->import(id).ok());
    CHECK((*db)->is_up_to_date(id).value());
    CHECK((*db)->is_up_to_date(id).value()); // served from the cache

    p.texture("t.png", 64, 0xff00ff00u); // new pixels
    std::filesystem::last_write_time(file, old + std::chrono::minutes(5));
    CHECK_FALSE((*db)->is_up_to_date(id).value());
    REQUIRE((*db)->import(id).ok());
    CHECK((*db)->is_up_to_date(id).value());
}
