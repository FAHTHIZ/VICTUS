# SPDX-License-Identifier: Apache-2.0
# `victus bake-lighting` end to end (ADR-0125): a fresh 3D template is marked Static Lighting,
# baked, saved with its Environment pointing at the file, re-baked, and (with a GPU) run on
# every quality tier with the lightmap.
#   cmake -DVICTUS_CLI=<victus> -DWORK=<dir> -P bake_lighting.cmake
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

function(run_ok)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "command failed (${rc}): ${ARGN}\n${out}\n${err}")
    endif()
    set(LAST_OUTPUT "${out}" PARENT_SCOPE)
endfunction()

set(dir "${WORK}/lit")
set(fast --samples 4 --bounces 1 --density 1 --max-size 256 --threads 2)
run_ok("${VICTUS_CLI}" new "${dir}" --template 3d --name "Lit")

# Nothing is marked yet: the bake refuses and writes nothing.
execute_process(COMMAND "${VICTUS_CLI}" bake-lighting "${dir}" ${fast} RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT err MATCHES "lightmapped")
    message(FATAL_ERROR "bake without Static Lighting meshes did not fail clearly (${rc}): ${err}")
endif()

# A dry run reports but changes nothing.
run_ok("${VICTUS_CLI}" bake-lighting "${dir}" ${fast} --mark-static --dry-run)
if(NOT LAST_OUTPUT MATCHES "dry run" OR EXISTS "${dir}/scenes/main.vlightmap")
    message(FATAL_ERROR "dry run wrote files: ${LAST_OUTPUT}")
endif()
file(READ "${dir}/scenes/main.vscene" scene)
if(scene MATCHES "\"lightmapped\"")
    message(FATAL_ERROR "dry run saved the scene")
endif()

run_ok("${VICTUS_CLI}" bake-lighting "${dir}" ${fast} --mark-static)
if(NOT LAST_OUTPUT MATCHES "marked [1-9][0-9]* mesh renderers" OR NOT LAST_OUTPUT MATCHES "baked [1-9][0-9]* lightmapped")
    message(FATAL_ERROR "unexpected bake output: ${LAST_OUTPUT}")
endif()
if(NOT EXISTS "${dir}/scenes/main.vlightmap")
    message(FATAL_ERROR "bake did not write scenes/main.vlightmap: ${LAST_OUTPUT}")
endif()
file(READ "${dir}/scenes/main.vscene" scene)
if(NOT scene MATCHES "\"lightmapped\": *true" OR NOT scene MATCHES "\"lightmap\": *\"scenes/main.vlightmap\"")
    message(FATAL_ERROR "scene was not saved with the bake")
endif()

# Re-baking uses the saved flags and the Environment's file.
run_ok("${VICTUS_CLI}" bake-lighting "${dir}" ${fast})
if(NOT LAST_OUTPUT MATCHES "baked [1-9][0-9]* lightmapped")
    message(FATAL_ERROR "re-bake failed: ${LAST_OUTPUT}")
endif()
run_ok("${VICTUS_CLI}" check "${dir}")

if("$ENV{VICTUS_REQUIRE_GPU}" STREQUAL "1" AND NOT "$ENV{VICTUS_NO_GPU}" STREQUAL "1")
    foreach(q low medium high ultra)
        run_ok("${VICTUS_CLI}" run "${dir}" --strict --frames 3 --width 320 --height 180 --quality ${q}
               --screenshot "${WORK}/lit_${q}.png")
        if(NOT EXISTS "${WORK}/lit_${q}.png")
            message(FATAL_ERROR "lightmapped run at ${q} wrote no screenshot")
        endif()
    endforeach()
endif()
