// SPDX-License-Identifier: Apache-2.0
// Procedural sprite art for the No-Code Platformer template: small anti-aliased PNGs drawn in
// code, so the template ships finished-looking characters and props without binary assets.
#pragma once

#include <string>
#include <vector>

namespace victus::project::detail {

struct SpriteArt {
    const char* name; // file stem under assets/textures/nocode/
    const char* uuid; // stable asset id, written to the .meta sidecar
    std::string png;  // encoded image
};

// Every sprite the template uses, in a fixed order. Encoded once per call.
[[nodiscard]] std::vector<SpriteArt> nocode_sprite_art();

// The UUID of a sprite by name (nullptr when unknown).
[[nodiscard]] const char* nocode_sprite_uuid(const char* name);

} // namespace victus::project::detail
