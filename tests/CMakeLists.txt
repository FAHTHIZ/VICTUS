# SPDX-License-Identifier: Apache-2.0
add_library(victus_doctest_main STATIC doctest_main.cpp)
target_include_directories(victus_doctest_main PUBLIC ${PROJECT_SOURCE_DIR}/third_party/doctest)
target_compile_features(victus_doctest_main PUBLIC cxx_std_20)

victus_add_test(victus_core_tests
    SOURCES
        core/cli_tests.cpp
        core/engine_clock_tests.cpp
        core/fs_tests.cpp
        core/hash_tests.cpp
        core/jobs_tests.cpp
        core/json_tests.cpp
        core/log_tests.cpp
        core/math_tests.cpp
        core/memory_tests.cpp
        core/pack_tests.cpp
        core/process_tests.cpp
        core/profiler_tests.cpp
    DEPS VICTUS::Core
    LABELS core)

victus_add_test(victus_ecs_tests
    SOURCES
        ecs/world_tests.cpp
    DEPS VICTUS::Ecs
    LABELS ecs)

victus_add_test(victus_scene_tests
    SOURCES
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
        script/scene_transition_tests.cpp
        script/graph_tests.cpp
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
        assets/animation_asset_tests.cpp
        assets/template_robot_tests.cpp
        assets/texture_compression_tests.cpp
        assets/mesh_lod_asset_tests.cpp
    DEPS VICTUS::Assets VICTUS::Project
    LABELS assets)

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
    DEPS VICTUS::Render VICTUS::Shadercompiler
    LABELS render)
target_compile_definitions(victus_render_tests PRIVATE VICTUS_SHADER_SOURCE_DIR="${PROJECT_SOURCE_DIR}/engine/render/shaders")

victus_add_test(victus_gpu_tests
    SOURCES
        render/gpu_fixture.cpp
        render/gpu_render_tests.cpp
        render/gpu_quality_tests.cpp
        render/gpu_instancing_tests.cpp
        render/gpu_skinning_tests.cpp
        render/gpu_scene_tests.cpp
        render/gpu_ui_tests.cpp
        render/gpu_particle_tests.cpp
        render/gpu_game_ui_tests.cpp
        render/gpu_hot_reload_tests.cpp
        render/gpu_material_tests.cpp
        render/gpu_surface_shader_tests.cpp
        render/gpu_material_graph_tests.cpp
        render/gpu_sprite_sheet_tests.cpp
        render/gpu_tilemap_tests.cpp
        render/gpu_transparency_tests.cpp
        render/gpu_vertex_color_tests.cpp
        render/gpu_procedural_mesh_tests.cpp
        render/gpu_environment_tests.cpp
        render/gpu_reflection_probe_tests.cpp
        render/gpu_irradiance_tests.cpp
        render/gpu_taa_tests.cpp
        render/gpu_light_tests.cpp
        render/gpu_texture_compression_tests.cpp
        render/gpu_lod_tests.cpp
        render/gpu_occlusion_tests.cpp
        render/gpu_local_shadow_tests.cpp
        render/gpu_motion_tests.cpp
    DEPS VICTUS::Runtime VICTUS::Platform VICTUS::Ui
    LABELS gpu)
target_compile_definitions(victus_gpu_tests PRIVATE VICTUS_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/test_output")

victus_add_test(victus_editor_tests
    SOURCES
        render/gpu_fixture.cpp
        editor/editor_tests.cpp
        editor/gizmo_tests.cpp
    DEPS VICTUS::EditorCore VICTUS::Platform
    LABELS "editor;gpu")
target_compile_definitions(victus_editor_tests PRIVATE VICTUS_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/test_output")
endif()

if(VICTUS_WITH_GRAPHICS)
victus_add_test(victus_export_tests
    SOURCES
        export/export_tests.cpp
        export/mobile_tests.cpp
    DEPS VICTUS::Player
    LABELS export)

add_test(NAME victus_cli_smoke
    COMMAND ${CMAKE_COMMAND}
        -DVICTUS_CLI=$<TARGET_FILE:victus_cli>
        -DVICTUS_PLAYER=$<TARGET_FILE:victus_player_app>
        -DWORK=${CMAKE_BINARY_DIR}/test_output/cli_smoke
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/cli_smoke.cmake)
set_tests_properties(victus_cli_smoke PROPERTIES LABELS "cli;gpu")
endif()

victus_add_test(victus_physics_tests
    SOURCES
        physics/physics_tests.cpp
        physics/character_joint_tests.cpp
        physics/character_joint_2d_tests.cpp
    DEPS VICTUS::Physics VICTUS::Core
    LABELS physics)

victus_add_test(victus_tilemap_tests
    SOURCES
        tilemap/tilemap_tests.cpp
    DEPS VICTUS::Tilemap VICTUS::Physics VICTUS::Core
    LABELS tilemap)

victus_add_test(victus_audio_tests
    SOURCES
        audio/audio_tests.cpp
    DEPS VICTUS::Audio VICTUS::Core
    LABELS audio)
target_compile_definitions(victus_audio_tests PRIVATE VICTUS_MINIAUDIO_DATA_DIR="${VICTUS_MINIAUDIO_DIR}/data")

victus_add_test(victus_animation_tests
    SOURCES animation/animation_tests.cpp animation/sprite_animation_tests.cpp
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

victus_add_test(victus_net_tests
    SOURCES net/net_tests.cpp
    DEPS VICTUS::Net VICTUS::Script VICTUS::Physics
    LABELS net)
