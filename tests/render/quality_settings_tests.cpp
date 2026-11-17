// SPDX-License-Identifier: Apache-2.0
#include "doctest.h"
#include "victus/render/scene_renderer.hpp"

#include <cmath>

using namespace victus::render;

TEST_CASE("quality tiers: names round-trip and presets scale monotonically") {
    const QualityTier tiers[] = {QualityTier::Low, QualityTier::Medium, QualityTier::High, QualityTier::Ultra};
    RenderSettings prev = RenderSettings::preset(QualityTier::Low);
    for (QualityTier t : tiers) {
        CHECK(parse_quality_tier(quality_tier_name(t)) == t);
        const RenderSettings s = RenderSettings::preset(t);
        CHECK(s.tier == t);
        CHECK(s.render_scale >= prev.render_scale);
        CHECK(s.shadow_cascades >= prev.shadow_cascades);
        CHECK(s.shadow_resolution >= prev.shadow_resolution);
        CHECK(s.max_point_lights >= prev.max_point_lights);
        CHECK(s.bloom_levels >= prev.bloom_levels);
        CHECK(s.ssao_samples >= prev.ssao_samples);
        // Presets are already within the supported ranges.
        const RenderSettings z = s.sanitized();
        CHECK(z.render_scale == s.render_scale);
        CHECK(z.shadow_cascades == s.shadow_cascades);
        CHECK(z.max_point_lights == s.max_point_lights);
        prev = s;
    }
    CHECK_FALSE(RenderSettings::preset(QualityTier::Low).ssao);
    CHECK_FALSE(RenderSettings::preset(QualityTier::Low).bloom);
    CHECK(RenderSettings::preset(QualityTier::High).ssao);
    CHECK_FALSE(parse_quality_tier("epic").has_value());
}

TEST_CASE("quality tiers: sanitized clamps hostile values") {
    RenderSettings s;
    s.render_scale = NAN;
    s.shadow_cascades = 9;
    s.shadow_resolution = 1u << 20;
    s.cascade_split_lambda = -3.0f;
    s.ssao_samples = 1000;
    s.bloom_levels = 0;
    s.max_point_lights = 100000;
    const RenderSettings z = s.sanitized();
    CHECK(z.render_scale == 1.0f);
    CHECK(z.shadow_cascades == 4);
    CHECK(z.shadow_resolution == 4096);
    CHECK(z.cascade_split_lambda == 0.0f);
    CHECK(z.ssao_samples == 32);
    CHECK(z.bloom_levels == 1);
    CHECK(z.max_point_lights == SceneRenderer::kMaxLights);
    s.render_scale = 10.0f;
    CHECK(s.sanitized().render_scale == 2.0f);
    s.render_scale = 0.01f;
    CHECK(s.sanitized().render_scale == 0.25f);
}
