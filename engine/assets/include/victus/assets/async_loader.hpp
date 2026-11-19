// SPDX-License-Identifier: Apache-2.0
// V.A.U.L.T. stage 1: asynchronous loading of cooked assets.
//
// The game thread asks for an asset; a Background job reads and decodes it (importing first in a
// project database when the source is stale); the game thread later takes the decoded payload and
// uploads it. Nothing here touches the GPU, so the loader is testable without a device; the GPU
// stage lives in runtime::GpuAssetCache (set_async_loading).
//
// Model: one slot per (asset, type), shared by every caller (deduplicated like Unreal's
// FStreamableManager handles or Godot's load_threaded_request per path). Requests are dispatched
// in priority order with a bound on decodes in flight, so a scene that references hundreds of
// textures does not flood the pool or hold hundreds of decoded images at once. Completed loads
// are handed out by take() in priority order under a byte budget (at least one per call), which
// is what keeps upload cost per frame bounded. Failures are kept (with their message) until
// forget(): a broken file is read once, not every frame.
//
// Threading: request/state/take/forget/wait_idle are game-thread calls. Only `fetch` runs on
// workers, and it must be safe to call concurrently with itself; AssetDatabase::load_texture /
// load_model are, provided refresh() is not called while loads are in flight (wait_idle() first).
#pragma once
#include "victus/assets/cooked.hpp"
#include "victus/core/jobs.hpp"
#include "victus/core/result.hpp"
#include "victus/core/hash.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <variant>
#include <vector>

namespace victus::assets {

enum class LoadType : std::uint8_t { Texture, Model };
enum class LoadPriority : std::uint8_t { Low, Normal, High };
enum class LoadState : std::uint8_t {
    None,     // never requested (or forgotten)
    Queued,   // waiting for a decode slot
    Loading,  // decoding on a worker
    Decoded,  // decoded, waiting to be taken
    Taken,    // handed to the caller by take()
    Failed,   // load failed; error() has the reason
};

using LoadPayload = std::variant<CookedTexture, CookedModel>;
using LoadFetch = std::function<Result<LoadPayload>(const Uuid&, LoadType)>;

// Bytes the payload will occupy once uploaded (the first compressed variant of a texture when it
// has one, else RGBA8 mips; vertex and index data plus embedded textures of a model).
[[nodiscard]] std::size_t upload_bytes(const LoadPayload& payload) noexcept;
[[nodiscard]] std::size_t upload_bytes(const CookedTexture& texture) noexcept;

struct LoaderSettings {
    std::uint32_t max_in_flight{4}; // decodes running at once (0 is treated as 1)
};

struct LoaderStats {
    std::uint32_t queued{0}, loading{0}, decoded{0}; // current
    std::uint64_t requested{0}, completed{0}, failed{0}, taken{0}, cancelled{0}; // totals
    std::size_t decoded_bytes{0}; // payload bytes waiting to be taken
};

class AsyncLoader {
public:
    // A loader with jobs == nullptr runs fetch synchronously inside request() (tools, tests and
    // deterministic captures keep the old behaviour through the same code path).
    AsyncLoader(jobs::JobSystem* jobs, LoadFetch fetch, LoaderSettings settings = {});
    ~AsyncLoader(); // waits for decodes in flight
    AsyncLoader(const AsyncLoader&) = delete;
    AsyncLoader& operator=(const AsyncLoader&) = delete;

    // Starts (or re-prioritises) a load. Returns the current state; requesting a Failed or
    // Taken asset does nothing until forget().
    LoadState request(const Uuid& uuid, LoadType type, LoadPriority priority = LoadPriority::Normal);
    [[nodiscard]] LoadState state(const Uuid& uuid, LoadType type) const;
    // The failure of a Failed slot (nullptr otherwise).
    [[nodiscard]] const Error* error(const Uuid& uuid, LoadType type) const;

    struct Completed {
        Uuid uuid;
        LoadType type{LoadType::Texture};
        LoadPriority priority{LoadPriority::Normal};
        LoadPayload payload;
        std::size_t bytes{0};
    };
    // Collects finished decodes, starts queued ones, and returns decoded payloads (highest
    // priority, then oldest request first) until their upload_bytes() exceed `byte_budget`. At
    // least one payload is returned when any is ready, so a large asset never starves.
    [[nodiscard]] std::vector<Completed> take(std::size_t byte_budget);
    // Drops a slot: a queued load never runs, a running one is discarded when it finishes, a
    // failure is forgotten (the next request retries).
    void forget(const Uuid& uuid);
    void forget_all();
    // Blocks until no decode is running (queued ones stay queued). Call before
    // AssetDatabase::refresh() or before destroying the job system.
    void wait_idle();
    [[nodiscard]] bool idle() const noexcept;

    [[nodiscard]] LoaderStats stats() const;
    [[nodiscard]] const LoaderSettings& settings() const noexcept { return settings_; }
    void set_settings(const LoaderSettings& s) noexcept { settings_ = s; }
    [[nodiscard]] bool asynchronous() const noexcept { return jobs_ != nullptr; }

private:
    struct Job; // shared between the game thread and one worker
    struct Slot {
        LoadState state{LoadState::Queued};
        LoadPriority priority{LoadPriority::Normal};
        std::uint64_t order{0};
        std::shared_ptr<Job> job;
        std::unique_ptr<LoadPayload> payload;
        std::size_t bytes{0};
        Error error;
    };
    struct Key {
        Uuid uuid;
        LoadType type;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept {
            return UuidHash{}(k.uuid) ^ (static_cast<std::size_t>(k.type) * 0x9e3779b97f4a7c15ULL);
        }
    };
    void collect();   // moves finished jobs into their slots
    void dispatch();  // starts queued slots up to max_in_flight
    void start(const Key& key, Slot& slot);
    void finish(Slot& slot, Result<LoadPayload> result);

    jobs::JobSystem* jobs_;
    LoadFetch fetch_;
    LoaderSettings settings_;
    jobs::Counter counter_;
    std::unordered_map<Key, Slot, KeyHash> slots_;
    std::vector<std::shared_ptr<Job>> orphans_; // forgotten while running; dropped once done
    std::uint32_t running_{0};
    std::uint64_t next_order_{0};
    LoaderStats totals_;
};

} // namespace victus::assets
