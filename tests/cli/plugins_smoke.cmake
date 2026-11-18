# SPDX-License-Identifier: Apache-2.0
# Native plugins end to end (ADR-0078): scaffold a plugin with `victus plugins --new`, build it
# with CMake like a user would, then list/check/load it, call it from Luau in `victus simulate`,
# and export it next to the pack.
#   cmake -DVICTUS_CLI=<victus> -DWORK=<dir> -DC_COMPILER=<cc> -P plugins_smoke.cmake
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

function(run_ok)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "command failed (${rc}): ${ARGN}\n${out}\n${err}")
    endif()
    set(LAST_OUTPUT "${out}" PARENT_SCOPE)
endfunction()
function(expect_output pattern)
    if(NOT LAST_OUTPUT MATCHES "${pattern}")
        message(FATAL_ERROR "output does not match '${pattern}':\n${LAST_OUTPUT}")
    endif()
endfunction()

set(GAME "${WORK}/game")
run_ok("${VICTUS_CLI}" new "${GAME}" --template simulation --name PluginGame)

# Before the plugin exists, Luau sees no plugin functions.
file(READ "${GAME}/assets/scripts/orbit_camera.luau" script)
string(REPLACE "function Orbit:start()\n" "function Orbit:start()\n    assert(not plugins.has(\"hello.greet\"))\n" script "${script}")
file(WRITE "${GAME}/assets/scripts/orbit_camera.luau" "${script}")
run_ok("${VICTUS_CLI}" simulate "${GAME}" --frames 2)
expect_output(" 1 script instances, 0 script errors")

run_ok("${VICTUS_CLI}" plugins "${GAME}" --new hello)
execute_process(COMMAND "${VICTUS_CLI}" plugins "${GAME}" --new hello RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
if(rc EQUAL 0)
    message(FATAL_ERROR "--new over an existing plugin must fail")
endif()
execute_process(COMMAND "${VICTUS_CLI}" plugins "${GAME}" --new "Bad-Name" RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
if(rc EQUAL 0)
    message(FATAL_ERROR "--new with an invalid name must fail")
endif()
foreach(f hello.vplugin src/hello.c CMakeLists.txt include/victus/plugin/victus_plugin.h)
    if(NOT EXISTS "${GAME}/plugins/hello/${f}")
        message(FATAL_ERROR "scaffold is missing ${f}")
    endif()
endforeach()

# Unbuilt: listed with a warning, and `check` warns without failing.
run_ok("${VICTUS_CLI}" plugins "${GAME}")
expect_output("hello +0\\.1\\.0 +warning: bin/hello\\.[a-z]+ is not built")
run_ok("${VICTUS_CLI}" check "${GAME}")
expect_output("plugin 'hello' library bin/hello\\.[a-z]+ is not built")

# Build the scaffold exactly as the printed instructions say.
set(build_args)
if(C_COMPILER)
    list(APPEND build_args -DCMAKE_C_COMPILER=${C_COMPILER})
endif()
run_ok("${CMAKE_COMMAND}" -S "${GAME}/plugins/hello" -B "${WORK}/hello_build" ${build_args})
run_ok("${CMAKE_COMMAND}" --build "${WORK}/hello_build" --config Release)

run_ok("${VICTUS_CLI}" plugins "${GAME}")
expect_output("hello +0\\.1\\.0 +ready")
expect_output("1 plugins, 0 problems")
run_ok("${VICTUS_CLI}" plugins "${GAME}" --load)
expect_output("hello +0\\.1\\.0 +loaded")
expect_output("system +plugin\\.hello\\.spin")
expect_output("function hello\\.greet")
run_ok("${VICTUS_CLI}" check "${GAME}")
if(LAST_OUTPUT MATCHES "plugin")
    message(FATAL_ERROR "a built plugin should check clean:\n${LAST_OUTPUT}")
endif()

# Luau calls the native function through plugins.call.
file(READ "${GAME}/assets/scripts/orbit_camera.luau" script)
string(REPLACE "assert(not plugins.has(\"hello.greet\"))"
    "assert(plugins.has(\"hello.greet\"))\n    assert(plugins.call(\"hello.greet\", { who = \"world\" }) == \"hello from hello\")"
    script "${script}")
file(WRITE "${GAME}/assets/scripts/orbit_camera.luau" "${script}")
run_ok("${VICTUS_CLI}" simulate "${GAME}" --frames 5)
expect_output(" 1 script instances, 0 script errors")
expect_output("plugins: 1 loaded")

# A broken dependency is a `check` error and a load failure, without breaking the others.
file(MAKE_DIRECTORY "${GAME}/plugins/needy")
file(WRITE "${GAME}/plugins/needy/needy.vplugin"
    "{\"name\":\"needy\",\"version\":\"1.0.0\",\"api\":\"1.0\",\"dependencies\":{\"absent\":\"1.0\"}}")
execute_process(COMMAND "${VICTUS_CLI}" check "${GAME}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT out MATCHES "plugin 'needy' cannot load: missing dependency 'absent'")
    message(FATAL_ERROR "check should fail on the missing dependency (${rc}):\n${out}\n${err}")
endif()
run_ok("${VICTUS_CLI}" simulate "${GAME}" --frames 2)
expect_output(" 1 script instances, 0 script errors")
file(REMOVE_RECURSE "${GAME}/plugins/needy")

# Export ships the manifest and library next to the pack.
run_ok("${VICTUS_CLI}" export "${GAME}" --out "${WORK}/export" --pack-only)
expect_output("exported 1 native plugin")
file(GLOB libs "${WORK}/export/game/plugins/hello/bin/hello.*")
if(NOT libs OR NOT EXISTS "${WORK}/export/game/plugins/hello/hello.vplugin")
    message(FATAL_ERROR "export is missing the plugin files")
endif()
file(READ "${WORK}/export/export.json" manifest)
if(NOT manifest MATCHES "\"plugins\": \\[[^]]*\"hello\"")
    message(FATAL_ERROR "export.json does not list the plugin:\n${manifest}")
endif()
message(STATUS "plugins smoke passed")
