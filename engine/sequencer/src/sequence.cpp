// SPDX-License-Identifier: Apache-2.0
// Sequence documents: parsing, validation, serialization and channel sampling.
#include "victus/audio/audio.hpp"
#include "victus/sequencer/sequencer.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

namespace victus::sequencer {

namespace {

struct InterpName {
    Interp interp;
    std::string_view name;
};
constexpr InterpName kInterps[] = {
    {Interp::Step, "step"}, {Interp::Linear, "linear"}, {Interp::Smooth, "smooth"}, {Interp::Cubic, "cubic"}};

struct TrackName {
    TrackType type;
    std::string_view name;
};
constexpr TrackName kTracks[] = {{TrackType::Transform, "transform"},
                                 {TrackType::Animation, "animation"},
                                 {TrackType::Audio, "audio"},
                                 {TrackType::Event, "event"},
                                 {TrackType::Camera, "camera"}};

Error fail(const std::string& where, const std::string& what) {
    return make_error(ErrorCode::ParseError, std::format("sequence: {}: {}", where, what));
}

bool finite(float f) noexcept { return std::isfinite(f); }

Result<float> key_time(const json::Value& k, const std::string& where) {
    const json::Value* t = k.find("t");
    if (!t) t = k.find("time");
    if (!t || !t->is_number()) return fail(where, "missing key time \"t\"");
    const float v = t->as_float();
    if (!finite(v) || v < 0) return fail(where, "key time must be a finite number >= 0");
    return v;
}

Result<Interp> key_interp(const json::Value& k, const std::string& where) {
    const json::Value* i = k.find("interp");
    if (!i) return Interp::Linear;
    if (!i->is_string()) return fail(where, "\"interp\" must be a string");
    if (auto r = interp_from_name(i->as_string())) return *r;
    return fail(where, "unknown interp \"" + i->as_string() + "\" (step, linear, smooth or cubic)");
}

Result<math::Vec3> vec3_of(const json::Value* v, const std::string& where) {
    if (!v || !v->is_array() || v->size() != 3) return fail(where, "value must be [x, y, z]");
    math::Vec3 r{(*v)[0].as_float(NAN), (*v)[1].as_float(NAN), (*v)[2].as_float(NAN)};
    if (!finite(r.x) || !finite(r.y) || !finite(r.z)) return fail(where, "value must be three finite numbers");
    return r;
}

// [pitch, yaw, roll] degrees or a quaternion [x, y, z, w].
Result<math::Quat> quat_of(const json::Value* v, const std::string& where) {
    if (v && v->is_array() && v->size() == 3) {
        auto e = vec3_of(v, where);
        if (!e) return std::move(e).error();
        return math::Quat::euler(math::radians(e->x), math::radians(e->y), math::radians(e->z));
    }
    if (!v || !v->is_array() || v->size() != 4)
        return fail(where, "rotation must be [pitch, yaw, roll] degrees or a quaternion [x, y, z, w]");
    math::Quat q{(*v)[0].as_float(NAN), (*v)[1].as_float(NAN), (*v)[2].as_float(NAN), (*v)[3].as_float(NAN)};
    if (!finite(q.x) || !finite(q.y) || !finite(q.z) || !finite(q.w)) return fail(where, "quaternion must be finite");
    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len < 1e-6f) return fail(where, "quaternion must not be zero");
    return math::normalize(q);
}

const json::Array* key_array(const json::Value& track, std::string_view field, const std::string& where,
                             std::optional<Error>& err) {
    const json::Value* a = track.find(field);
    if (!a) return nullptr;
    if (!a->is_array()) {
        err = fail(where, std::format("\"{}\" must be an array of keys", field));
        return nullptr;
    }
    if (a->size() > Sequence::kMaxKeysPerChannel) {
        err = make_error(ErrorCode::LimitExceeded, std::format("sequence: {}: more than {} keys in \"{}\"", where,
                                                               Sequence::kMaxKeysPerChannel, field));
        return nullptr;
    }
    return &a->as_array();
}

template <class K> void sort_keys(std::vector<K>& keys) {
    std::stable_sort(keys.begin(), keys.end(), [](const K& a, const K& b) { return a.time < b.time; });
}

template <class T, class Fn>
Status parse_channel(const json::Value& track, std::string_view field, const std::string& where,
                     std::vector<Key<T>>& out, Fn value) {
    std::optional<Error> err;
    const json::Array* keys = key_array(track, field, where, err);
    if (!keys) {
        if (err) return std::move(*err);
        return {};
    }
    for (std::size_t i = 0; i < keys->size(); ++i) {
        const std::string at = std::format("{}.{}[{}]", where, field, i);
        const json::Value& k = (*keys)[i];
        if (!k.is_object()) return fail(at, "a key must be an object");
        auto t = key_time(k, at);
        if (!t) return std::move(t).error();
        auto in = key_interp(k, at);
        if (!in) return std::move(in).error();
        auto v = value(k.find("v") ? k.find("v") : k.find("value"), at);
        if (!v) return std::move(v).error();
        out.push_back({*t, *v, *in});
    }
    sort_keys(out);
    return {};
}

template <class K, class Fn>
Status parse_keys(const json::Value& track, const std::string& where, std::vector<K>& out, Fn fill) {
    std::optional<Error> err;
    const json::Array* keys = key_array(track, "keys", where, err);
    if (!keys) {
        if (err) return std::move(*err);
        return {};
    }
    for (std::size_t i = 0; i < keys->size(); ++i) {
        const std::string at = std::format("{}.keys[{}]", where, i);
        const json::Value& k = (*keys)[i];
        if (!k.is_object()) return fail(at, "a key must be an object");
        auto t = key_time(k, at);
        if (!t) return std::move(t).error();
        K key;
        key.time = *t;
        if (auto st = fill(k, key, at); !st) return st;
        out.push_back(std::move(key));
    }
    sort_keys(out);
    return {};
}

float number_or(const json::Value& k, std::string_view field, float fallback) {
    const json::Value* v = k.find(field);
    if (!v || !v->is_number()) return fallback;
    const float f = v->as_float();
    return finite(f) ? f : fallback;
}

json::Value vec_json(math::Vec3 v) {
    json::Value a = json::Value::array();
    a.push(v.x);
    a.push(v.y);
    a.push(v.z);
    return a;
}

json::Value quat_json(math::Quat q) {
    json::Value a = json::Value::array();
    a.push(q.x);
    a.push(q.y);
    a.push(q.z);
    a.push(q.w);
    return a;
}

template <class T, class Fn>
void write_channel(json::Value& track, std::string_view field, const std::vector<Key<T>>& keys, Fn value) {
    if (keys.empty()) return;
    json::Value arr = json::Value::array();
    for (const auto& k : keys) {
        json::Value o = json::Value::object();
        o.set("t", k.time);
        o.set("v", value(k.value));
        if (k.interp != Interp::Linear) o.set("interp", interp_name(k.interp));
        arr.push(std::move(o));
    }
    track.set(field, std::move(arr));
}

// Index of the last key with time <= t (keys sorted), or -1 when t is before the first key.
template <class K> std::ptrdiff_t segment(const std::vector<K>& keys, float t) noexcept {
    auto it = std::upper_bound(keys.begin(), keys.end(), t, [](float x, const K& k) { return x < k.time; });
    return (it - keys.begin()) - 1;
}

float ease(float u) noexcept { return u * u * (3.0f - 2.0f * u); }

math::Vec3 catmull_rom(math::Vec3 p0, math::Vec3 p1, math::Vec3 p2, math::Vec3 p3, float u) noexcept {
    const float u2 = u * u, u3 = u2 * u;
    return (p1 * 2.0f + (p2 - p0) * u + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * u2 +
            (p1 * 3.0f - p0 - p2 * 3.0f + p3) * u3) *
           0.5f;
}

} // namespace

std::string_view interp_name(Interp i) noexcept {
    for (const auto& n : kInterps)
        if (n.interp == i) return n.name;
    return "linear";
}

std::optional<Interp> interp_from_name(std::string_view name) noexcept {
    for (const auto& n : kInterps)
        if (n.name == name) return n.interp;
    return std::nullopt;
}

std::string_view track_type_name(TrackType t) noexcept {
    for (const auto& n : kTracks)
        if (n.type == t) return n.name;
    return "?";
}

std::size_t Track::key_count() const noexcept {
    return position.size() + rotation.size() + scale.size() + animation.size() + audio.size() + events.size() +
           cameras.size();
}

float Track::end_time() const noexcept {
    float e = 0;
    auto last = [&](const auto& keys) {
        if (!keys.empty()) e = std::max(e, keys.back().time);
    };
    last(position);
    last(rotation);
    last(scale);
    last(animation);
    last(audio);
    last(events);
    last(cameras);
    return e;
}

Result<std::shared_ptr<const Sequence>> Sequence::from_json(const json::Value& doc, std::string name) {
    if (!doc.is_object()) return make_error(ErrorCode::ParseError, "sequence: expected an object");
    if (const json::Value* v = doc.find("version")) {
        if (!v->is_number() || v->as_int(0) < 1)
            return make_error(ErrorCode::ParseError, "sequence: \"version\" must be a positive integer");
        if (v->as_int(0) > static_cast<std::int64_t>(kSequenceVersion))
            return make_error(ErrorCode::Unsupported,
                              std::format("sequence: version {} is newer than this engine supports ({})", v->as_int(0),
                                          kSequenceVersion));
    }
    auto seq = std::make_shared<Sequence>();
    seq->name = std::move(name);
    const json::Value* tracks = doc.find("tracks");
    if (tracks && !tracks->is_array())
        return make_error(ErrorCode::ParseError, "sequence: \"tracks\" must be an array");
    if (tracks && tracks->size() > kMaxTracks)
        return make_error(ErrorCode::LimitExceeded, std::format("sequence: more than {} tracks", kMaxTracks));
    float end = 0;
    for (std::size_t ti = 0; tracks && ti < tracks->size(); ++ti) {
        const json::Value& tv = (*tracks)[ti];
        const std::string where = std::format("tracks[{}]", ti);
        if (!tv.is_object()) return fail(where, "a track must be an object");
        const json::Value* type = tv.find("type");
        if (!type || !type->is_string()) return fail(where, "missing \"type\"");
        const auto* tn = std::find_if(std::begin(kTracks), std::end(kTracks),
                                      [&](const TrackName& n) { return n.name == type->as_string(); });
        if (tn == std::end(kTracks))
            return fail(where, "unknown track type \"" + type->as_string() +
                                   "\" (transform, animation, audio, event or camera)");
        Track t;
        t.type = tn->type;
        t.name = tv["name"].as_string_or("");
        if (const json::Value* tg = tv.find("target"); tg && !tg->is_string())
            return fail(where, "\"target\" must be a string");
        t.target = tv["target"].as_string_or("");
        t.muted = tv["muted"].as_bool(false);
        Status st;
        switch (t.type) {
        case TrackType::Transform: {
            const std::string space = tv["space"].as_string_or("local");
            if (space != "local" && space != "relative")
                return fail(where, "\"space\" must be \"local\" or \"relative\"");
            t.relative = space == "relative";
            if (st = parse_channel<math::Vec3>(tv, "position", where, t.position, vec3_of); !st)
                return std::move(st).error();
            if (st = parse_channel<math::Quat>(tv, "rotation", where, t.rotation, quat_of); !st)
                return std::move(st).error();
            if (st = parse_channel<math::Vec3>(tv, "scale", where, t.scale, vec3_of); !st) return std::move(st).error();
            break;
        }
        case TrackType::Animation:
            st = parse_keys(tv, where, t.animation,
                            [](const json::Value& k, AnimationKey& a, const std::string& at) -> Status {
                                a.clip = k["clip"].as_string_or("");
                                if (a.clip.empty()) return fail(at, "animation key needs a \"clip\"");
                                a.fade = number_or(k, "fade", -1.0f);
                                a.speed = number_or(k, "speed", 1.0f);
                                a.loop = k["loop"].as_bool(true);
                                return {};
                            });
            break;
        case TrackType::Audio:
            st = parse_keys(tv, where, t.audio, [](const json::Value& k, AudioKey& a, const std::string& at) -> Status {
                a.clip = k["clip"].as_string_or("");
                if (a.clip.empty()) return fail(at, "audio key needs a \"clip\"");
                a.volume = std::max(number_or(k, "volume", 1.0f), 0.0f);
                a.pitch = number_or(k, "pitch", 1.0f);
                if (a.pitch <= 0) return fail(at, "\"pitch\" must be > 0");
                a.bus = k["bus"].as_string_or("Sfx");
                if (!audio::parse_bus(a.bus)) return fail(at, "unknown bus \"" + a.bus + "\"");
                a.spatial = k["spatial"].as_bool(false);
                return {};
            });
            break;
        case TrackType::Event:
            st = parse_keys(tv, where, t.events,
                            [](const json::Value& k, EventKey& ev, const std::string& at) -> Status {
                                ev.name = k["name"].as_string_or("");
                                if (ev.name.empty()) return fail(at, "event key needs a \"name\"");
                                const json::Value* p = k.find("payload");
                                if (p && !p->is_string()) return fail(at, "\"payload\" must be a string");
                                ev.payload = k["payload"].as_string_or("");
                                return {};
                            });
            break;
        case TrackType::Camera:
            st = parse_keys(tv, where, t.cameras,
                            [](const json::Value& k, CameraKey& c, const std::string& at) -> Status {
                                const json::Value* cam = k.find("camera");
                                if (!cam || !cam->is_string()) return fail(at, "camera key needs a \"camera\" target");
                                c.camera = cam->as_string();
                                return {};
                            });
            break;
        }
        if (!st) return std::move(st).error();
        end = std::max(end, t.end_time());
        seq->tracks.push_back(std::move(t));
    }
    if (const json::Value* d = doc.find("duration")) {
        const float dur = d->as_float(NAN);
        if (!d->is_number() || !finite(dur) || dur < 0)
            return make_error(ErrorCode::ParseError, "sequence: \"duration\" must be a finite number >= 0");
        seq->duration = dur;
    } else {
        seq->duration = end;
    }
    return std::shared_ptr<const Sequence>(std::move(seq));
}

Result<std::shared_ptr<const Sequence>> Sequence::parse(std::string_view text, std::string name) {
    auto doc = json::parse(text, {.max_depth = 32, .max_bytes = 64u * 1024u * 1024u, .max_string_bytes = 1u << 20});
    if (!doc) return make_error(ErrorCode::ParseError, "sequence: " + doc.error().message);
    return from_json(*doc, std::move(name));
}

json::Value Sequence::to_json() const {
    json::Value doc = json::Value::object();
    doc.set("version", kSequenceVersion);
    doc.set("duration", duration);
    json::Value arr = json::Value::array();
    for (const Track& t : tracks) {
        json::Value o = json::Value::object();
        o.set("type", track_type_name(t.type));
        if (!t.name.empty()) o.set("name", t.name);
        if (!t.target.empty()) o.set("target", t.target);
        if (t.muted) o.set("muted", true);
        switch (t.type) {
        case TrackType::Transform:
            if (t.relative) o.set("space", "relative");
            write_channel(o, "position", t.position, vec_json);
            write_channel(o, "rotation", t.rotation, quat_json);
            write_channel(o, "scale", t.scale, vec_json);
            break;
        case TrackType::Animation: {
            json::Value keys = json::Value::array();
            for (const auto& k : t.animation) {
                json::Value v = json::Value::object();
                v.set("t", k.time);
                v.set("clip", k.clip);
                if (k.fade >= 0) v.set("fade", k.fade);
                if (k.speed != 1.0f) v.set("speed", k.speed);
                if (!k.loop) v.set("loop", false);
                keys.push(std::move(v));
            }
            o.set("keys", std::move(keys));
            break;
        }
        case TrackType::Audio: {
            json::Value keys = json::Value::array();
            for (const auto& k : t.audio) {
                json::Value v = json::Value::object();
                v.set("t", k.time);
                v.set("clip", k.clip);
                if (k.volume != 1.0f) v.set("volume", k.volume);
                if (k.pitch != 1.0f) v.set("pitch", k.pitch);
                if (k.bus != "Sfx") v.set("bus", k.bus);
                if (k.spatial) v.set("spatial", true);
                keys.push(std::move(v));
            }
            o.set("keys", std::move(keys));
            break;
        }
        case TrackType::Event: {
            json::Value keys = json::Value::array();
            for (const auto& k : t.events) {
                json::Value v = json::Value::object();
                v.set("t", k.time);
                v.set("name", k.name);
                if (!k.payload.empty()) v.set("payload", k.payload);
                keys.push(std::move(v));
            }
            o.set("keys", std::move(keys));
            break;
        }
        case TrackType::Camera: {
            json::Value keys = json::Value::array();
            for (const auto& k : t.cameras) {
                json::Value v = json::Value::object();
                v.set("t", k.time);
                v.set("camera", k.camera);
                keys.push(std::move(v));
            }
            o.set("keys", std::move(keys));
            break;
        }
        }
        arr.push(std::move(o));
    }
    doc.set("tracks", std::move(arr));
    return doc;
}

std::optional<math::Vec3> sample(const std::vector<Key<math::Vec3>>& keys, float time) noexcept {
    if (keys.empty()) return std::nullopt;
    const std::ptrdiff_t i = segment(keys, time);
    if (i < 0) return keys.front().value;
    const auto n = static_cast<std::ptrdiff_t>(keys.size());
    if (i >= n - 1) return keys.back().value;
    const auto& a = keys[static_cast<std::size_t>(i)];
    const auto& b = keys[static_cast<std::size_t>(i + 1)];
    const float span = b.time - a.time;
    const float u = span > 0 ? std::clamp((time - a.time) / span, 0.0f, 1.0f) : 1.0f;
    switch (a.interp) {
    case Interp::Step:
        return u >= 1.0f ? b.value : a.value;
    case Interp::Linear:
        return math::lerp(a.value, b.value, u);
    case Interp::Smooth:
        return math::lerp(a.value, b.value, ease(u));
    case Interp::Cubic: {
        const math::Vec3 p0 = i > 0 ? keys[static_cast<std::size_t>(i - 1)].value : a.value;
        const math::Vec3 p3 = i + 2 < n ? keys[static_cast<std::size_t>(i + 2)].value : b.value;
        return catmull_rom(p0, a.value, b.value, p3, u);
    }
    }
    return a.value;
}

std::optional<math::Quat> sample(const std::vector<Key<math::Quat>>& keys, float time) noexcept {
    if (keys.empty()) return std::nullopt;
    const std::ptrdiff_t i = segment(keys, time);
    if (i < 0) return keys.front().value;
    if (i >= static_cast<std::ptrdiff_t>(keys.size()) - 1) return keys.back().value;
    const auto& a = keys[static_cast<std::size_t>(i)];
    const auto& b = keys[static_cast<std::size_t>(i + 1)];
    const float span = b.time - a.time;
    const float u = span > 0 ? std::clamp((time - a.time) / span, 0.0f, 1.0f) : 1.0f;
    switch (a.interp) {
    case Interp::Step:
        return u >= 1.0f ? b.value : a.value;
    case Interp::Linear:
        return math::slerp(a.value, b.value, u);
    case Interp::Smooth:
    case Interp::Cubic:
        return math::slerp(a.value, b.value, ease(u));
    }
    return a.value;
}

} // namespace victus::sequencer
