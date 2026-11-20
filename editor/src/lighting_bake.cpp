// SPDX-License-Identifier: Apache-2.0
// Assets > Bake Lighting (ADR-0125): the same bake as `victus bake-lighting`, gathered on the UI
// thread and traced on a worker so the Studio stays responsive.
#include "victus/editor/editor.hpp"

#include "victus/core/log.hpp"
#include "victus/gameplay/gameplay.hpp"

#include <chrono>
#include <filesystem>
#include <format>

namespace victus::editor {

namespace sfs = std::filesystem;
using scene::Entity;

Status Editor::start_lighting_bake(const render::LightmapSettings& settings) {
    if (!project_) return make_error(ErrorCode::InvalidArgument, "no project open");
    if (bake_future_.valid()) return make_error(ErrorCode::InvalidArgument, "lighting is already baking");
    if (play_state_ != PlayState::Editing) return make_error(ErrorCode::InvalidArgument, "stop play mode before baking lighting");
    if (scene_path_.empty()) return make_error(ErrorCode::InvalidArgument, "save the scene before baking its lighting");
    if (db_) {
        (void)db_->refresh();
        (void)db_->import_all();
    }
    scene_->update_transforms();
    auto prepared = runtime::prepare_scene_lightmap(*scene_, db_.get(), settings);
    if (!prepared) return std::move(prepared).error();
    std::string target;
    scene_->world().each<gameplay::EnvironmentComponent>([&](const gameplay::EnvironmentComponent& e) {
        if (target.empty()) target = e.lightmap;
    });
    if (target.empty()) target = runtime::default_lightmap_path(scene_path_);
    auto abs = project_->resolve(target);
    if (!abs) return std::move(abs).error();
    bake_target_ = target;
    bake_target_path_ = *abs;
    bake_error_.clear();
    bake_scene_ = scene_path_;
    bake_started_at_ = std::chrono::steady_clock::now();
    jobs::JobSystem* jobs = &workers();
    bake_future_ = std::async(std::launch::async, [p = std::move(prepared).value(), jobs]() {
        return runtime::bake_prepared_lightmap(p, jobs);
    });
    notify(std::format("Baking lighting into {}...", target));
    return {};
}

void Editor::poll_lighting_bake() {
    if (!bake_future_.valid()) return;
    if (bake_future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    auto r = bake_future_.get();
    if (!r) {
        bake_error_ = r.error().message;
        notify("Bake Lighting failed: " + bake_error_, true);
        return;
    }
    for (const auto& w : r->warnings) VICTUS_LOG_WARN("editor", "bake lighting: {}", w);
    std::error_code ec;
    sfs::create_directories(bake_target_path_.parent_path(), ec);
    if (auto st = fs::write_atomic(bake_target_path_, render::encode_lightmap_file(r->file)); !st) {
        bake_error_ = st.error().message;
        notify("Bake Lighting could not write " + bake_target_ + ": " + bake_error_, true);
        return;
    }
    last_bake_ = std::move(r).value();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - bake_started_at_).count();
    if (scene_path_ != bake_scene_ || play_state_ != PlayState::Editing) {
        notify(std::format("Baked lighting for {} into {}; set its Environment > Lightmap to use it", bake_scene_, bake_target_));
        return;
    }
    // Point the scene's Environment at the bake (one undo step); add one when the scene has none.
    scene::Scene& s = *scene_;
    Entity env_e;
    s.world().each<gameplay::EnvironmentComponent>([&](Entity e, const gameplay::EnvironmentComponent&) {
        if (env_e.is_null()) env_e = e;
    });
    if (env_e.is_null()) {
        env_e = create_entity(Primitive::Empty);
        if (auto* n = s.world().get<scene::Name>(env_e)) n->value = "Environment";
        (void)add_component(env_e, "Environment");
    }
    if (auto* env = s.world().get<gameplay::EnvironmentComponent>(env_e); env && env->lightmap != bake_target_) {
        auto before = capture(s, {s.uuid(env_e)});
        env->lightmap = bake_target_;
        record_edit("Bake Lighting", std::move(before));
    }
    if (cache_) cache_->evict_lightmaps();
    notify(std::format("Baked lighting for {} object(s) into {} ({}x{}, {:.1f} s){}", last_bake_->receivers, bake_target_,
                       last_bake_->stats.width, last_bake_->stats.height, secs,
                       last_bake_->warnings.empty() ? "" : std::format(", {} warning(s) in the Console", last_bake_->warnings.size())));
}

void Editor::wait_for_lighting_bake() {
    if (!bake_future_.valid()) return;
    bake_future_.wait();
    poll_lighting_bake();
}

} // namespace victus::editor
