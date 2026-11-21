# SPDX-License-Identifier: Apache-2.0
# Device loss end to end: with GPU failures injected (VICTUS_GPU_FAIL) the player and the editor
# must stop within a bounded time, exit with code 3 and say why, instead of hanging or crashing,
# when recovery is off. With recovery (the player's default, ADR-0157) the player recreates the
# device, keeps running and its final frame matches a run without a loss.
#   cmake -DVICTUS_CLI=<victus> -DVICTUS_PLAYER=<victus_player> -DVICTUS_EDITOR=<victus_editor> -DWORK=<dir> -P device_loss.cmake
if(NOT "$ENV{VICTUS_REQUIRE_GPU}" STREQUAL "1" OR "$ENV{VICTUS_NO_GPU}" STREQUAL "1")
    message(STATUS "VICTUS_DEVICE_LOSS_SKIPPED: needs VICTUS_REQUIRE_GPU=1")
    return()
endif()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(ENV{VICTUS_NO_DIALOGS} 1)
execute_process(COMMAND "${VICTUS_CLI}" new "${WORK}/game" --template 2d RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "victus new failed (${rc}):\n${out}\n${err}")
endif()

# Each kind fails from frame 5; --frames 100000 would never end without detection.
foreach(kind swapchain submit commandbuffer)
    set(ENV{VICTUS_GPU_FAIL} "${kind}@5")
    foreach(exe "${VICTUS_PLAYER}" "${VICTUS_EDITOR}")
        if(exe STREQUAL VICTUS_EDITOR)
            set(args "${WORK}/game" --frames 100000 --width 640 --height 360 --data-dir "${WORK}/editor-data")
        else()
            set(args "${WORK}/game" --frames 100000 --device-recoveries 0)
        endif()
        get_filename_component(name "${exe}" NAME_WE)
        execute_process(COMMAND "${exe}" ${args} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
        if(NOT rc EQUAL 3)
            message(FATAL_ERROR "${name} with VICTUS_GPU_FAIL=${kind}@5 exited with '${rc}', expected 3:\n${out}\n${err}")
        endif()
        if(NOT "${out}${err}" MATCHES "GPU device lost: [a-z ]+ failed [0-9]+ times in a row")
            message(FATAL_ERROR "${name} (${kind}) did not report the lost device:\n${out}\n${err}")
        endif()
        message(STATUS "${name} ${kind}: exit 3, device loss reported")
    endforeach()
endforeach()
unset(ENV{VICTUS_GPU_FAIL})

# Recovery: the injected loss happens once (recreated devices ignore VICTUS_GPU_FAIL). The player
# finishes its fixed-step run and the last frame matches a run without any failure.
execute_process(COMMAND "${VICTUS_PLAYER}" "${WORK}/game" --frames 90 --screenshot "${WORK}/clean.png"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "victus_player reference run failed (${rc}):\n${out}\n${err}")
endif()
foreach(kind swapchain submit commandbuffer)
    set(ENV{VICTUS_GPU_FAIL} "${kind}@5")
    execute_process(COMMAND "${VICTUS_PLAYER}" "${WORK}/game" --frames 90 --screenshot "${WORK}/recovered_${kind}.png"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
    unset(ENV{VICTUS_GPU_FAIL})
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "victus_player did not recover from an injected ${kind} loss (${rc}):\n${out}\n${err}")
    endif()
    if(NOT "${out}${err}" MATCHES "GPU device recreated")
        message(FATAL_ERROR "victus_player (${kind}) finished without recreating the device:\n${out}\n${err}")
    endif()
    # The simulation keeps ticking while frames fail, so moving objects (the spinning coins) are
    # a little further along; the rest of the frame (UI, text, sprites, background) must match.
    # Measured on lavapipe: FLIP mean 0.002-0.010, SSIM 0.991-0.998. A blank or partial frame
    # after recovery scores far worse.
    execute_process(COMMAND "${VICTUS_CLI}" compare "${WORK}/clean.png" "${WORK}/recovered_${kind}.png" --max-flip 0.03 --min-ssim 0.97
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "victus_player (${kind}): the frame after recovery does not match the clean run (${rc}):\n${out}\n${err}")
    endif()
    message(STATUS "victus_player ${kind}: device recreated, final frame matches a clean run")
endforeach()
