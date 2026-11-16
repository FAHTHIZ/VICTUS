# SPDX-License-Identifier: Apache-2.0
add_executable(victus_render_showcase render_showcase/main.cpp)
target_link_libraries(victus_render_showcase PRIVATE VICTUS::Render VICTUS::Platform)
victus_configure_target(victus_render_showcase)
