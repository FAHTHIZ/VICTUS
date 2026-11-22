# SPDX-License-Identifier: Apache-2.0
# Text shaping (HarfBuzz), bidirectional layout (SheenBidi) and glyph rasterisation (ADR-0224).
victus_add_module(text
    SOURCES src/text.cpp
    DEPS VICTUS::Core
    PRIVATE_DEPS victus_harfbuzz victus_sheenbidi)
target_include_directories(victus_text SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/stb)
