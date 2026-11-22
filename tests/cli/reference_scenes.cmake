# Perceptual reference scenes (ADR-0104/0105/0140): renders samples/reference_scenes (exterior, interior, effects,
# photoreal)
# at ultra, 320x180, frames 15 and 30, and scores them against the committed software-renderer
# goldens (lavapipe/llvmpipe) with FLIP + worst tile + temporal error. Also checks the
# rendering-path report shows each scene's key features running, and records low-vs-ultra
# metrics without gating on them.
#   VICTUS_UPDATE_GOLDENS=1 ctest -R victus_reference_scenes   # rewrite the goldens
foreach(v VICTUS_CLI SOURCE_DIR WORK)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "reference_scenes.cmake: -D${v}= is required")
    endif()
endforeach()
if(NOT "$ENV{VICTUS_REQUIRE_GPU}" STREQUAL "1" OR "$ENV{VICTUS_NO_GPU}" STREQUAL "1")
    message(STATUS "VICTUS_REFERENCE_SKIPPED: needs VICTUS_REQUIRE_GPU=1")
    return()
endif()
set(project "${SOURCE_DIR}/samples/reference_scenes")
set(golden "${project}/golden/lavapipe_ultra_320x180")
# Tolerances: the same driver reproduces the goldens bit-exactly (measured FLIP 0.0000), so the
# limits are tight enough to catch local regressions, measured on these scenes: exterior with
# bloom off gives mean FLIP 0.025 / worst tile 0.043, interior with spot shadows off gives worst
# tile 0.019 / tile SSIM 0.966 (low quality: mean 0.12 / 0.21). A different rasteriser (another
# Mesa or a hardware GPU) can exceed them; regenerate with VICTUS_UPDATE_GOLDENS=1 for that device.
set(limits --max-flip 0.005 --max-tile-flip 0.01 --min-tile-ssim 0.99 --max-temporal 0.005)
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

function(capture scene quality out_dir)
    execute_process(COMMAND "${VICTUS_CLI}" run "${project}" --scene scenes/${scene}.vscene --frames 30
                            --capture-dir "${out_dir}" --capture-every 15 --quality ${quality} --width 320 --height 180
                            --render-report "${out_dir}.path.json" ${ARGN}
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0 OR NOT out MATCHES "captured 2 frames")
        message(FATAL_ERROR "victus run ${scene} (${quality}) failed (${rc}):\n${out}\n${err}")
    endif()
endfunction()

function(feature_state json name out_var)
    string(JSON n LENGTH "${json}" features)
    math(EXPR last "${n} - 1")
    foreach(i RANGE ${last})
        string(JSON fname GET "${json}" features ${i} name)
        if(fname STREQUAL name)
            string(JSON st GET "${json}" features ${i} state)
            set(${out_var} "${st}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "missing" PARENT_SCOPE)
endfunction()

set(expect_exterior sun_shadows volumetric_fog bloom ssao taa)
set(expect_interior local_lights local_shadows volumetric_fog bloom ssao)
set(expect_effects decals water ssr particles gpu_particles reflection_probes sun_shadows)
set(expect_photoreal sun_shadows ssao taa bloom)
set(summary "")
foreach(scene exterior interior effects photoreal)
    capture(${scene} ultra "${WORK}/${scene}")
    if("$ENV{VICTUS_UPDATE_GOLDENS}" STREQUAL "1")
        file(MAKE_DIRECTORY "${golden}/${scene}")
        file(GLOB frames "${WORK}/${scene}/*.png")
        file(COPY ${frames} DESTINATION "${golden}/${scene}")
        message(STATUS "updated goldens in ${golden}/${scene}")
        continue()
    endif()
    execute_process(COMMAND "${VICTUS_CLI}" compare "${golden}/${scene}" "${WORK}/${scene}" ${limits}
                            --json "${WORK}/${scene}.compare.json" --heatmap "${WORK}/${scene}_heat.png"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${scene} drifted from its golden (${rc}); heatmaps in ${WORK}:\n${out}\n${err}")
    endif()
    file(READ "${WORK}/${scene}.path.json" path_json)
    foreach(f ${expect_${scene}})
        feature_state("${path_json}" ${f} st)
        if(NOT st STREQUAL "active")
            message(FATAL_ERROR "${scene}: render path feature ${f} is ${st}, expected active")
        endif()
    endforeach()
    # FP16 shading (ADR-0137) is held to the same FP32 goldens and limits (ultra defaults to FP32).
    feature_state("${path_json}" shading_precision st)
    if(NOT st STREQUAL "active")
        message(FATAL_ERROR "${scene}: ultra should shade in FP32 by default, shading_precision is ${st}")
    endif()
    capture(${scene} ultra "${WORK}/${scene}_fp16" --fp16 on)
    file(READ "${WORK}/${scene}_fp16.path.json" fp16_json)
    feature_state("${fp16_json}" shading_precision st)
    if(st STREQUAL "reduced")
        execute_process(COMMAND "${VICTUS_CLI}" compare "${golden}/${scene}" "${WORK}/${scene}_fp16" ${limits}
                                --json "${WORK}/${scene}_fp16.compare.json" --heatmap "${WORK}/${scene}_fp16_heat.png"
                        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "${scene}: FP16 shading drifted from the FP32 golden (${rc}):\n${out}\n${err}")
        endif()
        file(READ "${WORK}/${scene}_fp16.compare.json" report)
        string(JSON hflip GET "${report}" flip_mean)
        string(APPEND summary " ${scene}: fp16 FLIP ${hflip};")
    else()
        message(STATUS "${scene}: device has no shaderFloat16, FP16 golden check skipped")
    endif()
    # Temporal upscaling (ADR-0141/0142): photoreal at half resolution, TAA-upscaled vs the same
    # scale without TAA (bilinear), both against the native golden. Gated relative to bilinear so
    # another rasteriser does not need new limits; lavapipe: upscaled 0.049, bilinear 0.077.
    if(scene STREQUAL "photoreal")
        capture(${scene} ultra "${WORK}/${scene}_up50" --render-scale 0.5)
        capture(${scene} ultra "${WORK}/${scene}_bil50" --render-scale 0.5 --taa off)
        foreach(v up50 bil50)
            execute_process(COMMAND "${VICTUS_CLI}" compare "${golden}/${scene}" "${WORK}/${scene}_${v}"
                                    --json "${WORK}/${scene}_${v}.compare.json"
                            RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
            file(READ "${WORK}/${scene}_${v}.compare.json" report)
            string(JSON flip_${v} GET "${report}" flip_mean)
        endforeach()
        file(READ "${WORK}/${scene}_up50.path.json" up_json)
        feature_state("${up_json}" temporal_upscale st)
        if(NOT st STREQUAL "active")
            message(FATAL_ERROR "${scene}: --render-scale 0.5 should temporally upscale, temporal_upscale is ${st}")
        endif()
        if(flip_up50 GREATER_EQUAL 1 OR flip_bil50 LESS_EQUAL 0)
            message(FATAL_ERROR "${scene}: bad upscale FLIP values ${flip_up50} / ${flip_bil50}")
        endif()
        # CMake has no float math: compare FLIP x 10000 as integers.
        string(REGEX REPLACE "^0\\.([0-9][0-9][0-9][0-9]).*" "\\1" up_i "${flip_up50}0000")
        string(REGEX REPLACE "^0\\.([0-9][0-9][0-9][0-9]).*" "\\1" bil_i "${flip_bil50}0000")
        math(EXPR up_i "1${up_i} - 10000")
        math(EXPR bil_i "1${bil_i} - 10000")
        math(EXPR limit_i "${bil_i} * 3 / 4")
        if(up_i GREATER limit_i)
            message(FATAL_ERROR "${scene}: temporal upscale FLIP ${flip_up50} is not below 3/4 of bilinear ${flip_bil50}")
        endif()
        string(APPEND summary " ${scene}: 50% upscale FLIP ${flip_up50} (bilinear ${flip_bil50});")
    endif()
    # Low vs ultra: recorded for the log, not gated (it measures what the low tier gives up).
    capture(${scene} low "${WORK}/${scene}_low")
    execute_process(COMMAND "${VICTUS_CLI}" compare "${WORK}/${scene}" "${WORK}/${scene}_low"
                            --json "${WORK}/${scene}_low.compare.json"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    file(READ "${WORK}/${scene}_low.compare.json" report)
    string(JSON flip GET "${report}" flip_mean)
    file(READ "${WORK}/${scene}.compare.json" report)
    string(JSON gflip GET "${report}" flip_mean)
    string(APPEND summary " ${scene}: golden FLIP ${gflip}, low-vs-ultra FLIP ${flip};")
endforeach()
message(STATUS "reference scenes:${summary}")
