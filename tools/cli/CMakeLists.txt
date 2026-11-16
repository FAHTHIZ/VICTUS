# SPDX-License-Identifier: Apache-2.0
add_executable(victus_cli main.cpp)
set_target_properties(victus_cli PROPERTIES OUTPUT_NAME victus)
target_link_libraries(victus_cli PRIVATE VICTUS::Player VICTUS::Assets VICTUS::Project VICTUS::Script VICTUS::Gameplay)
victus_configure_target(victus_cli)
