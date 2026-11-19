# SPDX-License-Identifier: Apache-2.0
victus_add_module(gameplay
    SOURCES src/gameplay.cpp src/input_map.cpp src/save.cpp src/capture.cpp src/framework.cpp
    DEPS VICTUS::Scene)
