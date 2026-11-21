# SPDX-License-Identifier: Apache-2.0
# Asset Store (ADR-0174): provider catalogue, HTTPS transport and the project installer. Studio
# and CLI tooling only; exported games never link it.
victus_add_module(library
    SOURCES
        src/library.cpp
        src/http.cpp
        src/install.cpp
        src/polyhaven.cpp
        src/ambientcg.cpp
        src/smithsonian.cpp
        src/sketchfab.cpp
        src/bigsoundbank.cpp
        src/nps_sounds.cpp
        src/kenney.cpp
    DEPS VICTUS::Assets
    PRIVATE_DEPS victus_stb)
