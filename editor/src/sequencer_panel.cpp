// SPDX-License-Identifier: Apache-2.0
// Sequencer panel (Window > Sequencer, docs/SEQUENCER.md): edits one .vseq timeline with its own
// undo history, previews it on a SequencePlayer entity of the edit scene by scrubbing or playing,
// records transform keys from the scene and saves the file.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include "victus/core/fs.hpp"
#include "victus/core/hash.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>

namespace victus::editor {

using namespace scene;
using sequencer::Interp;
using sequencer::Sequence;
using sequencer::Tangent;
using sequencer::Track;
using sequencer::TrackType;
namespace sfs = std::filesystem;

namespace {

constexpr const char* kAssetPayload = "VICTUS_ASSET"; // matches the asset browser's drag source
constexpr std::size_t kSequenceUndoLimit = 128;
constexpr float kSameTime = 1e-4f;

template <class K> void sort_keys(std::vector<K>& keys) {
    std::stable_sort(keys.begin(), keys.end(), [](const K& a, const K& b) { return a.time < b.time; });
}

void normalize(Sequence& seq) {
    float end = 0;
    for (Track& t : seq.tracks) {
        sort_keys(t.position);
        sort_keys(t.rotation);
        sort_keys(t.scale);
        sort_keys(t.animation);
        sort_keys(t.audio);
        sort_keys(t.events);
        sort_keys(t.cameras);
        sort_keys(t.values);
        end = std::max(end, t.end_time());
    }
    if (!std::isfinite(seq.duration) || seq.duration < end) seq.duration = end;
}

// Calls fn(vector&) with the key vector a SeqKey channel refers to.
template <class Fn> auto with_channel(Track& t, Editor::SeqChannel c, Fn&& fn) {
    switch (c) {
    case Editor::SeqChannel::Position:
        return fn(t.position);
    case Editor::SeqChannel::Rotation:
        return fn(t.rotation);
    case Editor::SeqChannel::Scale:
        return fn(t.scale);
    case Editor::SeqChannel::Keys:
        break;
    }
    switch (t.type) {
    case TrackType::Animation:
        return fn(t.animation);
    case TrackType::Audio:
        return fn(t.audio);
    case TrackType::Camera:
        return fn(t.cameras);
    case TrackType::Property:
        return fn(t.values);
    case TrackType::Event:
    case TrackType::Transform:
        break;
    }
    return fn(t.events);
}

bool channel_valid(const Track& t, Editor::SeqChannel c) {
    return (t.type == TrackType::Transform) == (c != Editor::SeqChannel::Keys);
}

// Inserts `key` keeping the vector sorted; a key at the same time is replaced. Returns its index.
template <class K> int insert_key(std::vector<K>& keys, K key) {
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (std::abs(keys[i].time - key.time) <= kSameTime) {
            keys[i] = std::move(key);
            return static_cast<int>(i);
        }
    const auto it =
        std::upper_bound(keys.begin(), keys.end(), key.time, [](float t, const K& k) { return t < k.time; });
    const auto at = keys.insert(it, std::move(key));
    return static_cast<int>(at - keys.begin());
}

ImU32 track_color(TrackType t) {
    switch (t) {
    case TrackType::Transform:
        return IM_COL32(110, 170, 255, 255);
    case TrackType::Animation:
        return IM_COL32(120, 220, 140, 255);
    case TrackType::Audio:
        return IM_COL32(240, 190, 90, 255);
    case TrackType::Camera:
        return IM_COL32(220, 120, 230, 255);
    case TrackType::Property:
        return IM_COL32(90, 210, 210, 255);
    case TrackType::Event:
        break;
    }
    return IM_COL32(235, 235, 235, 255);
}

const char* const kTrackTypes[] = {"Transform", "Animation", "Audio", "Event", "Camera"};
const char* const kInterps[] = {"step", "linear", "smooth", "cubic", "bezier"};
constexpr int kInterpCount = 5;
const char* const kTangentNames[] = {"Auto", "Flat", "User", "Broken"};

// ---- curve keys (ADR-0190): position/scale (Vec3) and property (Vec4) channels ----

// Calls fn(keys, components) for a curve channel; false when the channel has no curve.
template <class T, class Fn> bool with_curve(T& t, Editor::SeqChannel c, Fn&& fn) {
    if (t.type == TrackType::Transform && c == Editor::SeqChannel::Position) {
        fn(t.position, 3);
        return true;
    }
    if (t.type == TrackType::Transform && c == Editor::SeqChannel::Scale) {
        fn(t.scale, 3);
        return true;
    }
    if (t.type == TrackType::Property && c == Editor::SeqChannel::Keys) {
        fn(t.values, std::clamp<int>(t.channels, 1, 4));
        return true;
    }
    return false;
}

template <class V> float& comp(V& v, int c) { return (&v.x)[c]; }
template <class V> float comp(const V& v, int c) { return (&v.x)[c]; }

// Freezes the slopes a key currently uses (so switching to User/Broken keeps the curve).
template <class K> void freeze_slopes(std::vector<K>& keys, std::size_t i) {
    const auto sl = sequencer::key_slopes(keys, i);
    keys[i].in_slope = sl.in;
    keys[i].out_slope = sl.out;
}

template <class K> void apply_tangent(std::vector<K>& keys, std::size_t i, Tangent tangent) {
    if ((tangent == Tangent::User || tangent == Tangent::Broken) &&
        (keys[i].tangent == Tangent::Auto || keys[i].tangent == Tangent::Flat))
        freeze_slopes(keys, i);
    if (tangent == Tangent::User) keys[i].in_slope = keys[i].out_slope;
    keys[i].tangent = tangent;
}

template <class K>
void apply_slope(std::vector<K>& keys, std::size_t i, int c, Editor::SeqSide side, float slope, bool break_tangent) {
    if (break_tangent)
        apply_tangent(keys, i, Tangent::Broken);
    else if (keys[i].tangent == Tangent::Auto || keys[i].tangent == Tangent::Flat)
        apply_tangent(keys, i, Tangent::User);
    K& k = keys[i];
    if (k.tangent == Tangent::Broken) {
        comp(side == Editor::SeqSide::In ? k.in_slope : k.out_slope, c) = slope;
    } else {
        comp(k.in_slope, c) = slope;
        comp(k.out_slope, c) = slope;
    }
}

// Moves key i to `time` with component c set to `value`; returns its index after re-sorting.
template <class K> int apply_move(std::vector<K>& keys, std::size_t i, float time, int c, float value) {
    K k = keys[i];
    keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(i));
    k.time = std::max(time, 0.0f);
    comp(k.value, c) = value;
    // Never merge into a neighbour at the same time: nudge past it instead.
    for (const K& o : keys)
        if (std::abs(o.time - k.time) <= kSameTime) k.time = o.time + 2 * kSameTime;
    const auto it =
        std::upper_bound(keys.begin(), keys.end(), k.time, [](float t, const K& x) { return t < x.time; });
    return static_cast<int>(keys.insert(it, std::move(k)) - keys.begin());
}

Entity resolve_track(Scene& s, Entity player, const std::string& target) {
    return player.is_null() ? s.find_by_name(target) : sequencer::resolve_target(s, player, target);
}
const char* const kBuses[] = {"Master", "Music", "Sfx", "Ui", "Voice", "Ambient"};

std::string entity_name(const Scene& s, Entity e) {
    const auto* n = s.world().get<Name>(e);
    return n ? n->value : std::string();
}

} // namespace

// ---- document ----------------------------------------------------------------------------------

Result<std::string> Editor::create_sequence(std::string_view relative_path, float duration) {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project open");
    const std::string ext(sequencer::kSequenceExtension);
    std::string rel(relative_path);
    if (rel.empty()) {
        rel = "sequences/Sequence" + ext;
        for (int i = 2; sfs::exists(db_->assets_dir() / rel); ++i)
            rel = std::format("sequences/Sequence_{}{}", i, ext);
    } else if (!rel.ends_with(ext)) {
        rel += ext;
    }
    auto clean = fs::sanitize_relative(rel);
    if (!clean) return std::move(clean).error();
    if (sfs::exists(db_->assets_dir() / *clean))
        return make_error(ErrorCode::AlreadyExists, *clean + " already exists");
    std::error_code ec;
    sfs::create_directories((db_->assets_dir() / *clean).parent_path(), ec);
    Sequence seq;
    seq.duration = std::isfinite(duration) && duration > 0 ? duration : 5.0f;
    const std::string text = json::stringify(seq.to_json());
    VICTUS_TRY_STATUS(fs::write_text_atomic(db_->assets_dir() / *clean, text));
    written_sequences_[*clean] = fnv1a64(text);
    (void)db_->refresh();
    if (!db_->find_path(*clean))
        return make_error(ErrorCode::IoError, "sequence was written but not registered: " + *clean);
    notify("Created sequence " + *clean);
    return *clean;
}

Status Editor::open_sequence(std::string_view path, Entity player) {
    if (!db_) return make_error(ErrorCode::InvalidArgument, "no project open");
    auto clean = fs::sanitize_relative(path);
    if (!clean) return std::move(clean).error();
    auto text = fs::read_text(db_->assets_dir() / *clean);
    if (!text) return std::move(text).error();
    auto parsed = Sequence::parse(*text, *clean);
    if (!parsed) return std::move(parsed).error();
    end_sequence_preview();
    seq_doc_ = **parsed;
    seq_path_ = *clean;
    seq_undo_.clear();
    seq_redo_.clear();
    seq_dirty_ = false;
    seq_time_ = 0;
    seq_selected_ = {};
    seq_track_selected_ = -1;
    seq_drag_.reset();
    show_sequencer_ = true;

    // Preview player: the given entity, the selection, else the first player of this sequence.
    Scene& s = edit_scene();
    seq_player_ = {};
    auto has_player = [&](Entity e) {
        return !e.is_null() && s.world().alive(e) && s.world().get<sequencer::SequencePlayerComponent>(e);
    };
    if (has_player(player))
        seq_player_ = s.uuid(player);
    else if (!selection_.empty() && has_player(s.find(selection_.front())))
        seq_player_ = selection_.front();
    else
        s.world().each<sequencer::SequencePlayerComponent>([&](Entity e, sequencer::SequencePlayerComponent& c) {
            if (seq_player_.is_nil() && c.sequence == *clean) seq_player_ = s.uuid(e);
        });
    return {};
}

bool Editor::assign_sequence(Entity e) {
    if (!seq_doc_ || play_state_ != PlayState::Editing) return false;
    Scene& s = edit_scene();
    if (!s.world().alive(e)) return false;
    end_sequence_preview();
    auto before = capture(s, {s.uuid(e)});
    if (auto* c = s.world().get<sequencer::SequencePlayerComponent>(e))
        c->sequence = seq_path_;
    else
        s.world().add<sequencer::SequencePlayerComponent>(e, sequencer::SequencePlayerComponent{.sequence = seq_path_});
    record_edit("Assign sequence", std::move(before));
    seq_player_ = s.uuid(e);
    return true;
}

void Editor::close_sequence() {
    end_sequence_preview();
    seq_doc_.reset();
    seq_path_.clear();
    seq_undo_.clear();
    seq_redo_.clear();
    seq_dirty_ = false;
    seq_player_ = {};
    seq_selected_ = {};
    seq_track_selected_ = -1;
    seq_drag_.reset();
}

Status Editor::save_sequence() {
    if (!db_ || !seq_doc_) return make_error(ErrorCode::InvalidArgument, "no sequence open");
    normalize(*seq_doc_);
    const std::string text = json::stringify(seq_doc_->to_json());
    // Never write a file the runtime would reject.
    if (auto check = Sequence::parse(text, seq_path_); !check) return std::move(check).error();
    const fs::Path path = db_->assets_dir() / seq_path_;
    std::error_code ec;
    sfs::create_directories(path.parent_path(), ec);
    VICTUS_TRY_STATUS(fs::write_text_atomic(path, text));
    written_sequences_[seq_path_] = fnv1a64(text);
    seq_dirty_ = false;
    if (play_sequencer_) play_sequencer_->reload(seq_path_); // a running play session picks it up now
    notify("Saved " + seq_path_);
    return {};
}

void Editor::sequence_changed_on_disk(const std::string& path) {
    if (const auto it = written_sequences_.find(path); it != written_sequences_.end() && db_)
        if (auto text = fs::read_text(db_->assets_dir() / path); text && fnv1a64(*text) == it->second)
            return; // our save
    notify("Reloaded sequence " + path);
    if (!seq_doc_ || path != seq_path_) return;
    if (seq_dirty_) {
        notify(path + " changed on disk; the Sequencer keeps your unsaved edits", true);
        return;
    }
    const float t = seq_time_;
    const Uuid player = seq_player_;
    const bool open = show_sequencer_;
    Scene& s = edit_scene();
    if (auto st = open_sequence(path, s.find(player)); !st) {
        notify("cannot reload " + path + ": " + st.error().message, true);
        return;
    }
    show_sequencer_ = open;
    seq_time_ = t;
}

bool Editor::edit_sequence(const std::function<void(Sequence&)>& edit) {
    if (!seq_doc_) return false;
    seq_undo_.push_back(*seq_doc_);
    if (seq_undo_.size() > kSequenceUndoLimit) seq_undo_.erase(seq_undo_.begin());
    seq_redo_.clear();
    edit(*seq_doc_);
    normalize(*seq_doc_);
    seq_dirty_ = true;
    if (seq_track_selected_ >= static_cast<int>(seq_doc_->tracks.size())) seq_track_selected_ = -1;
    if (seq_backup_) refresh_sequence_preview();
    return true;
}

bool Editor::sequence_undo() {
    if (!seq_doc_ || seq_undo_.empty()) return false;
    seq_redo_.push_back(std::move(*seq_doc_));
    seq_doc_ = std::move(seq_undo_.back());
    seq_undo_.pop_back();
    seq_dirty_ = true;
    seq_selected_ = {};
    if (seq_track_selected_ >= static_cast<int>(seq_doc_->tracks.size())) seq_track_selected_ = -1;
    if (seq_backup_) refresh_sequence_preview();
    return true;
}

bool Editor::sequence_redo() {
    if (!seq_doc_ || seq_redo_.empty()) return false;
    seq_undo_.push_back(std::move(*seq_doc_));
    seq_doc_ = std::move(seq_redo_.back());
    seq_redo_.pop_back();
    seq_dirty_ = true;
    seq_selected_ = {};
    if (seq_track_selected_ >= static_cast<int>(seq_doc_->tracks.size())) seq_track_selected_ = -1;
    if (seq_backup_) refresh_sequence_preview();
    return true;
}

int Editor::sequence_add_track(TrackType type, std::string target, std::string name) {
    if (!seq_doc_ || seq_doc_->tracks.size() >= Sequence::kMaxTracks) return -1;
    edit_sequence([&](Sequence& s) {
        Track t;
        t.type = type;
        t.target = std::move(target);
        t.name = std::move(name);
        s.tracks.push_back(std::move(t));
    });
    seq_track_selected_ = static_cast<int>(seq_doc_->tracks.size()) - 1;
    return seq_track_selected_;
}

int Editor::sequence_add_property_track(std::string target, std::string component, std::string property) {
    if (!seq_doc_ || seq_doc_->tracks.size() >= Sequence::kMaxTracks) return -1;
    Scene& s = active_scene();
    const Entity e = resolve_track(s, sequence_preview_player(), target);
    if (e.is_null()) {
        notify("Property track: target '" + target + "' was not found", true);
        return -1;
    }
    std::uint8_t channels = 0;
    for (const auto& p : sequencer::animatable_properties(s, e))
        if (p.component == component && p.property == property) channels = p.channels;
    if (channels == 0) {
        notify(std::format("Property track: '{}' has no animatable {}.{}", entity_name(s, e), component, property), true);
        return -1;
    }
    edit_sequence([&](Sequence& sq) {
        Track t;
        t.type = TrackType::Property;
        t.target = std::move(target);
        t.component = std::move(component);
        t.property = std::move(property);
        t.channels = channels;
        sq.tracks.push_back(std::move(t));
    });
    seq_track_selected_ = static_cast<int>(seq_doc_->tracks.size()) - 1;
    seq_curve_frame_ = true;
    return seq_track_selected_;
}

bool Editor::sequence_remove_track(int track) {
    if (!seq_doc_ || track < 0 || track >= static_cast<int>(seq_doc_->tracks.size())) return false;
    edit_sequence([&](Sequence& s) { s.tracks.erase(s.tracks.begin() + track); });
    seq_selected_ = {};
    seq_track_selected_ = -1;
    return true;
}

Editor::SeqKey Editor::sequence_record_transform(int track, float time) {
    if (!seq_doc_ || track < 0 || track >= static_cast<int>(seq_doc_->tracks.size()) || !std::isfinite(time)) return {};
    const Track& tr = seq_doc_->tracks[static_cast<std::size_t>(track)];
    if (tr.type != TrackType::Transform) return {};
    Scene& s = active_scene();
    const Entity player = sequence_preview_player();
    const Entity target =
        player.is_null() ? s.find_by_name(tr.target) : sequencer::resolve_target(s, player, tr.target);
    const auto* xf = target.is_null() ? nullptr : s.world().get<Transform>(target);
    if (!xf) {
        notify("Record: track target '" + tr.target + "' was not found", true);
        return {};
    }
    Transform value = *xf;
    if (tr.relative) {
        // Offsets from the transform the target had before the preview started.
        Transform base = value;
        if (seq_backup_) {
            const Uuid id = s.uuid(target);
            for (const auto& item : seq_backup_->items)
                if (item.uuid == id) base = item.transform;
        }
        value.position = xf->position - base.position;
        value.rotation = math::normalize(base.rotation.conjugate() * xf->rotation);
        auto div = [](float a, float b) { return std::abs(b) > 1e-6f ? a / b : 1.0f; };
        value.scale = {div(xf->scale.x, base.scale.x), div(xf->scale.y, base.scale.y), div(xf->scale.z, base.scale.z)};
    }
    const float t = std::max(time, 0.0f);
    int index = -1;
    edit_sequence([&](Sequence& sq) {
        Track& w = sq.tracks[static_cast<std::size_t>(track)];
        auto interp_at = [t](const auto& keys) {
            for (const auto& k : keys)
                if (std::abs(k.time - t) <= kSameTime) return k.interp;
            return sequencer::Interp::Linear;
        };
        index = insert_key(w.position, sequencer::Key<math::Vec3>{t, value.position, interp_at(w.position)});
        insert_key(w.rotation, sequencer::Key<math::Quat>{t, value.rotation, interp_at(w.rotation)});
        insert_key(w.scale, sequencer::Key<math::Vec3>{t, value.scale, interp_at(w.scale)});
    });
    seq_selected_ = {track, SeqChannel::Position, index};
    seq_track_selected_ = track;
    return seq_selected_;
}

Editor::SeqKey Editor::sequence_add_key(int track, float time, std::string value) {
    if (!seq_doc_ || track < 0 || track >= static_cast<int>(seq_doc_->tracks.size()) || !std::isfinite(time)) return {};
    const TrackType type = seq_doc_->tracks[static_cast<std::size_t>(track)].type;
    if (type == TrackType::Transform) return sequence_record_transform(track, time);
    const float t = std::max(time, 0.0f);
    std::optional<math::Vec4> current;
    if (type == TrackType::Property) {
        const Track& tr = seq_doc_->tracks[static_cast<std::size_t>(track)];
        Scene& s = active_scene();
        const Entity e = resolve_track(s, sequence_preview_player(), tr.target);
        current = e.is_null() ? std::nullopt : sequencer::read_property(s, e, tr.component, tr.property);
        if (!current) {
            notify(std::format("Record: '{}' has no {}.{}", tr.target, tr.component, tr.property), true);
            return {};
        }
    }
    int index = -1;
    edit_sequence([&](Sequence& sq) {
        Track& w = sq.tracks[static_cast<std::size_t>(track)];
        switch (type) {
        case TrackType::Animation:
            index = insert_key(w.animation, sequencer::AnimationKey{.time = t, .clip = value});
            break;
        case TrackType::Audio:
            index = insert_key(w.audio, sequencer::AudioKey{.time = t, .clip = value});
            break;
        case TrackType::Camera:
            index = insert_key(w.cameras, sequencer::CameraKey{.time = t, .camera = value});
            break;
        case TrackType::Event:
            index = insert_key(w.events,
                               sequencer::EventKey{.time = t, .name = value.empty() ? std::string("event") : value});
            break;
        case TrackType::Property: {
            Interp interp = Interp::Linear;
            for (const auto& k : w.values)
                if (std::abs(k.time - t) <= kSameTime) interp = k.interp;
            index = insert_key(w.values, sequencer::Key<math::Vec4>{t, *current, interp});
            break;
        }
        case TrackType::Transform:
            break;
        }
    });
    seq_selected_ = {track, SeqChannel::Keys, index};
    seq_track_selected_ = track;
    return seq_selected_;
}

std::optional<float> Editor::sequence_key_time(SeqKey key) const {
    if (!seq_doc_ || !key.valid() || key.track >= static_cast<int>(seq_doc_->tracks.size())) return std::nullopt;
    Track t = seq_doc_->tracks[static_cast<std::size_t>(key.track)];
    if (!channel_valid(t, key.channel)) return std::nullopt;
    return with_channel(t, key.channel, [&](auto& keys) -> std::optional<float> {
        if (key.index >= static_cast<int>(keys.size())) return std::nullopt;
        return keys[static_cast<std::size_t>(key.index)].time;
    });
}

Editor::SeqKey Editor::sequence_move_key(SeqKey key, float time) {
    if (!sequence_key_time(key) || !std::isfinite(time)) return {};
    const float t = std::max(time, 0.0f);
    int index = -1;
    edit_sequence([&](Sequence& sq) {
        Track& w = sq.tracks[static_cast<std::size_t>(key.track)];
        auto move = [&](auto& keys) {
            auto k = keys[static_cast<std::size_t>(key.index)];
            keys.erase(keys.begin() + key.index);
            k.time = t;
            return insert_key(keys, std::move(k));
        };
        if (key.channel == SeqChannel::Keys) {
            index = with_channel(w, key.channel, move);
            return;
        }
        // A transform key moves its position, rotation and scale keys at the same time together.
        const float old =
            with_channel(w, key.channel, [&](auto& keys) { return keys[static_cast<std::size_t>(key.index)].time; });
        for (SeqChannel c : {SeqChannel::Position, SeqChannel::Rotation, SeqChannel::Scale}) {
            with_channel(w, c, [&](auto& keys) {
                for (std::size_t i = 0; i < keys.size(); ++i)
                    if (std::abs(keys[i].time - old) <= kSameTime) {
                        auto k = keys[i];
                        keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(i));
                        k.time = t;
                        const int at = insert_key(keys, std::move(k));
                        if (c == key.channel) index = at;
                        break;
                    }
                return 0;
            });
        }
    });
    seq_selected_ = {key.track, key.channel, index};
    return seq_selected_;
}

bool Editor::sequence_delete_key(SeqKey key, bool all_channels) {
    const auto time = sequence_key_time(key);
    if (!time) return false;
    edit_sequence([&](Sequence& sq) {
        Track& w = sq.tracks[static_cast<std::size_t>(key.track)];
        if (key.channel == SeqChannel::Keys || !all_channels) {
            with_channel(w, key.channel, [&](auto& keys) {
                keys.erase(keys.begin() + key.index);
                return 0;
            });
            return;
        }
        for (SeqChannel c : {SeqChannel::Position, SeqChannel::Rotation, SeqChannel::Scale})
            with_channel(w, c, [&](auto& keys) {
                std::erase_if(keys, [&](const auto& k) { return std::abs(k.time - *time) <= kSameTime; });
                return 0;
            });
    });
    if (seq_selected_ == key) seq_selected_ = {};
    return true;
}

// ---- curve editing ----------------------------------------------------------------------------

int Editor::sequence_key_components(SeqKey key) const {
    if (!sequence_key_time(key)) return 0;
    int n = 0;
    with_curve(seq_doc_->tracks[static_cast<std::size_t>(key.track)], key.channel, [&](const auto&, int c) { n = c; });
    return n;
}

std::optional<float> Editor::sequence_key_value(SeqKey key, int component) const {
    if (component < 0 || component >= sequence_key_components(key)) return std::nullopt;
    float v = 0;
    with_curve(seq_doc_->tracks[static_cast<std::size_t>(key.track)], key.channel,
               [&](const auto& keys, int) { v = comp(keys[static_cast<std::size_t>(key.index)].value, component); });
    return v;
}

bool Editor::sequence_set_key_value(SeqKey key, int component, float value) {
    if (component < 0 || component >= sequence_key_components(key) || !std::isfinite(value)) return false;
    edit_sequence([&](Sequence& sq) {
        with_curve(sq.tracks[static_cast<std::size_t>(key.track)], key.channel, [&](auto& keys, int) {
            comp(keys[static_cast<std::size_t>(key.index)].value, component) = value;
        });
    });
    return true;
}

Editor::SeqKey Editor::sequence_move_curve_key(SeqKey key, float time, int component, float value) {
    if (component < 0 || component >= sequence_key_components(key) || !std::isfinite(time) || !std::isfinite(value))
        return {};
    int index = -1;
    edit_sequence([&](Sequence& sq) {
        with_curve(sq.tracks[static_cast<std::size_t>(key.track)], key.channel, [&](auto& keys, int) {
            index = apply_move(keys, static_cast<std::size_t>(key.index), time, component, value);
        });
    });
    seq_selected_ = {key.track, key.channel, index};
    seq_curve_component_ = component;
    return seq_selected_;
}

bool Editor::sequence_set_key_interp(SeqKey key, Interp interp) {
    if (!sequence_key_time(key)) return false;
    const Track& t = seq_doc_->tracks[static_cast<std::size_t>(key.track)];
    if (key.channel == SeqChannel::Keys && t.type != TrackType::Property) return false;
    edit_sequence([&](Sequence& sq) {
        with_channel(sq.tracks[static_cast<std::size_t>(key.track)], key.channel, [&](auto& keys) {
            if constexpr (requires { keys[0].interp; }) keys[static_cast<std::size_t>(key.index)].interp = interp;
            return 0;
        });
    });
    return true;
}

bool Editor::sequence_set_key_tangent(SeqKey key, Tangent tangent) {
    if (sequence_key_components(key) == 0) return false;
    edit_sequence([&](Sequence& sq) {
        with_curve(sq.tracks[static_cast<std::size_t>(key.track)], key.channel,
                   [&](auto& keys, int) { apply_tangent(keys, static_cast<std::size_t>(key.index), tangent); });
    });
    return true;
}

bool Editor::sequence_set_key_slope(SeqKey key, int component, SeqSide side, float slope, bool break_tangent) {
    if (component < 0 || component >= sequence_key_components(key) || !std::isfinite(slope)) return false;
    edit_sequence([&](Sequence& sq) {
        with_curve(sq.tracks[static_cast<std::size_t>(key.track)], key.channel, [&](auto& keys, int) {
            apply_slope(keys, static_cast<std::size_t>(key.index), component, side, slope, break_tangent);
        });
    });
    return true;
}

Editor::SeqKey Editor::sequence_insert_curve_key(int track, SeqChannel channel, float time) {
    if (!seq_doc_ || track < 0 || track >= static_cast<int>(seq_doc_->tracks.size()) || !std::isfinite(time)) return {};
    const Track& tr = seq_doc_->tracks[static_cast<std::size_t>(track)];
    bool curve = false;
    bool empty = false;
    with_curve(tr, channel, [&](const auto& keys, int) {
        curve = true;
        empty = keys.empty();
    });
    if (!curve) return {};
    if (empty) { // nothing to sample: key the target's current value
        if (tr.type == TrackType::Property) return sequence_add_key(track, time);
        return sequence_record_transform(track, time);
    }
    const float t = std::max(time, 0.0f);
    int index = -1;
    edit_sequence([&](Sequence& sq) {
        with_curve(sq.tracks[static_cast<std::size_t>(track)], channel, [&](auto& keys, int) {
            using K = std::decay_t<decltype(keys[0])>;
            K k;
            k.time = t;
            k.value = *sequencer::sample(keys, t);
            k.interp = Interp::Bezier;
            for (const K& o : keys)
                if (o.time <= t + kSameTime) k.interp = o.interp;
            index = insert_key(keys, k);
        });
    });
    seq_selected_ = {track, channel, index};
    seq_track_selected_ = track;
    return seq_selected_;
}

// ---- preview ----------------------------------------------------------------------------------

Entity Editor::sequence_preview_player() {
    if (seq_player_.is_nil()) return kNullEntity;
    Scene& s = active_scene();
    const Entity e = s.find(seq_player_);
    return !e.is_null() && s.world().get<sequencer::SequencePlayerComponent>(e) ? e : kNullEntity;
}

void Editor::refresh_sequence_preview() {
    if (!seq_doc_ || play_state_ != PlayState::Editing || play_scene_) return;
    const Entity player = sequence_preview_player();
    if (player.is_null()) return;
    Scene& s = edit_scene();
    if (!seq_backup_) {
        SeqBackup b;
        s.world().each<Transform>([&](Entity e, Transform& t) {
            SeqBackup::Item item{.uuid = s.uuid(e), .transform = t};
            if (const auto* c = s.world().get<CameraComponent>(e)) item.camera_primary = c->primary;
            if (const auto* a = s.world().get<animation::AnimatorComponent>(e)) item.animator = *a;
            b.items.push_back(std::move(item));
        });
        seq_backup_ = std::move(b);
    } else {
        for (const auto& item : seq_backup_->items) {
            const Entity e = s.find(item.uuid);
            if (e.is_null()) continue;
            if (auto* t = s.world().get<Transform>(e)) *t = item.transform;
        }
    }
    // Property tracks: put back the values captured so far, then capture newly animated ones.
    for (const auto& p : seq_backup_->properties)
        if (const Entity e = s.find(p.uuid); !e.is_null()) (void)sequencer::write_property(s, e, p.component, p.property, p.value);
    for (const Track& tr : seq_doc_->tracks) {
        if (tr.type != TrackType::Property) continue;
        const Entity e = sequencer::resolve_target(s, player, tr.target);
        if (e.is_null()) continue;
        const Uuid id = s.uuid(e);
        const bool known = std::any_of(seq_backup_->properties.begin(), seq_backup_->properties.end(), [&](const auto& p) {
            return p.uuid == id && p.component == tr.component && p.property == tr.property;
        });
        if (known) continue;
        if (const auto v = sequencer::read_property(s, e, tr.component, tr.property))
            seq_backup_->properties.push_back({id, tr.component, tr.property, *v});
    }
    if (!edit_sequencer_) edit_sequencer_ = std::make_unique<sequencer::SequencerSystem>();
    auto doc = std::make_shared<Sequence>(*seq_doc_);
    doc->name = seq_path_;
    edit_sequencer_->set_sequence(s, player, std::move(doc));
    seq_time_ = std::clamp(seq_time_, 0.0f, seq_doc_->duration);
    (void)edit_sequencer_->seek(s, player, seq_time_);
}

void Editor::sequence_seek(float time) {
    if (!seq_doc_ || !std::isfinite(time)) return;
    seq_time_ = std::clamp(time, 0.0f, seq_doc_->duration);
    const Entity player = sequence_preview_player();
    if (!seq_backup_ || !edit_sequencer_ || player.is_null()) {
        refresh_sequence_preview();
        return;
    }
    (void)edit_sequencer_->seek(edit_scene(), player, seq_time_);
}

void Editor::sequence_preview_play(bool playing) {
    seq_playing_ = playing && seq_doc_ && play_state_ == PlayState::Editing;
    if (seq_playing_ && seq_doc_->duration > 0 && seq_time_ >= seq_doc_->duration) seq_time_ = 0;
    if (seq_playing_) sequence_seek(seq_time_);
}

void Editor::update_sequence_preview(float dt) {
    if (!seq_playing_) return;
    if (!seq_doc_ || play_state_ != PlayState::Editing || play_scene_) {
        seq_playing_ = false;
        return;
    }
    float t = seq_time_ + dt;
    if (seq_doc_->duration <= 0)
        t = 0;
    else if (t > seq_doc_->duration)
        t = std::fmod(t, seq_doc_->duration);
    sequence_seek(t);
}

void Editor::end_sequence_preview() {
    seq_playing_ = false;
    seq_drag_.reset();
    if (!seq_backup_) {
        edit_sequencer_.reset();
        return;
    }
    Scene& s = edit_scene();
    if (edit_sequencer_) {
        const Entity player = s.find(seq_player_);
        if (!player.is_null() && s.world().alive(player)) edit_sequencer_->set_sequence(s, player, nullptr);
    }
    for (const auto& item : seq_backup_->items) {
        const Entity e = s.find(item.uuid);
        if (e.is_null()) continue;
        if (auto* t = s.world().get<Transform>(e)) *t = item.transform;
        if (auto* c = s.world().get<CameraComponent>(e); c && item.camera_primary) c->primary = *item.camera_primary;
        if (auto* a = s.world().get<animation::AnimatorComponent>(e); a && item.animator) *a = *item.animator;
    }
    for (const auto& p : seq_backup_->properties)
        if (const Entity e = s.find(p.uuid); !e.is_null()) (void)sequencer::write_property(s, e, p.component, p.property, p.value);
    seq_backup_.reset();
    edit_sequencer_.reset();
}

// ---- panel ------------------------------------------------------------------------------------

void Editor::draw_sequencer() {
    if (!show_sequencer_) return;
    ImGui::SetNextWindowSize(ImVec2(920, 340), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Sequencer", &show_sequencer_)) {
        ImGui::End();
        return;
    }
    Scene& s = active_scene();
    const bool editing = play_state_ == PlayState::Editing && !play_scene_;
    const Entity selected = selection_.empty() ? kNullEntity : s.find(selection_.front());

    // Drop a .vseq from the asset browser anywhere on the window to open it.
    auto accept_drop = [&] {
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                Uuid id;
                std::memcpy(&id, p->Data, sizeof(Uuid));
                if (const auto* rec = db_ ? db_->find(id) : nullptr; rec && rec->kind == assets::AssetKind::Sequence)
                    if (auto st = open_sequence(rec->path); !st) notify(st.error().message, true);
            }
            ImGui::EndDragDropTarget();
        }
    };

    if (!seq_doc_) {
        ImGui::TextDisabled("No sequence open. Drop a .vseq here, open the selection's sequence or create one.");
        if (ImGui::Button("New Sequence")) {
            if (auto path = create_sequence(); !path)
                notify(path.error().message, true);
            else if (auto st = open_sequence(*path); !st)
                notify(st.error().message, true);
            else if (!selected.is_null() && editing)
                assign_sequence(selected);
        }
        if (const auto* c = selected.is_null() ? nullptr : s.world().get<sequencer::SequencePlayerComponent>(selected);
            c && !c->sequence.empty()) {
            ImGui::SameLine();
            if (ImGui::Button(std::format("Open {}", c->sequence).c_str()))
                if (auto st = open_sequence(c->sequence, selected); !st) notify(st.error().message, true);
        }
        ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, std::max(ImGui::GetContentRegionAvail().y, 20.0f)));
        accept_drop();
        ImGui::End();
        return;
    }

    Sequence& doc = *seq_doc_;
    const Entity player = sequence_preview_player();

    // ---- toolbar ----
    ImGui::Text("%s%s", seq_path_.c_str(), seq_dirty_ ? " *" : "");
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        if (auto st = save_sequence(); !st) notify(st.error().message, true);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!sequence_can_undo());
    if (ImGui::Button("Undo")) sequence_undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!sequence_can_redo());
    if (ImGui::Button("Redo")) sequence_redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        close_sequence();
        ImGui::End();
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::BeginDisabled(!editing || player.is_null());
    if (ImGui::Button("|<")) sequence_seek(0);
    ImGui::SameLine();
    if (ImGui::Button(seq_playing_ ? "Pause" : "Play")) sequence_preview_play(!seq_playing_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    float t = seq_time_;
    if (ImGui::DragFloat("##time", &t, 0.01f, 0.0f, doc.duration, "%.2f s")) sequence_seek(t);
    ImGui::SameLine();
    if (ImGui::Button("End Preview")) end_sequence_preview();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    float duration = doc.duration;
    ImGui::DragFloat("Duration", &duration, 0.05f, 0.0f, 3600.0f, "%.2f s");
    if (ImGui::IsItemDeactivatedAfterEdit())
        edit_sequence([&](Sequence& sq) { sq.duration = std::max(duration, 0.0f); });
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::SliderFloat("Zoom", &seq_zoom_, 10.0f, 600.0f, "%.0f px/s", ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    if (ImGui::Checkbox("Curves", &seq_curves_)) seq_curve_frame_ = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the curve editor for the selected transform or property track");

    // Preview player.
    if (player.is_null()) {
        ImGui::TextColored(ui::tokens::kWarning, "No preview player.");
        ImGui::SameLine();
        ImGui::BeginDisabled(selected.is_null() || !editing);
        if (ImGui::Button("Assign to Selection")) assign_sequence(selected);
        ImGui::EndDisabled();
    } else {
        ImGui::Text("Previewing on '%s'%s", entity_name(s, player).c_str(),
                    sequence_previewing() ? " (scene restored when the preview ends)" : "");
        if (!selected.is_null() && selected != player && editing) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Preview on Selection")) {
                if (s.world().get<sequencer::SequencePlayerComponent>(selected)) {
                    end_sequence_preview();
                    seq_player_ = s.uuid(selected);
                } else {
                    assign_sequence(selected);
                }
            }
        }
    }

    // Add Track: targets the selection relative to the player.
    if (ImGui::Button("+ Track")) ImGui::OpenPopup("seq_add_track");
    if (ImGui::BeginPopup("seq_add_track")) {
        std::string target;
        if (!selected.is_null() && !player.is_null() && selected != player) {
            std::vector<std::string> parts;
            Entity e = selected;
            while (!e.is_null() && e != player) {
                parts.push_back(entity_name(s, e));
                e = s.parent(e);
            }
            if (e == player)
                for (auto it = parts.rbegin(); it != parts.rend(); ++it)
                    target += (target.empty() ? "" : "/") + *it;
            else
                target = entity_name(s, selected);
        }
        ImGui::TextDisabled("Target: %s", target.empty() ? "(player)" : target.c_str());
        for (int i = 0; i < 5; ++i)
            if (ImGui::MenuItem(kTrackTypes[i])) {
                const auto type = static_cast<TrackType>(i);
                sequence_add_track(type,
                                   type == TrackType::Camera || type == TrackType::Event ? std::string() : target);
            }
        if (ImGui::BeginMenu("Property")) {
            const Entity e = target.empty() ? (player.is_null() ? selected : player) : selected;
            const auto props = e.is_null() ? std::vector<sequencer::AnimatableProperty>{}
                                           : sequencer::animatable_properties(s, e);
            if (props.empty()) ImGui::TextDisabled("Select an entity with animatable components");
            std::string group;
            for (const auto& p : props) {
                if (p.component != group) {
                    if (!group.empty()) ImGui::Separator();
                    ImGui::TextDisabled("%s", p.component.c_str());
                    group = p.component;
                }
                if (ImGui::MenuItem(std::format("{}##{}.{}", p.label, p.component, p.property).c_str()))
                    sequence_add_property_track(target, p.component, p.property);
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    const TrackType selected_type = seq_track_selected_ >= 0 && seq_track_selected_ < static_cast<int>(doc.tracks.size())
                                        ? doc.tracks[static_cast<std::size_t>(seq_track_selected_)].type
                                        : TrackType::Event;
    const bool can_record = selected_type == TrackType::Transform || selected_type == TrackType::Property;
    ImGui::BeginDisabled(!can_record);
    if (ImGui::Button("Record Key")) {
        if (selected_type == TrackType::Transform)
            sequence_record_transform(seq_track_selected_, seq_time_);
        else
            sequence_add_key(seq_track_selected_, seq_time_);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Keys the selected transform or property track's target at the playhead from its current value");
    ImGui::SameLine();
    ImGui::BeginDisabled(!sequence_key_time(seq_selected_));
    if (ImGui::Button("Delete Key")) sequence_delete_key(seq_selected_);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Double-click a lane to add a key, drag keys to move them, click the ruler to scrub.");

    // ---- timeline ----
    const float header_w = 240.0f, row_h = 24.0f, ruler_h = 20.0f;
    const float inspector_h = sequence_key_time(seq_selected_) ? 104.0f : 0.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float timeline_h = std::max(avail.y - inspector_h, 60.0f);
    // The curve view replaces the timeline lanes (like a dope sheet / curves toggle).
    if (seq_curves_) {
        draw_sequence_curves(timeline_h);
    } else {
        ImGui::BeginChild("seq_timeline", ImVec2(0, timeline_h), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float lane_w = std::max(doc.duration * seq_zoom_ + 40.0f, ImGui::GetContentRegionAvail().x - header_w);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float x0 = origin.x + header_w;
        auto time_x = [&](float time) { return x0 + time * seq_zoom_; };
        auto x_time = [&](float x) { return std::max((x - x0) / seq_zoom_, 0.0f); };

        // Ruler with second ticks; clicking or dragging scrubs.
        ImGui::SetCursorScreenPos(ImVec2(x0, origin.y));
        ImGui::InvisibleButton("seq_ruler", ImVec2(lane_w, ruler_h));
        if (ImGui::IsItemActive() && editing) sequence_seek(x_time(ImGui::GetIO().MousePos.x));
        dl->AddRectFilled(ImVec2(x0, origin.y), ImVec2(x0 + lane_w, origin.y + ruler_h), IM_COL32(40, 40, 46, 255));
        const float step = seq_zoom_ >= 120 ? 0.5f : seq_zoom_ >= 30 ? 1.0f : 5.0f;
        for (float tick = 0; tick <= doc.duration + 1e-3f; tick += step) {
            const float x = time_x(tick);
            dl->AddLine(ImVec2(x, origin.y + ruler_h * 0.55f), ImVec2(x, origin.y + ruler_h), IM_COL32(150, 150, 160, 255));
            dl->AddText(ImVec2(x + 2, origin.y), IM_COL32(170, 170, 180, 255), std::format("{:g}", tick).c_str());
        }
        dl->AddLine(ImVec2(time_x(doc.duration), origin.y),
                    ImVec2(time_x(doc.duration), origin.y + ruler_h + row_h * static_cast<float>(doc.tracks.size())),
                    IM_COL32(200, 80, 80, 160));
        accept_drop();

        int remove_track = -1;
        for (int i = 0; i < static_cast<int>(doc.tracks.size()); ++i) {
            Track& tr = doc.tracks[static_cast<std::size_t>(i)];
            ImGui::PushID(i);
            const float y = origin.y + ruler_h + row_h * static_cast<float>(i);
            // Header: type, label, mute and a context menu.
            ImGui::SetCursorScreenPos(ImVec2(origin.x + 4, y + 2));
            bool enabled = !tr.muted;
            if (ImGui::Checkbox("##enabled", &enabled))
                edit_sequence([&](Sequence& sq) { sq.tracks[static_cast<std::size_t>(i)].muted = !enabled; });
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enabled (unchecked tracks are muted)");
            ImGui::SameLine();
            std::string label = std::format("{} {}{}", sequencer::track_type_name(tr.type),
                                            tr.name.empty() ? tr.target : tr.name, tr.relative ? " (rel)" : "");
            if (tr.type == TrackType::Property) label += std::format(" {}.{}", tr.component, tr.property);
            if (ImGui::Selectable(label.c_str(), seq_track_selected_ == i, 0, ImVec2(header_w - 40, row_h - 6))) {
                if (seq_track_selected_ != i) seq_curve_frame_ = true;
                seq_track_selected_ = i;
            }
            if (ImGui::BeginPopupContextItem("seq_track_menu")) {
                seq_track_selected_ = i;
                if (ImGui::IsWindowAppearing())
                    std::snprintf(seq_target_buf_, sizeof(seq_target_buf_), "%s", tr.target.c_str());
                ImGui::SetNextItemWidth(180);
                if (ImGui::InputText("Target", seq_target_buf_, sizeof(seq_target_buf_),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                    const std::string v = seq_target_buf_;
                    edit_sequence([&](Sequence& sq) { sq.tracks[static_cast<std::size_t>(i)].target = v; });
                    ImGui::CloseCurrentPopup();
                }
                if (tr.type == TrackType::Transform && ImGui::MenuItem("Relative", nullptr, tr.relative)) {
                    const bool rel = !tr.relative;
                    edit_sequence([&](Sequence& sq) { sq.tracks[static_cast<std::size_t>(i)].relative = rel; });
                }
                if (ImGui::MenuItem("Remove Track")) remove_track = i;
                ImGui::EndPopup();
            }

            // Lane.
            const ImVec2 lane_min(x0, y), lane_max(x0 + lane_w, y + row_h);
            dl->AddRectFilled(lane_min, lane_max, i % 2 ? IM_COL32(32, 32, 36, 255) : IM_COL32(36, 36, 41, 255));
            ImGui::SetCursorScreenPos(lane_min);
            ImGui::InvisibleButton("lane", ImVec2(lane_w, row_h));
            const bool lane_hovered = ImGui::IsItemHovered();
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const ImU32 color = tr.muted ? IM_COL32(110, 110, 110, 255) : track_color(tr.type);

            // Keys: transform tracks draw position (top), rotation (middle) and scale (bottom) lanes.
            struct Hit {
                SeqKey key;
                float dist{1e9f};
            } hit;
            auto draw_keys = [&](SeqChannel c, float yoff) {
                const std::vector<float> times = with_channel(tr, c, [](auto& keys) {
                    std::vector<float> out;
                    for (const auto& k : keys)
                        out.push_back(k.time);
                    return out;
                });
                for (int k = 0; k < static_cast<int>(times.size()); ++k) {
                    const SeqKey key{i, c, k};
                    float kt = times[static_cast<std::size_t>(k)];
                    if (seq_drag_ && seq_drag_->moved &&
                        (seq_drag_->key == key ||
                         (c != SeqChannel::Keys && seq_drag_->key.track == i &&
                          seq_drag_->key.channel != SeqChannel::Keys && sequence_key_time(seq_drag_->key) &&
                          std::abs(*sequence_key_time(seq_drag_->key) - kt) <= kSameTime)))
                        kt = seq_drag_->time;
                    const ImVec2 p(time_x(kt), y + row_h * 0.5f + yoff);
                    const float r = c == SeqChannel::Keys ? 5.0f : 3.5f;
                    const bool sel = seq_selected_ == key;
                    dl->AddQuadFilled(ImVec2(p.x, p.y - r), ImVec2(p.x + r, p.y), ImVec2(p.x, p.y + r),
                                      ImVec2(p.x - r, p.y), sel ? IM_COL32(255, 230, 120, 255) : color);
                    if (lane_hovered) {
                        const float d = std::abs(mouse.x - p.x) + std::abs(mouse.y - p.y) * 0.5f;
                        if (d < 8.0f && d < hit.dist) hit = {key, d};
                    }
                }
            };
            if (tr.type == TrackType::Transform) {
                draw_keys(SeqChannel::Position, -6.0f);
                draw_keys(SeqChannel::Rotation, 0.0f);
                draw_keys(SeqChannel::Scale, 6.0f);
            } else {
                draw_keys(SeqChannel::Keys, 0.0f);
            }
            // Event, clip and camera labels next to their keys.
            if (tr.type != TrackType::Transform && seq_zoom_ >= 40)
                with_channel(tr, SeqChannel::Keys, [&](auto& keys) {
                    for (const auto& k : keys) {
                        std::string text;
                        if constexpr (requires { k.clip; })
                            text = k.clip;
                        else if constexpr (requires { k.camera; })
                            text = k.camera;
                        else if constexpr (requires { k.name; })
                            text = k.name;
                        dl->AddText(ImVec2(time_x(k.time) + 7, y + 4), IM_COL32(190, 190, 200, 255), text.c_str());
                    }
                    return 0;
                });

            if (lane_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (seq_track_selected_ != i) seq_curve_frame_ = true;
                seq_track_selected_ = i;
                if (hit.key.valid()) {
                    seq_selected_ = hit.key;
                    seq_drag_ = SeqDrag{hit.key, *sequence_key_time(hit.key), false};
                } else {
                    seq_selected_ = {};
                }
            }
            if (lane_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !hit.key.valid()) {
                const float at = x_time(mouse.x);
                if (tr.type == TrackType::Transform) {
                    if (editing) sequence_record_transform(i, at);
                } else {
                    std::string value;
                    if (tr.type == TrackType::Camera && !selected.is_null()) value = entity_name(s, selected);
                    sequence_add_key(i, at, value);
                }
            }
            ImGui::PopID();
        }
        // Key dragging (one undo step on release).
        if (seq_drag_) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const float dx = ImGui::GetIO().MouseDelta.x;
                if (dx != 0.0f) {
                    seq_drag_->time = std::max(seq_drag_->time + dx / seq_zoom_, 0.0f);
                    seq_drag_->moved = true;
                }
            } else {
                const SeqDrag d = *seq_drag_;
                seq_drag_.reset();
                if (d.moved) sequence_move_key(d.key, std::round(d.time * 100.0f) / 100.0f);
            }
        }
        if (remove_track >= 0) sequence_remove_track(remove_track);

        // Playhead.
        const float bottom = origin.y + ruler_h + row_h * static_cast<float>(doc.tracks.size());
        dl->AddLine(ImVec2(time_x(seq_time_), origin.y), ImVec2(time_x(seq_time_), std::max(bottom, origin.y + ruler_h)),
                    IM_COL32(255, 210, 80, 255), 2.0f);
        ImGui::SetCursorScreenPos(ImVec2(origin.x, bottom + 4));
        ImGui::Dummy(ImVec2(header_w + lane_w, 4));
        if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_Delete) && sequence_key_time(seq_selected_))
                sequence_delete_key(seq_selected_);
            if (ImGui::IsKeyPressed(ImGuiKey_Space)) sequence_preview_play(!seq_playing_);
        }
        ImGui::EndChild();
    } // timeline

    // ---- key inspector ----
    if (const auto key_time = sequence_key_time(seq_selected_)) {
        const SeqKey key = seq_selected_;
        if (seq_scratch_for_ != key || !ImGui::IsAnyItemActive()) {
            seq_scratch_ = doc.tracks[static_cast<std::size_t>(key.track)];
            seq_scratch_for_ = key;
        }
        bool commit = false;
        float kt = *key_time;
        ImGui::SetNextItemWidth(90);
        ImGui::DragFloat("Time", &kt, 0.01f, 0.0f, 3600.0f, "%.2f s");
        const bool move = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        Track& w = seq_scratch_;
        const auto idx = static_cast<std::size_t>(key.index);
        auto done = [&] { commit |= ImGui::IsItemDeactivatedAfterEdit(); };
        bool tangent_changed = false;
        // Interpolation, tangent mode and (User/Broken) slopes of a curve key.
        auto curve_ui = [&](auto& k, int n) {
            int it = static_cast<int>(k.interp);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            if (ImGui::Combo("Interp", &it, kInterps, kInterpCount)) {
                k.interp = static_cast<Interp>(it);
                commit = true;
            }
            int tg = static_cast<int>(k.tangent);
            ImGui::SetNextItemWidth(90);
            if (ImGui::Combo("Tangent", &tg, kTangentNames, 4)) {
                sequence_set_key_tangent(key, static_cast<Tangent>(tg)); // freezes the current slopes
                tangent_changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Slopes used by bezier segments: auto (smooth, no overshoot), flat, user (one slope "
                                  "for both sides) or broken (separate in and out slopes)");
            if (k.tangent == Tangent::User || k.tangent == Tangent::Broken) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(60.0f * static_cast<float>(n));
                ImGui::DragScalarN(k.tangent == Tangent::User ? "Slope" : "Out", ImGuiDataType_Float, &k.out_slope.x, n,
                                   0.01f);
                done();
                if (k.tangent == Tangent::User) k.in_slope = k.out_slope;
            }
            if (k.tangent == Tangent::Broken) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(60.0f * static_cast<float>(n));
                ImGui::DragScalarN("In", ImGuiDataType_Float, &k.in_slope.x, n, 0.01f);
                done();
            }
        };
        switch (key.channel) {
        case SeqChannel::Position:
        case SeqChannel::Scale: {
            auto& k = key.channel == SeqChannel::Position ? w.position[idx] : w.scale[idx];
            ImGui::SetNextItemWidth(220);
            ImGui::DragFloat3(key.channel == SeqChannel::Position ? "Position" : "Scale", &k.value.x, 0.01f);
            done();
            curve_ui(k, 3);
            break;
        }
        case SeqChannel::Rotation: {
            auto& k = w.rotation[idx];
            const math::Vec3 e = math::to_euler(k.value);
            math::Vec3 deg{math::degrees(e.x), math::degrees(e.y), math::degrees(e.z)};
            ImGui::SetNextItemWidth(220);
            if (ImGui::DragFloat3("Rotation", &deg.x, 0.5f))
                k.value = math::Quat::euler(math::radians(deg.x), math::radians(deg.y), math::radians(deg.z));
            done();
            int it = static_cast<int>(k.interp);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            if (ImGui::Combo("Interp", &it, kInterps, kInterpCount)) {
                k.interp = static_cast<Interp>(it);
                commit = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rotations ease (smooth) for cubic and bezier keys");
            break;
        }
        case SeqChannel::Keys:
            switch (w.type) {
            case TrackType::Animation: {
                auto& k = w.animation[idx];
                ImGui::SetNextItemWidth(160);
                ImGui::InputText("Clip", &k.clip);
                done();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70);
                ImGui::DragFloat("Fade", &k.fade, 0.01f, -1.0f, 10.0f, "%.2f");
                done();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70);
                ImGui::DragFloat("Speed", &k.speed, 0.01f, -10.0f, 10.0f, "%.2f");
                done();
                ImGui::SameLine();
                commit |= ImGui::Checkbox("Loop", &k.loop);
                break;
            }
            case TrackType::Audio: {
                auto& k = w.audio[idx];
                ImGui::SetNextItemWidth(160);
                ImGui::InputText("Clip", &k.clip);
                done();
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                        Uuid id;
                        std::memcpy(&id, p->Data, sizeof(Uuid));
                        if (const auto* rec = db_ ? db_->find(id) : nullptr;
                            rec && rec->kind == assets::AssetKind::Audio) {
                            k.clip = rec->path;
                            commit = true;
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70);
                ImGui::DragFloat("Volume", &k.volume, 0.01f, 0.0f, 4.0f, "%.2f");
                done();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70);
                ImGui::DragFloat("Pitch", &k.pitch, 0.01f, 0.05f, 4.0f, "%.2f");
                done();
                ImGui::SameLine();
                int bus = 2;
                for (int b = 0; b < 6; ++b)
                    if (k.bus == kBuses[b]) bus = b;
                ImGui::SetNextItemWidth(90);
                if (ImGui::Combo("Bus", &bus, kBuses, 6)) {
                    k.bus = kBuses[bus];
                    commit = true;
                }
                ImGui::SameLine();
                commit |= ImGui::Checkbox("Spatial", &k.spatial);
                break;
            }
            case TrackType::Event: {
                auto& k = w.events[idx];
                ImGui::SetNextItemWidth(160);
                ImGui::InputText("Name", &k.name);
                done();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(200);
                ImGui::InputText("Payload", &k.payload);
                done();
                break;
            }
            case TrackType::Camera: {
                auto& k = w.cameras[idx];
                ImGui::SetNextItemWidth(200);
                ImGui::InputText("Camera", &k.camera);
                done();
                if (!selected.is_null() && s.world().get<CameraComponent>(selected)) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Use Selection")) {
                        k.camera = entity_name(s, selected);
                        commit = true;
                    }
                }
                break;
            }
            case TrackType::Property: {
                auto& k = w.values[idx];
                const int n = std::clamp<int>(w.channels, 1, 4);
                ImGui::SetNextItemWidth(n == 4 ? 240.0f : 70.0f * static_cast<float>(n));
                const std::string label = w.component + "." + w.property;
                if (n == 4)
                    ImGui::ColorEdit4(label.c_str(), &k.value.x, ImGuiColorEditFlags_Float);
                else
                    ImGui::DragScalarN(label.c_str(), ImGuiDataType_Float, &k.value.x, n, 0.01f);
                done();
                curve_ui(k, n);
                break;
            }
            case TrackType::Transform:
                break;
            }
            break;
        }
        if (tangent_changed) commit = false; // already applied as its own undo step
        if (w.type == TrackType::Event && w.events[idx].name.empty()) commit = false; // an event needs a name
        if (commit) {
            const Track edited = seq_scratch_;
            edit_sequence([&](Sequence& sq) { sq.tracks[static_cast<std::size_t>(key.track)] = edited; });
            seq_selected_ = key;
        }
        if (move && std::abs(kt - *key_time) > kSameTime) sequence_move_key(key, kt);
    }
    ImGui::End();
}

// ---- curve editor -----------------------------------------------------------------------------

namespace {

// Grid spacing near `target` units: 1, 2 or 5 times a power of ten.
float nice_step(float target) {
    if (!(target > 0) || !std::isfinite(target)) return 1.0f;
    const float p = std::pow(10.0f, std::floor(std::log10(target)));
    for (float m : {1.0f, 2.0f, 5.0f})
        if (m * p >= target) return m * p;
    return 10.0f * p;
}

} // namespace

constexpr float kCurvePad = 10.0f;

std::optional<math::Vec2> Editor::sequence_curve_key_screen(SeqKey key, int component) const {
    const auto t = sequence_key_time(key);
    const auto v = sequence_key_value(key, component);
    if (!t || !v || seq_curve_canvas_.z <= 0 || !(seq_curve_hi_ > seq_curve_lo_)) return std::nullopt;
    const float vscale = (seq_curve_canvas_.w - 2 * kCurvePad) / (seq_curve_hi_ - seq_curve_lo_);
    return math::Vec2{seq_curve_canvas_.x + kCurvePad + (*t - seq_curve_t0_) * seq_zoom_,
                      seq_curve_canvas_.y + seq_curve_canvas_.w - kCurvePad - (*v - seq_curve_lo_) * vscale};
}

void Editor::draw_sequence_curves(float height) {
    ImGui::BeginChild("seq_curves", ImVec2(0, height), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    Sequence& doc = *seq_doc_;
    auto is_curve_track = [&](int i) {
        return i >= 0 && i < static_cast<int>(doc.tracks.size()) &&
               (doc.tracks[static_cast<std::size_t>(i)].type == TrackType::Transform ||
                doc.tracks[static_cast<std::size_t>(i)].type == TrackType::Property);
    };
    if (!is_curve_track(seq_track_selected_)) // the timeline is hidden: pick the first curve track
        for (int i = 0; i < static_cast<int>(doc.tracks.size()); ++i)
            if (is_curve_track(i)) {
                seq_track_selected_ = i;
                seq_selected_ = {};
                seq_curve_frame_ = true;
                break;
            }
    const int ti = seq_track_selected_;
    if (!is_curve_track(ti)) {
        ImGui::TextDisabled("No transform or property tracks: add one with + Track, then edit its curves here.");
        ImGui::EndChild();
        return;
    }
    const Track& tr = doc.tracks[static_cast<std::size_t>(ti)];

    // Curves of the track: position/scale x, y, z or the property components.
    struct Curve {
        SeqChannel channel;
        int component;
        ImU32 color;
        std::string name;
        int bit;
    };
    std::vector<Curve> curves;
    const ImU32 kAxis[] = {IM_COL32(235, 95, 95, 255), IM_COL32(110, 210, 110, 255), IM_COL32(100, 150, 255, 255),
                           IM_COL32(220, 220, 220, 255)};
    if (tr.type == TrackType::Transform) {
        for (int c = 0; c < 3; ++c)
            curves.push_back({SeqChannel::Position, c, kAxis[c], std::format("Position.{}", "xyz"[c]), c});
        for (int c = 0; c < 3; ++c)
            curves.push_back({SeqChannel::Scale, c, kAxis[c], std::format("Scale.{}", "xyz"[c]), 8 + c});
    } else {
        const int n = std::clamp<int>(tr.channels, 1, 4);
        for (int c = 0; c < n; ++c)
            curves.push_back({SeqChannel::Keys, c, n == 1 ? IM_COL32(90, 210, 210, 255) : kAxis[c],
                              n == 1 ? tr.property : std::format("{}.{}", tr.property, n == 4 ? "rgba"[c] : "xyzw"[c]),
                              12 + c});
    }
    auto visible = [&](const Curve& c) { return (seq_curve_hidden_ & (1u << c.bit)) == 0; };

    // Track selector (the timeline is hidden in this view) and per-curve toggles.
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("##curve_track", std::format("{} {}{}", sequencer::track_type_name(tr.type),
                                                       tr.name.empty() ? tr.target : tr.name,
                                                       tr.type == TrackType::Property ? "." + tr.property : "")
                                               .c_str())) {
        for (int i = 0; i < static_cast<int>(doc.tracks.size()); ++i) {
            const Track& o = doc.tracks[static_cast<std::size_t>(i)];
            if (o.type != TrackType::Transform && o.type != TrackType::Property) continue;
            const std::string label = std::format("{} {}{}##{}", sequencer::track_type_name(o.type),
                                                  o.name.empty() ? o.target : o.name,
                                                  o.type == TrackType::Property ? " " + o.component + "." + o.property : "", i);
            if (ImGui::Selectable(label.c_str(), i == ti)) {
                seq_track_selected_ = i;
                seq_selected_ = {};
                seq_curve_frame_ = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    for (const Curve& c : curves) {
        bool on = visible(c);
        ImGui::PushStyleColor(ImGuiCol_CheckMark, c.color);
        if (ImGui::Checkbox(c.name.c_str(), &on)) seq_curve_hidden_ ^= 1u << c.bit;
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("Frame")) seq_curve_frame_ = true;
    ImGui::SameLine();
    ImGui::TextDisabled("Drag keys or tangent handles (Alt breaks), double-click adds a key, right-click for modes, "
                        "wheel zooms values (Shift: time), middle-drag pans.%s",
                        tr.type == TrackType::Transform ? " Rotation keys are edited in the timeline." : "");

    // Key data with an in-progress drag applied, so the curve follows the mouse.
    auto keys_of = [&](SeqChannel ch, auto&& fn) {
        with_curve(tr, ch, [&](const auto& stored, int) {
            auto keys = stored;
            if (seq_curve_drag_ && seq_curve_drag_->moved && seq_curve_drag_->key.track == ti &&
                seq_curve_drag_->key.channel == ch && seq_curve_drag_->key.index < static_cast<int>(keys.size())) {
                const auto& d = *seq_curve_drag_;
                const auto i = static_cast<std::size_t>(d.key.index);
                if (d.handle == 0)
                    (void)apply_move(keys, i, d.time, d.component, d.value);
                else
                    apply_slope(keys, i, d.component, d.handle == 1 ? SeqSide::In : SeqSide::Out, d.slope,
                                ImGui::GetIO().KeyAlt);
            }
            fn(keys);
        });
    };

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(ImGui::GetContentRegionAvail().x, 50.0f), std::max(ImGui::GetContentRegionAvail().y, 40.0f));
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    const float pad = kCurvePad;

    if (seq_curve_frame_) { // fit the visible curves (sampled, so bezier overshoot shows) and the duration
        seq_curve_frame_ = false;
        float lo = 1e30f, hi = -1e30f;
        for (const Curve& c : curves) {
            if (!visible(c)) continue;
            keys_of(c.channel, [&](const auto& keys) {
                if (keys.empty()) return;
                const float a = keys.front().time, b = keys.back().time;
                for (int i = 0; i <= 200; ++i) {
                    const float v = comp(*sequencer::sample(keys, a + (b - a) * static_cast<float>(i) / 200.0f), c.component);
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
            });
        }
        if (lo > hi) lo = -1, hi = 1;
        if (hi - lo < 1e-3f) lo -= 1, hi += 1;
        const float m = (hi - lo) * 0.1f;
        seq_curve_lo_ = lo - m;
        seq_curve_hi_ = hi + m;
        seq_curve_t0_ = 0;
        seq_zoom_ = std::clamp((size.x - 2 * pad) / std::max(doc.duration, 0.1f), 10.0f, 600.0f);
    }
    if (!(seq_curve_hi_ - seq_curve_lo_ > 1e-6f)) seq_curve_hi_ = seq_curve_lo_ + 1;
    seq_curve_canvas_ = {p0.x, p0.y, size.x, size.y};
    const float vscale = (size.y - 2 * pad) / (seq_curve_hi_ - seq_curve_lo_); // pixels per value unit
    auto tx = [&](float t) { return p0.x + pad + (t - seq_curve_t0_) * seq_zoom_; };
    auto xt = [&](float x) { return (x - p0.x - pad) / seq_zoom_ + seq_curve_t0_; };
    auto vy = [&](float v) { return p1.y - pad - (v - seq_curve_lo_) * vscale; };
    auto yv = [&](float y) { return (p1.y - pad - y) / vscale + seq_curve_lo_; };

    ImGui::InvisibleButton("seq_curve_canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;

    // Navigation.
    if (hovered && io.MouseWheel != 0.0f) {
        const float f = std::pow(1.15f, -io.MouseWheel);
        if (io.KeyShift) {
            const float at = xt(mouse.x);
            seq_zoom_ = std::clamp(seq_zoom_ / f, 10.0f, 600.0f);
            seq_curve_t0_ = at - (mouse.x - p0.x - pad) / seq_zoom_;
        } else {
            const float at = yv(mouse.y);
            seq_curve_lo_ = at + (seq_curve_lo_ - at) * f;
            seq_curve_hi_ = at + (seq_curve_hi_ - at) * f;
        }
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
        seq_curve_t0_ -= io.MouseDelta.x / seq_zoom_;
        const float dv = io.MouseDelta.y / vscale;
        seq_curve_lo_ += dv;
        seq_curve_hi_ += dv;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, p1, true);
    dl->AddRectFilled(p0, p1, IM_COL32(28, 28, 32, 255));
    // Grid: values (horizontal) and seconds (vertical), labelled.
    const float vstep = nice_step(40.0f / vscale);
    for (float v = std::ceil(seq_curve_lo_ / vstep) * vstep; v <= seq_curve_hi_; v += vstep) {
        const float y = vy(v);
        dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y),
                    std::abs(v) < vstep * 0.01f ? IM_COL32(90, 90, 100, 255) : IM_COL32(50, 50, 58, 255));
        dl->AddText(ImVec2(p0.x + 2, y - 14), IM_COL32(130, 130, 140, 255),
                    std::format("{:g}", std::abs(v) < vstep * 0.01f ? 0.0f : v).c_str());
    }
    const float tstep = nice_step(60.0f / seq_zoom_);
    for (float t = std::max(std::ceil(seq_curve_t0_ / tstep) * tstep, 0.0f); tx(t) <= p1.x; t += tstep) {
        dl->AddLine(ImVec2(tx(t), p0.y), ImVec2(tx(t), p1.y), IM_COL32(45, 45, 52, 255));
        dl->AddText(ImVec2(tx(t) + 2, p1.y - 16), IM_COL32(130, 130, 140, 255), std::format("{:g}s", t).c_str());
    }
    dl->AddLine(ImVec2(tx(doc.duration), p0.y), ImVec2(tx(doc.duration), p1.y), IM_COL32(200, 80, 80, 160));

    // Curves, keys and the selected key's tangent handles; hit testing on the way.
    struct Hit {
        SeqKey key;
        int component{0};
        int handle{0};
        float dist{1e9f};
    } hit;
    const float handle_len = 40.0f;
    struct Nearest {
        SeqChannel channel{SeqChannel::Keys};
        float dist{1e9f};
    } nearest;
    for (const Curve& c : curves) {
        if (!visible(c)) continue;
        keys_of(c.channel, [&](const auto& keys) {
            if (keys.empty()) return;
            const ImU32 col = c.color;
            const ImU32 dim = (col & 0x00ffffffu) | 0x60000000u;
            // Polyline sampled every 2 px; held (flat) before the first and after the last key.
            std::vector<ImVec2> pts;
            for (float x = p0.x; x <= p1.x + 2.0f; x += 2.0f) {
                const float v = comp(*sequencer::sample(keys, xt(x)), c.component);
                pts.emplace_back(x, vy(v));
            }
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), dim, 1.0f);
            const float a = tx(keys.front().time), b = tx(keys.back().time);
            std::vector<ImVec2> active;
            for (const ImVec2& p : pts)
                if (p.x >= a && p.x <= b) active.push_back(p);
            if (active.size() > 1) dl->AddPolyline(active.data(), static_cast<int>(active.size()), col, 2.0f);
            if (hovered) {
                const float d = std::abs(vy(comp(*sequencer::sample(keys, xt(mouse.x)), c.component)) - mouse.y);
                if (d < nearest.dist) nearest = {c.channel, d};
            }
            for (int k = 0; k < static_cast<int>(keys.size()); ++k) {
                const auto& key = keys[static_cast<std::size_t>(k)];
                const ImVec2 p(tx(key.time), vy(comp(key.value, c.component)));
                const SeqKey id{ti, c.channel, k};
                const bool sel = seq_selected_ == id && seq_curve_component_ == c.component;
                const float r = sel ? 4.5f : 3.5f;
                dl->AddRectFilled(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r),
                                  sel ? IM_COL32(255, 230, 120, 255) : col);
                if (hovered) {
                    const float d = std::abs(mouse.x - p.x) + std::abs(mouse.y - p.y);
                    if (d < 9.0f && d <= hit.dist) hit = {id, c.component, 0, d}; // topmost (last drawn) wins
                }
                if (!sel) continue;
                const auto sl = sequencer::key_slopes(keys, static_cast<std::size_t>(k));
                for (int side = 1; side <= 2; ++side) {
                    if (side == 1 && k == 0) continue;
                    if (side == 2 && k + 1 == static_cast<int>(keys.size())) continue;
                    const float slope = comp(side == 1 ? sl.in : sl.out, c.component);
                    // Handle direction in screen space: (zoom, -slope * vscale), fixed length.
                    float dx = seq_zoom_, dy = -slope * vscale;
                    const float len = std::sqrt(dx * dx + dy * dy);
                    dx = dx / len * handle_len;
                    dy = dy / len * handle_len;
                    if (side == 1) dx = -dx, dy = -dy;
                    const ImVec2 h(p.x + dx, p.y + dy);
                    dl->AddLine(p, h, IM_COL32(200, 200, 200, 200));
                    dl->AddCircleFilled(h, 3.5f, key.tangent == Tangent::Broken ? IM_COL32(255, 150, 80, 255)
                                                                                : IM_COL32(230, 230, 230, 255));
                    if (hovered) {
                        const float d = std::abs(mouse.x - h.x) + std::abs(mouse.y - h.y);
                        if (d < 9.0f && d <= hit.dist) hit = {id, c.component, side, d};
                    }
                }
            }
        });
    }
    // Playhead.
    dl->AddLine(ImVec2(tx(seq_time_), p0.y), ImVec2(tx(seq_time_), p1.y), IM_COL32(255, 210, 80, 255), 2.0f);
    dl->PopClipRect();

    // Selection and dragging (one undo step on release).
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hit.key.valid()) {
            seq_selected_ = hit.key;
            seq_curve_component_ = hit.component;
            const float t0 = *sequence_key_time(hit.key);
            const float v0 = *sequence_key_value(hit.key, hit.component);
            seq_curve_drag_ = SeqCurveDrag{hit.key, hit.component, hit.handle, t0, v0, 0.0f, false};
        } else {
            seq_selected_ = {};
        }
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !hit.key.valid() && nearest.dist < 1e8f) {
        sequence_insert_curve_key(ti, nearest.channel, std::round(std::max(xt(mouse.x), 0.0f) * 100.0f) / 100.0f);
        seq_curve_drag_.reset();
    }
    if (seq_curve_drag_) {
        SeqCurveDrag& d = *seq_curve_drag_;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
                d.moved = true;
                if (d.handle == 0) {
                    d.time = std::max(d.time + io.MouseDelta.x / seq_zoom_, 0.0f);
                    d.value -= io.MouseDelta.y / vscale;
                } else if (const auto kt = sequence_key_time(d.key)) {
                    const float kv = *sequence_key_value(d.key, d.component);
                    float dx = mouse.x - tx(*kt);
                    const float dy = mouse.y - vy(kv);
                    dx = d.handle == 1 ? std::min(dx, -1.0f) : std::max(dx, 1.0f);
                    d.slope = (-dy / vscale) / (dx / seq_zoom_);
                }
            }
        } else {
            const SeqCurveDrag done = d;
            seq_curve_drag_.reset();
            if (done.moved) {
                if (done.handle == 0)
                    sequence_move_curve_key(done.key, std::round(done.time * 100.0f) / 100.0f, done.component, done.value);
                else
                    sequence_set_key_slope(done.key, done.component, done.handle == 1 ? SeqSide::In : SeqSide::Out,
                                           done.slope, io.KeyAlt);
            }
        }
    }

    // Context menu: interpolation and tangent modes of the clicked key.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hit.key.valid()) {
        seq_selected_ = hit.key;
        seq_curve_component_ = hit.component;
        ImGui::OpenPopup("seq_curve_key");
    }
    if (ImGui::BeginPopup("seq_curve_key")) {
        const SeqKey key = seq_selected_;
        if (sequence_key_components(key) > 0) {
            Interp interp = Interp::Linear;
            Tangent tangent = Tangent::Auto;
            with_curve(doc.tracks[static_cast<std::size_t>(key.track)], key.channel, [&](const auto& keys, int) {
                interp = keys[static_cast<std::size_t>(key.index)].interp;
                tangent = keys[static_cast<std::size_t>(key.index)].tangent;
            });
            ImGui::TextDisabled("Interpolation");
            for (int i = 0; i < kInterpCount; ++i)
                if (ImGui::MenuItem(kInterps[i], nullptr, static_cast<int>(interp) == i))
                    sequence_set_key_interp(key, static_cast<Interp>(i));
            ImGui::Separator();
            ImGui::TextDisabled("Tangent");
            for (int i = 0; i < 4; ++i)
                if (ImGui::MenuItem(kTangentNames[i], nullptr, static_cast<int>(tangent) == i))
                    sequence_set_key_tangent(key, static_cast<Tangent>(i));
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Key", "Del")) sequence_delete_key(key, false);
        }
        ImGui::EndPopup();
    }
    if (ImGui::IsWindowFocused() && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete) &&
        sequence_key_components(seq_selected_) > 0)
        sequence_delete_key(seq_selected_, false);
    if (ImGui::IsWindowFocused() && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F)) seq_curve_frame_ = true;
    ImGui::EndChild();
}

} // namespace victus::editor
