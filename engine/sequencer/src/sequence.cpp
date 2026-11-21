// SPDX-License-Identifier: Apache-2.0
// Sequence documents: parsing, validation, serialization and channel sampling.
#include "victus/audio/audio.hpp"
#include "victus/sequencer/sequencer.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <type_traits>
#include <utility>

namespace victus::sequencer {

namespace {

struct InterpName {
    Interp interp;
    std::string_view name;
};
constexpr InterpName kInterps[] = {
    {Interp::Step, "step"}, {Interp::Linear, "linear"}, {Interp::Smooth, "smooth"}, {Interp::Cubic, "cubic"},
    {Interp::Bezier, "bezier"}};

struct TangentName {
    Tangent tangent;
    std::string_view name;
};
constexpr TangentName kTangents[] = {
    {Tangent::Auto, "auto"}, {Tangent::Flat, "flat"}, {Tangent::User, "user"}, {Tangent::Broken, "broken"}};

struct TrackName {
    TrackType type;
    std::string_view name;
};
constexpr TrackName kTracks[] = {{TrackType::Transform, "transform"},
                                 {TrackType::Animation, "animation"},
                                 {TrackType::Audio, "audio"},
                                 {TrackType::Event, "event"},
                                 {TrackType::Camera, "camera"},
                                 {TrackType::Property, "property"}};

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
    return fail(where, "unknown interp \"" + i->as_string() + "\" (step, linear, smooth, cubic or bezier)");
}

Result<Tangent> key_tangent(const json::Value& k, const std::string& where) {
    const json::Value* i = k.find("tangent");
    if (!i) return Tangent::Auto;
    if (!i->is_string()) return fail(where, "\"tangent\" must be a string");
    if (auto r = tangent_from_name(i->as_string())) return *r;
    return fail(where, "unknown tangent \"" + i->as_string() + "\" (auto, flat, user or broken)");
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
        Key<T> key{*t, *v, *in};
        if constexpr (!std::is_same_v<T, math::Quat>) {
            auto tg = key_tangent(k, at);
            if (!tg) return std::move(tg).error();
            key.tangent = *tg;
            if (key.tangent == Tangent::User || key.tangent == Tangent::Broken) {
                auto o = value(k.find("out"), at + ".out");
                if (!o) return std::move(o).error();
                key.out_slope = *o;
                key.in_slope = *o;
            }
            if (key.tangent == Tangent::Broken) {
                auto is = value(k.find("in"), at + ".in");
                if (!is) return std::move(is).error();
                key.in_slope = *is;
            }
        }
        out.push_back(key);
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
json::Value channel_json(const std::vector<Key<T>>& keys, Fn value) {
    json::Value arr = json::Value::array();
    for (const auto& k : keys) {
        json::Value o = json::Value::object();
        o.set("t", k.time);
        o.set("v", value(k.value));
        if (k.interp != Interp::Linear) o.set("interp", interp_name(k.interp));
        if constexpr (!std::is_same_v<T, math::Quat>) {
            if (k.tangent != Tangent::Auto) o.set("tangent", tangent_name(k.tangent));
            if (k.tangent == Tangent::Broken) o.set("in", value(k.in_slope));
            if (k.tangent == Tangent::User || k.tangent == Tangent::Broken) o.set("out", value(k.out_slope));
        }
        arr.push(std::move(o));
    }
    return arr;
}

template <class T, class Fn>
void write_channel(json::Value& track, std::string_view field, const std::vector<Key<T>>& keys, Fn value) {
    if (keys.empty()) return;
    track.set(field, channel_json(keys, value));
}

// Property values: a number for one channel, else [x, y(, z(, w))].
Result<math::Vec4> vec4_of(const json::Value* v, const std::string& where, std::uint8_t& channels) {
    math::Vec4 r;
    if (v && v->is_number()) {
        r.x = v->as_float(NAN);
        if (!finite(r.x)) return fail(where, "value must be a finite number");
        channels = 1;
        return r;
    }
    if (!v || !v->is_array() || v->size() < 1 || v->size() > 4) return fail(where, "value must be a number or 2 to 4 numbers");
    for (std::size_t c = 0; c < v->size(); ++c) {
        r[static_cast<int>(c)] = (*v)[c].as_float(NAN);
        if (!finite(r[static_cast<int>(c)])) return fail(where, "values must be finite numbers");
    }
    channels = static_cast<std::uint8_t>(v->size());
    return r;
}

// Index of the last key with time <= t (keys sorted), or -1 when t is before the first key.
template <class K> std::ptrdiff_t segment(const std::vector<K>& keys, float t) noexcept {
    auto it = std::upper_bound(keys.begin(), keys.end(), t, [](float x, const K& k) { return x < k.time; });
    return (it - keys.begin()) - 1;
}

float ease(float u) noexcept { return u * u * (3.0f - 2.0f * u); }

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

std::string_view tangent_name(Tangent t) noexcept {
    for (const auto& n : kTangents)
        if (n.tangent == t) return n.name;
    return "auto";
}

std::optional<Tangent> tangent_from_name(std::string_view name) noexcept {
    for (const auto& n : kTangents)
        if (n.name == name) return n.tangent;
    return std::nullopt;
}

std::string_view track_type_name(TrackType t) noexcept {
    for (const auto& n : kTracks)
        if (n.type == t) return n.name;
    return "?";
}

std::size_t Track::key_count() const noexcept {
    return position.size() + rotation.size() + scale.size() + animation.size() + audio.size() + events.size() +
           cameras.size() + values.size();
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
    last(values);
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
                                   "\" (transform, animation, audio, event, camera or property)");
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
        case TrackType::Property: {
            const json::Value* comp = tv.find("component");
            const json::Value* prop = tv.find("property");
            if (!comp || !comp->is_string() || comp->as_string().empty())
                return fail(where, "property track needs a \"component\" (e.g. \"Light\")");
            if (!prop || !prop->is_string() || prop->as_string().empty())
                return fail(where, "property track needs a \"property\" (e.g. \"intensity\")");
            t.component = comp->as_string();
            t.property = prop->as_string();
            int arity = -1;
            st = parse_channel<math::Vec4>(tv, "keys", where, t.values,
                                           [&](const json::Value* v, const std::string& at) -> Result<math::Vec4> {
                                               std::uint8_t n = 0;
                                               auto r = vec4_of(v, at, n);
                                               if (!r) return r;
                                               // Slopes ("in", "out") must match the values' arity too.
                                               if (arity < 0) arity = n;
                                               else if (n != arity)
                                                   return fail(at, std::format("expected {} value{} like the first key", arity,
                                                                               arity == 1 ? "" : "s"));
                                               return r;
                                           });
            t.channels = static_cast<std::uint8_t>(arity < 0 ? 1 : arity);
            break;
        }
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
        case TrackType::Property: {
            o.set("component", t.component);
            o.set("property", t.property);
            const std::uint8_t n = std::clamp<std::uint8_t>(t.channels, 1, 4);
            o.set("keys", channel_json(t.values, [n](math::Vec4 v) {
                      if (n == 1) return json::Value(v.x);
                      json::Value a = json::Value::array();
                      for (int c = 0; c < n; ++c) a.push(v[c]);
                      return a;
                  }));
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

namespace {

template <class T> constexpr int kDims = std::is_same_v<T, math::Vec4> ? 4 : 3;

// Fritsch-Carlson limited slope of one component at an interior key: zero at a peak or plateau,
// otherwise the centred difference clamped to three times either side's secant.
float auto_slope(float p0, float p1, float p2, float dt0, float dt1) noexcept {
    const float d0 = dt0 > 0 ? (p1 - p0) / dt0 : 0.0f, d1 = dt1 > 0 ? (p2 - p1) / dt1 : 0.0f;
    if (d0 * d1 <= 0.0f) return 0.0f;
    const float s = (dt0 + dt1) > 0 ? (p2 - p0) / (dt0 + dt1) : 0.0f;
    const float limit = 3.0f * std::min(std::abs(d0), std::abs(d1));
    return std::clamp(s, -limit, limit);
}

template <class T> std::pair<T, T> slopes_of(const std::vector<Key<T>>& keys, std::size_t i) noexcept {
    if (i >= keys.size()) return {};
    const Key<T>& k = keys[i];
    switch (k.tangent) {
    case Tangent::Flat: return {};
    case Tangent::User: return {k.out_slope, k.out_slope};
    case Tangent::Broken: return {k.in_slope, k.out_slope};
    case Tangent::Auto: break;
    }
    if (i == 0 || i + 1 >= keys.size()) return {};
    const Key<T>& a = keys[i - 1];
    const Key<T>& b = keys[i + 1];
    T s{};
    for (int c = 0; c < kDims<T>; ++c)
        s[c] = auto_slope(a.value[c], k.value[c], b.value[c], k.time - a.time, b.time - k.time);
    return {s, s};
}

template <class T> T sample_vec(const std::vector<Key<T>>& keys, std::ptrdiff_t i, float time) noexcept {
    const auto n = static_cast<std::ptrdiff_t>(keys.size());
    const auto& a = keys[static_cast<std::size_t>(i)];
    const auto& b = keys[static_cast<std::size_t>(i + 1)];
    const float span = b.time - a.time;
    const float u = span > 0 ? std::clamp((time - a.time) / span, 0.0f, 1.0f) : 1.0f;
    switch (a.interp) {
    case Interp::Step:
        return u >= 1.0f ? b.value : a.value;
    case Interp::Linear:
        return a.value + (b.value - a.value) * u;
    case Interp::Smooth:
        return a.value + (b.value - a.value) * ease(u);
    case Interp::Cubic: {
        const T p0 = i > 0 ? keys[static_cast<std::size_t>(i - 1)].value : a.value;
        const T p3 = i + 2 < n ? keys[static_cast<std::size_t>(i + 2)].value : b.value;
        const float u2 = u * u, u3 = u2 * u;
        return (a.value * 2.0f + (b.value - p0) * u + (p0 * 2.0f - a.value * 5.0f + b.value * 4.0f - p3) * u2 +
                (a.value * 3.0f - p0 - b.value * 3.0f + p3) * u3) *
               0.5f;
    }
    case Interp::Bezier: {
        const T m0 = slopes_of(keys, static_cast<std::size_t>(i)).second;
        const T m1 = slopes_of(keys, static_cast<std::size_t>(i + 1)).first;
        const float u2 = u * u, u3 = u2 * u;
        const float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
        return a.value * h00 + m0 * (h10 * span) + b.value * h01 + m1 * (h11 * span);
    }
    }
    return a.value;
}

template <class T> std::optional<T> sample_channel(const std::vector<Key<T>>& keys, float time) noexcept {
    if (keys.empty()) return std::nullopt;
    const std::ptrdiff_t i = segment(keys, time);
    if (i < 0) return keys.front().value;
    if (i >= static_cast<std::ptrdiff_t>(keys.size()) - 1) return keys.back().value;
    return sample_vec(keys, i, time);
}

} // namespace

std::optional<math::Vec3> sample(const std::vector<Key<math::Vec3>>& keys, float time) noexcept {
    return sample_channel(keys, time);
}

std::optional<math::Vec4> sample(const std::vector<Key<math::Vec4>>& keys, float time) noexcept {
    return sample_channel(keys, time);
}

Slopes3 key_slopes(const std::vector<Key<math::Vec3>>& keys, std::size_t i) noexcept {
    const auto [in, out] = slopes_of(keys, i);
    return {in, out};
}

Slopes4 key_slopes(const std::vector<Key<math::Vec4>>& keys, std::size_t i) noexcept {
    const auto [in, out] = slopes_of(keys, i);
    return {in, out};
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
    case Interp::Bezier:
        return math::slerp(a.value, b.value, ease(u));
    }
    return a.value;
}

} // namespace victus::sequencer
