// SPDX-License-Identifier: Apache-2.0
#include "victus/animation/sprite.hpp"
#include "victus/animation/animation.hpp"

#include "victus/core/fs.hpp"
#include "victus/core/log.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace victus::animation {

using json::Value;
using scene::Entity;
using scene::Scene;

namespace {

Error bad(std::string_view what) { return make_error(ErrorCode::ParseError, std::format("sprite sheet: {}", what)); }

bool read_count(const Value& v, std::uint32_t& out) {
    if (!v.is_number()) return false;
    const double d = v.as_number();
    if (d < 1 || d > kMaxSpriteSheetFrames || d != std::floor(d)) return false;
    out = static_cast<std::uint32_t>(d);
    return true;
}

bool read_frame(const Value& v, std::uint32_t frames, std::uint32_t& out) {
    if (!v.is_number()) return false;
    const double d = v.as_number();
    if (d < 0 || d >= frames || d != std::floor(d)) return false;
    out = static_cast<std::uint32_t>(d);
    return true;
}

} // namespace

math::Vec4 SpriteSheet::frame_uv(std::uint32_t frame) const noexcept {
    const std::uint32_t cols = std::max(columns, 1u), rws = std::max(rows, 1u);
    frame = std::min(frame, cols * rws - 1);
    const float c = static_cast<float>(frame % cols), r = static_cast<float>(frame / cols);
    const float w = 1.0f / static_cast<float>(cols), h = 1.0f / static_cast<float>(rws);
    return {c * w, r * h, (c + 1) * w, (r + 1) * h};
}

const SpriteClip* SpriteSheet::find(std::string_view clip) const noexcept {
    for (const auto& c : clips)
        if (c.name == clip) return &c;
    return nullptr;
}

Result<SpriteSheet> parse_sprite_sheet(const Value& doc) {
    if (!doc.is_object() || doc["format"].as_string_or("") != "victus.spritesheet") return bad("not a victus.spritesheet document");
    const std::int64_t version = doc["version"].as_int(0);
    if (version < 1 || version > kSpriteSheetVersion)
        return make_error(ErrorCode::VersionMismatch, std::format("sprite sheet: unsupported version {}", version));
    SpriteSheet s;
    if (const Value* t = doc.find("texture"); t && !t->is_null()) {
        if (!t->is_string() || !Uuid::parse(t->as_string(), s.texture)) return bad("'texture' must be a UUID string");
    }
    if (!read_count(doc["columns"], s.columns)) return bad("'columns' must be a whole number from 1");
    if (!read_count(doc["rows"], s.rows)) return bad("'rows' must be a whole number from 1");
    if (static_cast<std::uint64_t>(s.columns) * s.rows > kMaxSpriteSheetFrames)
        return bad(std::format("grid {}x{} has more than {} frames", s.columns, s.rows, kMaxSpriteSheetFrames));
    const std::uint32_t frames = s.frame_count();
    if (const Value* clips = doc.find("clips"); clips && !clips->is_null()) {
        if (!clips->is_object()) return bad("'clips' must be an object");
        for (const auto& [name, c] : clips->as_object()) {
            if (name.empty()) return bad("clip names must not be empty");
            if (!c.is_object()) return bad(std::format("clip '{}' must be an object", name));
            SpriteClip clip;
            clip.name = name;
            if (const Value* list = c.find("frames"); list) {
                if (!list->is_array() || list->size() == 0) return bad(std::format("clip '{}': 'frames' must be a non-empty array", name));
                for (const Value& f : list->as_array()) {
                    std::uint32_t idx = 0;
                    if (!read_frame(f, frames, idx)) return bad(std::format("clip '{}': frame {} is not in 0..{}", name, json::stringify(f, false), frames - 1));
                    clip.frames.push_back(idx);
                }
            } else {
                std::uint32_t from = 0, to = 0;
                if (!read_frame(c["from"], frames, from) || !read_frame(c["to"], frames, to))
                    return bad(std::format("clip '{}' needs 'frames' or 'from'/'to' in 0..{}", name, frames - 1));
                if (from <= to)
                    for (std::uint32_t i = from; i <= to; ++i) clip.frames.push_back(i);
                else
                    for (std::uint32_t i = from + 1; i-- > to;) clip.frames.push_back(i);
            }
            if (const Value* fps = c.find("fps"); fps) {
                if (!fps->is_number() || !(fps->as_number() > 0) || fps->as_number() > 1000)
                    return bad(std::format("clip '{}': 'fps' must be in (0, 1000]", name));
                clip.fps = fps->as_float();
            }
            if (const Value* loop = c.find("loop"); loop) {
                if (!loop->is_bool()) return bad(std::format("clip '{}': 'loop' must be true or false", name));
                clip.loop = loop->as_bool();
            }
            if (s.find(name)) return bad(std::format("clip '{}' is defined twice", name));
            s.clips.push_back(std::move(clip));
        }
    }
    return s;
}

Value to_json(const SpriteSheet& s) {
    Value v = Value::object();
    v.set("format", "victus.spritesheet");
    v.set("version", kSpriteSheetVersion);
    v.set("texture", s.texture.is_nil() ? Value{} : Value(s.texture.to_string()));
    v.set("columns", s.columns);
    v.set("rows", s.rows);
    Value clips = Value::object();
    for (const auto& c : s.clips) {
        Value cv = Value::object();
        // Contiguous ascending runs are written as ranges, everything else as a frame list.
        bool range = c.frames.size() > 1;
        for (std::size_t i = 1; i < c.frames.size() && range; ++i) range = c.frames[i] == c.frames[i - 1] + 1;
        if (range) {
            cv.set("from", c.frames.front());
            cv.set("to", c.frames.back());
        } else {
            Value list = Value::array();
            for (const std::uint32_t f : c.frames) list.push(f);
            cv.set("frames", std::move(list));
        }
        cv.set("fps", c.fps);
        cv.set("loop", c.loop);
        clips.set(c.name, std::move(cv));
    }
    v.set("clips", std::move(clips));
    return v;
}

Result<SpriteSheet> load_sprite_sheet_file(const std::filesystem::path& path) {
    auto text = fs::read_text(path);
    if (!text) return std::move(text).error();
    auto doc = json::parse(*text);
    if (!doc) return make_error(ErrorCode::ParseError, std::format("{}: {}", path.filename().string(), doc.error().message));
    auto sheet = parse_sprite_sheet(*doc);
    if (!sheet) return make_error(sheet.error().code, std::format("{}: {}", path.filename().string(), sheet.error().message));
    return sheet;
}

Status save_sprite_sheet_file(const std::filesystem::path& path, const SpriteSheet& sheet) {
    return fs::write_text_atomic(path, json::stringify(to_json(sheet), true) + "\n");
}

std::uint32_t clip_frame_at(const SpriteClip& clip, float time) noexcept {
    const auto n = static_cast<std::int64_t>(clip.frames.size());
    if (n <= 1 || !(clip.fps > 0) || !std::isfinite(time)) return 0;
    auto i = static_cast<std::int64_t>(std::floor(static_cast<double>(time) * clip.fps));
    if (clip.loop) {
        i %= n;
        if (i < 0) i += n;
    } else {
        i = std::clamp<std::int64_t>(i, 0, n - 1);
    }
    return static_cast<std::uint32_t>(i);
}

} // namespace victus::animation
