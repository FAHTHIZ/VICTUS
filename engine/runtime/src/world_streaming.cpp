// SPDX-License-Identifier: Apache-2.0
#include "victus/runtime/world_streaming.hpp"

namespace victus::runtime {

namespace {
// Calls fn(uuid, type) for each GPU asset `id` stands for; false when it is not one.
template <class Fn>
bool for_gpu_assets(GpuAssetCache& cache, const Uuid& id, Fn&& fn) {
    assets::AssetDatabase* db = cache.database();
    const assets::AssetRecord* rec = db ? db->find(id) : nullptr;
    if (!rec) return false;
    switch (rec->kind) {
    case assets::AssetKind::Texture: fn(id, assets::LoadType::Texture); return true;
    case assets::AssetKind::Model: fn(id, assets::LoadType::Model); return true;
    case assets::AssetKind::Material: {
        auto m = db->resolve_material(id);
        if (!m) return false;
        for (const Uuid* t : m->all_textures())
            if (!t->is_nil()) fn(*t, assets::LoadType::Texture);
        return true;
    }
    default: return false;
    }
}
} // namespace

world::Dependencies world_dependencies(GpuAssetCache& cache) {
    world::Dependencies d;
    d.request = [&cache](const Uuid& id) {
        (void)for_gpu_assets(cache, id, [&](const Uuid& a, assets::LoadType t) { cache.prefetch(a, t, assets::LoadPriority::Normal); });
    };
    d.ready = [&cache](const Uuid& id) {
        bool ready = true;
        (void)for_gpu_assets(cache, id, [&](const Uuid& a, assets::LoadType t) { ready = ready && cache.settled(a, t); });
        return ready;
    };
    return d;
}

} // namespace victus::runtime
