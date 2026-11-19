// SPDX-License-Identifier: Apache-2.0
// VICTUS projects: a folder owned by the developer containing `victus.project`, `assets/`,
// `scenes/` and a disposable `.victus/` cache. Shared by the editor, CLI and player.
#pragma once
#include "victus/core/fs.hpp"
#include "victus/core/json.hpp"
#include "victus/core/process.hpp"
#include "victus/gameplay/input_map.hpp"
#include "victus/project/characters.hpp"
#include "victus/scene/scene.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace victus::project {

inline constexpr std::string_view kProjectFileName = "victus.project";
inline constexpr int kProjectFormatVersion = 1;
inline constexpr std::string_view kSceneExtension = ".vscene";

struct ProjectSettings {
    std::string name{"Untitled"};
    std::string startup_scene{"scenes/main.vscene"}; // project-relative
    std::string template_id;
    std::string engine_version;   // engine version that created/last saved the project
    int window_width{1280};
    int window_height{720};
    bool vsync{true};
    // Rendering scalability: "low" | "medium" | "high" | "ultra" and an optional resolution
    // scale override (0 = the tier's default).
    std::string render_quality{"high"};
    float render_scale{0.0f};
    // Dynamic resolution: target frame rate the player holds by lowering the 3D render scale
    // (down to `dynamic_resolution_min`); 0 disables it.
    int target_fps{0};
    float dynamic_resolution_min{0.5f};
    // Temporal upscaling of a reduced render scale on TAA tiers (rendering.temporal_upscale,
    // ADR-0055); false keeps bilinear scaling.
    bool temporal_upscale{true};
    // Phones and tablets (rendering.mobile_quality / rendering.mobile_target_fps): the tier and
    // frame-rate target the player uses there instead of the desktop ones. Mobile GPUs share
    // power and heat with the CPU, so the defaults hold 30 fps at the medium tier.
    std::string mobile_quality{"medium"};
    // Resident texture + model memory the player keeps (rendering.asset_budget_mb, ADR-0088);
    // assets unused for about a second are evicted least recently used first. 0 = unlimited.
    int asset_budget_mb{0};
    int mobile_target_fps{30};
    // Input actions ("input" section); projects without one use the engine defaults.
    gameplay::InputMap input = gameplay::default_input_map();
    // Version of the game's save data ("save_version"); raise it when the shape of saved data
    // changes and register a migration (save.migrate) from the previous version.
    std::uint32_t save_version{1};
    // Locale whose string table (assets/localization/<locale>.vstrings) is used first and fills
    // in keys other locales lack ("default_locale").
    std::string default_locale{"en"};
    json::Value extra = json::Value::object(); // unknown keys preserved verbatim
};

struct TemplateInfo {
    std::string id;           // "3d", "2d", "simulation"
    std::string display_name;
    std::string description;
    bool characters{false}; // has a player character: TemplateOptions::character applies
};

// Choices made when creating a project from a template.
struct TemplateOptions {
    // Starter character (victus/project/characters.hpp) for templates with `characters`: "v01"
    // (default), "v02", or "none" for a plain capsule body without a skinned character.
    std::string character{kDefaultCharacter};
};

[[nodiscard]] const std::vector<TemplateInfo>& templates();
// Fills `scene` with the template's starter content (cleared first).
[[nodiscard]] Status build_template_scene(std::string_view template_id, scene::Scene& scene,
                                          const TemplateOptions& options = {});

struct TemplateFile {
    std::string path; // project-relative, e.g. "assets/scripts/player.luau"
    std::string content;
};
// Extra files (scripts) a template writes next to its scene.
[[nodiscard]] std::vector<TemplateFile> template_files(std::string_view template_id,
                                                      const TemplateOptions& options = {});

// Registers the serializers of every engine component a project scene may contain (gameplay
// behaviours, scripts). Idempotent; call before loading scenes.
void register_components();

class Project {
public:
    // Creates a new project in `directory` (must not exist or be empty).
    [[nodiscard]] static Result<Project> create(const fs::Path& directory, std::string_view name,
                                                std::string_view template_id, const TemplateOptions& options = {});
    // Opens a project folder or its `victus.project` file.
    [[nodiscard]] static Result<Project> open(const fs::Path& path);
    [[nodiscard]] static bool is_project_dir(const fs::Path& directory);

    [[nodiscard]] Status save() const;

    [[nodiscard]] const fs::Path& root() const noexcept { return root_; }
    // Where development runs (editor play mode, `victus run`, `victus simulate`) keep save games:
    // `.victus/saves`, separate from the saves of the exported game.
    [[nodiscard]] fs::Path dev_save_directory() const { return root_ / ".victus" / "saves"; }
    [[nodiscard]] fs::Path file() const { return root_ / kProjectFileName; }
    [[nodiscard]] fs::Path assets_dir() const { return root_ / "assets"; }
    [[nodiscard]] fs::Path scenes_dir() const { return root_ / "scenes"; }
    [[nodiscard]] fs::Path cache_dir() const { return root_ / ".victus"; }
    [[nodiscard]] ProjectSettings& settings() noexcept { return settings_; }
    [[nodiscard]] const ProjectSettings& settings() const noexcept { return settings_; }

    // Sandboxed resolution of project-relative paths (rejects absolute paths and '..').
    [[nodiscard]] Result<fs::Path> resolve(std::string_view relative) const;
    // Project-relative, forward-slash form of a path inside the project; empty if outside.
    [[nodiscard]] std::string relative(const fs::Path& path) const;
    [[nodiscard]] Result<fs::Path> startup_scene_path() const { return resolve(settings_.startup_scene); }
    // All *.vscene files under scenes/ (project-relative, sorted).
    [[nodiscard]] std::vector<std::string> scenes() const;

private:
    fs::Path root_;
    ProjectSettings settings_;
};

// The project's "bridges" section: helper processes scripts start with `bridge.open(name)`,
//   "bridges": {"sim": {"command": ["dotnet", "${env:SIM_DLL:-tools/sim.dll}", "serve", "--saves", "${saves}/sim"],
//                       "working_directory": "${project}", "discard_stderr": false}}
// Variables: ${project} (project root), ${saves} (the run's save folder), ${env:NAME} and
// ${env:NAME:-fallback}. A relative program path containing '/' is resolved against the project.
[[nodiscard]] Result<process::LaunchOptions> bridge_launch(const Project& project, std::string_view name,
                                                           const fs::Path& save_directory);
// Expands the variables above in one string.
[[nodiscard]] Result<std::string> expand_bridge_variables(std::string_view text, const fs::Path& project_root,
                                                          const fs::Path& save_directory);

// Recently opened projects (most recent first), persisted in the user data directory.
class RecentProjects {
public:
    explicit RecentProjects(fs::Path file = default_file());
    [[nodiscard]] static fs::Path default_file();
    [[nodiscard]] const std::vector<fs::Path>& entries() const noexcept { return entries_; }
    void add(const fs::Path& project_root);
    void remove(const fs::Path& project_root);
    [[nodiscard]] Status save() const;

private:
    fs::Path file_;
    std::vector<fs::Path> entries_;
    static constexpr std::size_t kMax = 16;
};

} // namespace victus::project
