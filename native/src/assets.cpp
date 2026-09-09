#include "assets.h"
#include <miniz.h>
#include <shlobj.h>
#include <regex>
#include <wincodec.h>
namespace edm {
namespace {
struct Zip {
    MappedFile file;
    mz_zip_archive zip{};
    std::mutex mutex;
    std::vector<std::string> names;
    std::map<std::string, uint32_t> index;
    explicit Zip(const fs::path& p) : file(p) {
        require(mz_zip_reader_init_mem(&zip, file.data, file.size, 0), "Invalid ZIP: " + pathString(p));
        for (uint32_t i = 0; i < mz_zip_reader_get_num_files(&zip); i++) {
            mz_zip_archive_file_stat s{};
            require(mz_zip_reader_file_stat(&zip, i, &s), "ZIP directory error");
            if (s.m_is_directory)
                continue;
            std::string n = s.m_filename;
            std::replace(n.begin(), n.end(), '\\', '/');
            names.push_back(n);
            index[lower(n)] = i;
        }
    }
    ~Zip() {
        mz_zip_reader_end(&zip);
    }
};
std::shared_ptr<Zip> archiveFile(const fs::path& path) {
    static std::mutex mutex;
    static std::map<std::string, std::pair<fs::file_time_type, std::shared_ptr<Zip>>> cache;
    std::lock_guard lock(mutex);
    auto key = lower(pathString(fs::absolute(path)));
    auto time = fs::last_write_time(path);
    auto it = cache.find(key);
    if (it != cache.end() && it->second.first == time)
        return it->second.second;
    auto zip = std::make_shared<Zip>(path);
    cache[key] = {time, zip};
    return zip;
}
std::vector<fs::path> children(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end;
         it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        out.push_back(it->path());
    }
    std::sort(out.begin(), out.end(),
              [](auto& a, auto& b) { return lower(pathString(a)) < lower(pathString(b)); });
    return out;
}
std::vector<fs::path> files(const fs::path& root, int depth = 5) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (it.depth() >= depth || it->is_symlink(ec))
            it.disable_recursion_pending();
        if (it->is_regular_file(ec))
            out.push_back(it->path());
    }
    std::sort(out.begin(), out.end(),
              [](auto& a, auto& b) { return lower(pathString(a)) < lower(pathString(b)); });
    return out;
}
bool isInstall(const fs::path& p) {
    return fs::is_directory(p / "Bazar") &&
           (fs::is_directory(p / "CoreMods") || fs::is_regular_file(p / "bin/DCS.exe"));
}
fs::path envPath(const wchar_t* name) {
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(name, buf, 32768);
    return n && n < 32768 ? fs::path(buf) : fs::path();
}
std::vector<fs::path> drives() {
    std::vector<fs::path> out;
    for (wchar_t c = L'C'; c <= L'Z'; c++) {
        std::wstring root{c, L':', L'\\'};
        if (GetDriveTypeW(root.c_str()) == DRIVE_FIXED)
            out.emplace_back(root);
    }
    return out;
}
std::optional<fs::path> registryPath(HKEY hive, const std::wstring& key, const wchar_t* value) {
    wchar_t buf[32768];
    DWORD size = sizeof(buf);
    if (RegGetValueW(hive, key.c_str(), value, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return fs::path(buf);
    return {};
}
bool imageType(const fs::path& p) {
    static const std::set<std::string> types = {".dds", ".png", ".jpg", ".jpeg", ".tga", ".bmp"};
    return types.contains(lower(pathString(p.extension())));
}
std::string textureKey(std::string n) {
    std::replace(n.begin(), n.end(), '\\', '/');
    fs::path p = wide(n);
    if (imageType(p))
        p.replace_extension();
    return lower(pathString(p.generic_wstring()));
}
std::string leaf(const std::string& n) {
    return pathString(fs::path(wide(n)).filename());
}
bool relativeAssetPath(const fs::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return false;
    const auto text = pathString(path.generic_wstring());
    return text.find(':') == std::string::npos;
}
bool boundedRelativeAssetPath(const fs::path& path) {
    if (!relativeAssetPath(path))
        return false;
    const auto normalized = path.lexically_normal();
    return normalized != "." && normalized.begin() != normalized.end() && *normalized.begin() != "..";
}
void hr(HRESULT result, const char* what) {
    require(SUCCEEDED(result), std::string(what) + " (HRESULT " + std::to_string(uint32_t(result)) + ")");
}
} // namespace
std::vector<std::string> zipEntries(const fs::path& path) {
    return archiveFile(path)->names;
}
std::vector<uint8_t> zipRead(const fs::path& path, const std::string& entry, size_t limit) {
    auto a = archiveFile(path);
    std::lock_guard lock(a->mutex);
    auto it = a->index.find(lower(entry));
    require(it != a->index.end(), "Missing ZIP entry " + entry);
    mz_zip_archive_file_stat s{};
    require(mz_zip_reader_file_stat(&a->zip, it->second, &s), "Invalid ZIP record");
    require(s.m_uncomp_size <= limit, "ZIP entry exceeds size limit");
    std::vector<uint8_t> bytes(size_t(s.m_uncomp_size));
    require(mz_zip_reader_extract_to_mem(&a->zip, it->second, bytes.data(), bytes.size(), 0),
            "ZIP extraction failed: " + entry);
    return bytes;
}
std::vector<uint8_t> ImageSource::bytes(size_t limit) const {
    return entry.empty() ? readFile(path, limit) : zipRead(path, entry, limit);
}
Json Livery::metadata() const {
    Json tex = Json::array(), custom = Json::object();
    for (auto& [k, t] : textures)
        tex.push_back({{"material", t.material}, {"slot", t.slot}, {"name", t.name}, {"common", t.common}});
    for (auto [a, v] : args)
        custom[std::to_string(a)] = v;
    return {{"name", name},         {"source", identifier()}, {"unit", unit},
            {"origin", origin},     {"custom_args", custom},  {"countries", countries},
            {"warnings", warnings}, {"lua", evaluation},      {"textures", tex}};
}
Livery readLivery(fs::path path, std::string entry, std::string origin, std::string unit,
                  const Json& context) {
    auto rawPath = pathString(path);
    auto delim = rawPath.find("::");
    if (entry.empty() && delim != std::string::npos) {
        entry = rawPath.substr(delim + 2);
        path = wide(rawPath.substr(0, delim));
    }
    if (fs::is_directory(path))
        path /= L"description.lua";
    std::vector<uint8_t> raw;
    std::string fallback;
    if (lower(pathString(path.extension())) == ".zip") {
        if (entry.empty()) {
            std::vector<std::string> descriptions;
            for (auto& e : zipEntries(path))
                if (lower(leaf(e)) == "description.lua")
                    descriptions.push_back(e);
            require(descriptions.size() == 1, "ZIP 中需要选择明确的 description.lua");
            entry = descriptions[0];
        }
        raw = zipRead(path, entry, 2000000);
        fallback = pathString(fs::path(wide(entry)).parent_path().filename());
        if (fallback.empty())
            fallback = pathString(path.stem());
    } else {
        raw = readFile(path, 2000000);
        fallback = pathString(path.parent_path().filename());
    }
    auto result = evaluateLua(decodeText(raw), path, entry, context);
    auto& env = result.at("environment");
    require(env.contains("livery") && env["livery"].is_object(), "没有可读取的 livery 表");
    Livery l;
    l.path = fs::absolute(path);
    l.entry = entry;
    l.name = env.contains("name") && env["name"].is_string() ? env["name"].get<std::string>() : fallback;
    l.unit = unit;
    l.origin = origin;
    l.evaluation = result.at("evaluation");
    for (auto& row : env["livery"]) {
        if (!row.is_object() || !row.contains("1") || !row["1"].is_string() || !row.contains("2") ||
            !row["2"].is_number() || !row.contains("3") || !row["3"].is_string() || !row.contains("4") ||
            !row["4"].is_boolean()) {
            l.warnings.push_back("跳过无效涂装记录。");
            continue;
        }
        double slot = row["2"].get<double>();
        if (!std::isfinite(slot) || slot != std::floor(slot) || slot < 0 || slot > 100000) {
            l.warnings.push_back("无效材质槽位。");
            continue;
        }
        TextureOverride t;
        t.material = row["1"];
        t.slot = int(slot);
        t.name = row["3"];
        t.common = row["4"];
        l.textures[{lower(t.material), t.slot}] = t;
    }
    if (env.contains("custom_args") && env["custom_args"].is_object())
        for (auto it = env["custom_args"].begin(); it != env["custom_args"].end(); ++it)
            try {
                size_t used = 0;
                int a = std::stoi(it.key(), &used);
                if (used == it.key().size() && a >= 0 && it.value().is_number()) {
                    double v = it.value().get<double>();
                    if (std::isfinite(v))
                        l.args[a] = v;
                }
            } catch (...) {
            }
    if (env.contains("countries") && env["countries"].is_object())
        for (auto& v : env["countries"])
            if (v.is_string())
                l.countries.push_back(v);
    require(env["livery"].empty() || !l.textures.empty(), "涂装没有有效材质贴图映射");
    return l;
}
std::vector<fs::path> installationPaths(const fs::path& source) {
    std::vector<fs::path> candidates, steam;
    if (!source.empty())
        for (auto p = fs::absolute(source).parent_path(); !p.empty() && p != p.parent_path();
             p = p.parent_path())
            if (isInstall(p))
                candidates.push_back(p);
    for (HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(hive, L"Software\\Eagle Dynamics", 0, KEY_READ, &key) == ERROR_SUCCESS) {
            for (DWORD i = 0;; i++) {
                wchar_t name[256];
                DWORD len = 256;
                if (RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                    break;
                for (auto value : {L"Path", L"InstallPath", L"InstallationPath"})
                    if (auto p =
                            registryPath(hive, L"Software\\Eagle Dynamics\\" + std::wstring(name), value))
                        candidates.push_back(*p);
            }
            RegCloseKey(key);
        }
    }
    for (auto [h, k] : std::vector<std::pair<HKEY, std::wstring>>{
             {HKEY_CURRENT_USER, L"Software\\Valve\\Steam"},
             {HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Valve\\Steam"}})
        for (auto v : {L"SteamPath", L"InstallPath"})
            if (auto p = registryPath(h, k, v))
                steam.push_back(*p);
    for (auto& drive : drives()) {
        steam.push_back(drive / "SteamLibrary");
        candidates.push_back(drive / "DCS World");
        candidates.push_back(drive / "DCSWorld");
    }
    for (auto name : {L"ProgramFiles", L"ProgramFiles(x86)"}) {
        auto p = envPath(name);
        if (p.empty())
            continue;
        steam.push_back(p / "Steam");
        candidates.push_back(p / "Eagle Dynamics/DCS World");
        candidates.push_back(p / "Eagle Dynamics/DCS World OpenBeta");
    }
    auto steamRoots = uniquePaths(steam);
    for (auto& root : steamRoots) {
        std::vector<fs::path> libraries{root};
        try {
            auto raw = readFile(root / "steamapps/libraryfolders.vdf", 2000000);
            auto text = decodeText(raw);
            std::regex pattern("\"path\"\\s*\"([^\"]+)\"");
            for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
                std::string p = (*it)[1];
                for (size_t n = 0; (n = p.find("\\\\", n)) != std::string::npos;)
                    p.replace(n, 2, "\\");
                libraries.emplace_back(wide(p));
            }
        } catch (...) {
        }
        for (auto& lib : libraries) {
            candidates.push_back(lib / "steamapps/common/DCSWorld");
            candidates.push_back(lib / "steamapps/common/DCS World");
        }
    }
    std::vector<fs::path> out;
    for (auto& p : uniquePaths(candidates))
        if (isInstall(p))
            out.push_back(p);
    return out;
}
fs::path modulePath(const fs::path& source) {
    for (auto p = fs::absolute(source).parent_path(); !p.empty() && p != p.parent_path();
         p = p.parent_path()) {
        auto parent = lower(pathString(p.parent_path().filename()));
        if ((parent == "aircraft" || parent == "tech") && fs::is_directory(p / "Textures"))
            return p;
    }
    return {};
}
Catalog discoverLiveries(const fs::path& source, const std::vector<fs::path>& extras, Progress progress,
                         const std::atomic_bool* cancel) {
    Catalog c;
    c.installs = installationPaths(source);
    std::vector<fs::path> saved;
    PWSTR known = nullptr;
    const GUID savedId = {0x4c5c32ff, 0xbb9d, 0x43b0, {0xb5, 0xb4, 0x2d, 0x72, 0xe5, 0x4e, 0xaa, 0xa4}};
    if (SUCCEEDED(SHGetKnownFolderPath(savedId, 0, nullptr, &known))) {
        saved.emplace_back(known);
        CoTaskMemFree(known);
    }
    auto profile = envPath(L"USERPROFILE");
    if (!profile.empty())
        saved.push_back(profile / "Saved Games");
    for (auto& drive : drives())
        for (auto& p : children(drive))
            if (fs::is_directory(p)) {
                auto n = normalized(pathString(p.filename()));
                if (n.find("savedgames") != std::string::npos || pathString(p.filename()) == "保存的游戏")
                    saved.push_back(p);
            }
    std::vector<std::pair<fs::path, std::string>> roots;
    auto add = [&](const fs::path& p, const std::string& label) {
        if (fs::is_directory(p))
            roots.emplace_back(p, label);
    };
    for (auto& save : uniquePaths(saved)) {
        auto profiles = children(save);
        if (lower(pathString(save.filename())).starts_with("dcs"))
            profiles.insert(profiles.begin(), save);
        for (auto& p : profiles)
            if (fs::is_directory(p) && lower(pathString(p.filename())).starts_with("dcs")) {
                std::string label =
                    "保存的游戏 / " + pathString(p.parent_path().filename()) + "/" + pathString(p.filename());
                add(p / "Liveries", label);
                for (auto category : {"aircraft", "tech"})
                    for (auto& mod : children(p / "Mods" / category))
                        add(mod / "Liveries", label);
            }
    }
    for (auto& p : uniquePaths(extras))
        if (isInstall(p))
            c.installs.push_back(p);
    c.installs = uniquePaths(c.installs);
    for (auto& install : c.installs) {
        auto label = "DCS 本体 / " + pathString(install.filename());
        add(install / "Bazar/Liveries", label);
        for (auto base : {"CoreMods", "Mods"})
            for (auto& category : children(install / base))
                if (fs::is_directory(category)) {
                    add(category / "Liveries", label);
                    for (auto& mod : children(category))
                        if (fs::is_directory(mod))
                            add(mod / "Liveries", label);
                }
    }
    for (auto& p : uniquePaths(extras))
        if (!isInstall(p)) {
            add(p, "添加目录");
            add(p / "Liveries", "添加目录");
            for (auto& child : children(p))
                if (fs::is_directory(child) && lower(pathString(child.filename())).starts_with("dcs"))
                    add(child / "Liveries", "添加目录");
        }
    auto name = std::regex_replace(pathString(source.stem()),
                                   std::regex("[_-](lod.*|collision.*)$", std::regex::icase), "");
    auto n = normalized(name);
    std::set<std::string> aliases{n};
    std::map<std::string, std::vector<std::string>> aliasMap{{"fa18c", {"fa18chornet"}},
                                                             {"f16cbl50", {"f16c50"}},
                                                             {"a10c", {"a10c", "a10cii"}},
                                                             {"a10c2", {"a10c", "a10cii"}},
                                                             {"ka50", {"ka50", "ka503"}}};
    for (auto& a : aliasMap[n])
        aliases.insert(a);
    if (n.starts_with("hb") && n.find("f14") != std::string::npos) {
        aliases.insert("f14b");
        aliases.insert("f14a135gr");
    }
    std::set<std::string> seen, found;
    std::vector<std::pair<fs::path, std::string>> targets;
    for (auto& [root, label] : roots) {
        if (!seen.insert(lower(pathString(root))).second)
            continue;
        c.roots.push_back(root);
        if (aliases.contains(normalized(pathString(root.filename())))) {
            targets.emplace_back(root, label);
            continue;
        }
        for (auto& p : children(root))
            if (fs::is_directory(p) && aliases.contains(normalized(pathString(p.filename()))))
                targets.emplace_back(p, label);
        if (fs::is_regular_file(root / "description.lua"))
            targets.emplace_back(root, label);
    }
    for (auto& [unit, label] : targets) {
        if (progress)
            progress("扫描涂装：" + pathString(unit));
        for (auto& file : files(unit)) {
            if (cancel && *cancel)
                throw std::runtime_error("Cancelled");
            std::vector<std::string> entries;
            try {
                if (lower(pathString(file.filename())) == "description.lua")
                    entries.push_back("");
                else if (lower(pathString(file.extension())) == ".zip")
                    for (auto& e : zipEntries(file))
                        if (lower(leaf(e)) == "description.lua")
                            entries.push_back(e);
                for (auto& e : entries) {
                    if (!found.insert(lower(pathString(file)) + "::" + e).second)
                        continue;
                    try {
                        c.liveries.push_back(readLivery(file, e, label, pathString(unit.filename())));
                    } catch (...) {
                        c.warnings.push_back(pathString(file) + "::" + e + ": " + exceptionText());
                    }
                }
            } catch (...) {
                c.warnings.push_back(pathString(file) + ": " + exceptionText());
            }
        }
    }
    std::sort(c.liveries.begin(), c.liveries.end(), [](auto& a, auto& b) {
        bool as = a.origin.starts_with("保存"), bs = b.origin.starts_with("保存");
        if (as != bs)
            return as;
        return lower(a.name) < lower(b.name);
    });
    return c;
}
TextureResolver::TextureResolver(const fs::path& path, const fs::path& extra,
                                 std::shared_ptr<Livery> selected)
    : source(path), extraDirectory(extra), livery(selected) {
    auto mod = modulePath(source);
    auto installs = installationPaths(source);
    if (!extra.empty())
        roots.push_back(extra);
    if (!mod.empty())
        roots.push_back(mod / "Textures");
    roots.insert(roots.end(), {source.parent_path() / "Textures",
                               source.parent_path().parent_path() / "Textures", source.parent_path()});
    for (auto& install : installs) {
        for (auto category : {"CoreMods/aircraft", "Mods/aircraft"})
            if (!mod.empty())
                roots.push_back(install / category / mod.filename() / "Textures");
            else
                for (auto& m : children(install / category))
                    if (fs::is_regular_file(m / "Shapes" / source.filename()))
                        roots.push_back(m / "Textures");
        roots.push_back(install / "Bazar/TempTextures");
        roots.push_back(install / "Bazar/Textures");
    }
    roots = uniquePaths(roots);
    pending = roots;
    if (livery) {
        if (!livery->entry.empty())
            archive(livery->path, local,
                    pathString(fs::path(wide(livery->entry)).parent_path().generic_wstring()));
        else
            directory(livery->path.parent_path(), local);
    }
}
void TextureResolver::put(Index& map, const std::string& name, const ImageSource& image) {
    auto key = textureKey(name);
    map.try_emplace(key, image);
    map.try_emplace(leaf(key), image);
}
void TextureResolver::archive(const fs::path& path, Index& into, const std::string& relative) {
    try {
        for (auto& n : zipEntries(path)) {
            fs::path p = wide(n);
            if (!imageType(p) || !boundedRelativeAssetPath(p))
                continue;
            std::string key = n;
            if (!relative.empty()) {
                key = pathString(p.lexically_relative(fs::path(wide(relative))).generic_wstring());
                if (key.starts_with("../"))
                    continue;
            }
            put(into, key, ImageSource{path, n});
        }
    } catch (...) {
        warnings.push_back(pathString(path.filename()) + ": " + exceptionText());
    }
}
void TextureResolver::directory(const fs::path& path, Index& into, bool recursive) {
    for (auto& p : recursive ? files(path, 8) : children(path)) {
        if (imageType(p))
            put(into, pathString(p.lexically_relative(path).generic_wstring()), ImageSource{p});
        else if (lower(pathString(p.extension())) == ".zip")
            archive(p, into);
    }
}
std::optional<ImageSource> TextureResolver::common(const std::string& name) {
    auto key = textureKey(name);
    while (!index.contains(key) && !pending.empty()) {
        auto root = pending.front();
        pending.erase(pending.begin());
        directory(root, index, root != source.parent_path());
    }
    auto it = index.find(key);
    if (it != index.end())
        return it->second;
    if (key == "empty")
        return ImageSource{{}, "", true};
    return {};
}
std::optional<ImageSource> TextureResolver::localImage(const std::string& name) {
    if (!livery)
        return {};
    std::string raw = name;
    std::replace(raw.begin(), raw.end(), '\\', '/');
    const fs::path rel = fs::path(wide(raw)).lexically_normal();
    if (!relativeAssetPath(fs::path(wide(raw))) || rel.filename().empty() || rel.filename() == ".." ||
        rel.filename() == ".")
        return {};

    // DCS exposes A.zip as a virtual A directory. Consequently ../B/wing from A.zip's
    // description can refer to B.zip::wing.dds, just as it can refer to B/wing.dds on disk.
    // Keep this namespace bounded to the selected livery's parent (usually one aircraft's
    // Liveries folder); never search unrelated skins by a matching bare texture name.
    const auto container = fs::absolute(livery->path).lexically_normal();
    const bool zipped = !livery->entry.empty();
    const auto root = zipped ? container.parent_path() / container.stem() : container.parent_path();
    const auto scope = zipped ? container.parent_path() : root.parent_path();
    const auto entryDirectory = zipped ? fs::path(wide(livery->entry)).parent_path() : fs::path();
    const auto target = (root / entryDirectory / rel).lexically_normal();
    if (!boundedRelativeAssetPath(target.lexically_relative(scope)))
        return {};
    auto key = textureKey(pathString(rel.generic_wstring()));
    if (boundedRelativeAssetPath(rel))
        if (auto found = local.find(key); found != local.end())
            if (!found->second.entry.empty() || within(found->second.path, scope))
                return found->second;
    auto inArchive = [&](const fs::path& zip, const fs::path& relative) -> std::optional<ImageSource> {
        if (!boundedRelativeAssetPath(relative))
            return {};
        const auto wanted = textureKey(pathString(relative.lexically_normal().generic_wstring()));
        try {
            for (const auto& entry : zipEntries(zip)) {
                const fs::path path = wide(entry);
                if (imageType(path) && boundedRelativeAssetPath(path) &&
                    textureKey(pathString(path.lexically_normal().generic_wstring())) == wanted)
                    return ImageSource{zip, entry};
            }
        } catch (...) {
            warnings.push_back(pathString(zip.filename()) + ": " + exceptionText());
        }
        return {};
    };
    // Relative references inside a nested ZIP description keep their original meaning.
    if (zipped)
        if (auto found = inArchive(container, (entryDirectory / rel).lexically_normal()))
            return found;

    // Loose files take precedence over a ZIP mounted at the same virtual directory.
    auto directory = target.parent_path();
    std::error_code directoryError;
    if (fs::is_directory(directory, directoryError) && within(directory, scope))
        for (const auto& file : children(directory))
            if (imageType(file) && textureKey(pathString(file.filename())) == leaf(key) &&
                within(file, scope))
                return ImageSource{file};

    // Probe only archives whose exact virtual path was explicitly referenced. This also handles
    // nested textures (B.zip::sub/wing.dds) without scanning every neighbouring livery archive.
    for (auto mount = target.parent_path(); mount != scope && !mount.empty(); mount = mount.parent_path()) {
        if (!boundedRelativeAssetPath(mount.lexically_relative(scope)))
            break;
        auto zip = mount;
        zip += L".zip";
        std::error_code ec;
        if (!fs::is_regular_file(zip, ec) || !within(zip, scope) || (zipped && zip == container))
            continue;
        if (auto found = inArchive(zip, target.lexically_relative(mount)))
            return found;
    }
    return {};
}
std::optional<ImageSource> TextureResolver::material(const Material& mat, int slot) {
    auto sourceKey = [](const fs::path& path) {
        return lower(pathString(fs::absolute(path).lexically_normal()));
    };
    if (!mat.source.empty() && sourceKey(mat.source) != sourceKey(source)) {
        auto& entry = sourceResolvers[sourceKey(mat.source)];
        if (!entry.resolver) {
            entry.resolver = std::make_unique<TextureResolver>(mat.source, extraDirectory, livery);
            for (const auto& root : entry.resolver->roots)
                if (std::find(roots.begin(), roots.end(), root) == roots.end())
                    roots.push_back(root);
        }
        auto found = entry.resolver->material(mat, slot);
        // Names alone are not unique after attaching another aircraft, rack, or weapon.
        // Preserve the source in diagnostics as well as in the resolver's lookup scope.
        const auto prefix = pathString(mat.source) + " :: ";
        for (; entry.missingCount < entry.resolver->missing.size(); ++entry.missingCount)
            missing.push_back(prefix + entry.resolver->missing[entry.missingCount]);
        for (; entry.warningCount < entry.resolver->warnings.size(); ++entry.warningCount)
            warnings.push_back(prefix + entry.resolver->warnings[entry.warningCount]);
        for (auto it = entry.resolver->resolved.begin(); it != entry.resolver->resolved.end(); ++it)
            resolved[prefix + it.key()] = it.value();
        return found;
    }
    const TextureOverride* over = nullptr;
    auto overrideAt = [&](int s) -> const TextureOverride* {
        if (!livery)
            return nullptr;
        auto it = livery->textures.find({lower(mat.name), s});
        return it == livery->textures.end() ? nullptr : &it->second;
    };
    over = overrideAt(slot);
    const TextureRef* ref = mat.texture(slot);
    if (slot == 13) {
        auto legacy = overrideAt(2);
        if (!over && legacy && lower(legacy->name).find("roughmet") != std::string::npos)
            over = legacy;
        auto old = mat.texture(2);
        if (!ref && old && lower(old->name).find("roughmet") != std::string::npos)
            ref = old;
    }
    std::string name;
    std::optional<ImageSource> found;
    if (over) {
        name = over->name;
        found = over->common ? common(name) : localImage(name);
    } else if (ref) {
        name = ref->name;
        found = common(name);
    } else if (slot == 13) {
        auto diffuse = overrideAt(0);
        if (diffuse) {
            name = textureKey(diffuse->name) + "_roughmet";
            found = diffuse->common ? common(name) : localImage(name);
        }
        if (!found)
            if (auto base = mat.texture(0)) {
                name = textureKey(base->name) + "_roughmet";
                found = common(name);
            }
        if (!found)
            return {};
    } else
        return {};
    auto label = mat.name + " [" + std::to_string(slot) + "] → " + name;
    if (found)
        resolved[label] = found->key();
    else
        missing.push_back(label);
    return found;
}
size_t TextureImage::bytes() const {
    size_t total = 0;
    for (size_t mip = firstMip; mip < pixels.GetMetadata().mipLevels; mip++) {
        auto* img = pixels.GetImage(mip, 0, 0);
        if (img)
            total += img->slicePitch;
    }
    return total;
}
std::shared_ptr<TextureImage> loadTexture(const ImageSource& source, size_t maxSize) {
    ComApartment apartment;
    auto out = std::make_shared<TextureImage>();
    if (source.empty) {
        hr(out->pixels.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1), "Transparent image");
        std::memset(out->pixels.GetPixels(), 0, 4);
        return out;
    }
    auto bytes = source.bytes();
    auto ext = lower(pathString(
        fs::path(wide(source.entry.empty() ? pathString(source.path) : source.entry)).extension()));
    if (ext == ".dds")
        hr(DirectX::LoadFromDDSMemory(bytes.data(), bytes.size(), DirectX::DDS_FLAGS_NONE, nullptr,
                                      out->pixels),
           "Read DDS");
    else if (ext == ".tga")
        hr(DirectX::LoadFromTGAMemory(bytes.data(), bytes.size(), nullptr, out->pixels), "Read TGA");
    else
        hr(DirectX::LoadFromWICMemory(bytes.data(), bytes.size(), DirectX::WIC_FLAGS_FORCE_RGB, nullptr,
                                      out->pixels),
           "Read image");
    auto meta = out->pixels.GetMetadata();
    require(meta.dimension == DirectX::TEX_DIMENSION_TEXTURE2D && meta.arraySize == 1,
            "Only 2D material textures supported");
    require(meta.width <= 32768 && meta.height <= 32768, "Texture dimensions exceed limit");
    if (maxSize) {
        while (out->firstMip + 1 < meta.mipLevels) {
            auto* img = out->pixels.GetImage(out->firstMip, 0, 0);
            if (std::max(img->width, img->height) <= maxSize)
                break;
            out->firstMip++;
        }
        auto* image = out->pixels.GetImage(out->firstMip, 0, 0);
        if (std::max(image->width, image->height) > maxSize) {
            DirectX::ScratchImage decoded, resized;
            if (DirectX::IsCompressed(image->format)) {
                hr(DirectX::Decompress(*image, DXGI_FORMAT_R8G8B8A8_UNORM, decoded), "Decompress image");
                image = decoded.GetImage(0, 0, 0);
            }
            double scale = double(maxSize) / std::max(image->width, image->height);
            hr(DirectX::Resize(*image, std::max(size_t(1), size_t(image->width * scale)),
                               std::max(size_t(1), size_t(image->height * scale)),
                               DirectX::TEX_FILTER_DEFAULT, resized),
               "Resize preview image");
            out->pixels = std::move(resized);
            out->firstMip = 0;
        }
    }
    if (maxSize && out->pixels.GetMetadata().mipLevels == 1 &&
        !DirectX::IsCompressed(out->pixels.GetMetadata().format)) {
        DirectX::ScratchImage mips;
        if (SUCCEEDED(DirectX::GenerateMipMaps(*out->pixels.GetImage(0, 0, 0), DirectX::TEX_FILTER_DEFAULT, 0,
                                               mips)))
            out->pixels = std::move(mips);
    }
    return out;
}
std::vector<uint8_t> pngTexture(const ImageSource& source) {
    ComApartment apartment;
    auto texture = loadTexture(source, 0);
    const auto* image = texture->pixels.GetImage(0, 0, 0);
    DirectX::ScratchImage decompressed, rgba;
    if (DirectX::IsCompressed(image->format)) {
        hr(DirectX::Decompress(*image, DXGI_FORMAT_R8G8B8A8_UNORM, decompressed), "Decode DDS for export");
        image = decompressed.GetImage(0, 0, 0);
    }
    if (image->format != DXGI_FORMAT_R8G8B8A8_UNORM && image->format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        hr(DirectX::Convert(*image, DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT, 0, rgba),
           "Convert texture for export");
        image = rgba.GetImage(0, 0, 0);
    }
    DirectX::Blob png;
    hr(DirectX::SaveToWICMemory(*image, DirectX::WIC_FLAGS_NONE, GUID_ContainerFormatPng, png), "Encode PNG");
    auto* p = static_cast<const uint8_t*>(png.GetBufferPointer());
    return {p, p + png.GetBufferSize()};
}
} // namespace edm
