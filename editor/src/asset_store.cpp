// SPDX-License-Identifier: Apache-2.0
// Studio Asset Store window (ADR-0174). See victus/editor/asset_store.hpp.
#include "victus/editor/asset_store.hpp"

#include "victus/core/json.hpp"
#include "victus/core/log.hpp"
#include "victus/editor/editor.hpp"
#include "victus/platform/platform.hpp"
#include "victus/render/gpu_context.hpp"
#include "victus/render/image.hpp"
#include "victus/ui/style.hpp"
#include "victus/ui/ui.hpp"
#include "widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <format>

namespace victus::editor {

namespace sfs = std::filesystem;
namespace tok = ui::tokens;
using library::AssetType;
using library::LibraryAsset;

namespace {

constexpr std::size_t kMaxThumbnails = 256; // GPU previews kept; least recently drawn go first

kit::Icon icon_for(AssetType t) noexcept {
    switch (t) {
    case AssetType::Model:
        return kit::Icon::Model;
    case AssetType::Material:
        return kit::Icon::Material;
    case AssetType::Hdri:
        return kit::Icon::Globe;
    case AssetType::Texture:
        return kit::Icon::Texture;
    case AssetType::Audio:
        return kit::Icon::Audio;
    case AssetType::Pack:
        return kit::Icon::Folder;
    case AssetType::Count:
        break;
    }
    return kit::Icon::File;
}

const char* type_label(AssetType t) noexcept {
    switch (t) {
    case AssetType::Model:
        return "3D Models";
    case AssetType::Material:
        return "Materials";
    case AssetType::Hdri:
        return "Skies (HDRI)";
    case AssetType::Texture:
        return "Textures";
    case AssetType::Audio:
        return "Sounds";
    case AssetType::Pack:
        return "Kits and packs";
    case AssetType::Count:
        break;
    }
    return "";
}

// Short licence summary for tiles and the source list.
std::string licence_short(const library::Licence& l) {
    if (l.id == "CC0-1.0") return "CC0";
    if (l.id.starts_with("CC-BY")) return "CC BY";
    if (l.id.find("Government") != std::string::npos || l.id.find("Public") != std::string::npos) return "Public domain";
    return l.id;
}

ui::Tone licence_tone(const library::Licence& l) {
    if (!l.installable()) return ui::Tone::Danger;
    return l.attribution_required ? ui::Tone::Warning : ui::Tone::Success;
}

const char* provider_licences(std::string_view id) noexcept {
    if (id == "sketchfab") return "CC0 / CC BY";
    if (id == "nps") return "Public domain";
    return "CC0";
}

fs::Path default_user_path(const char* leaf) {
    const fs::Path base = fs::user_data_dir();
    return base.empty() ? fs::Path{} : base / leaf;
}

// Draws `tex` letterboxed into [min, min + size).
void draw_fitted(ImDrawList* dl, const AssetStore::Thumb& t, ImVec2 min, ImVec2 size) {
    const float aspect = t.height ? static_cast<float>(t.width) / static_cast<float>(t.height) : 1.0f;
    float w = size.x, h = size.x / aspect;
    if (h > size.y) h = size.y, w = size.y * aspect;
    const ImVec2 a(min.x + (size.x - w) * 0.5f, min.y + (size.y - h) * 0.5f);
    dl->AddImageRounded(ImTextureRef(ui::UiContext::texture_id(t.texture)), a, ImVec2(a.x + w, a.y + h), ImVec2(0, 0),
                        ImVec2(1, 1), IM_COL32_WHITE, tok::kRadius);
}

} // namespace

// ---- worker pool ------------------------------------------------------------------------------

AssetStore::Pool::Pool(unsigned n) {
    for (unsigned i = 0; i < std::max(1u, n); ++i)
        threads.emplace_back([this] {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock lock(mutex);
                    cv.wait(lock, [&] { return stop || !tasks.empty(); });
                    if (stop && tasks.empty()) return;
                    task = std::move(tasks.front());
                    tasks.pop_front();
                    ++running;
                }
                task();
                std::lock_guard lock(mutex);
                --running;
            }
        });
}

AssetStore::Pool::~Pool() {
    {
        std::lock_guard lock(mutex);
        stop = true;
    }
    cv.notify_all();
    for (auto& t : threads)
        t.join();
}

void AssetStore::Pool::push(std::function<void()> task) {
    {
        std::lock_guard lock(mutex);
        tasks.push_back(std::move(task));
    }
    cv.notify_one();
}

bool AssetStore::Pool::idle() const {
    std::lock_guard lock(mutex);
    return tasks.empty() && running == 0;
}

// ---- construction -----------------------------------------------------------------------------

AssetStore::AssetStore(Options o) : providers_(std::move(o.providers)) {
    cache_dir_ = o.cache_dir.empty() ? default_user_path("store_cache") : o.cache_dir;
    settings_path_ = o.settings_path.empty() ? default_user_path("asset_store.json") : o.settings_path;
    if (o.http) {
        owned_http_ = std::move(o.http);
        http_ = owned_http_.get();
    } else {
        owned_http_ = library::make_curl_client();
        cache_http_ = std::make_unique<library::CachingHttpClient>(*owned_http_, cache_dir_ / "http", std::chrono::hours(12));
        http_ = cache_http_.get();
    }
    if (providers_.empty()) providers_ = library::make_all_providers();
    for (const auto& p : providers_)
        infos_.push_back(p->info());
    load_settings();
    for (std::size_t i = 0; i < providers_.size(); ++i)
        if (const auto it = api_keys_.find(infos_[i].id); it != api_keys_.end()) providers_[i]->set_api_key(it->second);
    work_ = std::make_unique<Pool>(1);
    images_ = std::make_unique<Pool>(o.thumbnail_threads);
}

AssetStore::~AssetStore() {
    cancel_ = true;
    images_.reset();
    work_.reset();
    release_thumbnails(gpu_);
}

void AssetStore::release_thumbnails(render::GpuContext* gpu) {
    if (gpu)
        for (auto& [url, e] : thumbs_)
            if (e.thumb.texture) gpu->device().destroy(e.thumb.texture);
    thumbs_.clear();
}

void AssetStore::load_settings() {
    if (settings_path_.empty()) return;
    auto text = fs::read_text(settings_path_);
    if (!text) return;
    auto v = json::parse(text.value());
    if (!v || !v.value()["api_keys"].is_object()) return;
    for (const auto& [k, val] : v.value()["api_keys"].as_object())
        if (val.is_string()) api_keys_[k] = val.as_string();
}

void AssetStore::save_settings() const {
    if (settings_path_.empty()) return;
    json::Value keys = json::Value::object();
    for (const auto& [k, val] : api_keys_)
        if (!val.empty()) keys.set(k, val);
    json::Value v = json::Value::object();
    v.set("victus_asset_store", 1);
    v.set("api_keys", std::move(keys));
    std::error_code ec;
    sfs::create_directories(settings_path_.parent_path(), ec);
    if (auto st = fs::write_text_atomic(settings_path_, json::stringify(v, true)); !st) {
        VICTUS_LOG_WARN("editor", "asset store: cannot save settings: {}", st.error().message);
        return;
    }
    // The file holds account tokens: readable by the user only.
    sfs::permissions(settings_path_, sfs::perms::owner_read | sfs::perms::owner_write, sfs::perm_options::replace, ec);
}

const std::string& AssetStore::api_key(std::string_view provider_id) const {
    static const std::string kEmpty;
    const auto it = api_keys_.find(provider_id);
    return it == api_keys_.end() ? kEmpty : it->second;
}

void AssetStore::set_api_key(std::string_view provider_id, std::string key) {
    while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back())))
        key.pop_back();
    while (!key.empty() && std::isspace(static_cast<unsigned char>(key.front())))
        key.erase(key.begin());
    api_keys_[std::string(provider_id)] = key;
    if (const auto i = find_provider(provider_id)) {
        // Searches run on the worker; hand the key over there so the provider is never shared.
        library::Provider* p = providers_[*i].get();
        work_->push([p, key] { p->set_api_key(key); });
    }
    save_settings();
}

std::optional<std::size_t> AssetStore::find_provider(std::string_view id) const {
    for (std::size_t i = 0; i < infos_.size(); ++i)
        if (infos_[i].id == id) return i;
    return std::nullopt;
}

void AssetStore::complete(Completion c) {
    std::lock_guard lock(completions_mutex_);
    completions_.push_back(std::move(c));
}

// ---- searching --------------------------------------------------------------------------------

void AssetStore::select_provider(std::size_t index) {
    if (index >= providers_.size()) return;
    show_installed_ = false;
    if (index == provider_ && searched_once_) return;
    provider_ = index;
    // Keep the filter only if the new source offers that type.
    if (type_filter >= 0) {
        const auto& types = infos_[index].types;
        if (std::find(types.begin(), types.end(), static_cast<AssetType>(type_filter)) == types.end()) type_filter = -1;
    }
    search();
}

void AssetStore::search() { start_search(false); }

void AssetStore::load_more() {
    if (has_more_ && !search_pending_) start_search(true);
}

void AssetStore::start_search(bool append) {
    searched_once_ = true;
    const std::uint64_t gen = ++search_generation_;
    if (!append) {
        results_.clear();
        selected = -1;
        page_ = 0;
        has_more_ = false;
        total_ = 0;
    } else {
        ++page_;
    }
    search_error_.clear();
    search_pending_ = true;
    library::SearchQuery q;
    q.text = query;
    q.type = type_filter;
    q.page = page_;
    q.page_size = 30;
    library::Provider* p = providers_[provider_].get();
    library::HttpClient* http = http_;
    work_->push([this, p, http, q, gen] {
        auto r = p->search(*http, q);
        complete([this, gen, r = std::move(r)](Editor&) mutable {
            if (gen != search_generation_) return; // a newer search replaced this one
            search_pending_ = false;
            if (!r) {
                search_error_ = r.error().message;
                return;
            }
            for (auto& a : r->assets)
                results_.push_back(std::move(a));
            has_more_ = r->has_more;
            total_ = r->total;
        });
    });
}

const LibraryAsset* AssetStore::selected_asset() const {
    if (selected < 0 || static_cast<std::size_t>(selected) >= results_.size()) return nullptr;
    return &results_[static_cast<std::size_t>(selected)];
}

// ---- previews ---------------------------------------------------------------------------------

const AssetStore::Thumb* AssetStore::thumbnail(const std::string& url) {
    if (url.empty()) return nullptr;
    auto [it, inserted] = thumbs_.try_emplace(url);
    ThumbEntry& e = it->second;
    e.last_used = frame_;
    if (!e.requested) {
        e.requested = true;
        ++thumbs_pending_;
        library::HttpClient* http = http_;
        images_->push([this, http, url] {
            library::HttpRequest req;
            req.url = url;
            req.max_bytes = 8ull << 20;
            req.timeout_seconds = 30;
            auto body = http->get(req);
            std::optional<render::Image> img;
            if (body) {
                const auto* data = reinterpret_cast<const std::uint8_t*>(body->data());
                if (auto d = render::decode_image({data, body->size()})) img = std::move(d.value());
            }
            complete([this, url, img = std::move(img)](Editor& ed) mutable {
                --thumbs_pending_;
                auto f = thumbs_.find(url);
                if (f == thumbs_.end()) return;
                if (!img || img->empty()) {
                    f->second.thumb.failed = true;
                    return;
                }
                render::TextureOptions o;
                o.srgb = false; // UI draws the stored bytes as-is
                o.mipmaps = true;
                o.name = "store thumbnail";
                auto tex = ed.gpu().create_texture(*img, o);
                if (!tex) {
                    f->second.thumb.failed = true;
                    return;
                }
                f->second.thumb = {tex.value(), img->width, img->height, false};
            });
        });
    }
    return e.thumb.texture || e.thumb.failed ? &e.thumb : nullptr;
}

// ---- installing -------------------------------------------------------------------------------

AssetStore::InstallState AssetStore::install_state() const {
    std::lock_guard lock(state_mutex_);
    return install_;
}

bool AssetStore::installing() const { return install_busy_.load(); }

bool AssetStore::install(Editor& editor, const LibraryAsset& asset, std::string variant, bool replace) {
    const auto fail = [&](std::string why) {
        std::lock_guard lock(state_mutex_);
        install_ = {};
        install_.phase = InstallPhase::Failed;
        install_.key = asset.key();
        install_.name = asset.name;
        install_.error = std::move(why);
        return false;
    };
    if (install_busy_) return fail("Another download is still running.");
    if (!editor.project() || !editor.assets()) return fail("Open a project first.");
    const auto pi = find_provider(asset.provider);
    if (!pi) return fail("Unknown source '" + asset.provider + "'.");
    if (!asset.licence.installable()) return fail(asset.licence.notes.empty() ? "This licence does not allow use in games." : asset.licence.notes);
    install_busy_ = true;
    cancel_ = false;
    {
        std::lock_guard lock(state_mutex_);
        install_ = {};
        install_.phase = InstallPhase::Downloading;
        install_.key = asset.key();
        install_.name = asset.name;
        install_.stage = "Preparing...";
    }
    library::Provider* p = providers_[*pi].get();
    library::HttpClient* http = http_;
    const fs::Path root = editor.project()->root();
    work_->push([this, p, http, asset, variant = std::move(variant), replace, root] {
        library::InstallOptions o;
        o.variant = variant;
        o.replace = replace;
        o.cancel = &cancel_;
        o.progress = [this](const library::InstallProgress& pr) {
            std::lock_guard lock(state_mutex_);
            install_.stage = pr.stage;
            install_.fraction = pr.fraction;
        };
        auto staged = library::stage(*p, *http, asset, root, o);
        complete([this, staged = std::move(staged)](Editor& ed) mutable {
            if (!staged || cancel_) {
                if (staged) library::discard(*staged);
                std::lock_guard lock(state_mutex_);
                const bool cancelled = cancel_ || (!staged && staged.error().code == ErrorCode::Cancelled);
                install_.phase = cancelled ? InstallPhase::Cancelled : InstallPhase::Failed;
                install_.error = cancelled ? std::string("Cancelled.") : staged.error().message;
                install_busy_ = false;
                return;
            }
            {
                std::lock_guard lock(state_mutex_);
                install_.phase = InstallPhase::Importing;
                install_.stage = "Adding to the project...";
                install_.fraction = 0.97f;
            }
            auto r = ed.commit_store_install(std::move(staged.value()));
            std::lock_guard lock(state_mutex_);
            if (!r) {
                install_.phase = InstallPhase::Failed;
                install_.error = r.error().message;
            } else {
                install_.phase = InstallPhase::Done;
                install_.fraction = 1.0f;
                install_.stage = r->already_installed ? "Already in this project" : "Added";
                install_.result = std::move(r.value());
            }
            installed_root_.clear();
            install_busy_ = false;
        });
    });
    return true;
}

bool AssetStore::import_archive(Editor& editor, const LibraryAsset& asset, const fs::Path& archive) {
    if (install_busy_ || !editor.project()) return false;
    install_busy_ = true;
    cancel_ = false;
    {
        std::lock_guard lock(state_mutex_);
        install_ = {};
        install_.phase = InstallPhase::Downloading;
        install_.key = asset.key();
        install_.name = asset.name;
        install_.stage = "Reading the archive...";
    }
    const fs::Path root = editor.project()->root();
    work_->push([this, asset, archive, root] {
        library::InstallOptions o;
        o.replace = true;
        o.cancel = &cancel_;
        auto staged = library::stage_archive(asset, archive, root, o);
        complete([this, staged = std::move(staged)](Editor& ed) mutable {
            std::lock_guard lock(state_mutex_);
            if (!staged) {
                install_.phase = InstallPhase::Failed;
                install_.error = staged.error().message;
            } else if (auto r = ed.commit_store_install(std::move(staged.value())); !r) {
                install_.phase = InstallPhase::Failed;
                install_.error = r.error().message;
            } else {
                install_.phase = InstallPhase::Done;
                install_.fraction = 1.0f;
                install_.stage = "Added";
                install_.result = std::move(r.value());
            }
            installed_root_.clear();
            install_busy_ = false;
        });
    });
    return true;
}

const std::vector<library::InstalledAsset>& AssetStore::installed(const Editor& editor) {
    const auto* proj = const_cast<Editor&>(editor).project();
    const std::string root = proj ? proj->root().string() : std::string();
    if (root != installed_root_ || root.empty()) {
        installed_root_ = root;
        installed_ = root.empty() ? std::vector<library::InstalledAsset>{} : library::installed_assets(proj->root());
    }
    return installed_;
}

const library::InstalledAsset* AssetStore::installed_copy(const Editor& editor, const LibraryAsset& asset) {
    const std::string folder = library::install_folder(asset);
    for (const auto& ia : installed(editor))
        if (ia.folder == folder) return &ia;
    return nullptr;
}

Status AssetStore::uninstall(Editor& editor, const std::string& folder) {
    if (!editor.assets()) return make_error(ErrorCode::InvalidArgument, "no project is open");
    Status st = library::uninstall(*editor.assets(), folder);
    installed_root_.clear();
    if (st) (void)editor.reimport_assets();
    return st;
}

// ---- frame ------------------------------------------------------------------------------------

void AssetStore::poll(Editor& editor) {
    gpu_ = &editor.gpu();
    ++frame_;
    std::vector<Completion> done;
    {
        std::lock_guard lock(completions_mutex_);
        done.swap(completions_);
    }
    for (auto& c : done)
        c(editor);
    // Forget the least recently drawn previews beyond the budget.
    if (thumbs_.size() > kMaxThumbnails) {
        std::vector<std::pair<std::uint64_t, std::string>> order;
        for (const auto& [url, e] : thumbs_)
            if (e.thumb.texture || e.thumb.failed) order.emplace_back(e.last_used, url);
        std::sort(order.begin(), order.end());
        for (std::size_t i = 0; i + kMaxThumbnails < thumbs_.size() && i < order.size(); ++i) {
            auto it = thumbs_.find(order[i].second);
            if (it->second.last_used + 2 >= frame_) break; // still on screen
            if (it->second.thumb.texture) editor.gpu().device().destroy(it->second.thumb.texture);
            thumbs_.erase(it);
        }
    }
}

bool AssetStore::wait_idle(Editor& editor, std::chrono::milliseconds timeout) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        poll(editor);
        bool pending = false;
        {
            std::lock_guard lock(completions_mutex_);
            pending = !completions_.empty();
        }
        if (!pending && !search_pending_ && !install_busy_ && thumbs_pending_ == 0 && work_->idle() && images_->idle()) return true;
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// ---- UI ---------------------------------------------------------------------------------------

void AssetStore::draw(Editor& editor, bool* open) {
    ImGui::SetNextWindowSize(ImVec2(1100, 680), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Asset Store", open)) {
        ImGui::End();
        return;
    }
    if (!searched_once_ && !providers_.empty()) search();
    const float sidebar_w = std::clamp(ImGui::GetContentRegionAvail().x * 0.2f, 170.0f, 210.0f) * ImGui::GetFontSize() / tok::kFontBase;
    ImGui::BeginChild("##store_sources", ImVec2(sidebar_w, 0), ImGuiChildFlags_Borders);
    draw_sidebar(editor);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##store_main", ImVec2(0, 0), ImGuiChildFlags_None);
    if (show_installed_) draw_installed(editor);
    else draw_results(editor);
    ImGui::EndChild();
    ImGui::End();
}

void AssetStore::draw_sidebar(Editor& editor) {
    kit::heading("Sources");
    ImGui::Spacing();
    for (std::size_t i = 0; i < infos_.size(); ++i) {
        const auto& info = infos_[i];
        ImGui::PushID(static_cast<int>(i));
        const bool sel = !show_installed_ && i == provider_;
        const float h = ImGui::GetTextLineHeightWithSpacing() * 2.1f;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##src", sel, ImGuiSelectableFlags_None, ImVec2(0, h))) select_provider(i);
        std::string tip = info.description;
        if (info.needs_api_key) tip += "\nDownloads need your own free account token.";
        if (info.browse_only) tip += "\nDownload on the website, then import the ZIP here.";
        kit::tooltip(tip.c_str());
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float line = ImGui::GetTextLineHeight();
        const AssetType first = info.types.empty() ? AssetType::Model : info.types.front();
        kit::draw_icon(dl, icon_for(first), ImVec2(at.x + 4, at.y + (h - line) * 0.5f), line, ImGui::GetColorU32(sel ? tok::kAccent : tok::kTextMuted));
        dl->AddText(ImVec2(at.x + line + 12, at.y + 2), ImGui::GetColorU32(tok::kText), info.name.c_str());
        std::string sub = provider_licences(info.id);
        if (info.needs_api_key) sub += " - account";
        if (info.browse_only) sub += " - via website";
        dl->AddText(ImVec2(at.x + line + 12, at.y + 2 + line), ImGui::GetColorU32(tok::kTextMuted), sub.c_str());
        ImGui::PopID();
    }
    ImGui::Separator();
    const auto& inst = installed(editor);
    const std::string label = std::format("In this project ({})", inst.size());
    if (ImGui::Selectable(label.c_str(), show_installed_)) show_installed_ = true;
    kit::tooltip("Assets added from the store, with their licences and credits");
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(tok::kTextMuted, "Only content you may use, change and ship in a game you sell is offered. Credits go to CREDITS.md.");
    ImGui::PopTextWrapPos();
}

void AssetStore::draw_account_card(const library::ProviderInfo& info) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, tok::kSurfaceRaised);
    ImGui::BeginChild("##account", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    kit::icon(kit::Icon::Lock, tok::kWarning);
    ImGui::SameLine();
    ImGui::TextUnformatted(std::format("{} downloads need your own API token", info.name).c_str());
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(tok::kTextMuted, "%s", info.api_key_help.c_str());
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(-90);
    ImGui::InputTextWithHint("##token", "Paste your token", token_buf_.data(), token_buf_.size(), ImGuiInputTextFlags_Password);
    ImGui::SameLine();
    if (ImGui::Button("Save") && token_buf_[0]) {
        set_api_key(info.id, token_buf_.data());
        token_buf_.fill(0);
        search();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void AssetStore::draw_results(Editor& editor) {
    const auto& info = infos_[provider_];
    // Search bar: words, type filter, search.
    const float type_w = 150.0f * ImGui::GetFontSize() / tok::kFontBase;
    bool go = false;
    // Searches start half a second after typing stops (or on Enter), so remote sources are not
    // asked once per keystroke.
    if (kit::search_input("##store_query", ("Search " + info.name).c_str(), query, ImGui::GetContentRegionAvail().x - type_w - 100))
        query_edited_at_ = ImGui::GetTime();
    if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter)) go = true;
    if (query_edited_at_ > 0 && ImGui::GetTime() - query_edited_at_ > 0.5) go = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(type_w);
    const char* current = type_filter < 0 ? "All types" : type_label(static_cast<AssetType>(type_filter));
    if (ImGui::BeginCombo("##store_type", current)) {
        if (ImGui::Selectable("All types", type_filter < 0)) type_filter = -1, go = true;
        for (AssetType t : info.types)
            if (ImGui::Selectable(type_label(t), type_filter == static_cast<int>(t))) type_filter = static_cast<int>(t), go = true;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (kit::icon_text_button("Search", kit::Icon::Search, true)) go = true;
    if (go) {
        query_edited_at_ = 0;
        search();
    }
    // Provider credit (Poly Haven's API terms ask for a visible credit).
    ImGui::TextColored(tok::kTextMuted, "%s", info.credit.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("-");
    ImGui::SameLine();
    if (ImGui::SmallButton("Website")) editor.open_url(info.homepage);
    if (info.needs_api_key && api_key(info.id).empty()) draw_account_card(info);
    ImGui::Separator();

    // The details pane gives up width first so the grid keeps at least two columns in small docks.
    const float scale = ImGui::GetFontSize() / tok::kFontBase;
    const float detail_w = std::clamp(ImGui::GetContentRegionAvail().x * 0.36f, 230.0f * scale, 330.0f * scale);
    ImGui::BeginChild("##store_grid", ImVec2(-detail_w - 8, 0));
    if (!search_error_.empty()) {
        if (kit::empty_state(kit::Icon::Warning, "Could not reach " + info.name, search_error_, "Try again")) search();
    } else if (results_.empty() && search_pending_) {
        kit::empty_state(kit::Icon::Search, "Searching " + info.name + "...", "Results appear in a moment.");
    } else if (results_.empty()) {
        kit::empty_state(kit::Icon::Search, "Nothing found", "Try other words or another type.");
    } else {
        const float gap = tok::kSpace3;
        const float avail = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
        const float tile = std::clamp((avail - gap) / 2.0f, 104.0f * scale, 150.0f * scale);
        const int cols = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (tile + gap)));
        const float line = ImGui::GetTextLineHeight();
        const float tile_h = tile + line * 2.6f;
        for (std::size_t i = 0; i < results_.size(); ++i) {
            const LibraryAsset& a = results_[i];
            if (i % static_cast<std::size_t>(cols) != 0) ImGui::SameLine(0, gap);
            ImGui::PushID(static_cast<int>(i));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const bool sel = selected == static_cast<int>(i);
            if (ImGui::InvisibleButton("##tile", ImVec2(tile, tile_h))) selected = static_cast<int>(i);
            const bool hovered = ImGui::IsItemHovered();
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !info.browse_only) (void)install(editor, a);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(at, ImVec2(at.x + tile, at.y + tile_h), ImGui::GetColorU32(hovered ? tok::kSurfaceHover : tok::kSurfaceRaised), tok::kRadiusLarge);
            if (sel) dl->AddRect(at, ImVec2(at.x + tile, at.y + tile_h), ImGui::GetColorU32(tok::kAccent), tok::kRadiusLarge, 2.0f);
            const ImVec2 img_min(at.x + 6, at.y + 6);
            const ImVec2 img_size(tile - 12, tile - 12);
            // Only request previews for tiles on screen.
            const Thumb* t = ImGui::IsItemVisible() ? thumbnail(a.thumbnail_url) : nullptr;
            if (t && t->texture) {
                draw_fitted(dl, *t, img_min, img_size);
            } else {
                dl->AddRectFilled(img_min, ImVec2(img_min.x + img_size.x, img_min.y + img_size.y), ImGui::GetColorU32(tok::kSurfacePanel), tok::kRadius);
                const float s = img_size.x * 0.32f;
                kit::draw_icon(dl, icon_for(a.type), ImVec2(img_min.x + (img_size.x - s) * 0.5f, img_min.y + (img_size.y - s) * 0.5f), s,
                               ImGui::GetColorU32(tok::kTextDisabled));
            }
            // Name (clipped) and licence pill.
            const ImVec2 name_at(at.x + 8, at.y + tile - 2);
            ImGui::PushClipRect(name_at, ImVec2(at.x + tile - 8, name_at.y + line + 2), true);
            dl->AddText(name_at, ImGui::GetColorU32(tok::kText), a.name.c_str());
            ImGui::PopClipRect();
            const std::string lic = licence_short(a.licence);
            dl->AddText(ImVec2(at.x + 8, name_at.y + line + 2), ImGui::GetColorU32(ui::tone(licence_tone(a.licence))), lic.c_str());
            if (installed_copy(editor, a)) {
                const float s = line;
                kit::draw_icon(dl, kit::Icon::Check, ImVec2(at.x + tile - s - 8, name_at.y + line + 2), s, ImGui::GetColorU32(tok::kSuccess));
            }
            if (hovered) kit::tooltip(a.author.empty() ? a.name.c_str() : (a.name + "\nby " + a.author).c_str());
            ImGui::PopID();
        }
        ImGui::Spacing();
        if (search_pending_) ImGui::TextDisabled("Loading more...");
        else if (has_more_ && ImGui::Button("Show more", ImVec2(-1, 0))) load_more();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##store_detail", ImVec2(detail_w, 0), ImGuiChildFlags_Borders);
    draw_detail(editor);
    ImGui::EndChild();
}

void AssetStore::draw_detail(Editor& editor) {
    const LibraryAsset* a = selected_asset();
    const auto& info = infos_[provider_];
    if (!a) {
        kit::empty_state(kit::Icon::Info, "Pick an asset", "Its preview, licence and download options appear here.");
        return;
    }
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (const Thumb* t = thumbnail(a->thumbnail_url); t && t->texture) draw_fitted(ImGui::GetWindowDrawList(), *t, at, ImVec2(w, w * 0.75f));
    else kit::draw_icon(ImGui::GetWindowDrawList(), icon_for(a->type), ImVec2(at.x + w * 0.35f, at.y + w * 0.12f), w * 0.3f, ImGui::GetColorU32(tok::kTextDisabled));
    ImGui::Dummy(ImVec2(w, w * 0.75f + 4));
    ImGui::PushTextWrapPos(0);
    kit::heading(a->name);
    std::string by = type_label(a->type);
    if (!a->author.empty()) by += " by " + a->author;
    if (!a->category.empty()) by += " - " + a->category;
    ImGui::TextColored(tok::kTextMuted, "%s", by.c_str());
    if (!a->description.empty()) ImGui::TextUnformatted(a->description.c_str());
    ImGui::Spacing();

    // Licence card.
    const auto& l = a->licence;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, tok::kSurfaceRaised);
    ImGui::BeginChild("##licence", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::PushTextWrapPos(0); // wrap state is per window, so the card needs its own
    kit::pill(licence_short(l), licence_tone(l));
    ImGui::SameLine();
    ImGui::TextUnformatted(l.name.c_str());
    const auto yes_no = [](const char* what, bool ok) {
        kit::icon(ok ? kit::Icon::Check : kit::Icon::Close, ok ? tok::kSuccess : tok::kDanger);
        ImGui::SameLine();
        ImGui::TextUnformatted(what);
    };
    yes_no("Use in commercial games", l.commercial_use);
    yes_no("Change and remix", l.modification);
    yes_no("Ship inside your game", l.redistribute_in_game);
    if (l.attribution_required) {
        kit::icon(kit::Icon::Warning, tok::kWarning);
        ImGui::SameLine();
        ImGui::TextUnformatted("Credit required - added to CREDITS.md");
    }
    if (!l.notes.empty()) ImGui::TextColored(tok::kTextMuted, "%s", l.notes.c_str());
    ImGui::PopTextWrapPos();
    if (!l.url.empty() && ImGui::SmallButton("Read the licence")) editor.open_url(l.url);
    if (!a->page_url.empty()) {
        const float need = ImGui::CalcTextSize("Open asset page").x + ImGui::GetStyle().FramePadding.x * 2;
        if (!l.url.empty() && ImGui::GetContentRegionAvail().x - ImGui::GetItemRectSize().x - ImGui::GetStyle().ItemSpacing.x >= need)
            ImGui::SameLine();
        if (ImGui::SmallButton("Open asset page")) editor.open_url(a->page_url);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // Variant + action.
    int& variant_index = variant_index_;
    if (variant_for_ != a->key()) variant_for_ = a->key(), variant_index = 0;
    if (!a->variants.empty()) {
        variant_index = std::clamp(variant_index, 0, static_cast<int>(a->variants.size()) - 1);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##variant", a->variants[static_cast<std::size_t>(variant_index)].label.c_str())) {
            for (std::size_t i = 0; i < a->variants.size(); ++i)
                if (ImGui::Selectable(a->variants[i].label.c_str(), variant_index == static_cast<int>(i))) variant_index = static_cast<int>(i);
            ImGui::EndCombo();
        }
        kit::tooltip("Resolution or file format to download. Bigger looks sharper up close but uses more memory.");
    }
    const std::string variant = a->variants.empty() ? std::string() : a->variants[static_cast<std::size_t>(variant_index)].id;
    const InstallState st = install_state();
    const bool this_one = st.key == a->key();
    const auto* have = installed_copy(editor, *a);
    if (!editor.project()) {
        ImGui::TextColored(tok::kWarning, "Open a project to add assets.");
    } else if (this_one && (st.phase == InstallPhase::Downloading || st.phase == InstallPhase::Importing)) {
        ImGui::ProgressBar(st.fraction, ImVec2(-1, 0), std::format("{:.0f}%", st.fraction * 100.0f).c_str());
        ImGui::TextColored(tok::kTextMuted, "%s", st.stage.c_str());
        if (st.phase == InstallPhase::Downloading && ImGui::Button("Cancel", ImVec2(-1, 0))) cancel_install();
    } else if (info.browse_only) {
        if (kit::icon_text_button(("Get it on " + info.name).c_str(), kit::Icon::Globe, true, true, ImVec2(-1, 0))) editor.open_url(a->page_url);
        kit::tooltip("Downloads happen on the creator's website, where you can also support them.");
        if (kit::icon_text_button("Import Downloaded ZIP...", kit::Icon::FolderOpen, false, !installing(), ImVec2(-1, 0))) {
            const LibraryAsset copy = *a;
            editor.pick_files([this, &editor, copy](std::vector<std::string> files) {
                if (!files.empty()) (void)import_archive(editor, copy, files.front());
            });
        }
    } else {
        const bool locked = info.needs_api_key && api_key(info.id).empty();
        const char* label = have ? "Download Again" : "Add to Project";
        if (kit::icon_text_button(label, have ? kit::Icon::Undo : kit::Icon::Plus, !have, !installing() && !locked && l.installable(),
                                  ImVec2(-1, 0)))
            (void)install(editor, *a, variant, have != nullptr);
        if (locked) kit::tooltip("Save your account token above first.");
    }
    if (this_one && (st.phase == InstallPhase::Failed || st.phase == InstallPhase::Cancelled))
        ImGui::TextColored(st.phase == InstallPhase::Failed ? tok::kDanger : tok::kTextMuted, "%s", st.error.c_str());
    if (have) {
        ImGui::Spacing();
        kit::icon(kit::Icon::Check, tok::kSuccess);
        ImGui::SameLine();
        ImGui::TextColored(tok::kSuccess, "In this project: assets/%s", have->folder.c_str());
        const std::string primary = this_one && st.phase == InstallPhase::Done ? st.result.primary : std::string();
        const assets::AssetRecord* rec = nullptr;
        if (editor.assets()) {
            for (const auto& f : have->files) {
                const std::string full = have->folder + "/" + f;
                const auto* r = editor.assets()->find_path(full);
                if (!r) continue;
                if (r->kind == assets::AssetKind::Model || r->kind == assets::AssetKind::Material) {
                    rec = r;
                    if (full == primary) break;
                }
            }
        }
        if (rec && rec->kind == assets::AssetKind::Model && ImGui::Button("Place in Scene", ImVec2(-1, 0))) {
            if (editor.instantiate_asset(rec->uuid) == scene::kNullEntity) editor.notify("Could not place " + a->name, true);
        }
        if (rec && rec->kind == assets::AssetKind::Material) {
            const scene::Entity e = editor.selected();
            if (ImGui::Button("Use on Selected Object", ImVec2(-1, 0)) && !editor.assign_material(e, rec->uuid))
                editor.notify("Select an object with a mesh first", true);
        }
        if (ImGui::Button("Show in Assets", ImVec2(-1, 0))) {
            editor.set_asset_folder(have->folder);
            editor.show_panel(Panel::Assets);
        }
    }
    ImGui::PopTextWrapPos();
}

void AssetStore::draw_installed(Editor& editor) {
    kit::heading("Added to this project");
    if (!editor.project()) {
        kit::empty_state(kit::Icon::FolderOpen, "No project open", "Open a project to see its store assets.");
        return;
    }
    const auto& list = installed(editor);
    if (list.empty()) {
        kit::empty_state(kit::Icon::Plus, "Nothing added yet", "Pick a source on the left and add an asset.");
        return;
    }
    ImGui::TextColored(tok::kTextMuted, "Credits for all of these are kept in CREDITS.md, which Export Game ships with your game.");
    std::string remove;
    if (ImGui::BeginTable("##installed", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Asset");
        ImGui::TableSetupColumn("Source");
        ImGui::TableSetupColumn("Licence");
        ImGui::TableSetupColumn("Folder");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableHeadersRow();
        for (const auto& ia : list) {
            ImGui::PushID(ia.folder.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(ia.asset.name.c_str());
            if (!ia.asset.author.empty()) kit::tooltip(("by " + ia.asset.author).c_str());
            ImGui::TableNextColumn();
            const auto pi = find_provider(ia.asset.provider);
            ImGui::TextUnformatted(pi ? infos_[*pi].name.c_str() : ia.asset.provider.c_str());
            ImGui::TableNextColumn();
            kit::pill(licence_short(ia.asset.licence), licence_tone(ia.asset.licence));
            ImGui::TableNextColumn();
            ImGui::TextColored(tok::kTextMuted, "%s", ia.folder.c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Show")) {
                editor.set_asset_folder(ia.folder);
                editor.show_panel(Panel::Assets);
            }
            ImGui::SameLine();
            if (!ia.asset.page_url.empty() && ImGui::SmallButton("Page")) editor.open_url(ia.asset.page_url);
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) remove = ia.folder;
            kit::tooltip("Deletes the folder from the project. Objects that use it lose their model or material.");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!remove.empty()) {
        if (auto st = uninstall(editor, remove); !st) editor.notify("Remove: " + st.error().message, true);
        else editor.notify("Removed assets/" + remove);
    }
}

// ---- Editor glue ------------------------------------------------------------------------------

AssetStore& Editor::asset_store() {
    if (!asset_store_) asset_store_ = std::make_unique<AssetStore>(AssetStore::Options{});
    return *asset_store_;
}

void Editor::set_asset_store(std::unique_ptr<AssetStore> store) { asset_store_ = std::move(store); }

Result<library::InstallResult> Editor::commit_store_install(library::StagedInstall staged) {
    if (!db_ || !project_) {
        library::discard(staged);
        return make_error(ErrorCode::InvalidArgument, "no project is open");
    }
    const std::string name = staged.asset.name;
    auto r = library::commit(std::move(staged), *db_);
    if (!r) return r;
    const auto report = db_->import_all(&workers());
    for (const auto& m : report.messages)
        notify(m, report.failed > 0);
    if (report.imported > 0) {
        cache_->clear();
        if (thumbnails_) thumbnails_->clear();
    }
    set_asset_folder(r->folder);
    if (r->already_installed) notify(name + " is already in this project (assets/" + r->folder + ")");
    else notify(std::format("Added {} to assets/{}", name, r->folder), report.failed > 0);
    return r;
}

void Editor::pick_files(std::function<void(std::vector<std::string>)> done) {
    if (!platform_) {
        done({});
        return;
    }
    platform_->show_open_files_dialog(window_, {}, std::move(done));
}

bool Editor::open_url(const std::string& url) {
    if (platform::Platform::open_url(url)) return true;
    notify("Could not open " + url, true);
    return false;
}

void Editor::draw_asset_store() {
    if (asset_store_) asset_store_->poll(*this); // finish installs even while the window is closed
    if (!show_asset_store_) return;
    asset_store().draw(*this, &show_asset_store_);
}

} // namespace victus::editor
