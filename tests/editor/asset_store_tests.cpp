// SPDX-License-Identifier: Apache-2.0
// VICTUS Studio Asset Store window (ADR-0174): search, thumbnails, licence-checked installs into
// the open project, the "In this project" list and removal - driven headlessly with an offline
// HTTP fixture and a fake provider (no network in CI).
#include "../library/fixtures.hpp"
#include "../render/gpu_fixture.hpp"
#include "victus/assets/database.hpp"
#include "victus/core/fs.hpp"
#include "victus/editor/asset_store.hpp"
#include "victus/editor/editor.hpp"
#include "victus/library/install.hpp"
#include "victus/ui/ui.hpp"

#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <mutex>

using namespace victus;
using victus::test::gpu;
namespace sfs = std::filesystem;
using namespace std::chrono_literals;

namespace {

// The store calls the transport from several worker threads; the shared fixture is not
// thread-safe on its own.
class LockedHttp final : public library::HttpClient {
public:
    explicit LockedHttp(library::test::FixtureHttp* inner) : inner_(inner) {}
    Result<std::string> get(const library::HttpRequest& r, const std::atomic<bool>* cancel) override {
        std::scoped_lock lock(mutex_);
        return inner_->get(r, cancel);
    }
    Status download(const library::HttpRequest& r, const fs::Path& dest, const library::Progress& progress, const std::atomic<bool>* cancel) override {
        std::scoped_lock lock(mutex_);
        return inner_->download(r, dest, progress, cancel);
    }

private:
    library::test::FixtureHttp* inner_;
    std::mutex mutex_;
};

library::LibraryAsset make_asset(std::string id, library::Licence licence) {
    library::LibraryAsset a;
    a.provider = "fake";
    a.id = id;
    a.name = "Crate " + id;
    a.type = library::AssetType::Model;
    a.author = "Someone";
    a.page_url = "https://example.com/" + id;
    a.thumbnail_url = "https://example.com/" + id + ".png";
    a.licence = std::move(licence);
    return a;
}

class FakeProvider final : public library::Provider {
public:
    library::ProviderInfo info_{.id = "fake", .name = "Fake Source", .homepage = "https://example.com", .credit = "Fake credit"};
    std::vector<std::string> searched;
    const library::ProviderInfo& info() const override { return info_; }
    Result<library::SearchPage> search(library::HttpClient&, const library::SearchQuery& q) override {
        searched.push_back(q.text);
        library::SearchPage page;
        library::Licence nc = library::licence_cc_by();
        nc.id = "CC-BY-NC-4.0";
        nc.commercial_use = false;
        page.assets = {make_asset("Crate01", library::licence_cc0()), make_asset("Crate02", nc)};
        page.total = 2;
        return page;
    }
    Result<library::InstallPlan> plan(library::HttpClient&, const library::LibraryAsset& a, std::string_view) override {
        library::InstallPlan p;
        library::FileRequest f;
        f.url = "https://example.com/" + a.id + ".bin";
        f.relative = "crate.bin";
        p.files = {f};
        p.primary = "crate.bin";
        return p;
    }
};

struct Harness {
    std::unique_ptr<ui::UiContext> ui;
    std::unique_ptr<editor::Editor> ed;
    sfs::path dir;
    library::test::FixtureHttp http;
    FakeProvider* provider{nullptr};

    bool init() {
        auto& ctx = *gpu().ctx;
        dir = sfs::temp_directory_path() / ("victus_store_" + Uuid::generate().to_string().substr(0, 8));
        sfs::create_directories(dir);
        auto u = ui::UiContext::create(ctx, rhi::Format::RGBA8_UNORM, nullptr, nullptr);
        if (!u) return false;
        ui = std::move(u).value();
        editor::EditorOptions o;
        o.recent_file = dir / "recent.json";
        o.default_project_dir = dir;
        o.diagnostics_dir = dir / "diagnostics";
        auto e = editor::Editor::create(ctx, *ui, nullptr, nullptr, o);
        if (!e) return false;
        ed = std::move(e).value();
        ed->set_viewport_size_override(320, 180);

        http.responses["https://example.com/Crate01.png"] = library::test::png(8, 8, 3, 200);
        http.responses["https://example.com/Crate02.png"] = "not an image";
        http.responses["https://example.com/Crate01.bin"] = "crate bytes";
        editor::AssetStore::Options so;
        so.http = std::make_unique<LockedHttp>(&http);
        auto p = std::make_unique<FakeProvider>();
        provider = p.get();
        so.providers.push_back(std::move(p));
        so.cache_dir = dir / "cache";
        so.settings_path = dir / "asset_store.json";
        so.thumbnail_threads = 2;
        ed->set_asset_store(std::make_unique<editor::AssetStore>(std::move(so)));
        return true;
    }
    void frames(int n) {
        auto& dev = gpu().ctx->device();
        for (int i = 0; i < n; ++i) {
            ui->new_frame(1.0f / 60.0f, {1280.0f, 720.0f});
            ed->frame(1.0f / 60.0f);
            auto cmd = dev.begin_commands();
            REQUIRE(ed->render(cmd).ok());
            REQUIRE(ui->prepare(cmd).ok());
            cmd.submit_and_wait();
        }
    }
    ~Harness() {
        ed.reset();
        ui.reset();
        std::error_code ec;
        sfs::remove_all(dir, ec);
    }
};

} // namespace

TEST_CASE("asset store: search, previews, licence-checked install, list and remove") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    REQUIRE(h.ed->create_project(h.dir / "Store Game", "Store Game", "3d").ok());
    auto& store = h.ed->asset_store();
    REQUIRE(store.provider_count() == 1);
    CHECK(store.provider_info(0).name == "Fake Source");
    CHECK(store.find_provider("fake") == std::optional<std::size_t>(0));
    CHECK_FALSE(store.find_provider("nope").has_value());

    // Opening the window searches once by itself; results arrive on the UI thread.
    h.ed->show_panel(editor::Panel::AssetStore);
    h.frames(2);
    REQUIRE(store.wait_idle(*h.ed, 5s));
    REQUIRE(store.results().size() == 2);
    CHECK(store.search_error().empty());
    CHECK(store.total() == 2);

    // Typed searches reach the provider with the text.
    store.query = "crate";
    store.search();
    REQUIRE(store.wait_idle(*h.ed, 5s));
    CHECK(h.provider->searched.back() == "crate");

    // Thumbnails decode into GPU textures; a broken image is marked failed rather than retried.
    (void)store.thumbnail(store.results()[0].thumbnail_url);
    (void)store.thumbnail(store.results()[1].thumbnail_url);
    REQUIRE(store.wait_idle(*h.ed, 5s));
    const auto* good = store.thumbnail(store.results()[0].thumbnail_url);
    const auto* bad = store.thumbnail(store.results()[1].thumbnail_url);
    REQUIRE(good);
    CHECK(good->texture.valid());
    CHECK(good->width == 8);
    REQUIRE(bad);
    CHECK(bad->failed);

    // The selected asset renders its detail pane without errors.
    store.selected = 0;
    h.frames(3);

    // A licence that forbids commercial use is refused before anything downloads.
    const auto before = h.http.requested.size();
    CHECK_FALSE(store.install(*h.ed, store.results()[1]));
    REQUIRE(store.wait_idle(*h.ed, 5s));
    CHECK_FALSE(store.install_state().error.empty());
    for (std::size_t i = before; i < h.http.requested.size(); ++i)
        CHECK(h.http.requested[i].find(".bin") == std::string::npos);

    // A CC0 asset downloads, commits into assets/library and is imported into the database.
    REQUIRE(store.install(*h.ed, store.results()[0]));
    REQUIRE(store.wait_idle(*h.ed, 5s));
    const auto st = store.install_state();
    REQUIRE_MESSAGE(st.phase == editor::AssetStore::InstallPhase::Done, st.error);
    CHECK(st.result.folder == "library/fake/crate01");
    const fs::Path root = h.ed->project()->root();
    CHECK(fs::read_text(root / "assets" / "library" / "fake" / "crate01" / "crate.bin").value_or("") == "crate bytes");
    CHECK(sfs::exists(root / "assets" / "library" / "fake" / "crate01" / "LICENSE.txt"));
    CHECK(sfs::exists(root / "CREDITS.md"));
    REQUIRE(h.ed->assets());
    CHECK(h.ed->assets()->find_path("library/fake/crate01/crate.bin") != nullptr);
    CHECK(h.ed->asset_folder() == "library/fake/crate01");

    const auto& list = store.installed(*h.ed);
    REQUIRE(list.size() == 1);
    CHECK(list[0].asset.id == "Crate01");
    CHECK(store.installed_copy(*h.ed, store.results()[0]) != nullptr);
    CHECK(store.installed_copy(*h.ed, store.results()[1]) == nullptr);

    // Installing again is a no-op that reports the existing copy.
    REQUIRE(store.install(*h.ed, store.results()[0]));
    REQUIRE(store.wait_idle(*h.ed, 5s));
    CHECK(store.install_state().phase == editor::AssetStore::InstallPhase::Done);
    CHECK(store.install_state().result.already_installed);
    h.frames(2);

    // Remove deletes the folder and its credits entry.
    REQUIRE(store.uninstall(*h.ed, "library/fake/crate01").ok());
    CHECK_FALSE(sfs::exists(root / "assets" / "library" / "fake" / "crate01"));
    CHECK(store.installed(*h.ed).empty());
    CHECK(h.ed->assets()->find_path("library/fake/crate01/crate.bin") == nullptr);
    h.frames(2);
}

TEST_CASE("asset store: account tokens are saved privately and installs need a project") {
    VICTUS_REQUIRE_GPU_OR_SKIP();
    Harness h;
    REQUIRE(h.init());
    auto& store = h.ed->asset_store();
    CHECK(store.api_key("fake").empty());
    store.set_api_key("fake", "secret-token");
    CHECK(store.api_key("fake") == "secret-token");
    const auto text = fs::read_text(h.dir / "asset_store.json").value_or("");
    CHECK(text.find("secret-token") != std::string::npos);
#ifndef _WIN32
    const auto perms = sfs::status(h.dir / "asset_store.json").permissions();
    CHECK((perms & (sfs::perms::group_all | sfs::perms::others_all)) == sfs::perms::none);
#endif
    // No project open: install refuses with a reason instead of downloading.
    CHECK_FALSE(store.install(*h.ed, make_asset("Crate01", library::licence_cc0())));
    CHECK_FALSE(store.install_state().error.empty());
}
