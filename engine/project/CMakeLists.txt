# SPDX-License-Identifier: Apache-2.0
# Starter characters (docs/CHARACTERS.md): the committed GLBs are embedded at build time so
# templates can write them into new projects (ADR-0069).
set(_victus_characters ${CMAKE_CURRENT_SOURCE_DIR}/data/characters)
set(_victus_character_sources)
# .incbin keeps multi-megabyte GLBs fast to compile; MSVC falls back to a byte-array literal.
if(MSVC)
    set(_victus_embed_mode hex)
else()
    set(_victus_embed_mode incbin)
endif()
foreach(_v orion nova)
    string(SUBSTRING ${_v} 0 1 _first)
    string(TOUPPER ${_first} _first)
    string(SUBSTRING ${_v} 1 -1 _rest)
    set(_out ${CMAKE_CURRENT_BINARY_DIR}/generated/character_${_v}_glb.cpp)
    add_custom_command(OUTPUT ${_out}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${_victus_characters}/victus_${_v}.glb -DOUTPUT=${_out}
                -DSYMBOL=k${_first}${_rest}Glb -DNAMESPACE=victus::project::embedded -DMODE=${_victus_embed_mode}
                -P ${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake
        DEPENDS ${_victus_characters}/victus_${_v}.glb ${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake
        COMMENT "Embedding victus_${_v}.glb"
        VERBATIM)
    set_source_files_properties(${_out} PROPERTIES OBJECT_DEPENDS ${_victus_characters}/victus_${_v}.glb)
    list(APPEND _victus_character_sources ${_out})
endforeach()

# Starter materials and sky (data/materials/LICENSE.md, ADR-0197): CC0 texture sets and an HDR sky
# the 3D templates write into new projects. Symbols are kStarter_<folder>_<file>.
set(_victus_starter_data ${CMAKE_CURRENT_SOURCE_DIR}/data)
set(_victus_starter_files
    materials/grass/color.jpg materials/grass/normal.jpg materials/grass/orm.jpg
    materials/rock/color.jpg materials/rock/normal.jpg materials/rock/orm.jpg
    materials/planks/color.jpg materials/planks/normal.jpg materials/planks/orm.jpg
    materials/metal/color.jpg materials/metal/normal.jpg materials/metal/orm.jpg
    materials/paving/color.jpg materials/paving/normal.jpg materials/paving/orm.jpg
    sky/partly_cloudy.hdr)
foreach(_f ${_victus_starter_files})
    string(REGEX REPLACE "^(materials|sky)/" "" _sym ${_f})
    string(REGEX REPLACE "[/.]" "_" _sym ${_sym})
    set(_out ${CMAKE_CURRENT_BINARY_DIR}/generated/starter_${_sym}.cpp)
    add_custom_command(OUTPUT ${_out}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${_victus_starter_data}/${_f} -DOUTPUT=${_out}
                -DSYMBOL=kStarter_${_sym} -DNAMESPACE=victus::project::embedded -DMODE=${_victus_embed_mode}
                -P ${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake
        DEPENDS ${_victus_starter_data}/${_f} ${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake
        COMMENT "Embedding ${_f}"
        VERBATIM)
    set_source_files_properties(${_out} PROPERTIES OBJECT_DEPENDS ${_victus_starter_data}/${_f})
    list(APPEND _victus_character_sources ${_out})
endforeach()

victus_add_module(project
    SOURCES src/project.cpp src/templates.cpp src/sprite_art.cpp src/characters.cpp src/character_validate.cpp
            src/starter_materials.cpp ${_victus_character_sources}
    DEPS VICTUS::Assets VICTUS::Scene VICTUS::Gameplay VICTUS::Script VICTUS::Physics VICTUS::Audio VICTUS::Animation VICTUS::Particles VICTUS::Gameui VICTUS::Navigation VICTUS::Traffic VICTUS::Ai VICTUS::Sequencer VICTUS::Net VICTUS::Tilemap VICTUS::Terrain VICTUS::World)
