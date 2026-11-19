// SPDX-License-Identifier: Apache-2.0
// V.A.U.L.T. stage 1 (ADR-0087): AsyncLoader deduplicates requests, dispatches by priority under
// a decode limit, hands payloads out under a byte budget and keeps failures until forgotten.
#include "doctest.h"
#include "victus/assets/async_loader.hpp"

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

using namespace victus;
using namespace victus::assets;

namespace {
// A texture payload of `bytes` RGBA8 bytes (one mip).
CookedTexture texture_of(std::size_t bytes, std::string name) {
    CookedTexture t;
    t.name = std::move(name);
    t.mips.push_back({.width = 1, .height = static_cast<std::uint32_t>(bytes / 4), .pixels = std::vector<std::uint8_t>(bytes, 7)});
    return t;
}

struct FakeSource {
    std::mutex mutex;
    std::map<Uuid, std::size_t> sizes;           // assets that exist, by payload size
    std::map<Uuid, int> fetches;                 // calls per asset
    std::vector<Uuid> order;                     // fetch start order
    std::atomic<bool> gate{true};                // false: fetches block until opened
    std::atomic<int> running{0}, max_running{0};

    LoadFetch fetch() {
        return [this](const Uuid& id, LoadType) -> Result<LoadPayload> {
            const int now = ++running;
            for (int m = max_running.load(); now > m && !max_running.compare_exchange_weak(m, now);) {
            }
            std::size_t bytes = 0;
            bool found = false;
            {
                std::lock_guard lock(mutex);
                ++fetches[id];
                order.push_back(id);
                if (const auto it = sizes.find(id); it != sizes.end()) {
                    bytes = it->second;
                    found = true;
                }
            }
            while (!gate.load()) std::this_thread::yield();
            --running;
            if (!found) return make_error(ErrorCode::NotFound, "no asset " + id.to_string());
            return LoadPayload{texture_of(bytes, id.to_string())};
        };
    }
};

std::size_t drain(AsyncLoader& loader, std::vector<AsyncLoader::Completed>& out, std::size_t budget = ~std::size_t{0}) {
    std::size_t calls = 0;
    for (int i = 0; i < 10000; ++i) {
        loader.wait_idle();
        auto got = loader.take(budget);
        if (!got.empty()) ++calls;
        for (auto& c : got) out.push_back(std::move(c));
        const auto st = loader.stats();
        if (st.queued == 0 && st.loading == 0 && st.decoded == 0) break;
    }
    return calls;
}
} // namespace

TEST_CASE("async loader: requests are deduplicated and payloads arrive once") {
    jobs::JobSystem js(2);
    FakeSource src;
    const Uuid a = Uuid::generate();
    src.sizes[a] = 64;
    AsyncLoader loader(&js, src.fetch());
    CHECK(loader.asynchronous());
    for (int i = 0; i < 5; ++i) (void)loader.request(a, LoadType::Texture);
    std::vector<AsyncLoader::Completed> got;
    (void)drain(loader, got);
    REQUIRE(got.size() == 1);
    CHECK(got[0].uuid == a);
    CHECK(got[0].bytes == 64);
    CHECK(std::get<CookedTexture>(got[0].payload).mips[0].pixels.size() == 64);
    CHECK(src.fetches[a] == 1);
    CHECK(loader.state(a, LoadType::Texture) == LoadState::Taken);
    // A taken asset is not loaded again until forgotten.
    (void)loader.request(a, LoadType::Texture);
    loader.wait_idle();
    CHECK(loader.take(1 << 20).empty());
    CHECK(src.fetches[a] == 1);
    loader.forget(a);
    CHECK(loader.state(a, LoadType::Texture) == LoadState::None);
    (void)loader.request(a, LoadType::Texture);
    got.clear();
    (void)drain(loader, got);
    CHECK(got.size() == 1);
    CHECK(src.fetches[a] == 2);
    // The same asset as a different type is a separate slot.
    CHECK(loader.state(a, LoadType::Model) == LoadState::None);
    const auto s = loader.stats();
    CHECK(s.requested == 2);
    CHECK(s.completed == 2);
    CHECK(s.taken == 2);
}

TEST_CASE("async loader: queued loads start in priority order under the in-flight limit") {
    jobs::JobSystem js(2);
    FakeSource src;
    src.gate = false; // the first fetch blocks until we open the gate
    std::vector<Uuid> ids;
    for (int i = 0; i < 6; ++i) {
        ids.push_back(Uuid::generate());
        src.sizes[ids.back()] = 16;
    }
    AsyncLoader loader(&js, src.fetch(), {.max_in_flight = 1});
    CHECK(loader.request(ids[0], LoadType::Texture, LoadPriority::Low) == LoadState::Loading);
    CHECK(loader.request(ids[1], LoadType::Texture, LoadPriority::Low) == LoadState::Queued);
    CHECK(loader.request(ids[2], LoadType::Texture, LoadPriority::Normal) == LoadState::Queued);
    CHECK(loader.request(ids[3], LoadType::Texture, LoadPriority::High) == LoadState::Queued);
    CHECK(loader.request(ids[4], LoadType::Texture, LoadPriority::Low) == LoadState::Queued);
    // Re-requesting at a higher priority promotes a queued load.
    CHECK(loader.request(ids[4], LoadType::Texture, LoadPriority::High) == LoadState::Queued);
    CHECK(loader.stats().loading == 1);
    CHECK(loader.stats().queued == 4);
    src.gate = true;
    std::vector<AsyncLoader::Completed> got;
    (void)drain(loader, got);
    CHECK(src.max_running.load() == 1);
    REQUIRE(src.order.size() == 5);
    // ids[0] was already running; then High (3 before 4: request order), Normal, Low.
    CHECK(src.order[0] == ids[0]);
    CHECK(src.order[1] == ids[3]);
    CHECK(src.order[2] == ids[4]);
    CHECK(src.order[3] == ids[2]);
    CHECK(src.order[4] == ids[1]);
    CHECK(got.size() == 5);
}

TEST_CASE("async loader: the byte budget bounds each take but never starves a large asset") {
    jobs::JobSystem js(2);
    FakeSource src;
    const Uuid small1 = Uuid::generate(), small2 = Uuid::generate(), big = Uuid::generate();
    src.sizes[small1] = 100;
    src.sizes[small2] = 100;
    src.sizes[big] = 1000;
    AsyncLoader loader(&js, src.fetch(), {.max_in_flight = 8});
    (void)loader.request(big, LoadType::Texture, LoadPriority::High);
    (void)loader.request(small1, LoadType::Texture);
    (void)loader.request(small2, LoadType::Texture);
    loader.wait_idle();
    CHECK(loader.stats().decoded == 3); // wait_idle() collected the finished decodes
    auto first = loader.take(150);
    REQUIRE(first.size() == 1); // over budget alone, but at least one is always handed out
    CHECK(first[0].uuid == big); // highest priority first
    CHECK(loader.stats().decoded == 2);
    CHECK(loader.stats().decoded_bytes == 200);
    auto second = loader.take(150);
    REQUIRE(second.size() == 1); // 100 + 100 > 150
    CHECK(second[0].uuid == small1);
    auto third = loader.take(150);
    REQUIRE(third.size() == 1);
    CHECK(third[0].uuid == small2);
    (void)loader.request(Uuid::generate(), LoadType::Texture); // missing: fails, nothing to take
    loader.wait_idle();
    CHECK(loader.take(1 << 20).empty());
}

TEST_CASE("async loader: failures are kept until forgotten; forgotten loads are discarded") {
    jobs::JobSystem js(1);
    FakeSource src;
    const Uuid missing = Uuid::generate();
    AsyncLoader loader(&js, src.fetch());
    (void)loader.request(missing, LoadType::Model);
    loader.wait_idle();
    CHECK(loader.take(1 << 20).empty());
    REQUIRE(loader.state(missing, LoadType::Model) == LoadState::Failed);
    REQUIRE(loader.error(missing, LoadType::Model));
    CHECK(loader.error(missing, LoadType::Model)->code == ErrorCode::NotFound);
    CHECK(loader.request(missing, LoadType::Model) == LoadState::Failed); // not retried
    CHECK(src.fetches[missing] == 1);
    loader.forget(missing);
    CHECK(loader.error(missing, LoadType::Model) == nullptr);
    (void)loader.request(missing, LoadType::Model); // retried after forget()
    loader.wait_idle();
    (void)loader.take(0);
    CHECK(src.fetches[missing] == 2);
    CHECK(loader.stats().failed == 2);

    // Forgetting a running load discards its result; the slot stays free for a new request.
    const Uuid a = Uuid::generate();
    src.sizes[a] = 8;
    src.gate = false;
    CHECK(loader.request(a, LoadType::Texture) == LoadState::Loading);
    loader.forget(a);
    CHECK(loader.state(a, LoadType::Texture) == LoadState::None);
    CHECK_FALSE(loader.idle());
    src.gate = true;
    loader.wait_idle();
    CHECK(loader.take(1 << 20).empty());
    CHECK(loader.stats().cancelled == 1);
    CHECK(loader.stats().loading == 0);
    // forget_all() cancels queued work.
    src.gate = false;
    AsyncLoader limited(&js, src.fetch(), {.max_in_flight = 1});
    const Uuid b = Uuid::generate(), c = Uuid::generate();
    src.sizes[b] = src.sizes[c] = 4;
    (void)limited.request(b, LoadType::Texture);
    CHECK(limited.request(c, LoadType::Texture) == LoadState::Queued);
    limited.forget_all();
    src.gate = true;
    limited.wait_idle();
    CHECK(limited.take(1 << 20).empty());
    CHECK(src.fetches[c] == 0); // never started
    CHECK(limited.stats().cancelled == 2);
}

TEST_CASE("async loader: without a job system loads complete inside request()") {
    FakeSource src;
    const Uuid a = Uuid::generate();
    src.sizes[a] = 12;
    AsyncLoader loader(nullptr, src.fetch());
    CHECK_FALSE(loader.asynchronous());
    CHECK(loader.request(a, LoadType::Texture) == LoadState::Decoded);
    auto got = loader.take(0);
    REQUIRE(got.size() == 1);
    CHECK(got[0].bytes == 12);
}

TEST_CASE("async loader: upload_bytes prefers the first compressed variant") {
    CookedTexture t = texture_of(64, "t");
    CHECK(upload_bytes(t) == 64);
    t.compressed.push_back({.codec = TextureCodec::BC7, .levels = {std::vector<std::uint8_t>(16)}});
    CHECK(upload_bytes(t) == 16);
    CookedModel m;
    m.meshes.push_back({});
    m.meshes[0].data.vertices.resize(3);
    m.meshes[0].data.indices = {0, 1, 2};
    m.textures.push_back(texture_of(32, "m"));
    CHECK(upload_bytes(LoadPayload{m}) == 3 * sizeof(render::Vertex3D) + 3 * sizeof(std::uint32_t) + 32);
}
