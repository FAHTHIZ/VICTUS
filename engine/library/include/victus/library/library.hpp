// SPDX-License-Identifier: Apache-2.0
// VICTUS Asset Store (ADR-0174): one catalogue model over several legally usable content
// providers (Poly Haven, ambientCG, Smithsonian Open Access 3D, Sketchfab, BigSoundBank, U.S.
// National Park Service sounds, Kenney). Providers translate their public APIs or pages into
// LibraryAsset records that always carry an explicit licence; the installer (install.hpp) copies
// the chosen files into a project through the existing AssetDatabase and records provenance.
//
// This module is Studio/CLI tooling. Exported games never link it, never read the user-level
// store cache and never reach the network: installed assets are ordinary project files.
#pragma once
#include "victus/core/json.hpp"
#include "victus/core/result.hpp"
#include "victus/library/http.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace victus::library {

enum class AssetType : std::uint8_t {
    Model,     // 3D model (glTF/GLB)
    Material,  // PBR texture set; installs as textures + a .vmat
    Hdri,      // HDR environment map
    Texture,   // single image / decal / atlas
    Audio,     // sound effect, ambience or music
    Pack,      // multi-asset archive (Kenney kits): models, sprites, sounds, UI
    Count
};
[[nodiscard]] const char* to_string(AssetType type) noexcept;
[[nodiscard]] AssetType asset_type_from_string(std::string_view s) noexcept; // unknown -> Model

// What a licence allows, independent of its name, so the installer can gate on rules rather
// than on string matching. Every LibraryAsset has one; providers never leave it implicit.
struct Licence {
    std::string id;            // SPDX identifier where one exists ("CC0-1.0", "CC-BY-4.0") or a provider id
    std::string name;          // human readable
    std::string url;           // licence text
    bool attribution_required{false};
    bool commercial_use{true};
    bool modification{true};
    bool share_alike{false};
    bool redistribute_in_game{true}; // may ship inside an exported game
    std::string notes;               // provider-specific caveats shown in Studio

    // True when the asset may be installed into a project that might be sold: commercial use,
    // modification and redistribution inside a game are all allowed.
    [[nodiscard]] bool installable() const noexcept { return commercial_use && modification && redistribute_in_game; }
    [[nodiscard]] json::Value to_json() const;
    [[nodiscard]] static Licence from_json(const json::Value& v);
};
[[nodiscard]] Licence licence_cc0();
[[nodiscard]] Licence licence_public_domain_us_gov(); // U.S. federal government works (17 U.S.C. 105)
[[nodiscard]] Licence licence_cc_by(std::string_view version = "4.0");
[[nodiscard]] Licence licence_cc_by_sa(std::string_view version = "4.0");
// Maps Sketchfab/Creative Commons labels and slugs ("CC Attribution", "by-nc", "cc0") to a
// Licence; unknown labels yield a non-installable licence that records the label.
[[nodiscard]] Licence licence_from_cc_label(std::string_view label);

// One downloadable variant (resolution / format) of an asset.
struct Variant {
    std::string id;        // "1k", "2K-JPG", "glb", "ogg"
    std::string label;     // "1K JPG (8 MB)"
    std::uint64_t bytes{0}; // 0 = unknown
};

struct LibraryAsset {
    std::string provider;     // provider id ("polyhaven")
    std::string id;           // provider-local id (slug, uid)
    std::string name;
    AssetType type{AssetType::Model};
    std::string category;     // provider category ("Bricks", "furniture")
    std::vector<std::string> tags;
    std::string author;
    std::string description;
    std::string page_url;      // canonical page for people (always shown with the licence)
    std::string thumbnail_url; // small preview image (jpg/png); empty when none
    Licence licence;
    std::vector<Variant> variants; // empty: resolved at install time
    bool requires_account{false};  // download needs the user's provider credentials
    json::Value extra;             // provider data needed to resolve downloads (not shown)

    [[nodiscard]] std::string key() const { return provider + ":" + id; }
    [[nodiscard]] std::string attribution() const; // credit line for CREDITS.md and the Studio
    [[nodiscard]] json::Value to_json() const;
    [[nodiscard]] static Result<LibraryAsset> from_json(const json::Value& v);
};

struct SearchQuery {
    std::string text;
    int type{-1};          // AssetType or -1 for all the provider offers
    std::uint32_t page{0}; // zero-based
    std::uint32_t page_size{24};
};

struct SearchPage {
    std::vector<LibraryAsset> assets;
    std::uint64_t total{0}; // total matches when the provider reports it, else assets.size()
    bool has_more{false};
};

// One file the installer fetches (or extracts) into the asset's project folder.
struct FileRequest {
    std::string url;
    std::string relative;   // destination under the asset folder ('/'-separated, sanitized)
    std::uint64_t bytes{0}; // expected size, 0 = unknown
    std::string md5;        // expected md5 hex when the provider publishes one
    std::vector<HttpHeader> headers;
    bool unzip{false};      // a ZIP archive: entries matching `keep` are extracted under `relative`
    std::vector<std::string> keep; // lower-case suffixes to keep from an archive (empty = all)
};

// Installer post-step for texture sets: pack AO/roughness/metallic into one ORM texture and write
// a .vmat that references every map by UUID.
struct MaterialRecipe {
    // Relative paths; empty or missing files are skipped. When occlusion, roughness and metallic
    // name the same file it is already a glTF ORM texture (R=AO, G=roughness, B=metal) and is
    // used as-is. `opacity` (greyscale) is merged into the base colour alpha with alpha masking.
    std::string base_color, normal, roughness, metallic, occlusion, emissive, height, opacity;
    bool normal_directx{false}; // green channel flipped to the glTF/OpenGL convention on install
};

struct InstallPlan {
    std::vector<FileRequest> files;
    bool make_material{false};
    MaterialRecipe material;
    std::string primary; // relative path of the file to select/instantiate after install
};

struct ProviderInfo {
    std::string id;
    std::string name;
    std::string homepage;
    std::string credit;        // shown wherever the provider's content is listed
    std::string description;
    std::vector<AssetType> types;
    bool needs_api_key{false}; // searching or downloading needs a user-supplied key/token
    std::string api_key_help;  // where the user gets one
    bool browse_only{false};   // downloads happen on the provider's site; Studio imports the file
};

// A content source. Implementations are stateless apart from configuration and safe to call
// from a worker thread.
class Provider {
public:
    virtual ~Provider() = default;
    [[nodiscard]] virtual const ProviderInfo& info() const = 0;
    [[nodiscard]] virtual Result<SearchPage> search(HttpClient& http, const SearchQuery& query) = 0;
    // Downloads to fetch for `variant` (empty = the provider's default). Fails with
    // ErrorCode::InvalidArgument when the licence does not allow installation.
    [[nodiscard]] virtual Result<InstallPlan> plan(HttpClient& http, const LibraryAsset& asset, std::string_view variant) = 0;
    // User credentials (Sketchfab API token). Providers without accounts ignore it.
    virtual void set_api_key(std::string /*key*/) {}
};

[[nodiscard]] std::unique_ptr<Provider> make_polyhaven();
[[nodiscard]] std::unique_ptr<Provider> make_ambientcg();
[[nodiscard]] std::unique_ptr<Provider> make_smithsonian();
[[nodiscard]] std::unique_ptr<Provider> make_sketchfab();
[[nodiscard]] std::unique_ptr<Provider> make_bigsoundbank();
[[nodiscard]] std::unique_ptr<Provider> make_nps_sounds();
[[nodiscard]] std::unique_ptr<Provider> make_kenney();

// Every built-in provider, in display order.
[[nodiscard]] std::vector<std::unique_ptr<Provider>> make_all_providers();

// The User-Agent every request sends (Poly Haven's API terms require one naming the software).
[[nodiscard]] std::string user_agent();

// Small helpers shared by providers and tests.
[[nodiscard]] std::string url_encode(std::string_view s);
[[nodiscard]] std::string html_unescape(std::string_view s);
[[nodiscard]] std::string slugify(std::string_view s); // lower-case [a-z0-9_-], never empty
[[nodiscard]] std::string format_bytes(std::uint64_t bytes);

} // namespace victus::library
