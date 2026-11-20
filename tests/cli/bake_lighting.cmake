# SPDX-License-Identifier: Apache-2.0
# `victus bake-lighting` end to end (ADR-0125, ADR-0134): `victus new` bakes 3D templates and not
# 2D ones; an unbaked 3D template is baked, saved with its Environment pointing at the file,
# re-baked, and (with a GPU) run on every quality tier with the lightmap.
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

set(fast --samples 4 --bounces 1 --density 1 --max-size 256 --threads 2)

# 3D templates ship baked (ADR-0134): `victus new` writes the lightmap and references it.
set(baked "${WORK}/baked")
run_ok("${VICTUS_CLI}" new "${baked}" --template 3d --name "Baked")
if(NOT LAST_OUTPUT MATCHES "Baked static lighting" OR NOT EXISTS "${baked}/scenes/main.vlightmap")
    message(FATAL_ERROR "victus new did not bake the 3D template: ${LAST_OUTPUT}")
endif()
file(READ "${baked}/scenes/main.vscene" scene)
if(NOT scene MATCHES "\"lightmapped\": *true" OR NOT scene MATCHES "\"lightmap\": *\"scenes/main.vlightmap\"")
    message(FATAL_ERROR "the baked template scene does not reference its lightmap")
endif()
run_ok("${VICTUS_CLI}" check "${baked}")
# 2D templates have nothing to bake; the bake refuses clearly and writes nothing.
set(flat "${WORK}/flat")
run_ok("${VICTUS_CLI}" new "${flat}" --template 2d --name "Flat")
if(LAST_OUTPUT MATCHES "Baked static lighting")
    message(FATAL_ERROR "victus new baked a 2D template: ${LAST_OUTPUT}")
endif()
execute_process(COMMAND "${VICTUS_CLI}" bake-lighting "${flat}" ${fast} RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT err MATCHES "lightmapped")
    message(FATAL_ERROR "bake without Static Lighting meshes did not fail clearly (${rc}): ${err}")
endif()

# --no-bake leaves the Static Lighting flags but writes no lightmap.
set(dir "${WORK}/lit")
run_ok("${VICTUS_CLI}" new "${dir}" --template 3d --name "Lit" --no-bake)
if(LAST_OUTPUT MATCHES "Baked static lighting" OR EXISTS "${dir}/scenes/main.vlightmap")
    message(FATAL_ERROR "--no-bake still baked: ${LAST_OUTPUT}")
endif()

# A dry run reports but changes nothing.
file(READ "${dir}/scenes/main.vscene" before)
run_ok("${VICTUS_CLI}" bake-lighting "${dir}" ${fast} --mark-static --dry-run)
if(NOT LAST_OUTPUT MATCHES "dry run" OR EXISTS "${dir}/scenes/main.vlightmap")
    message(FATAL_ERROR "dry run wrote files: ${LAST_OUTPUT}")
endif()
file(READ "${dir}/scenes/main.vscene" scene)
if(NOT scene STREQUAL before)
    message(FATAL_ERROR "dry run saved the scene")
endif()

run_ok("${VICTUS_CLI}" bake-lighting "${dir}" ${fast} --mark-static)
if(NOT LAST_OUTPUT MATCHES "marked [0-9]+ mesh renderers" OR NOT LAST_OUTPUT MATCHES "baked [1-9][0-9]* lightmapped"
   OR NOT LAST_OUTPUT MATCHES "baked [1-9][0-9]* light probes")
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
run_ok("${VICTUS_CLI}" bake-lighting "${dir}" ${fast} --no-probes --dry-run)
if(LAST_OUTPUT MATCHES "light probes")
    message(FATAL_ERROR "--no-probes still baked probes: ${LAST_OUTPUT}")
endif()
execute_process(COMMAND "${VICTUS_CLI}" bake-lighting "${dir}" ${fast} --probe-spacing 0.1 RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT err MATCHES "probe-spacing")
    message(FATAL_ERROR "--probe-spacing 0.1 was not rejected (${rc}): ${err}")
endif()
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
