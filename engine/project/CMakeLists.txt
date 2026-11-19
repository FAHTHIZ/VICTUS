# SPDX-License-Identifier: Apache-2.0
# Starter characters (docs/CHARACTERS.md): the committed GLBs are embedded at build time so
# templates can write them into new projects (ADR-0069).
set(_victus_characters ${CMAKE_CURRENT_SOURCE_DIR}/data/characters)
set(_victus_character_sources)
foreach(_v 01 02)
    set(_out ${CMAKE_CURRENT_BINARY_DIR}/generated/vanguard_${_v}_glb.cpp)
    add_custom_command(OUTPUT ${_out}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${_victus_characters}/victus_vanguard_${_v}.glb -DOUTPUT=${_out}
                -DSYMBOL=kVanguard${_v}Glb -DNAMESPACE=victus::project::embedded
                -P ${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake
        DEPENDS ${_victus_characters}/victus_vanguard_${_v}.glb ${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake
        COMMENT "Embedding victus_vanguard_${_v}.glb"
        VERBATIM)
    list(APPEND _victus_character_sources ${_out})
endforeach()

victus_add_module(project
    SOURCES src/project.cpp src/templates.cpp src/characters.cpp ${_victus_character_sources}
    DEPS VICTUS::Assets VICTUS::Scene VICTUS::Gameplay VICTUS::Script VICTUS::Physics VICTUS::Audio VICTUS::Animation VICTUS::Particles VICTUS::Gameui VICTUS::Navigation VICTUS::Ai VICTUS::Sequencer VICTUS::Net VICTUS::Tilemap VICTUS::Terrain VICTUS::World)
