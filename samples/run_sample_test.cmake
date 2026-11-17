# SPDX-License-Identifier: Apache-2.0
# Validates a sample game project end to end with the `victus` CLI:
#   1. copies the project to WORK (so derived data never lands in the source tree),
#   2. `victus check` must report 0 problems,
#   3. `victus simulate` runs SCENE for FRAMES with SEED and saves the final scene,
#   4. the saved scene must match every regular expression in EXPECT (a ;-list).
# Usage: cmake -DVICTUS=<cli> -DSOURCE=<project> -DWORK=<dir> [-DSCENE=..] [-DFRAMES=..]
#              [-DSEED=..] [-DEXPECT=..] -P run_sample_test.cmake
foreach(v VICTUS SOURCE WORK)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "${v} is required")
    endif()
endforeach()
if(NOT DEFINED FRAMES)
    set(FRAMES 600)
endif()
if(NOT DEFINED SEED)
    set(SEED 1)
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
file(COPY "${SOURCE}/" DESTINATION "${WORK}/project" PATTERN ".victus" EXCLUDE)

execute_process(COMMAND "${VICTUS}" check "${WORK}/project" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message(STATUS "${out}${err}")
if(NOT rc EQUAL 0 OR NOT out MATCHES " 0 problems")
    message(FATAL_ERROR "victus check failed (exit ${rc})")
endif()

set(args simulate "${WORK}/project" --frames ${FRAMES} --seed ${SEED} --save "${WORK}/final.vscene")
if(DEFINED SCENE)
    list(APPEND args --scene "${SCENE}")
endif()
execute_process(COMMAND "${VICTUS}" ${args} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message(STATUS "${out}${err}")
if(NOT rc EQUAL 0 OR NOT out MATCHES " 0 script errors")
    message(FATAL_ERROR "victus simulate failed (exit ${rc})")
endif()

file(READ "${WORK}/final.vscene" final)
foreach(pattern IN LISTS EXPECT)
    if(NOT final MATCHES "${pattern}")
        message(FATAL_ERROR "the final scene does not match '${pattern}'")
    endif()
    message(STATUS "final scene matches '${pattern}'")
endforeach()
