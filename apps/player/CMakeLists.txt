# SPDX-License-Identifier: Apache-2.0
# The library target is victus_player (engine/player); the executable keeps that file name.
add_executable(victus_player_app main.cpp)
set_target_properties(victus_player_app PROPERTIES OUTPUT_NAME victus_player)
target_link_libraries(victus_player_app PRIVATE VICTUS::Player)
victus_configure_target(victus_player_app)
