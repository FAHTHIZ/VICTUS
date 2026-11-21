// SPDX-License-Identifier: Apache-2.0
// Asset reference index, reference-safe rename/move and delete (ADR-0175).
#include "victus/assets/references.hpp"

#include "victus/core/fs.hpp"
#include "victus/core/json.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <chrono>
#include <format>
#include <map>
#include <set>

namespace victus::assets {
namespace {

namespace sfs = std::filesystem;
constexpr std::uint64_t kMaxScanBytes = 16ull << 20;

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Files that cannot name another asset: decoded media, model containers and archives.
bool binary_extension(const std::string& ext) {
    static const std::set<std::string> kBinary = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".exr", ".ktx2", ".dds",
                                                  ".glb", ".fbx", ".bin", ".wav", ".ogg", ".mp3", ".flac", ".ttf", ".otf",
                                                  ".zip", ".vpak", ".meta", ".blend", ".psd"};
    return kBinary.contains(ext);
}

bool is_hex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

// "8-4-4-4-12" hex at text[i].
bool uuid_at(std::string_view text, std::size_t i) {
    if (i + 36 > text.size()) return false;
    for (std::size_t k = 0; k < 36; ++k) {
        const char c = text[i + k];
        if (k == 8 || k == 13 || k == 18 || k == 23) {
            if (c != '-') return false;
        } else if (!is_hex(c)) {
            return false;
        }
    }
    return i + 36 == text.size() || !is_hex(text[i + 36]);
}

void scan_text(const AssetDatabase& db, const std::string& file, std::string_view text, std::vector<AssetReference>& out) {
    std::map<std::pair<Uuid, bool>, std::uint32_t> found;
    const AssetRecord* self = file.starts_with("assets/") ? db.find_path(std::string_view(file).substr(7)) : nullptr;
    // UUIDs.
    for (std::size_t i = 0; i + 36 <= text.size(); ++i) {
        if (!is_hex(text[i]) || (i > 0 && is_hex(text[i - 1])) || !uuid_at(text, i)) continue;
        Uuid u;
        if (Uuid::parse(text.substr(i, 36), u) && !u.is_nil()) {
            if (const AssetRecord* r = db.find(u); r && r != self) ++found[{u, false}];
        }
        i += 35;
    }
    // Quoted asset paths ("audio/hit.wav" or 'audio/hit.wav', optionally prefixed "assets/").
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char q = text[i];
        if (q != '"' && q != '\'') continue;
        const std::size_t end = text.find(q, i + 1);
        if (end == std::string_view::npos) break;
        std::string_view lit = text.substr(i + 1, end - i - 1);
        if (!lit.empty() && lit.size() < 512 && lit.find('\n') == std::string_view::npos && lit.find('.') != std::string_view::npos) {
            if (lit.starts_with("assets/")) lit.remove_prefix(7);
            if (const AssetRecord* r = db.find_path(lit); r && r != self) ++found[{r->uuid, true}];
        }
        i = end;
    }
    for (const auto& [key, n] : found) out.push_back({file, key.first, key.second, n});
}

void scan_dir(const AssetDatabase& db, const fs::Path& root, const fs::Path& dir, std::vector<AssetReference>& out, std::size_t& scanned) {
    std::error_code ec;
    if (!sfs::is_directory(dir, ec)) return;
    for (auto it = sfs::recursive_directory_iterator(dir, ec); !ec && it != sfs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::Path& p = it->path();
        const std::string name = p.filename().string();
        if (!name.empty() && name[0] == '.') {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec) || binary_extension(lower(p.extension().string()))) continue;
        if (it->file_size(ec) > kMaxScanBytes) continue;
        auto text = fs::read_text(p, kMaxScanBytes);
        if (!text || text->find('\0') != std::string::npos) continue;
        ++scanned;
        scan_text(db, sfs::relative(p, root, ec).generic_string(), *text, out);
    }
}

bool safe_relative(std::string_view rel) {
    if (rel.empty() || rel.front() == '/' || rel.back() == '/' || rel.find('\\') != std::string_view::npos ||
        rel.find(':') != std::string_view::npos)
        return false;
    std::size_t start = 0;
    while (start <= rel.size()) {
        const std::size_t slash = std::min(rel.find('/', start), rel.size());
        const std::string_view part = rel.substr(start, slash - start);
        if (part.empty() || part == "." || part == ".." || part.front() == '.' || part.ends_with(".meta")) return false;
        for (const char c : part)
            if (static_cast<unsigned char>(c) < 0x20 || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
        start = slash + 1;
    }
    return true;
}

std::string parent_of(std::string_view rel) {
    const auto slash = rel.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(rel.substr(0, slash));
}

void replace_all(std::string& s, std::string_view from, std::string_view to, std::size_t& n) {
    for (std::size_t pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + to.size())) {
        s.replace(pos, from.size(), to);
        ++n;
    }
}

} // namespace

std::string project_relative(const AssetRecord& r) { return "assets/" + r.path; }

ReferenceIndex ReferenceIndex::build(const AssetDatabase& db) {
    ReferenceIndex idx;
    const fs::Path& root = db.project_root();
    scan_dir(db, root, root / "scenes", idx.refs_, idx.files_scanned_);
    scan_dir(db, root, db.assets_dir(), idx.refs_, idx.files_scanned_);
    if (auto text = fs::read_text(root / "victus.project", kMaxScanBytes); text) {
        ++idx.files_scanned_;
        scan_text(db, "victus.project", *text, idx.refs_);
    }
    std::sort(idx.refs_.begin(), idx.refs_.end(), [](const AssetReference& a, const AssetReference& b) {
        return a.file != b.file ? a.file < b.file : a.target.to_string() < b.target.to_string();
    });
    return idx;
}

std::vector<AssetReference> ReferenceIndex::users(const Uuid& asset) const {
    std::vector<AssetReference> out;
    for (const auto& r : refs_)
        if (r.target == asset) {
            if (!out.empty() && out.back().file == r.file) {
                out.back().count += r.count; // referenced both by UUID and by path
                out.back().by_path = out.back().by_path || r.by_path;
            } else {
                out.push_back(r);
            }
        }
    return out;
}

std::vector<AssetReference> ReferenceIndex::dependencies(std::string_view file) const {
    std::vector<AssetReference> out;
    for (const auto& r : refs_)
        if (r.file == file) out.push_back(r);
    return out;
}

std::vector<Uuid> ReferenceIndex::unreferenced(const AssetDatabase& db) const {
    std::unordered_map<Uuid, bool, UuidHash> used;
    for (const auto& r : refs_) used[r.target] = true;
    std::vector<Uuid> out;
    for (const auto& rec : db.records())
        if (rec.kind != AssetKind::Scene && !used.contains(rec.uuid)) out.push_back(rec.uuid);
    return out;
}

Result<RenameReport> rename_asset(AssetDatabase& db, const Uuid& asset, std::string_view new_relative) {
    if (db.packaged()) return make_error(ErrorCode::InvalidArgument, "an exported game cannot be changed");
    const AssetRecord* rec = db.find(asset);
    if (!rec) return make_error(ErrorCode::NotFound, "asset not found");
    const AssetRecord r = *rec; // copy: refresh() below replaces the records
    if (!safe_relative(new_relative))
        return make_error(ErrorCode::InvalidArgument, std::format("'{}' is not a valid name (no '..', hidden names or special characters)", new_relative));
    if (new_relative == r.path) return make_error(ErrorCode::InvalidArgument, "the new name is the same as the old one");
    const std::string old_ext = lower(fs::Path(r.path).extension().string());
    if (lower(fs::Path(std::string(new_relative)).extension().string()) != old_ext)
        return make_error(ErrorCode::InvalidArgument, std::format("keep the {} extension so the asset stays the same type", old_ext));
    if ((old_ext == ".gltf" || old_ext == ".obj") && parent_of(new_relative) != parent_of(r.path))
        return make_error(ErrorCode::InvalidArgument,
                          std::format("{} files name their buffers and textures relative to their folder; move the whole folder instead", old_ext));
    const fs::Path from = db.source_path(r);
    const fs::Path to = db.assets_dir() / fs::Path(std::string(new_relative));
    std::error_code ec;
    if (sfs::exists(to, ec) || sfs::exists(fs::Path(to.string() + ".meta"), ec))
        return make_error(ErrorCode::AlreadyExists, std::format("assets/{} already exists", new_relative));

    const ReferenceIndex idx = ReferenceIndex::build(db);
    const auto users = idx.users(asset);

    sfs::create_directories(to.parent_path(), ec);
    if (ec) return make_error(ErrorCode::IoError, std::format("cannot create {}: {}", to.parent_path().string(), ec.message()));
    sfs::rename(from, to, ec);
    if (ec) return make_error(ErrorCode::IoError, std::format("cannot move {}: {}", r.path, ec.message()));
    const fs::Path meta_from(from.string() + ".meta");
    if (sfs::exists(meta_from, ec)) {
        sfs::rename(meta_from, fs::Path(to.string() + ".meta"), ec);
        if (ec) {
            std::error_code undo;
            sfs::rename(to, from, undo);
            return make_error(ErrorCode::IoError, std::format("cannot move {}.meta: {}", r.path, ec.message()));
        }
    }

    RenameReport report;
    report.from = r.path;
    report.to = std::string(new_relative);
    for (const auto& u : users) {
        if (!u.by_path) continue;
        const fs::Path file = db.project_root() / fs::Path(u.file);
        auto text = fs::read_text(file, kMaxScanBytes);
        if (!text) continue;
        std::size_t n = 0;
        for (const char q : {'"', '\''}) {
            replace_all(*text, std::format("{}{}{}", q, r.path, q), std::format("{}{}{}", q, new_relative, q), n);
            replace_all(*text, std::format("{}assets/{}{}", q, r.path, q), std::format("{}assets/{}{}", q, new_relative, q), n);
        }
        if (n == 0) continue;
        if (auto st = fs::write_text_atomic(file, *text); !st) return make_error(ErrorCode::IoError, std::format("moved, but could not update {}: {}", u.file, st.error().message));
        report.rewritten.push_back(u.file);
    }
    (void)db.refresh();
    return report;
}

Status delete_asset(AssetDatabase& db, const Uuid& asset) {
    if (db.packaged()) return make_error(ErrorCode::InvalidArgument, "an exported game cannot be changed");
    const AssetRecord* rec = db.find(asset);
    if (!rec) return make_error(ErrorCode::NotFound, "asset not found");
    const fs::Path src = db.source_path(*rec);
    std::error_code ec;
    sfs::remove(src, ec);
    if (ec) return make_error(ErrorCode::IoError, std::format("cannot delete {}: {}", rec->path, ec.message()));
    sfs::remove(fs::Path(src.string() + ".meta"), ec);
    sfs::remove(db.cooked_path(asset), ec);
    (void)db.refresh();
    return {};
}

fs::Path trash_dir(const AssetDatabase& db) { return db.project_root() / ".victus" / "trash"; }

namespace {

bool valid_trash_id(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-'; });
}

// Moves a file, falling back to copy + remove across filesystems.
Status move_file(const fs::Path& from, const fs::Path& to) {
    std::error_code ec;
    sfs::create_directories(to.parent_path(), ec);
    sfs::rename(from, to, ec);
    if (!ec) return {};
    ec.clear();
    sfs::copy_file(from, to, sfs::copy_options::overwrite_existing, ec);
    if (ec) return make_error(ErrorCode::IoError, std::format("cannot move {}: {}", from.string(), ec.message()));
    sfs::remove(from, ec);
    return {};
}

std::optional<TrashEntry> read_entry(const fs::Path& folder) {
    auto text = fs::read_text(folder / "entry.json", 1u << 16);
    if (!text) return std::nullopt;
    auto doc = json::parse(*text);
    if (!doc || !doc->is_object()) return std::nullopt;
    TrashEntry e;
    e.id = folder.filename().string();
    e.original_path = (*doc)["path"].as_string_or("");
    if (e.original_path.empty() || !Uuid::parse((*doc)["uuid"].as_string_or(""), e.uuid)) return std::nullopt;
    e.deleted_unix = (*doc)["deleted"].as_int(0);
    e.bytes = static_cast<std::uint64_t>(std::max<std::int64_t>((*doc)["bytes"].as_int(0), 0));
    return e;
}

} // namespace

Result<TrashEntry> trash_asset(AssetDatabase& db, const Uuid& asset) {
    if (db.packaged()) return make_error(ErrorCode::InvalidArgument, "an exported game cannot be changed");
    const AssetRecord* rec = db.find(asset);
    if (!rec) return make_error(ErrorCode::NotFound, "asset not found");
    const fs::Path src = db.source_path(*rec);
    TrashEntry e;
    e.original_path = rec->path;
    e.uuid = asset;
    e.deleted_unix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::error_code ec;
    e.bytes = sfs::file_size(src, ec);
    if (ec) e.bytes = 0;
    e.id = std::format("{}-{}", e.deleted_unix, asset.to_string().substr(0, 8));
    for (int i = 2; sfs::exists(trash_dir(db) / e.id); ++i) e.id = std::format("{}-{}-{}", e.deleted_unix, asset.to_string().substr(0, 8), i);
    const fs::Path folder = trash_dir(db) / e.id;
    sfs::create_directories(folder, ec);
    if (ec) return make_error(ErrorCode::IoError, std::format("cannot create {}: {}", folder.string(), ec.message()));
    json::Value doc = json::Value::object();
    doc.set("path", e.original_path);
    doc.set("uuid", asset.to_string());
    doc.set("deleted", e.deleted_unix);
    doc.set("bytes", e.bytes);
    VICTUS_TRY_STATUS(fs::write_text_atomic(folder / "entry.json", json::stringify(doc, true)));
    const std::string file = src.filename().string();
    if (auto st = move_file(src, folder / file); !st.ok()) {
        sfs::remove_all(folder, ec);
        return std::move(st).error();
    }
    const fs::Path meta(src.string() + ".meta");
    if (sfs::exists(meta)) (void)move_file(meta, folder / (file + ".meta"));
    sfs::remove(db.cooked_path(asset), ec);
    (void)db.refresh();
    return e;
}

std::vector<TrashEntry> list_trash(const AssetDatabase& db) {
    std::vector<TrashEntry> out;
    std::error_code ec;
    for (sfs::directory_iterator it(trash_dir(db), ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec))
            if (auto e = read_entry(it->path())) out.push_back(std::move(*e));
    std::sort(out.begin(), out.end(), [](const TrashEntry& a, const TrashEntry& b) {
        return a.deleted_unix != b.deleted_unix ? a.deleted_unix > b.deleted_unix : a.id > b.id;
    });
    return out;
}

Status restore_from_trash(AssetDatabase& db, std::string_view id, std::string_view to) {
    if (db.packaged()) return make_error(ErrorCode::InvalidArgument, "an exported game cannot be changed");
    if (!valid_trash_id(id)) return make_error(ErrorCode::InvalidArgument, "not a trash entry");
    const fs::Path folder = trash_dir(db) / std::string(id);
    auto e = read_entry(folder);
    if (!e) return make_error(ErrorCode::NotFound, std::format("trash entry {} not found", id));
    auto rel = fs::sanitize_relative(to.empty() ? std::string_view(e->original_path) : to);
    if (!rel) return std::move(rel).error();
    const fs::Path original(e->original_path);
    if (fs::Path(*rel).extension() != original.extension())
        return make_error(ErrorCode::InvalidArgument, "keep the file extension " + original.extension().string());
    const fs::Path dest = db.assets_dir() / *rel;
    if (sfs::exists(dest)) return make_error(ErrorCode::AlreadyExists, *rel + " already exists; restore under another name");
    if (db.find(e->uuid)) return make_error(ErrorCode::AlreadyExists, "an asset with the same ID is back in the project");
    const std::string file = original.filename().string();
    VICTUS_TRY_STATUS(move_file(folder / file, dest));
    if (sfs::exists(folder / (file + ".meta"))) (void)move_file(folder / (file + ".meta"), fs::Path(dest.string() + ".meta"));
    std::error_code ec;
    sfs::remove_all(folder, ec);
    (void)db.refresh();
    return {};
}

Status purge_trash(AssetDatabase& db, std::string_view id) {
    std::error_code ec;
    if (id.empty()) {
        sfs::remove_all(trash_dir(db), ec);
        if (ec) return make_error(ErrorCode::IoError, "cannot empty the trash: " + ec.message());
        return {};
    }
    if (!valid_trash_id(id)) return make_error(ErrorCode::InvalidArgument, "not a trash entry");
    const fs::Path folder = trash_dir(db) / std::string(id);
    if (!sfs::is_directory(folder)) return make_error(ErrorCode::NotFound, std::format("trash entry {} not found", id));
    sfs::remove_all(folder, ec);
    if (ec) return make_error(ErrorCode::IoError, "cannot delete: " + ec.message());
    return {};
}

} // namespace victus::assets
