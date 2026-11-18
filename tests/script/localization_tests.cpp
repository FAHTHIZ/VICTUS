// SPDX-License-Identifier: Apache-2.0
// The `loc` script API and keyed UiText through the script host (string tables on disk).
#include "victus/core/hash.hpp"
#include "victus/gameplay/gameplay.hpp"
#include "victus/gameui/gameui.hpp"
#include "victus/gameui/localization.hpp"
#include "victus/script/script.hpp"

#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace victus;
using scene::Entity;
using scene::Scene;

namespace {

struct LocDir {
    std::filesystem::path path;
    LocDir() {
        path = std::filesystem::temp_directory_path() / ("victus_loc_" + Uuid::generate().to_string().substr(0, 8));
        std::filesystem::create_directories(path / "localization");
    }
    ~LocDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    void write(const std::string& rel, const std::string& text) const {
        const auto p = path / rel;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
    }
};

std::unique_ptr<script::ScriptHost> host_for(const LocDir& d, std::string fallback = "en") {
    script::HostOptions o;
    o.assets_root = d.path;
    o.default_locale = std::move(fallback);
    auto h = script::ScriptHost::create(o);
    REQUIRE(h.ok());
    return std::move(h).value();
}

std::string eval_str(script::ScriptHost& h, const char* src, Scene* s = nullptr) {
    auto r = h.eval(src, s);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    return r->as_string_or("<not a string>");
}

} // namespace

TEST_CASE("script: loc API reads string tables, formats parameters and switches locale") {
    LocDir d;
    d.write("localization/en.vstrings", R"j({"name":"English","strings":{"menu":{"start":"Start","quit":"Quit"},"hud":{"score":"Score: {score} ({name})"}}})j");
    d.write("localization/fr.vstrings", R"j({"name":"Français","strings":{"menu":{"start":"Commencer"},"hud":{"score":"Score : {score} ({name})"}}})j");
    d.write("localization/broken.vstrings", R"({"strings":{"a":1}})"); // skipped with a warning
    d.write("localization/notes.txt", "not a table");
    auto host = host_for(d);
    CHECK(eval_str(*host, "return loc.locale()") == "en");
    CHECK(eval_str(*host, "return table.concat(loc.locales(), ',')") == "en,fr");
    CHECK(eval_str(*host, "return loc.text('menu.start')") == "Start");
    CHECK(eval_str(*host, "return loc.text('hud.score', {score = 12, name = 'Ada'})") == "Score: 12 (Ada)");
    CHECK(eval_str(*host, "return loc.text('hud.score', {score = 1.5})") == "Score: 1.5 ({name})");
    CHECK(eval_str(*host, "return loc.text('menu.missing')") == "menu.missing");
    CHECK(eval_str(*host, "return tostring(loc.has('menu.quit')) .. tostring(loc.has('nope'))") == "truefalse");
    CHECK(eval_str(*host, "return tostring(loc.set_locale('de')) .. loc.locale()") == "falseen");
    CHECK(eval_str(*host, "return tostring(loc.set_locale('fr')) .. loc.locale()") == "truefr");
    CHECK(eval_str(*host, "return loc.text('menu.start')") == "Commencer");
    CHECK(eval_str(*host, "return loc.text('menu.quit')") == "Quit"); // fallback locale
    CHECK(eval_str(*host, "return loc.name() .. '/' .. loc.name('en') .. '/' .. loc.name('xx')") == "Français/English/xx");
    CHECK_FALSE(host->eval("return loc.text('a', 5)").ok());

    // Keyed UiText in a scene the host runs follows the session locale.
    gameui::register_components();
    script::register_components();
    Scene s;
    const Entity c = s.create("Canvas");
    s.world().add<gameui::UiCanvasComponent>(c);
    const Entity label = s.create("Label", c);
    s.world().add<gameui::UiRectComponent>(label).size = {200, 40};
    auto& t = s.world().add<gameui::UiTextComponent>(label);
    t.text = "Start";
    t.key = "menu.start";
    gameplay::FrameTime ft;
    ft.dt = 1.0f / 60.0f;
    host->update(s, ft, nullptr);
    gameui::layout(s);
    std::vector<gameui::DrawItem> items;
    gameui::build_draw_list(s, items);
    REQUIRE(items.size() == 1);
    CHECK(items[0].text == "Commencer");

    // set_text replaces the key with literal text; set_text_key restores translation.
    REQUIRE(host->eval("scene.find('Label'):set_text('Literal')", &s).ok());
    gameui::build_draw_list(s, items);
    CHECK(items[0].text == "Literal");
    CHECK(s.world().get<gameui::UiTextComponent>(label)->key.empty());
    REQUIRE(host->eval("scene.find('Label'):set_text_key('menu.start')", &s).ok());
    gameui::build_draw_list(s, items);
    CHECK(items[0].text == "Commencer");

    // Hot reload keeps the chosen locale and picks up the new text.
    d.write("localization/fr.vstrings", R"({"strings":{"menu":{"start":"Jouer"}}})");
    CHECK(host->reload_localization() == 2);
    CHECK(eval_str(*host, "return loc.locale()") == "fr");
    gameui::build_draw_list(s, items);
    REQUIRE(items.size() == 1);
    CHECK(items[0].text == "Jouer");
}

TEST_CASE("script: loc starts at the first locale when the fallback has no table") {
    LocDir d;
    d.write("localization/fr.vstrings", R"({"strings":{"a":"b"}})");
    auto host = host_for(d, "en");
    CHECK(eval_str(*host, "return loc.locale()") == "fr");
    CHECK(eval_str(*host, "return loc.text('a')") == "b");
    LocDir empty;
    auto none = host_for(empty, "de");
    CHECK(eval_str(*none, "return loc.locale()") == "de");
    CHECK(eval_str(*none, "return loc.text('menu.start')") == "menu.start");
}
