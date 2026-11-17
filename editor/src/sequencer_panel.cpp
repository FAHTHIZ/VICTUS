// SPDX-License-Identifier: Apache-2.0
// Sequencer panel (Window > Sequencer, docs/SEQUENCER.md): edits one .vseq timeline with its own
// undo history, previews it on a SequencePlayer entity of the edit scene by scrubbing or playing,
// records transform keys from the scene and saves the file.
#include "victus/editor/editor.hpp"

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
using sequencer::Sequence;
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
    case TrackType::Event:
        break;
    }
    return IM_COL32(235, 235, 235, 255);
}

const char* const kTrackTypes[] = {"Transform", "Animation", "Audio", "Event", "Camera"};
const char* const kInterps[] = {"step", "linear", "smooth", "cubic"};
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

bool Editor::sequence_delete_key(SeqKey key) {
    const auto time = sequence_key_time(key);
    if (!time) return false;
    edit_sequence([&](Sequence& sq) {
        Track& w = sq.tracks[static_cast<std::size_t>(key.track)];
        if (key.channel == SeqChannel::Keys) {
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

    // Preview player.
    if (player.is_null()) {
        ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "No preview player.");
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
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    const bool can_record = seq_track_selected_ >= 0 && seq_track_selected_ < static_cast<int>(doc.tracks.size()) &&
                            doc.tracks[static_cast<std::size_t>(seq_track_selected_)].type == TrackType::Transform;
    ImGui::BeginDisabled(!can_record);
    if (ImGui::Button("Record Key")) sequence_record_transform(seq_track_selected_, seq_time_);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Keys the selected transform track's target at the playhead from its current transform");
    ImGui::SameLine();
    ImGui::BeginDisabled(!sequence_key_time(seq_selected_));
    if (ImGui::Button("Delete Key")) sequence_delete_key(seq_selected_);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Double-click a lane to add a key, drag keys to move them, click the ruler to scrub.");

    // ---- timeline ----
    const float header_w = 240.0f, row_h = 24.0f, ruler_h = 20.0f;
    const float inspector_h = sequence_key_time(seq_selected_) ? 96.0f : 0.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("seq_timeline", ImVec2(0, std::max(avail.y - inspector_h, 60.0f)), ImGuiChildFlags_Borders,
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
        const std::string label = std::format("{} {}{}", sequencer::track_type_name(tr.type),
                                              tr.name.empty() ? tr.target : tr.name, tr.relative ? " (rel)" : "");
        if (ImGui::Selectable(label.c_str(), seq_track_selected_ == i, 0, ImVec2(header_w - 40, row_h - 6)))
            seq_track_selected_ = i;
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
        switch (key.channel) {
        case SeqChannel::Position:
        case SeqChannel::Scale: {
            auto& k = key.channel == SeqChannel::Position ? w.position[idx] : w.scale[idx];
            ImGui::SetNextItemWidth(220);
            ImGui::DragFloat3(key.channel == SeqChannel::Position ? "Position" : "Scale", &k.value.x, 0.01f);
            done();
            int it = static_cast<int>(k.interp);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            if (ImGui::Combo("Interp", &it, kInterps, 4)) {
                k.interp = static_cast<sequencer::Interp>(it);
                commit = true;
            }
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
            if (ImGui::Combo("Interp", &it, kInterps, 4)) {
                k.interp = static_cast<sequencer::Interp>(it);
                commit = true;
            }
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
            case TrackType::Transform:
                break;
            }
            break;
        }
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

} // namespace victus::editor
