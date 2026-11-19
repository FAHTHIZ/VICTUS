# Perceptual capture/compare harness end to end (ADR-0102): `victus run --capture-dir` writes a
# frame sequence, `victus compare` scores it against a reference with FLIP/SSIM/worst tile.
#  1. Two fixed-frame runs of the 3D template at the same quality must render identical frames
#     (regression test: surface-shader time used to follow the wall clock).
#  2. Low vs ultra quality must differ measurably, and a tight worst-tile limit must fail with
#     exit 4 while the JSON report records the failure.
foreach(v VICTUS_CLI WORK)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "render_compare.cmake: -D${v}= is required")
    endif()
endforeach()
if(NOT "$ENV{VICTUS_REQUIRE_GPU}" STREQUAL "1" OR "$ENV{VICTUS_NO_GPU}" STREQUAL "1")
    message(STATUS "VICTUS_COMPARE_SKIPPED: needs VICTUS_REQUIRE_GPU=1")
    return()
endif()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(project "${WORK}/game")
execute_process(COMMAND "${VICTUS_CLI}" new "${project}" --template 3d RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "victus new failed (${rc}):\n${out}")
endif()

function(capture name quality)
    execute_process(COMMAND "${VICTUS_CLI}" run "${project}" --frames 20 --capture-dir "${WORK}/${name}" --capture-every 10
                            --quality ${quality} --width 320 --height 180
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0 OR NOT out MATCHES "captured 2 frames")
        message(FATAL_ERROR "victus run ${name} failed (${rc}):\n${out}\n${err}")
    endif()
endfunction()
capture(ultra_a ultra)
capture(ultra_b ultra)
capture(low low)

execute_process(COMMAND "${VICTUS_CLI}" compare "${WORK}/ultra_a" "${WORK}/ultra_b" --max-flip 0 --max-tile-flip 0 --max-temporal 0
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "two identical fixed-frame runs rendered different frames (${rc}):\n${out}\n${err}")
endif()

execute_process(COMMAND "${VICTUS_CLI}" compare "${WORK}/ultra_a" "${WORK}/low" --json "${WORK}/low.json"
                        --heatmap "${WORK}/low_heat.png" --max-tile-flip 0.0001
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 4 OR NOT err MATCHES "outside tolerance: worst-tile FLIP")
    message(FATAL_ERROR "low vs ultra should exceed a 0.0001 worst-tile limit with exit 4 (${rc}):\n${out}\n${err}")
endif()
if(NOT EXISTS "${WORK}/low_heat.png")
    message(FATAL_ERROR "no heatmap written")
endif()
file(READ "${WORK}/low.json" report)
string(JSON pass GET "${report}" pass)
string(JSON flip GET "${report}" flip_mean)
string(JSON frames LENGTH "${report}" frames)
if(pass OR NOT frames EQUAL 2 OR NOT flip GREATER 0)
    message(FATAL_ERROR "unexpected compare report:\n${report}")
endif()
# Rendering-path report (ADR-0103) of a real frame: low drops SSAO by its tier, ultra runs it.
foreach(q low ultra)
    execute_process(COMMAND "${VICTUS_CLI}" run "${project}" --frames 3 --quality ${q} --width 320 --height 180
                            --render-report "${WORK}/path_${q}.json"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0 OR NOT out MATCHES "render path: ${q} tier")
        message(FATAL_ERROR "victus run --render-report (${q}) failed (${rc}):\n${out}\n${err}")
    endif()
    file(READ "${WORK}/path_${q}.json" path_json)
    string(JSON n LENGTH "${path_json}" features)
    set(ssao_state "")
    math(EXPR last "${n} - 1")
    foreach(i RANGE ${last})
        string(JSON name GET "${path_json}" features ${i} name)
        if(name STREQUAL "ssao")
            string(JSON ssao_state GET "${path_json}" features ${i} state)
            string(JSON ssao_reason GET "${path_json}" features ${i} reason)
        endif()
    endforeach()
    set(path_${q} "${ssao_state}/${ssao_reason}")
endforeach()
if(NOT path_low STREQUAL "off/quality_tier" OR NOT path_ultra STREQUAL "active/none")
    message(FATAL_ERROR "unexpected ssao path: low ${path_low}, ultra ${path_ultra}")
endif()
message(STATUS "identical runs match; low vs ultra mean FLIP ${flip}")
