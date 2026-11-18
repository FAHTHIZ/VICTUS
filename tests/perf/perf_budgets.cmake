# SPDX-License-Identifier: Apache-2.0
# Performance regression gate (roadmap priority 3, docs/PERFORMANCE_STRATEGY.md).
#
# Runs every template and sample for a fixed number of 1/60 s frames at a fixed size and
# quality tier, reads the deterministic counters `victus run --profile` writes for the last
# frame (draw calls, render passes, triangles, batches, entities, GPU memory) and fails when
# one leaves the [min, max] range in budgets.json. CPU frame time p95 is checked against a
# generous per-case ceiling only (it depends on the machine; lavapipe renders on the CPU).
#
#   cmake -DVICTUS_CLI=<victus> -DSOURCE_DIR=<repo> -DBUDGETS=<budgets.json> -DWORK=<dir>
#         [-DCASES=template-3d;sample-arena] -P perf_budgets.cmake
#
# Without a GPU (VICTUS_REQUIRE_GPU unset or VICTUS_NO_GPU=1) the test prints
# VICTUS_PERF_SKIPPED and CTest reports it as skipped. Measured values of every run are
# written to <WORK>/measured.json; copy them into budgets.json (with headroom) when a change
# legitimately moves a counter, and say why in the commit.
cmake_minimum_required(VERSION 3.24)

if(NOT "$ENV{VICTUS_REQUIRE_GPU}" STREQUAL "1" OR "$ENV{VICTUS_NO_GPU}" STREQUAL "1")
    message("VICTUS_PERF_SKIPPED: performance budgets need a GPU (set VICTUS_REQUIRE_GPU=1)")
    return()
endif()

file(READ "${BUDGETS}" budgets)
string(JSON frames GET "${budgets}" settings frames)
string(JSON width GET "${budgets}" settings width)
string(JSON height GET "${budgets}" settings height)
string(JSON quality GET "${budgets}" settings quality)

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

function(run_ok)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "command failed (${rc}): ${ARGN}\n${out}\n${err}")
    endif()
endfunction()

string(JSON case_count LENGTH "${budgets}" cases)
math(EXPR last "${case_count} - 1")
set(failures "")
set(measured "{}")
foreach(i RANGE ${last})
    string(JSON name MEMBER "${budgets}" cases ${i})
    if(DEFINED CASES AND NOT name IN_LIST CASES)
        continue()
    endif()
    string(JSON kind GET "${budgets}" cases ${name} kind)
    string(JSON source GET "${budgets}" cases ${name} source)
    set(dir "${WORK}/${name}")
    if(kind STREQUAL "template")
        run_ok("${VICTUS_CLI}" new "${dir}" --template ${source} --name "Perf ${source}")
    else()
        file(COPY "${SOURCE_DIR}/${source}/" DESTINATION "${dir}")
        file(REMOVE_RECURSE "${dir}/.victus")
    endif()
    run_ok("${VICTUS_CLI}" import "${dir}")
    set(scene_args "")
    string(JSON scene ERROR_VARIABLE no_scene GET "${budgets}" cases ${name} scene)
    if(NOT no_scene)
        set(scene_args --scene "${scene}")
    endif()
    set(profile "${WORK}/${name}.profile.json")
    run_ok("${VICTUS_CLI}" run "${dir}" ${scene_args} --frames ${frames} --width ${width} --height ${height}
           --quality ${quality} --no-hot-reload --profile "${profile}")
    file(READ "${profile}" report)

    set(case_measured "{}")
    string(JSON p95 GET "${report}" frame p95_ms)
    string(JSON case_measured SET "${case_measured}" frame_p95_ms "${p95}")
    string(JSON p95_max GET "${budgets}" cases ${name} frame_p95_ms_max)
    if(p95 GREATER p95_max)
        list(APPEND failures "${name}: frame p95 ${p95} ms > ${p95_max} ms")
    endif()

    string(JSON counter_count LENGTH "${budgets}" cases ${name} counters)
    math(EXPR clast "${counter_count} - 1")
    foreach(c RANGE ${clast})
        string(JSON counter MEMBER "${budgets}" cases ${name} counters ${c})
        string(JSON lo GET "${budgets}" cases ${name} counters ${counter} 0)
        string(JSON hi GET "${budgets}" cases ${name} counters ${counter} 1)
        string(JSON value ERROR_VARIABLE missing GET "${report}" counters ${counter})
        if(missing)
            list(APPEND failures "${name}: counter ${counter} missing from the profile report")
            continue()
        endif()
        string(JSON case_measured SET "${case_measured}" "${counter}" "${value}")
        if(value LESS lo OR value GREATER hi)
            list(APPEND failures "${name}: ${counter} = ${value}, budget [${lo}, ${hi}]")
        endif()
    endforeach()
    string(JSON measured SET "${measured}" "${name}" "${case_measured}")
    message(STATUS "${name}: ${case_measured}")
endforeach()

file(WRITE "${WORK}/measured.json" "${measured}\n")
if(failures)
    list(JOIN failures "\n  " text)
    message(FATAL_ERROR "performance budgets exceeded (measured values in ${WORK}/measured.json):\n  ${text}")
endif()
message(STATUS "all performance budgets met")
