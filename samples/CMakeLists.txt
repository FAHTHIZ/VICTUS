# SPDX-License-Identifier: Apache-2.0
add_executable(victus_render_showcase render_showcase/main.cpp)
target_link_libraries(victus_render_showcase PRIVATE VICTUS::Render VICTUS::Platform)
victus_configure_target(victus_render_showcase)

# Sample games are plain projects (open them with victus_editor or run them with `victus run`).
# Each one is validated headless: `victus check`, then a scripted play-through.
if(VICTUS_BUILD_TESTS)
    # Dungeon Crawler: the autoplay scene's hero clears all three generated floors by itself.
    add_test(NAME victus_sample_dungeon_crawler
        COMMAND ${CMAKE_COMMAND}
            -DVICTUS=$<TARGET_FILE:victus_cli>
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/dungeon_crawler
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/sample_dungeon_crawler
            -DSCENE=scenes/autoplay.vscene
            -DFRAMES=36000
            -DSEED=1
            "-DEXPECT=Dungeon cleared!;Floor 3 / 3"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_sample_test.cmake)
    set_tests_properties(victus_sample_dungeon_crawler PROPERTIES LABELS samples TIMEOUT 300)

    # Robot Arena: the autoplay scene's fighter clears all five waves of navmesh-driven robots.
    add_test(NAME victus_sample_arena
        COMMAND ${CMAKE_COMMAND}
            -DVICTUS=$<TARGET_FILE:victus_cli>
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/arena
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/sample_arena
            -DSCENE=scenes/autoplay.vscene
            -DFRAMES=12000
            -DSEED=1
            "-DEXPECT=Arena cleared!;Wave 5 / 5    Robots 0"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_sample_test.cmake)
    set_tests_properties(victus_sample_arena PROPERTIES LABELS samples TIMEOUT 300)
endif()
