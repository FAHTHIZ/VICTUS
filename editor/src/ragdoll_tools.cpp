// SPDX-License-Identifier: Apache-2.0
// The Inspector's ragdoll card (A.R.C., ADR-0091): turns a character such as Orion or
// Nova into a ragdoll in one undoable step (physics::make_humanoid_ragdoll), switches how the body
// behaves with four plain-language buttons and removes the ragdoll again.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include "victus/core/fs.hpp"
#include "victus/project/characters.hpp"
#include "victus/assets/instantiate.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

namespace victus::editor {

namespace tk = ui::tokens;
using namespace scene;
using RagdollMode = physics::RagdollComponent::Mode;

namespace {

constexpr std::size_t kMaxSubtree = 4096;

void subtree(const Scene& s, Entity e, std::vector<Entity>& out, int depth) {
    if (depth > 128 || out.size() >= kMaxSubtree) return;
    out.push_back(e);
    for (const Entity c : s.children(e)) subtree(s, c, out, depth + 1);
}

std::vector<Entity> subtree(const Scene& s, Entity root) {
    std::vector<Entity> out;
    if (s.world().alive(root)) subtree(s, root, out, 0);
    return out;
}

std::vector<Uuid> uuids(const Scene& s, const std::vector<Entity>& es) {
    std::vector<Uuid> ids;
    ids.reserve(es.size());
    for (const Entity e : es) ids.push_back(s.uuid(e));
    return ids;
}

struct ModeInfo {
    RagdollMode mode;
    const char* label;
    const char* help;
};

constexpr ModeInfo kModes[] = {
    {RagdollMode::Off, "Animated", "No ragdoll: the character plays its animations as usual."},
    {RagdollMode::Kinematic, "Solid",
     "The body follows the animation but is solid: thrown objects bounce off its arms, legs and head."},
    {RagdollMode::Powered, "Powered",
     "Muscles: the joints try to hold the animated pose but react to hits and pushes, like a stumbling person."},
    {RagdollMode::Passive, "Floppy", "Limp: the character falls and tumbles freely, like a knocked-out person."},
};

} // namespace

const char* ragdoll_mode_label(RagdollMode mode) noexcept {
    for (const ModeInfo& m : kModes)
        if (m.mode == mode) return m.label;
    return "Animated";
}

bool Editor::can_make_ragdoll(Entity root) const {
    const Scene& s = const_cast<Editor*>(this)->active_scene();
    for (const Entity e : subtree(s, root)) {
        const auto* n = s.world().get<Name>(e);
        if (!n) continue;
        std::string lower = n->value;
        std::ranges::transform(lower, lower.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        if (lower.find("hips") != std::string::npos || lower.find("pelvis") != std::string::npos) return true;
    }
    return false;
}

std::size_t Editor::ragdoll_bone_count(Entity root) const {
    const Scene& s = const_cast<Editor*>(this)->active_scene();
    std::size_t n = 0;
    for (const Entity e : subtree(s, root))
        if (s.world().get<physics::RagdollBoneComponent>(e)) ++n;
    return n;
}

std::size_t Editor::make_ragdoll(Entity root) {
    Scene& s = active_scene();
    if (!s.world().alive(root)) return 0;
    auto before = capture(s, uuids(s, subtree(s, root)));
    const std::size_t bones = physics::make_humanoid_ragdoll(s, root);
    if (bones == 0) {
        notify("No humanoid skeleton found: a ragdoll needs bones named like Hips, Spine, Head, UpperArm.L ...", true);
        return 0;
    }
    record_edit("Make Ragdoll", std::move(before));
    notify(std::format("Ragdoll made: {} body parts. Choose Floppy and press Play to see it fall.", bones));
    return bones;
}

bool Editor::set_ragdoll_mode(Entity root, RagdollMode mode) {
    Scene& s = active_scene();
    auto* rd = s.world().alive(root) ? s.world().get<physics::RagdollComponent>(root) : nullptr;
    if (!rd) return false;
    if (rd->mode == mode) return true;
    auto before = capture(s, {s.uuid(root)});
    rd->mode = mode;
    record_edit(std::format("Ragdoll: {}", ragdoll_mode_label(mode)), std::move(before));
    return true;
}

bool Editor::ragdoll_get_up(Entity root) {
    if (play_state_ == PlayState::Editing || !play_physics_) {
        notify("Get Up works in play mode: press Play, let the character fall, then press Get Up.");
        return false;
    }
    Scene& s = active_scene();
    if (!s.world().alive(root)) return false;
    const physics::RagdollGetUp g = play_physics_->begin_ragdoll_get_up(s, root);
    if (!g.started) {
        notify(std::format("Cannot get up: {}.", g.reason));
        return false;
    }
    notify(g.face_up ? "Getting up from the back." : "Getting up from the front.");
    return true;
}

std::size_t Editor::remove_ragdoll(Entity root) {
    Scene& s = active_scene();
    const auto all = subtree(s, root);
    auto before = capture(s, uuids(s, all));
    std::size_t removed = 0;
    for (const Entity e : all)
        if (s.world().get<physics::RagdollBoneComponent>(e)) {
            s.world().remove<physics::RagdollBoneComponent>(e);
            ++removed;
        }
    const bool had_root = s.world().get<physics::RagdollComponent>(root) != nullptr;
    if (had_root) s.world().remove<physics::RagdollComponent>(root);
    if (removed == 0 && !had_root) return 0;
    record_edit("Remove Ragdoll", std::move(before));
    return removed + (had_root ? 1 : 0);
}

Result<Uuid> Editor::ensure_starter_character(std::string_view id) {
    if (!project_ || !db_) return make_error(ErrorCode::InvalidArgument, "no project open");
    const project::StarterCharacter* c = project::find_starter_character(id);
    if (!c) return make_error(ErrorCode::NotFound, std::format("no starter character '{}'", id));
    Uuid uuid;
    if (!Uuid::parse(c->uuid, uuid)) return make_error(ErrorCode::InvalidArgument, "bad starter character uuid");
    if (db_->find(uuid)) return uuid;
    // Projects made without a character: copy the model in, like Import would.
    const fs::Path file = project_->root() / fs::Path(std::string(c->model_path));
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    {
        const auto glb = project::starter_character_glb(*c);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
        if (!out) return make_error(ErrorCode::IoError, "cannot write " + file.string());
    }
    fs::Path meta = file;
    meta += ".meta";
    if (auto st = fs::write_text_atomic(meta, project::starter_character_meta(*c)); !st.ok()) return st.error();
    (void)db_->refresh();
    (void)db_->import_all();
    if (!db_->find(uuid)) return make_error(ErrorCode::NotFound, "the character model did not import");
    return uuid;
}

Entity Editor::add_character(std::string_view id, Entity parent, math::Vec3 position, bool ragdoll, RagdollMode mode,
                             bool record) {
    auto uuid = ensure_starter_character(id);
    if (!uuid) {
        notify("cannot add the character: " + uuid.error().message, true);
        return kNullEntity;
    }
    auto model = cache_ ? cache_->model(uuid.value()) : decltype(cache_->model(uuid.value()))(
                                                            make_error(ErrorCode::InvalidArgument, "no asset cache"));
    if (!model) {
        notify("cannot load the character: " + model.error().message, true);
        return kNullEntity;
    }
    const project::StarterCharacter* c = project::find_starter_character(id);
    Scene& s = active_scene();
    const Entity root = assets::instantiate_model(s, uuid.value(), (*model)->cpu, std::string(c->display_name));
    if (!parent.is_null()) s.set_parent(root, parent);
    s.world().get<Transform>(root)->position = position;
    if (auto* a = s.world().get<animation::AnimatorComponent>(root)) a->clip = "Idle";
    if (ragdoll && physics::make_humanoid_ragdoll(s, root) > 0)
        if (auto* rd = s.world().get<physics::RagdollComponent>(root)) rd->mode = mode;
    s.update_transforms();
    if (record && play_state_ == PlayState::Editing) {
        auto after = capture_subtrees(s, {s.uuid(root)});
        undo_.push({std::format("Add {}", c->display_name), absent(after), after});
        update_title();
    }
    selection_ = {s.uuid(root)};
    return root;
}

Entity Editor::add_ragdoll_lab() {
    if (!project_) return kNullEntity;
    Scene& s = active_scene();
    const math::Vec3 at = camera_.target;
    const Entity lab = s.create("Ragdoll Lab");
    s.world().get<Transform>(lab)->position = at;
    // A solid floor so the bodies have something to land on in any scene.
    const Entity floor = s.create("Lab Floor", lab);
    {
        auto& m = s.world().add<MeshRendererComponent>(floor);
        m.primitive = "cube";
        m.base_color = {0.32f, 0.34f, 0.38f, 1};
    }
    auto* ft = s.world().get<Transform>(floor);
    ft->position = {0, -0.1f, 0};
    ft->scale = {8, 0.2f, 8};
    s.world().add<physics::RigidBodyComponent>(floor).type = physics::BodyType::Static;
    s.world().add<physics::ColliderComponent>(floor);
    // Orion goes limp; Nova keeps her muscles and stumbles. Both start slightly in the air.
    const Entity a = add_character("orion", lab, {-0.9f, 0.4f, 0}, true, RagdollMode::Passive, false);
    const Entity b = add_character("nova", lab, {0.9f, 0.0f, 0}, true, RagdollMode::Powered, false);
    if (!a.is_null()) s.world().get<Transform>(a)->rotation = math::Quat::euler(math::radians(12.0f), 0, math::radians(-8.0f));
    if (!b.is_null()) s.world().get<Transform>(b)->rotation = math::Quat::euler(0, math::radians(-20.0f), 0);
    s.update_transforms();
    if (play_state_ == PlayState::Editing) {
        auto after = capture_subtrees(s, {s.uuid(lab)});
        undo_.push({"Add Ragdoll Lab", absent(after), after});
        update_title();
    }
    selection_ = {s.uuid(lab)};
    notify("Ragdoll Lab added: press Play. Orion is floppy, Nova is powered.");
    return lab;
}

void Editor::draw_ragdoll_section(Entity e) {
    Scene& s = active_scene();
    auto* rd = s.world().get<physics::RagdollComponent>(e);
    if (!rd && !can_make_ragdoll(e)) return;
    ImGui::PushID("##ragdoll_card");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, tk::kSurfaceRaised);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, tk::kRadius);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(tk::kSpace4, tk::kSpace3));
    ImGui::BeginChild("##ragdoll", ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
    kit::icon(kit::Icon::Physics, tk::kAccent);
    ImGui::SameLine();
    if (!rd) {
        ImGui::PushFont(kit::bold_font(), 0.0f);
        ImGui::TextUnformatted("Ragdoll body");
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(tk::kTextMuted,
                           "This looks like a character. Give it a physical body so it can fall, tumble and be "
                           "knocked over.");
        ImGui::PopTextWrapPos();
        if (kit::icon_text_button("Make Ragdoll", kit::Icon::Plus, true, true, ImVec2(-1, 0))) (void)make_ragdoll(e);
        kit::tooltip("Adds a physical body part to the hips, spine, chest, head, arms, hands, legs and feet, "
                     "with joint limits like a real body. Undo removes it again.");
    } else {
        const std::size_t bones = ragdoll_bone_count(e);
        ImGui::PushFont(kit::bold_font(), 0.0f);
        ImGui::TextUnformatted("Ragdoll");
        ImGui::PopFont();
        ImGui::SameLine();
        kit::pill(std::format("{} body parts", bones), bones > 0 ? ui::Tone::Success : ui::Tone::Warning);
        ImGui::TextColored(tk::kTextMuted, "How the body behaves:");
        const float gap = tk::kSpace2;
        const float w = (ImGui::GetContentRegionAvail().x - gap * 3) / 4.0f;
        for (std::size_t i = 0; i < std::size(kModes); ++i) {
            if (i > 0) ImGui::SameLine(0, gap);
            const bool on = rd->mode == kModes[i].mode;
            ImGui::PushStyleColor(ImGuiCol_Button, on ? tk::kAccent : tk::kSurfacePanel);
            ImGui::PushStyleColor(ImGuiCol_Text, on ? tk::kSurfaceBase : tk::kText);
            if (ImGui::Button(kModes[i].label, ImVec2(std::max(w, 1.0f), ImGui::GetFrameHeight() * 1.2f)))
                (void)set_ragdoll_mode(e, kModes[i].mode);
            ImGui::PopStyleColor(2);
            kit::tooltip(kModes[i].help);
        }
        ImGui::PushTextWrapPos(0.0f);
        for (const ModeInfo& m : kModes)
            if (m.mode == rd->mode) ImGui::TextColored(tk::kTextMuted, "%s", m.help);
        if (play_state_ == PlayState::Editing && rd->mode != RagdollMode::Off)
            ImGui::TextColored(tk::kInfo, "Press Play to see it move.");
        if (bones == 0)
            ImGui::TextColored(tk::kWarning, "No body parts yet: press Make Ragdoll again or add RagdollBone components.");
        ImGui::PopTextWrapPos();
        if (bones == 0 && kit::icon_text_button("Make Ragdoll", kit::Icon::Plus, true)) (void)make_ragdoll(e);
        if (bones == 0) ImGui::SameLine();
        if (play_state_ != PlayState::Editing && (rd->mode == RagdollMode::Passive || rd->mode == RagdollMode::Powered)) {
            if (kit::icon_text_button("Get Up", kit::Icon::Play)) (void)ragdoll_get_up(e);
            kit::tooltip("Stands the fallen character up where it lies: it turns to face where a get-up "
                         "animation starts and blends smoothly from the lying pose back to its animation.");
            ImGui::SameLine();
        }
        if (kit::icon_text_button("Remove Ragdoll", kit::Icon::Close)) (void)remove_ragdoll(e);
        kit::tooltip("Removes the ragdoll from this character and all its bones.");
        ImGui::SameLine();
        ImGui::TextDisabled("Scripts: entity:set_ragdoll(\"passive\"), entity:ragdoll_get_up()");
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::PopID();
    ImGui::Spacing();
}

} // namespace victus::editor
