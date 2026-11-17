// SPDX-License-Identifier: Apache-2.0
// 2D sprite animation: sprite sheets (.vsheet) slice a texture into a grid of frames and name
// clips (frame lists with a rate and loop flag); SpriteAnimator plays them on a Sprite.
//
// .vsheet (JSON):
//   {
//     "format": "victus.spritesheet", "version": 1,
//     "texture": "<uuid of the sheet texture>",
//     "columns": 4, "rows": 2,                      // grid, frames numbered row by row from top-left
//     "clips": {
//       "idle": { "frames": [0, 1, 2, 1], "fps": 6 },          // explicit frames (repeats allowed)
//       "run":  { "from": 4, "to": 7, "fps": 12, "loop": true } // inclusive range (to < from plays back)
//     }
//   }
#pragma once

#include "victus/core/json.hpp"
#include "victus/core/math.hpp"
#include "victus/core/result.hpp"
#include "victus/core/hash.hpp"
#include "victus/scene/scene.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace victus::animation {

inline constexpr std::string_view kSpriteSheetExtension = ".vsheet";
inline constexpr std::int64_t kSpriteSheetVersion = 1;
inline constexpr std::uint32_t kMaxSpriteSheetFrames = 65536;

struct SpriteClip {
    std::string name;
    std::vector<std::uint32_t> frames; // sheet frame indices, at least one
    float fps{10.0f};
    bool loop{true};
    [[nodiscard]] float duration() const noexcept { return fps > 0 ? static_cast<float>(frames.size()) / fps : 0.0f; }
};

struct SpriteSheet {
    Uuid texture;
    std::uint32_t columns{1}, rows{1};
    std::vector<SpriteClip> clips;

    [[nodiscard]] std::uint32_t frame_count() const noexcept { return columns * rows; }
    // u0, v0 (top), u1, v1 (bottom) of a frame (clamped to the grid), as render::Sprite::uv.
    [[nodiscard]] math::Vec4 frame_uv(std::uint32_t frame) const noexcept;
    [[nodiscard]] const SpriteClip* find(std::string_view clip) const noexcept;
};

// Strict parse: unknown format/version, bad types, empty or out-of-range frames, fps <= 0 and
// grids over kMaxSpriteSheetFrames are errors naming the field.
[[nodiscard]] Result<SpriteSheet> parse_sprite_sheet(const json::Value& doc);
[[nodiscard]] json::Value to_json(const SpriteSheet& sheet);
[[nodiscard]] Result<SpriteSheet> load_sprite_sheet_file(const std::filesystem::path& path);
[[nodiscard]] Status save_sprite_sheet_file(const std::filesystem::path& path, const SpriteSheet& sheet);

// Plays clips of a sprite sheet on this entity's Sprite: each update sets the Sprite's texture
// to the sheet's and its uv to the current frame.
struct SpriteAnimatorComponent {
    Uuid sheet;            // .vsheet asset
    std::string clip;      // clip to play; empty shows `frame` (a static atlas cell)
    bool playing{true};
    float speed{1.0f};     // playback rate (negative plays backwards)
    float time{0.0f};      // seconds into the clip; restarts when `clip` changes
    std::uint32_t frame{0}; // sheet frame shown when `clip` is empty
};

struct SpriteAnimatorStatus {
    bool valid{false};           // entity has a SpriteAnimator whose sheet resolved
    std::string clip;            // playing clip ("" when showing a static frame)
    std::uint32_t frame{0};      // sheet frame shown
    std::uint32_t clip_frame{0}; // index into the clip's frame list
    float time{0}, duration{0};
    bool playing{false}, looping{false}, finished{false};
};

// Returns the sheet of a .vsheet asset, or null when it does not exist or is invalid.
using SheetProvider = std::function<std::shared_ptr<const SpriteSheet>(const Uuid& sheet)>;

// Clip frame index shown `time` seconds into `clip` (loops wrap, others clamp; negative times
// count from the end of looping clips).
[[nodiscard]] std::uint32_t clip_frame_at(const SpriteClip& clip, float time) noexcept;

} // namespace victus::animation
