# SPDX-License-Identifier: Apache-2.0
add_executable(victus_cli main.cpp)
set_target_properties(victus_cli PROPERTIES OUTPUT_NAME victus)
target_link_libraries(victus_cli PRIVATE VICTUS::Library VICTUS::Player VICTUS::Assets VICTUS::Project VICTUS::Script VICTUS::Gameplay VICTUS::Net VICTUS::World VICTUS::Runtime)
victus_configure_target(victus_cli)
# `victus export` ships victus_player from the same folder by default.
add_dependencies(victus_cli victus_player_app)
