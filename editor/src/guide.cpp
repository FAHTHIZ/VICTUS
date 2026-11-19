// SPDX-License-Identifier: Apache-2.0
// Studio guidance for people who have never used a game engine: the interface size setting,
// the Getting Started checklist (steps complete from real editor actions and each has a
// "Do it for me" button), built-in Help topics with search, the Undo History list and the
// Project Settings window. Preferences live in studio_preferences.json beside the recent list.
#include "victus/editor/editor.hpp"
#include "widgets.hpp"

#include "victus/core/fs.hpp"
#include "victus/core/json.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace victus::editor {

namespace tk = ui::tokens;
using namespace scene;

// ---- help topics --------------------------------------------------------------------------------

namespace {

constexpr HelpTopic kTopics[] = {
    {"Start here", "What is on the screen",
     "The Studio is split into panels. You can drag any panel by its tab to move it, and Window > Reset Layout puts "
     "everything back.\n\n"
     "- Viewport (middle): your scene. Click an object to select it.\n"
     "- Hierarchy (left): every object in the scene as a list. Objects can sit inside other objects.\n"
     "- Inspector (right): the settings of the selected object, such as its position, colour or physics.\n"
     "- Assets (bottom): the files of your project: 3D models, images, sounds, scripts and scenes.\n"
     "- Console (bottom): messages from the engine and your game. Errors are red.\n\n"
     "The toolbar at the top has Save, Undo and Redo on the left and Play, Pause and Stop in the middle.",
     "panels layout screen interface overview tour"},
    {"Start here", "Make your first game in five minutes",
     "1. On the start screen pick a template (No-Code Platformer is the easiest, or 3D Game or 2D Game) and press "
     "Create. The template is already playable.\n\n"
     "2. Press the green Play button (or Ctrl+P). Click inside the viewport so the game gets the keyboard, then use "
     "WASD or the arrow keys to move and Space to jump.\n\n"
     "3. Press Stop. Anything you changed while playing is undone, so experimenting is safe.\n\n"
     "4. Add something: Entity > Cube, then drag the coloured arrows in the viewport to place it.\n\n"
     "5. Press Ctrl+S to save, and File > Export Game when you want a version other people can run.",
     "tutorial beginner first quick start new"},
    {"Start here", "Make the Studio bigger or smaller",
     "View > Interface Size changes the size of all text and buttons. Pick Large or Extra Large if the text is hard "
     "to read, or Compact on a small laptop screen.\n\n"
     "Shortcuts: Ctrl+= makes it bigger, Ctrl+- smaller, and Ctrl+0 returns to the default size. Your choice is "
     "remembered the next time you open the Studio.",
     "zoom scale font text size dpi accessibility readable large"},
    {"Scenes and objects", "Add, select and arrange objects",
     "Use the Entity menu or the + Add button at the top of the Hierarchy to add cubes, spheres, lights, cameras and "
     "more. Drag a model from the Assets panel into the viewport to place it.\n\n"
     "- Click an object in the viewport or the Hierarchy to select it. Ctrl+click adds to the selection.\n"
     "- W moves, E rotates, R scales. Drag the coloured handles in the viewport.\n"
     "- Ctrl+D duplicates, Delete removes, F frames the selection in the viewport.\n"
     "- Drag one object onto another in the Hierarchy to make it a child; children move with their parent.",
     "entity create cube select move rotate scale gizmo duplicate delete parent child hierarchy"},
    {"Scenes and objects", "Move the camera around the scene",
     "In a 3D scene: hold the right mouse button and move the mouse to look around. While holding it, W A S D move "
     "and Q E go down and up; hold Shift to go faster. The mouse wheel zooms. The middle mouse button (or "
     "Shift + right button) pans.\n\n"
     "In a 2D scene: drag with the right or middle mouse button to pan and use the mouse wheel to zoom.\n\n"
     "Press F to jump to the selected object.",
     "camera navigate orbit pan zoom fly look view"},
    {"Scenes and objects", "Change an object's settings",
     "Select the object and look at the Inspector. Each block (a component) adds one ability: Mesh Renderer draws a "
     "shape, Rigid Body makes it fall and collide, Light gives off light, Script runs game logic.\n\n"
     "Click Add Component at the bottom of the Inspector and search by name, for example \"rigid\" or \"sound\". "
     "Drag a number left or right to change it, or double-click to type a value.",
     "inspector component property add component settings"},
    {"Scenes and objects", "Undo anything",
     "Ctrl+Z undoes and Ctrl+Y redoes. Window > Undo History lists every change; click any line to go back to that "
     "moment. Edits made while playing are always thrown away when you press Stop.\n\n"
     "The Studio also saves a recovery copy of unsaved work every 30 seconds. If it closes unexpectedly you are "
     "offered that copy the next time you open the scene.",
     "undo redo history mistake recover autosave crash"},
    {"Playing and testing", "Play, pause and stop",
     "Press Play (Ctrl+P) to run the game inside the Studio. Click the viewport so the game receives the keyboard; "
     "the Studio keeps its own shortcuts out of the way while the game has focus.\n\n"
     "Pause freezes the game so you can inspect objects. Stop ends the game and restores the scene exactly as it "
     "was before you pressed Play.",
     "play test run pause stop game mode keyboard focus"},
    {"Playing and testing", "Physics and ragdolls",
     "Add a Rigid Body and a Collider (Inspector > Add Component) to make an object fall, bounce and collide. Static "
     "bodies never move, dynamic bodies are pushed around, kinematic bodies follow their animation.\n\n"
     "Characters can become ragdolls: select a character (for example the Vanguard robot of the 3D template) and "
     "press Make Ragdoll in the Inspector. Its body then gets physical bones. Choose Floppy to let it fall like a "
     "rag doll, Powered to let motors hold the animated pose, or Animated to turn physics off again.\n\n"
     "View > Physics Colliders shows the collision shapes as outlines.",
     "physics rigid body collider gravity ragdoll fall bounce joint vanguard character"},
    {"Playing and testing", "Find and fix problems",
     "The Problems panel checks your scenes, scripts, materials and assets in the background and lists anything "
     "that would break the game, in plain words. Double-click a problem to jump to it.\n\n"
     "The status bar at the bottom counts errors and warnings; click the count to open the Console.",
     "problems errors warnings validate check console fix broken"},
    {"Making it look good", "Materials and colours",
     "Select an object with a Mesh Renderer and use Assets > New Material to give it its own material. The Material "
     "Editor lets you change colour, roughness (shiny or matte), metal and glow. Drop an image on a texture slot to "
     "paint the surface with it.",
     "material colour color texture shiny metal roughness emissive glow"},
    {"Making it look good", "Rendering quality",
     "View > Rendering Quality picks Low, Medium, High or Ultra. Lower tiers run faster on weak computers and "
     "phones; the choice is saved with the project and used by the exported game too.",
     "quality graphics performance fast slow fps low high ultra"},
    {"Logic without code", "Behaviours: gameplay without code",
     "Behaviours are ready-made abilities. Select an object, press Add Component and open the Behaviours group. "
     "Each one starts working the moment you press Play.\n\n"
     "Quickest start: select an object and press one of the Make it a... buttons in the Inspector (Player, Enemy, "
     "Coin, Hazard, Checkpoint, Goal, Moving Platform, Key, Door, Sign). Each adds the right Behaviours in one step, plus Game Rules and a camera "
     "that follows the player if the scene has none. Ctrl+Z undoes it.\n\n"
     "- Platformer Movement or Top-Down Movement: the player runs and jumps with the keyboard or a gamepad.\n"
     "- Camera Follow: the camera keeps the player in view.\n"
     "- Patrol and Chase: enemies walk back and forth or follow the player. Patrols turn at walls and, with Turn "
     "At Ledges, at the edge of a platform.\n"
     "- Moving Platform: lifts and moving ledges that carry whoever stands on them; Spin turns saws and fans.\n"
     "- Trigger: something happens when the player touches it: a sign shows a message, a switch starts a lift, "
     "a door opens if the player carries a Key, a finish line wins. Tick Press To Use to wait for E.\n"
     "- Jump Pad: a spring that launches the player high; tilt Direction for a sideways launch.\n"
     "- Hazard: hurts what touches it. Tick Stompable so the player can jump on it.\n"
     "- Collectible: a coin, heart or extra life that disappears when picked up.\n"
     "- Health, Checkpoint, Goal, Spawner, Shooter, Projectile and Lifetime.\n"
     "- Game Rules: score, lives, a timer, the win and lose screens, pausing with Escape, and an optional title screen (set Title).\n\n"
     "The No-Code Platformer template is a whole level made only from behaviours. Click any object in it to see "
     "how it works.",
     "behaviour behavior no code platformer movement patrol chase hazard collectible coin enemy health checkpoint "
     "goal spawner shooter game rules score lives"},
    {"Logic without code", "Visual scripts",
     "Assets > New Visual Script creates a graph and opens the Graph Editor. Right-click the canvas to add nodes, "
     "then drag from an output pin to an input pin to connect them. Start from an event node such as On Start or On "
     "Key Pressed.\n\n"
     "Select an object and press Assign to Selected in the Graph Editor so the graph runs on that object when you "
     "play.",
     "visual script graph nodes no code blueprint logic event"},
    {"Logic without code", "Keys and controller buttons",
     "Edit > Input Actions lists the game's actions, such as Jump or Move. Click Bind and press a key or controller "
     "button to add it. Scripts ask for the action (\"jump\"), so you can change the keys later without touching "
     "the scripts.",
     "input keys keyboard gamepad controller bind action jump"},
    {"Sharing your game", "Export a game other people can play",
     "File > Export Game builds a folder with your game and everything it needs. It runs on its own without the "
     "Studio and needs no account. Zip the folder and send it to friends or upload it to a store.\n\n"
     "The Problems panel lists anything that would stop the export; fix those first.",
     "export build ship package release share distribute zip"},
    {"Sharing your game", "Report a problem with the Studio",
     "Help > Report a Problem makes a .zip with recent logs and crash reports, with your folder names hidden. Attach "
     "it to a bug report so the problem can be reproduced.",
     "bug crash report diagnostics support"},
};

constexpr const char* kShortcuts[][2] = {
    {"Ctrl+S", "Save the scene"},
    {"Ctrl+Z / Ctrl+Y", "Undo / redo"},
    {"Ctrl+P", "Play or stop"},
    {"Ctrl+D", "Duplicate the selection"},
    {"Delete", "Delete the selection"},
    {"F", "Frame the selection in the viewport"},
    {"W / E / R", "Move / rotate / scale tool"},
    {"T", "Switch between world and local space"},
    {"Right mouse + WASD", "Fly the 3D camera (Q/E down and up)"},
    {"Mouse wheel", "Zoom the viewport"},
    {"Ctrl+Shift+P or F1", "Search every command"},
    {"Ctrl+= / Ctrl+- / Ctrl+0", "Bigger / smaller / default interface size"},
};

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

// Body text with "- " bullet lines and numbered paragraphs drawn as wrapped text.
void draw_body(const char* body) {
    std::string_view rest(body);
    ImGui::PushTextWrapPos(0);
    while (!rest.empty()) {
        const std::size_t nl = rest.find('\n');
        const std::string_view line = rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
        if (line.empty()) {
            ImGui::Dummy(ImVec2(0, tk::kSpace2));
        } else if (line.starts_with("- ")) {
            ImGui::Bullet();
            ImGui::TextUnformatted(line.data() + 2, line.data() + line.size());
        } else {
            ImGui::TextUnformatted(line.data(), line.data() + line.size());
        }
    }
    ImGui::PopTextWrapPos();
}

constexpr std::array<const char*, static_cast<std::size_t>(GuideStep::Count)> kStepHow = {
    "Use Entity > Cube, the + Add button in the Hierarchy, or drag a model from Assets into the viewport.",
    "Select an object, then drag one of the coloured arrows in the viewport (W move, E rotate, R scale).",
    "Change any value in the Inspector, or press Add Component and pick one, e.g. Rigid Body.",
    "Press the green Play button (Ctrl+P), click the viewport, try the controls, then press Stop.",
    "Press Ctrl+S or the disk button in the toolbar.",
    "File > Export Game builds a folder anyone can run without VICTUS.",
    "Behaviours make things act without code. Select an object, press Add Component > Behaviours and pick one, "
    "e.g. Patrol to make it walk back and forth.",
};
constexpr std::array<GuideStep, static_cast<std::size_t>(GuideStep::Count)> kStepOrder = {
    GuideStep::AddObject, GuideStep::MoveObject, GuideStep::EditSetting, GuideStep::AddBehaviour,
    GuideStep::Play,      GuideStep::Save,       GuideStep::Export,
};
constexpr std::array<const char*, static_cast<std::size_t>(GuideStep::Count)> kStepAction = {
    "Add a cube for me", "Lift it up for me", "Add physics to it", "Play now", "Save now", "Open Export",
    "Make it patrol",
};

} // namespace

std::span<const HelpTopic> help_topics() noexcept { return kTopics; }
std::span<const GuideStep> guide_step_order() noexcept { return kStepOrder; }

std::vector<const HelpTopic*> find_help_topics(std::string_view query) {
    std::vector<std::string> words;
    {
        const std::string q = lower(query);
        std::size_t i = 0;
        while (i < q.size()) {
            while (i < q.size() && q[i] == ' ') ++i;
            std::size_t j = i;
            while (j < q.size() && q[j] != ' ') ++j;
            if (j > i) words.push_back(q.substr(i, j - i));
            i = j;
        }
    }
    std::vector<std::pair<int, const HelpTopic*>> hits;
    for (const HelpTopic& t : kTopics) {
        const std::string title = lower(t.title);
        const std::string all = title + ' ' + lower(t.category) + ' ' + lower(t.body) + ' ' + lower(t.keywords);
        bool ok = true;
        int score = 0;
        for (const std::string& w : words) {
            if (all.find(w) == std::string::npos) {
                ok = false;
                break;
            }
            if (title.find(w) != std::string::npos) score += 2;
            if (std::string_view(t.keywords).find(w) != std::string_view::npos) score += 1;
        }
        if (ok) hits.emplace_back(score, &t);
    }
    std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<const HelpTopic*> out;
    out.reserve(hits.size());
    for (const auto& h : hits)
        out.push_back(h.second);
    return out;
}

const char* guide_step_title(GuideStep step) noexcept {
    switch (step) {
    case GuideStep::AddObject:
        return "Add an object to the scene";
    case GuideStep::MoveObject:
        return "Move, rotate or scale it";
    case GuideStep::EditSetting:
        return "Change a setting in the Inspector";
    case GuideStep::Play:
        return "Play your game";
    case GuideStep::Save:
        return "Save the scene";
    case GuideStep::Export:
        return "Export a game anyone can run";
    case GuideStep::AddBehaviour:
        return "Give it a behaviour";
    case GuideStep::Count:
        break;
    }
    return "";
}

// ---- preferences and interface size -------------------------------------------------------------

fs::Path Editor::preferences_file() const {
    if (!options_.preferences_file.empty()) return options_.preferences_file;
    return options_.recent_file.parent_path() / "studio_preferences.json";
}

void Editor::load_preferences() {
    ui_scale_ = std::clamp(options_.default_ui_scale, kMinUiScale, kMaxUiScale);
    auto text = fs::read_text(preferences_file(), 4u << 20);
    if (!text) return;
    auto doc = json::parse(*text);
    if (!doc || !doc->is_object()) return;
    ui_scale_ = std::clamp((*doc)["ui_scale"].as_float(ui_scale_), kMinUiScale, kMaxUiScale);
    const json::Value& guide = (*doc)["getting_started"];
    guide_done_ = static_cast<std::uint32_t>(guide["done"].as_int(0)) & ((1u << static_cast<unsigned>(GuideStep::Count)) - 1u);
    guide_dismissed_ = guide["dismissed"].as_bool(false);
    const json::Value& ws = (*doc)["workspace"];
    if (ws.is_object()) {
        const std::string layout = ws["layout"].as_string_or("");
        for (int i = 0; i < static_cast<int>(Layout::Count); ++i)
            if (layout == layout_name(static_cast<Layout>(i))) layout_ = static_cast<Layout>(i);
        const json::Value& open = ws["panels"];
        if (open.is_array()) {
            std::uint32_t mask = 0;
            for (std::size_t k = 0; k < open.size(); ++k)
                for (int i = 0; i < static_cast<int>(Panel::Count); ++i)
                    if (open[k].as_string_or("") == panel_title(static_cast<Panel>(i))) mask |= 1u << i;
            set_open_panels(mask);
            saved_panels_ = mask;
            panels_restored_ = true;
        }
        custom_layouts_.clear();
        const json::Value& mine = ws["custom_layouts"];
        for (std::size_t k = 0; mine.is_array() && k < mine.size(); ++k) {
            const json::Value& c = mine[k];
            CustomLayout l;
            l.name = c["name"].as_string_or("");
            l.ini = c["ini"].as_string_or("");
            const json::Value& ps = c["panels"];
            for (std::size_t j = 0; ps.is_array() && j < ps.size(); ++j)
                for (int i = 0; i < static_cast<int>(Panel::Count); ++i)
                    if (ps[j].as_string_or("") == panel_title(static_cast<Panel>(i))) l.panels |= 1u << i;
            if (!l.name.empty()) custom_layouts_.push_back(std::move(l));
        }
    }
}

void Editor::save_preferences() const {
    json::Value doc = json::Value::object();
    doc.set("ui_scale", static_cast<double>(ui_scale_));
    json::Value guide = json::Value::object();
    guide.set("done", static_cast<double>(guide_done_));
    guide.set("dismissed", guide_dismissed_);
    doc.set("getting_started", std::move(guide));
    json::Value ws = json::Value::object();
    ws.set("layout", layout_name(layout_));
    json::Value open = json::Value::array();
    for (int i = 0; i < static_cast<int>(Panel::Count); ++i)
        if ((saved_panels_ >> i) & 1u) open.push(json::Value(panel_title(static_cast<Panel>(i))));
    if (panels_restored_ || saved_panels_ != 0) ws.set("panels", std::move(open));
    if (!custom_layouts_.empty()) {
        json::Value mine = json::Value::array();
        for (const CustomLayout& c : custom_layouts_) {
            json::Value l = json::Value::object();
            l.set("name", c.name);
            json::Value ps = json::Value::array();
            for (int i = 0; i < static_cast<int>(Panel::Count); ++i)
                if ((c.panels >> i) & 1u) ps.push(json::Value(panel_title(static_cast<Panel>(i))));
            l.set("panels", std::move(ps));
            l.set("ini", c.ini);
            mine.push(std::move(l));
        }
        ws.set("custom_layouts", std::move(mine));
    }
    doc.set("workspace", std::move(ws));
    if (auto st = fs::write_text_atomic(preferences_file(), json::stringify(doc) + "\n"); !st.ok())
        VICTUS_LOG_WARN("editor", "could not save Studio preferences: {}", st.error().message);
}

void Editor::set_ui_scale(float scale) {
    const float s = std::clamp(scale, kMinUiScale, kMaxUiScale);
    if (std::abs(s - ui_scale_) < 1e-4f) return;
    ui_scale_ = s;
    save_preferences();
    notify(std::format("Interface size {:.0f} %", static_cast<double>(s) * 100.0));
}

void Editor::step_ui_scale(int direction) {
    if (direction == 0) {
        set_ui_scale(options_.default_ui_scale);
        return;
    }
    const std::size_t n = std::size(kUiScales);
    if (direction > 0) {
        for (std::size_t i = 0; i < n; ++i)
            if (kUiScales[i].scale > ui_scale_ + 1e-3f) return set_ui_scale(kUiScales[i].scale);
    } else {
        for (std::size_t i = n; i-- > 0;)
            if (kUiScales[i].scale < ui_scale_ - 1e-3f) return set_ui_scale(kUiScales[i].scale);
    }
}

// Rebuilds the style from the theme captured at start-up (already DPI scaled) so repeated size
// changes never compound rounding. Runs at the end of a frame; the next frame uses it.
void Editor::apply_ui_scale() {
    if (std::abs(applied_ui_scale_ - ui_scale_) < 1e-4f) return;
    ImGuiStyle& style = ImGui::GetStyle();
    const float dpi = style.FontScaleDpi;
    style = base_style_;
    style.ScaleAllSizes(ui_scale_);
    style.FontScaleMain = ui_scale_;
    style.FontScaleDpi = dpi;
    applied_ui_scale_ = ui_scale_;
}

// ---- Getting Started -----------------------------------------------------------------------------

std::size_t Editor::guide_done_count() const noexcept {
    std::size_t n = 0;
    for (int i = 0; i < static_cast<int>(GuideStep::Count); ++i)
        n += guide_done(static_cast<GuideStep>(i)) ? 1 : 0;
    return n;
}

void Editor::complete_guide_step(GuideStep step) {
    if (step == GuideStep::Count || guide_done(step)) return;
    guide_done_ |= 1u << static_cast<unsigned>(step);
    save_preferences();
    if (guide_done_count() == static_cast<std::size_t>(GuideStep::Count))
        notify("Getting Started complete - you made, played, saved and exported a game");
}

void Editor::observe_guide() {
    if (!project_) return;
    if (play_state_ != PlayState::Editing) complete_guide_step(GuideStep::Play);
    const std::uint64_t rev = undo_.revision();
    if (rev == guide_seen_revision_) return;
    guide_seen_revision_ = rev;
    const SceneCommand* c = undo_.next_undo();
    if (!c) return;
    const std::string_view label = c->label;
    const bool creates = std::any_of(c->before.begin(), c->before.end(), [](const EntityState& s) { return !s.snapshot; });
    if (label == "Move" || label == "Rotate" || label == "Scale") complete_guide_step(GuideStep::MoveObject);
    else if (creates || label.starts_with("Create ") || label == "Duplicate") complete_guide_step(GuideStep::AddObject);
    else if (label == "Edit" || label == "Rename" || label.starts_with("Add ") || label.starts_with("Assign"))
        complete_guide_step(GuideStep::EditSetting);
}

void Editor::reset_guide() {
    guide_done_ = 0;
    guide_dismissed_ = false;
    save_preferences();
    show_panel(Panel::GettingStarted);
}

void Editor::dismiss_guide() {
    guide_dismissed_ = true;
    show_getting_started_ = false;
    save_preferences();
}

void Editor::run_guide_step(GuideStep step) {
    if (!project_) return;
    const bool editing = play_state_ == PlayState::Editing;
    // 2D scenes get a sprite, 3D scenes a cube: something the user can see in the current view.
    const Primitive starter =
        runtime::scene_view_mode(*scene_) == runtime::ViewMode::TwoD ? Primitive::Sprite : Primitive::Cube;
    switch (step) {
    case GuideStep::AddObject: {
        if (!editing) stop();
        const Entity e = create_entity(starter);
        if (!e.is_null()) {
            select(e);
            frame_selected();
        }
        break;
    }
    case GuideStep::MoveObject:
    case GuideStep::EditSetting: {
        if (!editing) stop();
        Entity e = selected();
        if (e.is_null() || !scene_->world().get<Transform>(e)) {
            e = create_entity(starter);
            if (e.is_null()) return;
            select(e);
        }
        if (step == GuideStep::MoveObject) {
            gizmo_.mode = gizmo::Mode::Translate;
            auto before = capture(*scene_, {scene_->uuid(e)});
            scene_->world().get<Transform>(e)->position.y += 1.0f;
            record_edit("Move", std::move(before));
            frame_selected();
            notify("Moved it up 1 m. Now drag the arrows in the viewport yourself");
        } else {
            const bool two_d = runtime::scene_view_mode(*scene_) == runtime::ViewMode::TwoD;
            const char* id = two_d ? "RigidBody2D" : "RigidBody";
            if (!add_component(e, id)) add_component(e, two_d ? "Collider2D" : "Collider");
            show_panel(Panel::Inspector);
            notify("Added physics in the Inspector. Press Play and watch it fall");
        }
        break;
    }
    case GuideStep::Play:
        if (play_state_ != PlayState::Playing) play();
        break;
    case GuideStep::Save:
        if (!editing) stop();
        if (scene_path_.empty()) show_save_as_ = true;
        else if (auto st = save_scene(); !st.ok()) notify(st.error().message, true);
        break;
    case GuideStep::Export:
        if (!editing) stop();
        open_export_dialog();
        break;
    case GuideStep::AddBehaviour: {
        if (!editing) stop();
        Entity e = selected();
        if (e.is_null() || !scene_->world().get<Transform>(e)) {
            e = create_entity(starter);
            if (e.is_null()) return;
            select(e);
        }
        if (add_component(e, "Patrol")) {
            show_panel(Panel::Inspector);
            notify("Added Patrol. Press Play and it walks back and forth; change Offset and Speed in the Inspector");
        } else {
            notify("It already has Patrol. Try another one from Add Component > Behaviours");
            complete_guide_step(GuideStep::AddBehaviour);
        }
        break;
    }
    case GuideStep::Count:
        break;
    }
}

void Editor::draw_getting_started() {
    if (!show_getting_started_) return;
    ImGui::SetNextWindowSize(ImVec2(380, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Getting Started", &show_getting_started_)) {
        ImGui::End();
        return;
    }
    if (lesson_ != Lesson::None) {
        draw_lesson();
        ImGui::End();
        return;
    }
    const std::size_t total = static_cast<std::size_t>(GuideStep::Count);
    const std::size_t done = guide_done_count();
    kit::heading("Your first game", tk::kTypeTitle);
    kit::subtitle("Seven small steps. Each one ticks itself off when you do it, or press the button to let the "
                  "Studio do it for you.");
    ImGui::Dummy(ImVec2(0, tk::kSpace2));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, done == total ? tk::kSuccess : tk::kAccent);
    ImGui::ProgressBar(static_cast<float>(done) / static_cast<float>(total), ImVec2(-1, 0),
                       std::format("{} of {} done", done, total).c_str());
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, tk::kSpace3));
    draw_lesson_picker();

    bool next_marked = false;
    for (std::size_t i = 0; i < total; ++i) {
        const GuideStep step = kStepOrder[i];
        const auto idx = static_cast<std::size_t>(step);
        const bool ok = guide_done(step);
        const bool next = !ok && !next_marked;
        if (next) next_marked = true;
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float line = ImGui::GetTextLineHeight();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0, tk::kSpace2));
        ImGui::Indent(tk::kSpace3);
        // Number or tick in a circle.
        const ImVec2 c(ImGui::GetCursorScreenPos().x + line * 0.6f, ImGui::GetCursorScreenPos().y + line * 0.5f);
        if (ok) {
            dl->AddCircleFilled(c, line * 0.6f, ImGui::GetColorU32(tk::kSuccess));
            kit::draw_icon(dl, kit::Icon::Check, ImVec2(c.x - line * 0.4f, c.y - line * 0.4f), line * 0.8f,
                           ImGui::GetColorU32(tk::kSurfaceBase));
        } else {
            dl->AddCircle(c, line * 0.6f, ImGui::GetColorU32(next ? tk::kAccent : tk::kTextDisabled), 0, 1.5f);
            const std::string n = std::to_string(i + 1);
            const ImVec2 ts = ImGui::CalcTextSize(n.c_str());
            dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                        ImGui::GetColorU32(next ? tk::kAccent : tk::kTextMuted), n.c_str());
        }
        ImGui::Dummy(ImVec2(line * 1.2f, line));
        ImGui::SameLine(0, tk::kSpace3);
        ImGui::BeginGroup();
        ImGui::PushFont(kit::bold_font(), 0.0f);
        ImGui::TextColored(ok ? tk::kTextMuted : tk::kText, "%s", guide_step_title(step));
        ImGui::PopFont();
        if (!ok) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - line * 2.4f - tk::kSpace5);
            const bool two_d = scene_ && runtime::scene_view_mode(*scene_) == runtime::ViewMode::TwoD;
            const char* how = kStepHow[idx];
            const char* action = kStepAction[idx];
            if (two_d && step == GuideStep::AddObject) {
                how = "Use Entity > Sprite, the + Add button in the Hierarchy, or drag an image from Assets into the viewport.";
                action = "Add a sprite for me";
            }
            ImGui::TextColored(tk::kTextMuted, "%s", how);
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, tk::kSpace1));
            if (kit::icon_text_button(action, kit::Icon::Rocket, next, project_.has_value())) run_guide_step(step);
        } else {
            ImGui::TextColored(tk::kSuccess, "Done");
        }
        ImGui::EndGroup();
        ImGui::Unindent(tk::kSpace3);
        ImGui::Dummy(ImVec2(0, tk::kSpace2));
        ImGui::EndGroup();
        const ImVec2 q(p.x + w, ImGui::GetItemRectMax().y);
        dl->AddRect(p, q, ImGui::GetColorU32(next ? tk::kAccentSoft : tk::kBorder), tk::kRadiusLarge, next ? 1.5f : 1.0f);
        ImGui::Dummy(ImVec2(0, tk::kSpace2));
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, tk::kSpace3));
    if (kit::icon_text_button("Open Help", kit::Icon::Help)) open_help();
    ImGui::SameLine();
    if (ImGui::Button(done == total ? "Close checklist" : "Hide checklist")) dismiss_guide();
    kit::tooltip("Bring it back any time with Help > Getting Started");
    ImGui::End();
}

// ---- Help ------------------------------------------------------------------------------------------

void Editor::open_help(std::string_view topic, std::string_view query) {
    help_topic_ = std::string(topic);
    help_query_ = std::string(query);
    help_focus_ = 2;
    help_place_ = 3; // a centred floating window: readable however narrow the docked columns are
    show_panel(Panel::Help);
}

void Editor::draw_help() {
    if (!show_help_) {
        help_was_open_ = false;
        return;
    }
    if (!help_was_open_) help_place_ = 3; // opened from any menu: centre it (held while docking settles)
    help_was_open_ = true;
    if (help_place_ > 0) {
        --help_place_;
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImVec2 size(std::min(860.0f * ui_scale_, vp->WorkSize.x * 0.85f),
                          std::min(600.0f * ui_scale_, vp->WorkSize.y * 0.85f));
        ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowFocus();
    }
    if (!ImGui::Begin("Help", &show_help_)) {
        ImGui::End();
        return;
    }
    // Side by side when there is room, otherwise the topic list sits above the text.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const bool stacked = avail.x < 560.0f * ui_scale_;
    const float list_w = stacked ? 0.0f : std::max(220.0f * ui_scale_, avail.x * 0.34f);
    ImGui::BeginChild("##helplist", ImVec2(list_w, stacked ? avail.y * 0.4f : 0.0f), ImGuiChildFlags_Borders);
    if (help_focus_ > 0) {
        --help_focus_;
        if (help_focus_ == 0) ImGui::SetKeyboardFocusHere();
    }
    kit::search_input("##helpsearch", "Search help", help_query_);
    ImGui::Dummy(ImVec2(0, tk::kSpace2));
    const auto hits = find_help_topics(help_query_);
    if (hits.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(tk::kTextMuted, "No topic matches \"%s\". Try fewer words, or search the commands with "
                                           "Ctrl+Shift+P.", help_query_.c_str());
        ImGui::PopTextWrapPos();
    }
    std::string_view category;
    for (const HelpTopic* t : hits) {
        if (help_query_.empty() && category != t->category) {
            category = t->category;
            ImGui::Dummy(ImVec2(0, tk::kSpace1));
            ImGui::TextColored(tk::kTextDisabled, "%s", t->category);
        }
        if (ImGui::Selectable(t->title, help_topic_ == t->title)) help_topic_ = t->title;
    }
    ImGui::Dummy(ImVec2(0, tk::kSpace2));
    ImGui::TextColored(tk::kTextDisabled, "Reference");
    if (ImGui::Selectable("Keyboard shortcuts", help_topic_ == "Keyboard shortcuts")) help_topic_ = "Keyboard shortcuts";
    ImGui::EndChild();
    if (!stacked) ImGui::SameLine();
    ImGui::BeginChild("##helpbody", ImVec2(0, 0), ImGuiChildFlags_None);
    const HelpTopic* shown = nullptr;
    for (const HelpTopic& t : kTopics)
        if (help_topic_ == t.title) shown = &t;
    if (!shown && help_topic_ != "Keyboard shortcuts") shown = hits.empty() ? &kTopics[0] : hits.front();
    if (shown) {
        ImGui::TextColored(tk::kTextMuted, "%s", shown->category);
        kit::heading(shown->title, tk::kTypeTitle);
        ImGui::Dummy(ImVec2(0, tk::kSpace3));
        draw_body(shown->body);
    } else {
        kit::heading("Keyboard shortcuts", tk::kTypeTitle);
        ImGui::Dummy(ImVec2(0, tk::kSpace3));
        if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 13.0f);
            ImGui::TableSetupColumn("Action");
            for (const auto& k : kShortcuts) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(tk::kAccent, "%s", k[0]);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(k[1]);
            }
            ImGui::EndTable();
        }
    }
    ImGui::Dummy(ImVec2(0, tk::kSpace5));
    ImGui::Separator();
    ImGui::TextColored(tk::kTextMuted, "More: docs/USER_GUIDE.md in the VICTUS folder covers every system in depth.");
    ImGui::EndChild();
    ImGui::End();
}

void Editor::draw_help_menu_items() {
    if (ImGui::MenuItem("Getting Started", nullptr, show_getting_started_)) show_panel(Panel::GettingStarted);
    kit::tooltip("A seven-step checklist from an empty scene to an exported game");
    if (ImGui::MenuItem("Help Topics", "F2")) open_help();
    if (ImGui::MenuItem("Keyboard Shortcuts")) open_help("Keyboard shortcuts");
    if (ImGui::MenuItem("Restart Getting Started", nullptr, false, guide_done_ != 0 || guide_dismissed_)) reset_guide();
}

// ---- Undo History ----------------------------------------------------------------------------------

bool Editor::undo_to(std::ptrdiff_t index) {
    if (play_state_ != PlayState::Editing) return false;
    const auto size = static_cast<std::ptrdiff_t>(undo_.size());
    if (index < -1 || index >= size) return false;
    const auto target = static_cast<std::size_t>(index + 1);
    while (undo_.position() > target && undo_.can_undo()) undo();
    while (undo_.position() < target && undo_.can_redo()) redo();
    return undo_.position() == target;
}

void Editor::draw_history() {
    if (!show_history_) return;
    ImGui::SetNextWindowSize(ImVec2(320, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Undo History", &show_history_)) {
        ImGui::End();
        return;
    }
    const bool editing = play_state_ == PlayState::Editing;
    if (!editing) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(tk::kWarning, "Playing: changes made now are thrown away on Stop, so they are not listed.");
        ImGui::PopTextWrapPos();
    }
    if (undo_.size() == 0) {
        kit::empty_state(kit::Icon::Undo, "No changes yet", "Every change you make to the scene shows up here. Click a "
                                                             "line to go back to that moment.");
        ImGui::End();
        return;
    }
    ImGui::TextColored(tk::kTextMuted, "Click a line to go back to that moment.");
    ImGui::BeginChild("##history", ImVec2(0, 0), ImGuiChildFlags_Borders);
    const std::size_t pos = undo_.position();
    if (ImGui::Selectable("Opened scene", pos == 0, editing ? 0 : ImGuiSelectableFlags_Disabled)) (void)undo_to(-1);
    const auto& cmds = undo_.commands();
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const bool undone = i >= pos;
        if (undone) ImGui::PushStyleColor(ImGuiCol_Text, tk::kTextDisabled);
        if (ImGui::Selectable(cmds[i].label.c_str(), pos == i + 1, editing ? 0 : ImGuiSelectableFlags_Disabled))
            (void)undo_to(static_cast<std::ptrdiff_t>(i));
        if (undone) {
            ImGui::PopStyleColor();
            kit::tooltip("Undone - click to redo up to here");
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---- Project Settings ------------------------------------------------------------------------------

void Editor::draw_project_settings() {
    if (!show_project_settings_ || !project_) return;
    ImGui::SetNextWindowSize(ImVec2(460, 440), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Project Settings", &show_project_settings_)) {
        ImGui::End();
        return;
    }
    project::ProjectSettings& s = project_->settings();
    bool changed = false;
    const float label_w = ImGui::GetFontSize() * 9.0f;
    const auto row = [&](const char* label, const char* hint) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (hint) kit::tooltip(hint);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1);
    };
    kit::heading("Game");
    row("Name", "Shown in the window title and used for the exported game");
    ImGui::InputText("##name", &s.name);
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    row("First scene", "The scene the game starts in");
    if (ImGui::BeginCombo("##startup", s.startup_scene.c_str())) {
        for (const std::string& sc : project_->scenes())
            if (ImGui::Selectable(sc.c_str(), sc == s.startup_scene)) {
                s.startup_scene = sc;
                changed = true;
            }
        ImGui::EndCombo();
    }
    ImGui::Dummy(ImVec2(0, tk::kSpace3));
    kit::heading("Window");
    row("Width", "Window width of the game in pixels");
    ImGui::InputInt("##w", &s.window_width, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    row("Height", "Window height of the game in pixels");
    ImGui::InputInt("##h", &s.window_height, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    s.window_width = std::clamp(s.window_width, 320, 7680);
    s.window_height = std::clamp(s.window_height, 240, 4320);
    row("Vertical sync", "Limits the frame rate to the screen's refresh rate (no tearing)");
    changed |= ImGui::Checkbox("##vsync", &s.vsync);
    ImGui::Dummy(ImVec2(0, tk::kSpace3));
    kit::heading("Performance");
    row("Quality", "Low runs on weak computers and phones, Ultra looks best");
    static constexpr render::QualityTier kTiers[] = {render::QualityTier::Low, render::QualityTier::Medium,
                                                     render::QualityTier::High, render::QualityTier::Ultra};
    const std::string current(render::quality_tier_name(quality_));
    if (ImGui::BeginCombo("##quality", current.c_str())) {
        for (auto t : kTiers) {
            const std::string n(render::quality_tier_name(t));
            if (ImGui::Selectable(n.c_str(), t == quality_))
                if (auto st = set_quality(t); !st.ok()) notify("could not save quality: " + st.error().message, true);
        }
        ImGui::EndCombo();
    }
    row("Target frame rate", "0 = off. Otherwise the game lowers its resolution to hold this frame rate");
    ImGui::InputInt("##fps", &s.target_fps, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    s.target_fps = std::clamp(s.target_fps, 0, 360);
    row("Phone frame rate", "Frame-rate target on phones and tablets");
    ImGui::InputInt("##mfps", &s.mobile_target_fps, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    s.mobile_target_fps = std::clamp(s.mobile_target_fps, 15, 240);
    ImGui::Dummy(ImVec2(0, tk::kSpace3));
    kit::heading("Controls");
    if (kit::icon_text_button("Edit keys and buttons...", kit::Icon::Gear)) show_panel(Panel::InputActions);
    ImGui::Dummy(ImVec2(0, tk::kSpace3));
    ImGui::TextColored(tk::kTextMuted, "Changes are saved to victus.project right away.");
    if (changed) {
        if (s.name.empty()) s.name = "Untitled";
        if (auto st = project_->save(); !st.ok()) notify("could not save project settings: " + st.error().message, true);
        else notify("Project settings saved");
        update_title();
    }
    ImGui::End();
}

} // namespace victus::editor
