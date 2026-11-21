// SPDX-License-Identifier: Apache-2.0
// Studio Asset Store window (ADR-0174): browses the engine/library providers, shows previews and
// licences, and installs into the open project. Network work (searches, thumbnails, downloads)
// runs on worker threads; everything that touches the project's AssetDatabase or the GPU runs on
// the main thread inside poll(). The class is UI-independent apart from draw(), so tests drive it
// with a fixture HTTP client and no window.
#pragma once

#include "victus/core/fs.hpp"
#include "victus/core/result.hpp"
#include "victus/core/hash.hpp"
#include "victus/library/http.hpp"
#include "victus/library/install.hpp"
#include "victus/library/library.hpp"
#include "victus/rhi/rhi.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace victus::render {
class GpuContext;
}

namespace victus::editor {

class Editor;

class AssetStore {
public:
    struct Options {
        // Transport; null = the curl client wrapped in a 12-hour cache under `cache_dir`.
        std::unique_ptr<library::HttpClient> http;
        // Providers; empty = library::make_all_providers().
        std::vector<std::unique_ptr<library::Provider>> providers;
        fs::Path cache_dir;     // default: <user data>/store_cache
        fs::Path settings_path; // Sketchfab token etc.; default: <user data>/asset_store.json
        unsigned thumbnail_threads{3};
    };
    explicit AssetStore(Options options);
    ~AssetStore();
    AssetStore(const AssetStore&) = delete;
    AssetStore& operator=(const AssetStore&) = delete;

    // ---- catalogue ----
    [[nodiscard]] std::size_t provider_count() const noexcept { return infos_.size(); }
    [[nodiscard]] const library::ProviderInfo& provider_info(std::size_t i) const { return infos_[i]; }
    [[nodiscard]] std::size_t provider() const noexcept { return provider_; }
    void select_provider(std::size_t index); // clears results and searches again
    [[nodiscard]] std::optional<std::size_t> find_provider(std::string_view id) const;

    // Starts a search with the current text/type on the selected provider (page 0) or appends
    // the next page. Results arrive in poll().
    void search();
    void load_more();
    std::string query;
    int type_filter{-1}; // AssetType or -1

    [[nodiscard]] const std::vector<library::LibraryAsset>& results() const noexcept { return results_; }
    [[nodiscard]] bool searching() const noexcept { return search_pending_; }
    [[nodiscard]] bool has_more() const noexcept { return has_more_; }
    [[nodiscard]] std::uint64_t total() const noexcept { return total_; }
    [[nodiscard]] const std::string& search_error() const noexcept { return search_error_; }
    int selected{-1}; // index into results()
    [[nodiscard]] const library::LibraryAsset* selected_asset() const;

    // ---- installs (one at a time) ----
    enum class InstallPhase : std::uint8_t { Idle, Downloading, Importing, Done, Failed, Cancelled };
    struct InstallState {
        InstallPhase phase{InstallPhase::Idle};
        std::string key;     // asset key being or last installed
        std::string name;
        std::string stage;   // progress text
        float fraction{0.0f};
        std::string error;
        library::InstallResult result; // valid when Done
    };
    // Starts downloading `asset` (variant id or empty) into the open project of `editor`.
    // False (with a reason in install_state().error) when another install runs or no project is open.
    bool install(Editor& editor, const library::LibraryAsset& asset, std::string variant = {}, bool replace = false);
    // Imports a pack archive the user downloaded (Kenney) the same way.
    bool import_archive(Editor& editor, const library::LibraryAsset& asset, const fs::Path& archive);
    void cancel_install() noexcept { cancel_ = true; }
    [[nodiscard]] InstallState install_state() const;
    [[nodiscard]] bool installing() const;

    // Assets of the open project installed from the store (refreshed after installs/removals
    // and when the project changes).
    [[nodiscard]] const std::vector<library::InstalledAsset>& installed(const Editor& editor);
    [[nodiscard]] const library::InstalledAsset* installed_copy(const Editor& editor, const library::LibraryAsset& asset);
    Status uninstall(Editor& editor, const std::string& folder);
    void invalidate_installed() noexcept { installed_root_.clear(); }

    // ---- previews ----
    // GPU texture of a thumbnail URL once downloaded and decoded; queues the download on first
    // call. Null texture while pending or when the image could not be decoded.
    struct Thumb {
        rhi::TextureHandle texture{};
        std::uint32_t width{0}, height{0};
        bool failed{false};
    };
    [[nodiscard]] const Thumb* thumbnail(const std::string& url);

    // ---- accounts ----
    [[nodiscard]] const std::string& api_key(std::string_view provider_id) const;
    void set_api_key(std::string_view provider_id, std::string key); // saved to settings_path

    // Applies finished background work: search results, decoded thumbnails (GPU uploads) and
    // staged installs (committed into the project, then imported). Call once per frame.
    void poll(Editor& editor);
    // Tests: waits until no search, thumbnail or download is pending (polling the editor), or
    // the timeout passes. Returns true when idle.
    bool wait_idle(Editor& editor, std::chrono::milliseconds timeout);

    // The window (Window > Asset Store).
    void draw(Editor& editor, bool* open);

private:
    using Completion = std::function<void(Editor&)>;
    struct Pool {
        explicit Pool(unsigned threads);
        ~Pool();
        void push(std::function<void()> task);
        [[nodiscard]] bool idle() const;
        std::vector<std::thread> threads;
        std::deque<std::function<void()>> tasks;
        mutable std::mutex mutex;
        std::condition_variable cv;
        unsigned running{0};
        bool stop{false};
    };
    void complete(Completion c);
    void start_search(bool append);
    void load_settings();
    void save_settings() const;
    void release_thumbnails(render::GpuContext* gpu);
    void draw_sidebar(Editor& editor);
    void draw_results(Editor& editor);
    void draw_detail(Editor& editor);
    void draw_installed(Editor& editor);
    void draw_account_card(const library::ProviderInfo& info);

    std::unique_ptr<library::HttpClient> owned_http_;
    std::unique_ptr<library::CachingHttpClient> cache_http_;
    library::HttpClient* http_{nullptr};
    std::vector<std::unique_ptr<library::Provider>> providers_;
    std::vector<library::ProviderInfo> infos_;
    fs::Path cache_dir_, settings_path_;
    std::map<std::string, std::string, std::less<>> api_keys_;

    std::size_t provider_{0};
    std::vector<library::LibraryAsset> results_;
    std::uint32_t page_{0};
    bool has_more_{false};
    std::uint64_t total_{0};
    bool search_pending_{false};
    std::uint64_t search_generation_{0};
    std::string search_error_;
    bool searched_once_{false};
    bool show_installed_{false};
    double query_edited_at_{0};
    std::array<char, 160> token_buf_{};
    int variant_index_{0};
    std::string variant_for_;

    mutable std::mutex state_mutex_;
    InstallState install_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> install_busy_{false};

    std::string installed_root_;
    std::vector<library::InstalledAsset> installed_;

    struct ThumbEntry {
        Thumb thumb;
        bool requested{false};
        std::uint64_t last_used{0};
    };
    std::map<std::string, ThumbEntry, std::less<>> thumbs_;
    std::uint64_t frame_{0};
    std::atomic<int> thumbs_pending_{0};
    render::GpuContext* gpu_{nullptr};

    std::mutex completions_mutex_;
    std::vector<Completion> completions_;

    // Declared last: destroyed (threads joined) first, while everything above is still alive.
    std::unique_ptr<Pool> work_;   // searches, plans and downloads: one thread, providers are serial
    std::unique_ptr<Pool> images_; // thumbnails
};

} // namespace victus::editor
