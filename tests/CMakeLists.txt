# SPDX-License-Identifier: Apache-2.0
add_library(victus_doctest_main STATIC doctest_main.cpp)
target_include_directories(victus_doctest_main PUBLIC ${PROJECT_SOURCE_DIR}/third_party/doctest)
target_compile_features(victus_doctest_main PUBLIC cxx_std_20)

victus_add_test(victus_core_tests
    SOURCES
        core/cli_tests.cpp
        core/crash_tests.cpp
        core/engine_clock_tests.cpp
        core/fs_tests.cpp
        core/hash_tests.cpp
        core/jobs_tests.cpp
        core/json_tests.cpp
        core/log_tests.cpp
        core/migration_tests.cpp
        core/math_tests.cpp
        core/memory_tests.cpp
        core/pack_tests.cpp
        core/process_tests.cpp
        core/profiler_tests.cpp
        core/zip_tests.cpp
    DEPS VICTUS::Core
    LABELS core)

victus_add_test(victus_platform_tests
    SOURCES
        platform/input_state_tests.cpp
    DEPS VICTUS::Platform
    LABELS platform)

victus_add_test(victus_ecs_tests
    SOURCES
        ecs/world_tests.cpp
    DEPS VICTUS::Ecs
    LABELS ecs)

victus_add_test(victus_scene_tests
    SOURCES
        scene/migration_tests.cpp
        scene/prefab_tests.cpp
        scene/scene_tests.cpp
    DEPS VICTUS::Scene
    LABELS scene)

victus_add_test(victus_script_tests
    SOURCES
        script/script_tests.cpp
        script/procedural_bridge_tests.cpp
        script/input_map_tests.cpp
        script/save_tests.cpp
        script/capture_tests.cpp
        script/gameplay_framework_tests.cpp
        script/water_tests.cpp
        script/debugger_tests.cpp
        script/scene_transition_tests.cpp
        script/graph_tests.cpp
        script/localization_tests.cpp
        script/behaviour_tests.cpp
    DEPS VICTUS::Script VICTUS::Animation VICTUS::Tilemap
    LABELS script)

victus_add_test(victus_project_tests
    SOURCES
        project/project_tests.cpp
    DEPS VICTUS::Project VICTUS::Assets
    LABELS project)

if(VICTUS_WITH_GRAPHICS)
victus_add_test(victus_asset_tests
    SOURCES
        assets/asset_tests.cpp
        assets/async_loader_tests.cpp
        assets/animation_asset_tests.cpp
        assets/template_character_tests.cpp
        assets/texture_compression_tests.cpp
        assets/mesh_lod_asset_tests.cpp
        assets/morph_asset_tests.cpp
    DEPS VICTUS::Assets VICTUS::Project
    LABELS assets)

victus_add_test(victus_robustness_tests
    SOURCES
        robustness/parser_fuzz_tests.cpp
    DEPS VICTUS::Project VICTUS::Assets VICTUS::Net VICTUS::Terrain VICTUS::Sequencer VICTUS::Ai
         VICTUS::Tilemap VICTUS::Animation VICTUS::Gameplay VICTUS::Graph VICTUS::Render VICTUS::Audio
    LABELS robustness)
target_compile_definitions(victus_robustness_tests PRIVATE VICTUS_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    VICTUS_MINIAUDIO_DATA_DIR="${VICTUS_MINIAUDIO_DIR}/data")

victus_add_test(victus_render_tests
    SOURCES
        render/render_graph_tests.cpp
        render/image_mesh_tests.cpp
        render/quality_settings_tests.cpp
        render/shader_compiler_tests.cpp
        render/environment_map_tests.cpp
        render/atmosphere_tests.cpp
        render/light_clusters_tests.cpp
        render/mesh_lod_tests.cpp
        render/image_compare_tests.cpp
        render/render_path_tests.cpp
        render/lightmap_tests.cpp
    DEPS VICTUS::Render VICTUS::Shadercompiler
    LABELS render)
target_compile_definitions(victus_render_tests PRIVATE VICTUS_SHADER_SOURCE_DIR="${PROJECT_SOURCE_DIR}/engine/render/shaders")

victus_add_test(victus_gpu_tests
    SOURCES
        render/gpu_fixture.cpp
        render/gpu_render_tests.cpp
        render/gpu_device_loss_tests.cpp
        render/gpu_quality_tests.cpp
        render/gpu_instancing_tests.cpp
        render/gpu_skinning_tests.cpp
        render/gpu_scene_tests.cpp
        render/gpu_ui_tests.cpp
        render/gpu_particle_tests.cpp
        render/gpu_game_ui_tests.cpp
        render/gpu_hot_reload_tests.cpp
        render/gpu_async_loading_tests.cpp
        render/gpu_material_tests.cpp
        render/gpu_surface_shader_tests.cpp
        render/gpu_material_graph_tests.cpp
        render/gpu_sprite_sheet_tests.cpp
        render/gpu_tilemap_tests.cpp
        render/gpu_terrain_tests.cpp
        render/gpu_transparency_tests.cpp
        render/gpu_vertex_color_tests.cpp
        render/gpu_procedural_mesh_tests.cpp
        render/gpu_morph_tests.cpp
        render/gpu_environment_tests.cpp
        render/gpu_reflection_probe_tests.cpp
        render/gpu_irradiance_tests.cpp
        render/gpu_lightmap_tests.cpp
        render/gpu_scene_lightmap_tests.cpp
        render/gpu_taa_tests.cpp
        render/gpu_light_tests.cpp
        render/gpu_texture_compression_tests.cpp
        render/gpu_lod_tests.cpp
        render/gpu_texture_mip_tests.cpp
    render/gpu_alpha_coverage_tests.cpp
    render/gpu_overdraw_tests.cpp
    render/gpu_anisotropy_tests.cpp
    render/gpu_thin_geometry_tests.cpp
        render/gpu_character_lod_tests.cpp
        render/gpu_occlusion_tests.cpp
        render/gpu_local_shadow_tests.cpp
        render/gpu_motion_tests.cpp
        render/gpu_decal_tests.cpp
        render/gpu_water_tests.cpp
        render/gpu_volumetric_fog_tests.cpp
        render/gpu_volumetric_cloud_tests.cpp
        render/gpu_weather_tests.cpp
        render/gpu_texture_3d_tests.cpp
        render/gpu_effect_fog_tests.cpp
        render/gpu_light2d_tests.cpp
    DEPS VICTUS::Runtime VICTUS::Platform VICTUS::Ui
    LABELS gpu)
target_compile_definitions(victus_gpu_tests PRIVATE VICTUS_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/test_output"
    VICTUS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

victus_add_test(victus_editor_tests
    SOURCES
        render/gpu_fixture.cpp
        editor/editor_tests.cpp
        editor/gizmo_tests.cpp
        editor/studio_ux_tests.cpp
    DEPS VICTUS::EditorCore VICTUS::Platform
    LABELS "editor;gpu")
target_compile_definitions(victus_editor_tests PRIVATE VICTUS_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/test_output")
endif()

if(VICTUS_WITH_GRAPHICS)
victus_add_test(victus_export_tests
    SOURCES
        export/diagnostics_tests.cpp
        export/export_tests.cpp
        export/migrate_tests.cpp
        export/mobile_tests.cpp
        export/validate_tests.cpp
    DEPS VICTUS::Player
    LABELS export)

add_test(NAME victus_cli_smoke
    COMMAND ${CMAKE_COMMAND}
        -DVICTUS_CLI=$<TARGET_FILE:victus_cli>
        -DVICTUS_PLAYER=$<TARGET_FILE:victus_player_app>
        -DWORK=${CMAKE_BINARY_DIR}/test_output/cli_smoke
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/cli_smoke.cmake)
set_tests_properties(victus_cli_smoke PROPERTIES LABELS "cli;gpu")

# `victus bake-lighting` on a fresh 3D template (ADR-0125); runs it on every tier with a GPU.
add_test(NAME victus_cli_bake_lighting
    COMMAND ${CMAKE_COMMAND}
        -DVICTUS_CLI=$<TARGET_FILE:victus_cli>
        -DWORK=${CMAKE_BINARY_DIR}/test_output/cli_bake_lighting
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/bake_lighting.cmake)
set_tests_properties(victus_cli_bake_lighting PROPERTIES LABELS "cli;gpu")

# Crash handlers end to end (real SIGSEGV / std::terminate in a child process) and
# `victus diagnose` bundling the reports (ADR-0072). No GPU needed.
add_test(NAME victus_cli_diagnostics
    COMMAND ${CMAKE_COMMAND}
        -DVICTUS_CLI=$<TARGET_FILE:victus_cli>
        -DWORK=${CMAKE_BINARY_DIR}/test_output/cli_diagnostics
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/diagnostics_smoke.cmake)
set_tests_properties(victus_cli_diagnostics PROPERTIES LABELS "cli")

# Script debugging over the Debug Adapter Protocol end to end (ADR-0099). No GPU needed.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME victus_cli_debug_adapter
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli/dap_smoke.py
            $<TARGET_FILE:victus_cli> ${CMAKE_BINARY_DIR}/test_output/cli_debug_adapter)
    set_tests_properties(victus_cli_debug_adapter PROPERTIES LABELS "cli" TIMEOUT 300)
endif()

# Native plugins end to end (ADR-0078): scaffold, build, check, load, Luau call, export.
add_test(NAME victus_cli_plugins
    COMMAND ${CMAKE_COMMAND}
        -DVICTUS_CLI=$<TARGET_FILE:victus_cli>
        -DC_COMPILER=${CMAKE_C_COMPILER}
        -DWORK=${CMAKE_BINARY_DIR}/test_output/cli_plugins
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/plugins_smoke.cmake)
set_tests_properties(victus_cli_plugins PROPERTIES LABELS "cli" TIMEOUT 300)

# Fixed-step determinism of a full sample (headless; no GPU needed).
add_test(NAME victus_cli_determinism
    COMMAND ${CMAKE_COMMAND}
        -DVICTUS_CLI=$<TARGET_FILE:victus_cli>
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DWORK=${CMAKE_BINARY_DIR}/test_output/cli_determinism
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/determinism.cmake)
set_tests_properties(victus_cli_determinism PROPERTIES LABELS "cli" TIMEOUT 300)

# Perceptual capture/compare harness and render determinism (ADR-0102; skipped without a GPU).
add_test(NAME victus_cli_render_compare
    COMMAND ${CMAKE_COMMAND}
        -DVICTUS_CLI=$<TARGET_FILE:victus_cli>
        -DWORK=${CMAKE_BINARY_DIR}/test_output/cli_render_compare
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/render_compare.cmake)
set_tests_properties(victus_cli_render_compare PROPERTIES LABELS "cli;gpu" SKIP_REGULAR_EXPRESSION "VICTUS_COMPARE_SKIPPED"
    TIMEOUT 600)

# Perceptual reference scenes vs committed software-renderer goldens (ADR-0104; skipped without a GPU).
add_test(NAME victus_reference_scenes
    COMMAND ${CMAKE_COMMAND}
        -DVICTUS_CLI=$<TARGET_FILE:victus_cli>
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DWORK=${CMAKE_BINARY_DIR}/test_output/reference_scenes
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/reference_scenes.cmake)
set_tests_properties(victus_reference_scenes PROPERTIES LABELS "cli;gpu" SKIP_REGULAR_EXPRESSION "VICTUS_REFERENCE_SKIPPED"
    TIMEOUT 600)

# Device loss: the player and editor exit cleanly with a clear message when GPU operations keep
# failing (injected with VICTUS_GPU_FAIL); the editor autosaves unsaved edits first.
add_test(NAME victus_device_loss
    COMMAND ${CMAKE_COMMAND}
        -DVICTUS_CLI=$<TARGET_FILE:victus_cli>
        -DVICTUS_PLAYER=$<TARGET_FILE:victus_player_app>
        -DVICTUS_EDITOR=$<TARGET_FILE:victus_editor>
        -DWORK=${CMAKE_BINARY_DIR}/test_output/device_loss
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/device_loss.cmake)
set_tests_properties(victus_device_loss PROPERTIES LABELS "cli;gpu" SKIP_REGULAR_EXPRESSION "VICTUS_DEVICE_LOSS_SKIPPED"
    TIMEOUT 300)

# Performance regression gate: deterministic per-frame counters of every template and sample
# against tests/perf/budgets.json (skipped without a GPU; see docs/PERFORMANCE_STRATEGY.md).
add_test(NAME victus_perf_budgets
    COMMAND ${CMAKE_COMMAND}
        -DVICTUS_CLI=$<TARGET_FILE:victus_cli>
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DBUDGETS=${CMAKE_CURRENT_SOURCE_DIR}/perf/budgets.json
        -DWORK=${CMAKE_BINARY_DIR}/test_output/perf_budgets
        -P ${CMAKE_CURRENT_SOURCE_DIR}/perf/perf_budgets.cmake)
set_tests_properties(victus_perf_budgets PROPERTIES LABELS "perf;gpu" SKIP_REGULAR_EXPRESSION "VICTUS_PERF_SKIPPED"
    TIMEOUT 900)
endif()

victus_add_test(victus_physics_tests
    SOURCES
        physics/physics_tests.cpp
        physics/character_joint_tests.cpp
        physics/character_joint_2d_tests.cpp
        physics/mesh_collider_tests.cpp
        physics/polygon_chain_2d_tests.cpp
        physics/ragdoll_tests.cpp
        physics/destruction_tests.cpp
        physics/water_buoyancy_tests.cpp
    DEPS VICTUS::Physics VICTUS::Core
    LABELS physics)

victus_add_test(victus_tilemap_tests
    SOURCES
        tilemap/tilemap_tests.cpp
    DEPS VICTUS::Tilemap VICTUS::Physics VICTUS::Core
    LABELS tilemap)

victus_add_test(victus_world_tests
    SOURCES
        world/world_tests.cpp
    DEPS VICTUS::World VICTUS::Core
    LABELS world)

victus_add_test(victus_terrain_tests
    SOURCES
        terrain/terrain_tests.cpp
    DEPS VICTUS::Terrain VICTUS::Physics VICTUS::Core
    LABELS terrain)

victus_add_test(victus_audio_tests
    SOURCES
        audio/audio_tests.cpp audio/acoustics_tests.cpp
    DEPS VICTUS::Audio VICTUS::Physics VICTUS::Core
    LABELS audio)
target_compile_definitions(victus_audio_tests PRIVATE VICTUS_MINIAUDIO_DATA_DIR="${VICTUS_MINIAUDIO_DIR}/data")

victus_add_test(victus_animation_tests
    SOURCES animation/animation_tests.cpp animation/sprite_animation_tests.cpp animation/controller_tests.cpp animation/ik_tests.cpp animation/layer_tests.cpp animation/retarget_tests.cpp animation/morph_tests.cpp
    DEPS VICTUS::Animation VICTUS::Core
    LABELS animation)

victus_add_test(victus_gameui_tests
    SOURCES gameui/gameui_tests.cpp
    DEPS VICTUS::Gameui VICTUS::Core
    LABELS gameui)

victus_add_test(victus_particles_tests
    SOURCES particles/particles_tests.cpp
    DEPS VICTUS::Particles VICTUS::Core
    LABELS particles)

victus_add_test(victus_navigation_tests
    SOURCES navigation/navigation_tests.cpp
    DEPS VICTUS::Navigation VICTUS::Physics VICTUS::Core
    LABELS navigation)

victus_add_test(victus_ai_tests
    SOURCES ai/ai_tests.cpp
    DEPS VICTUS::Ai VICTUS::Navigation VICTUS::Physics VICTUS::Core
    LABELS ai)

victus_add_test(victus_net_tests
    SOURCES net/net_tests.cpp
    DEPS VICTUS::Net VICTUS::Script VICTUS::Physics
    LABELS net)

victus_add_test(victus_sequencer_tests
    SOURCES sequencer/sequencer_tests.cpp
    DEPS VICTUS::Sequencer VICTUS::Animation VICTUS::Audio VICTUS::Core
    LABELS sequencer)

# Release tooling (tools/release/release.py, tools/crash/symbolize.py): version/changelog checks,
# packaging with split debug symbols (Linux only part), build-id-checked symbol lookup.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME victus_release_tools COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/release_tests.py)
    set_tests_properties(victus_release_tools PROPERTIES LABELS "tools")
endif()

# Native plugin SDK (ADR-0078): one C source built as several plugin libraries.
foreach(mode IN ITEMS OK FAIL_INIT NEWER_API NO_ENTRY)
    string(TOLOWER ${mode} mode_lower)
    add_library(victus_test_plugin_${mode_lower} MODULE plugin/test_plugin.c)
    target_link_libraries(victus_test_plugin_${mode_lower} PRIVATE VICTUS::PluginSdk)
    target_compile_definitions(victus_test_plugin_${mode_lower} PRIVATE TEST_PLUGIN_MODE=MODE_${mode})
    set_target_properties(victus_test_plugin_${mode_lower} PROPERTIES C_VISIBILITY_PRESET hidden PREFIX "")
endforeach()
victus_add_test(victus_plugin_tests
    SOURCES
        plugin/plugin_tests.cpp
    DEPS VICTUS::Plugin
    LABELS plugin)
foreach(mode IN ITEMS OK FAIL_INIT NEWER_API NO_ENTRY)
    string(TOLOWER ${mode} mode_lower)
    add_dependencies(victus_plugin_tests victus_test_plugin_${mode_lower})
    target_compile_definitions(victus_plugin_tests PRIVATE
        VICTUS_TEST_PLUGIN_${mode}="$<TARGET_FILE:victus_test_plugin_${mode_lower}>")
endforeach()
