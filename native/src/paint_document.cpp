#include "paint_document.h"
#include <iomanip>
#include <sstream>

namespace edm {
namespace {
std::string luaString(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '\\' || c == '"') {
            out += '\\';
            out += char(c);
        } else if (c < 32) {
            char digits[5];
            std::snprintf(digits, sizeof(digits), "\\%03u", unsigned(c));
            out += digits;
        } else
            out += char(c);
    }
    return out + '"';
}
void cancelled(const std::atomic_bool* cancel) {
    require(!cancel || !cancel->load(), "涂装工程操作已取消");
}
struct Fingerprint {
    uint64_t value = 14695981039346656037ull;
    void bytes(const void* pointer, size_t size) {
        const auto* data = static_cast<const uint8_t*>(pointer);
        for (size_t i = 0; i < size; ++i) {
            value ^= data[i];
            value *= 1099511628211ull;
        }
    }
    void text(const std::string& text) {
        const uint64_t count = text.size();
        bytes(&count, sizeof(count));
        bytes(text.data(), text.size());
    }
    std::string string() const {
        std::ostringstream out;
        out << std::hex << std::setfill('0') << std::setw(16) << value;
        return out.str();
    }
};
Json materialIdentity(const Material& material) {
    Json textures = Json::array();
    for (const auto& ref : material.textures)
        textures.push_back({{"slot", ref.slot}, {"name", ref.name}, {"matrix", matJson(ref.matrix)}});
    return {{"name", material.name},
            {"shader", material.shader},
            {"format", material.format},
            {"uv_channels", material.uvChannels},
            {"textures", textures}};
}
std::string sceneFingerprint(const Scene& scene, const std::atomic_bool* cancel) {
    Fingerprint fingerprint;
    fingerprint.text("EDM Studio Paint identity v2");
    fingerprint.text(std::to_string(scene.version));
    for (const auto& material : scene.materials)
        fingerprint.text(materialIdentity(material).dump());
    for (const auto& mesh : scene.meshes) {
        cancelled(cancel);
        fingerprint.text(mesh.name);
        fingerprint.text(std::to_string(mesh.material));
        fingerprint.text(std::to_string(mesh.positions.size()));
        fingerprint.bytes(mesh.positions.data(), mesh.positions.size() * sizeof(F3));
        fingerprint.text(std::to_string(mesh.indices.size()));
        fingerprint.bytes(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t));
        fingerprint.text(std::to_string(mesh.uvs.size()));
        for (const auto& uv : mesh.uvs) {
            fingerprint.text(std::to_string(uv.size()));
            fingerprint.bytes(uv.data(), uv.size() * sizeof(F2));
        }
    }
    return fingerprint.string();
}
std::string imageFingerprint(const PaintImage& image) {
    Fingerprint result;
    result.bytes(&image.width, sizeof(image.width));
    result.bytes(&image.height, sizeof(image.height));
    result.bytes(image.rgba.data(), image.rgba.size());
    return result.string();
}
bool sameImage(const PaintImage& a, const PaintImage& b) {
    return a.width == b.width && a.height == b.height && a.rgba == b.rgba;
}
bool meaningful(const std::string& name) {
    return name.find_first_not_of(" \t\r\n") != std::string::npos;
}
Material cleanMaterial(const Material& source) {
    Material result = source;
    std::erase_if(result.textures, [](const auto& texture) { return !meaningful(texture.name); });
    return result;
}
const TextureOverride* overrideAt(const std::shared_ptr<Livery>& livery, const std::string& name, int slot) {
    if (!livery)
        return nullptr;
    auto found = livery->textures.find({lower(name), slot});
    return found == livery->textures.end() ? nullptr : &found->second;
}
bool legacyRoughMet(const Material& material, const std::shared_ptr<Livery>& livery) {
    const auto* over = overrideAt(livery, material.name, 2);
    const auto* reference = material.texture(2);
    return lower(over ? over->name : reference ? reference->name : "").find("roughmet") != std::string::npos;
}
Json appearanceSnapshot(const std::shared_ptr<Livery>& livery, const fs::path& textureDirectory) {
    Json appearance = {
        {"version", 1},
        {"livery", nullptr},
        {"texture_directory", textureDirectory.empty() ? "" : pathString(fs::absolute(textureDirectory))}};
    if (!livery)
        return appearance;
    Json bindings = Json::array(), arguments = Json::object();
    for (const auto& [key, binding] : livery->textures)
        bindings.push_back({{"material", binding.material},
                            {"slot", binding.slot},
                            {"name", binding.name},
                            {"common", binding.common}});
    for (const auto& [argument, value] : livery->args)
        if (argument >= 0 && std::isfinite(value))
            arguments[std::to_string(argument)] = value;
    appearance["livery"] = {{"path", livery->path.empty() ? "" : pathString(fs::absolute(livery->path))},
                            {"entry", livery->entry},
                            {"name", livery->name},
                            {"unit", livery->unit},
                            {"origin", livery->origin},
                            {"textures", bindings},
                            {"custom_args", arguments},
                            {"countries", livery->countries},
                            {"evaluation", livery->evaluation},
                            {"warnings", livery->warnings}};
    return appearance;
}
// DDS dependencies retain their exact compression, mipmaps and linear RoughMet values.
// Convert other image types to DDS so DCS does not need PNG support.
void copyDDS(const ImageSource& source, const fs::path& target, const std::atomic_bool* cancel) {
    const auto path = source.entry.empty() ? source.path : fs::path(wide(source.entry));
    if (lower(pathString(path.extension())) == ".dds") {
        auto bytes = source.bytes();
        DirectX::TexMetadata metadata;
        require(SUCCEEDED(DirectX::GetMetadataFromDDSMemory(bytes.data(), bytes.size(),
                                                            DirectX::DDS_FLAGS_NONE, metadata)),
                "源 DDS 文件无效: " + source.key());
        cancelled(cancel);
        writeFile(target, bytes);
        return;
    }
    auto texture = loadTexture(source, 0);
    DirectX::Blob encoded;
    require(
        SUCCEEDED(DirectX::SaveToDDSMemory(texture->pixels.GetImages(), texture->pixels.GetImageCount(),
                                           texture->pixels.GetMetadata(), DirectX::DDS_FLAGS_NONE, encoded)),
        "无法将涂装依赖转为 DDS: " + source.key());
    cancelled(cancel);
    writeFile(target, {static_cast<const uint8_t*>(encoded.GetBufferPointer()), encoded.GetBufferSize()});
}
void textFile(const fs::path& path, const std::string& text) {
    writeFile(path, {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
}
struct Staging {
    fs::path directory, final;
    bool published = false, owned = false;
    ~Staging() {
        if (published || !owned || directory.empty())
            return;
        std::error_code ignored;
        fs::remove(directory / "description.lua", ignored);
        fs::remove(directory / "project.edmpaint.json", ignored);
    }
};
Json save(const Scene& scene, const PaintSnapshot& images, const fs::path& directory, const std::string& name,
          std::shared_ptr<Livery> livery, const fs::path& textureDirectory, Progress progress,
          const std::atomic_bool* cancel, bool recovery) {
    require(!images.empty(), "尚无可保存的绘制材质");
    require(!directory.empty(), "请选择保存目录");
    cancelled(cancel);
    std::map<std::string, std::vector<int>> groups;
    for (size_t i = 0; i < scene.materials.size(); ++i)
        groups[recovery ? std::to_string(i) : lower(scene.materials[i].name)].push_back(int(i));
    std::map<std::string, std::shared_ptr<const PaintImage>> groupImages;
    for (const auto& [material, image] : images) {
        require(material >= 0 && material < int(scene.materials.size()) && image, "无效的绘制材质或空图像");
        image->validate();
        require(image->width <= 8192 && image->height <= 8192, "工程画布单边不能超过 8192 像素");
        const auto& materialName = scene.materials[material].name;
        require(recovery || meaningful(materialName), "DCS 无法按空材质名称应用绘制涂装");
        auto [found, inserted] =
            groupImages.try_emplace(recovery ? std::to_string(material) : lower(materialName), image);
        require(inserted || sameImage(*found->second, *image), "同名材质有不同绘制内容，DCS 无法分别寻址：" +
                                                                   materialName +
                                                                   "。请先统一同名材质的绘制图像。");
    }
    size_t expandedBytes = 0;
    constexpr size_t editingBudget = 1024ull * 1024 * 1024;
    for (const auto& [key, image] : groupImages)
        for (size_t copy = 0; copy < groups.at(key).size(); ++copy) {
            require(image->rgba.size() <= editingBudget - expandedBytes,
                    "按材质展开的工程画布超过 1 GiB 编辑预算");
            expandedBytes += image->rgba.size();
        }
    Json manifest = {{"format", "EDM Studio Paint"},
                     {"version", 2},
                     {"source", pathString(scene.source)},
                     {"scene_fingerprint", sceneFingerprint(scene, cancel)},
                     {"name", name},
                     {"textures", Json::array()},
                     {"dependencies", Json::array()},
                     {"warnings", Json::array()},
                     {"self_contained", !recovery},
                     {"recovery_only", recovery}};
    if (recovery)
        manifest["appearance"] = appearanceSnapshot(livery, textureDirectory);
    else
        manifest["original_diffuse"] = Json::array();
    fs::create_directories(directory);
    const auto parent = fs::weakly_canonical(directory);
    require(fs::is_directory(parent), "保存目标不是目录");
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char stamp[96];
    std::snprintf(stamp, sizeof(stamp), "%s-%04u%02u%02u-%02u%02u%02u-%03u-%lu",
                  recovery ? "EDM-Recovery" : "EDM-Paint", now.wYear, now.wMonth, now.wDay, now.wHour,
                  now.wMinute, now.wSecond, now.wMilliseconds, GetCurrentProcessId());
    Staging staging;
    for (size_t suffix = 0;; ++suffix) {
        std::string leaf = stamp + (suffix ? "-" + std::to_string(suffix) : std::string());
        staging.final = parent / wide(leaf);
        staging.directory = parent / wide(leaf + ".partial");
        if (fs::exists(staging.final))
            continue;
        if (fs::create_directory(staging.directory)) {
            staging.owned = true;
            break;
        }
    }
    textFile(staging.directory / "INCOMPLETE.txt",
             "This staging folder is incomplete. It is not a DCS livery or saved paint project.\n");
    std::unique_ptr<TextureResolver> resolver;
    if (!recovery)
        resolver = std::make_unique<TextureResolver>(scene.source, textureDirectory, livery);
    std::set<int> numberMaterials;
    for (const auto& mesh : scene.meshes)
        if (!mesh.selectors.empty() ||
            (mesh.extras.is_object() && mesh.extras.value("edm_type", "") == "NumberNode"))
            numberMaterials.insert(mesh.material);
    std::ostringstream description;
    description
        << "-- Created by EDM Studio. Images and evaluated Lua configuration are baked.\nlivery = {\n";
    std::map<std::string, std::string> copied;
    size_t completed = 0;
    for (const auto& [key, members] : groups) {
        cancelled(cancel);
        if (progress)
            progress("保存涂装材质 " + std::to_string(++completed) + " / " + std::to_string(groups.size()));
        cancelled(cancel);
        const auto& mat = scene.materials[members.front()];
        if (!recovery && !meaningful(mat.name))
            continue;
        auto edited = groupImages.find(key);
        if (edited != groupImages.end()) {
            const std::string stem = "paint_" + std::to_string(members.front());
            edited->second->savePNG(staging.directory / (stem + ".png"));
            cancelled(cancel);
            if (!recovery)
                edited->second->saveDDS(staging.directory / (stem + ".dds"));
            const auto checksum = imageFingerprint(*edited->second);
            for (int material : members)
                manifest["textures"].push_back(
                    {{"material", material},
                     {"material_name", scene.materials[material].name},
                     {"material_identity", materialIdentity(scene.materials[material])},
                     {"file", stem + ".png"},
                     {"dds_file", recovery ? "" : stem + ".dds"},
                     {"width", edited->second->width},
                     {"height", edited->second->height},
                     {"image_fingerprint", checksum}});
            if (members.size() > 1)
                manifest["warnings"].push_back("DCS 按材质名称共享涂装：" + mat.name + " 的绘制同时应用于 " +
                                               std::to_string(members.size()) + " 个同名材质。");
            description << "  {" << luaString(mat.name) << ", 0, " << luaString(stem) << ", false},\n";
        }
        if (recovery)
            continue;
        std::set<int> slots{0, 1, 2, 13};
        if (overrideAt(livery, mat.name, 3) ||
            std::any_of(members.begin(), members.end(), [&](int m) { return numberMaterials.contains(m); }))
            slots.insert(3);
        // Explicit livery indices already use DCS roles; arbitrary raw EDM texture indices do not.
        if (livery)
            for (const auto& [binding, over] : livery->textures)
                if (binding.first == key && meaningful(over.name))
                    slots.insert(binding.second);
        for (int slot : slots) {
            cancelled(cancel);
            const bool originalDiffuse = slot == 0 && edited != groupImages.end();
            if (slot == 2 && legacyRoughMet(mat, livery))
                continue;
            const auto* explicitOverride = overrideAt(livery, mat.name, slot);
            if (explicitOverride && !meaningful(explicitOverride->name) && !originalDiffuse)
                continue;
            std::optional<ImageSource> source;
            bool first = true, ambiguous = false;
            for (int material : members) {
                auto cleaned = cleanMaterial(scene.materials[material]);
                if (slot != 13 && !explicitOverride && !cleaned.texture(slot)) {
                    if (!first && source)
                        ambiguous = true;
                    first = false;
                    continue;
                }
                auto found = !originalDiffuse && explicitOverride && explicitOverride->common &&
                                     lower(explicitOverride->name) == "empty"
                                 ? std::optional<ImageSource>{ImageSource{{}, "", true}}
                                 : resolver->material(cleaned, slot);
                if (!first &&
                    (bool(source) != bool(found) || (source && found && source->key() != found->key())))
                    ambiguous = true;
                if (first)
                    source = std::move(found);
                first = false;
            }
            Json original;
            if (originalDiffuse)
                original = {{"material", members.front()}, {"material_name", mat.name}};
            if (ambiguous) {
                manifest["warnings"].push_back("同名材质 " + mat.name + " 的默认槽 " + std::to_string(slot) +
                                               " 不同；保留各自默认纹理，避免错误覆盖。");
                manifest["self_contained"] = false;
                if (originalDiffuse) {
                    original["state"] = "default";
                    manifest["original_diffuse"].push_back(std::move(original));
                }
                continue;
            }
            if (!source) {
                if (originalDiffuse) {
                    // A blank local override deliberately stays unresolved. Dropping it would
                    // expose the model's default diffuse and change the unedited appearance.
                    original["state"] = explicitOverride || mat.texture(0) ? "missing" : "default";
                    manifest["original_diffuse"].push_back(std::move(original));
                }
                continue;
            }
            std::string textureName = "empty";
            if (!source->empty) {
                auto known = copied.find(source->key());
                if (known != copied.end())
                    textureName = known->second;
                else {
                    textureName = "base_" + std::to_string(members.front()) + "_" + std::to_string(slot);
                    copyDDS(*source, staging.directory / (textureName + ".dds"), cancel);
                    copied[source->key()] = textureName;
                    manifest["dependencies"].push_back(
                        {{"source", source->key()}, {"file", textureName + ".dds"}});
                }
            }
            if (originalDiffuse) {
                original["state"] = source->empty ? "empty" : "file";
                if (!source->empty)
                    original["file"] = textureName + ".dds";
                manifest["original_diffuse"].push_back(std::move(original));
                continue; // DCS still receives the edited paint_N.dds binding written above.
            }
            description << "  {" << luaString(mat.name) << ", " << slot << ", " << luaString(textureName)
                        << ", " << (source->empty ? "true" : "false") << "},\n";
        }
    }
    description << "}\nname = " << luaString(name.empty() ? "EDM Studio painted livery" : name) << "\n";
    if (livery && !livery->args.empty()) {
        description << "custom_args = {\n" << std::setprecision(17);
        for (const auto [argument, value] : livery->args)
            if (std::isfinite(value))
                description << "  [" << argument << "] = " << value << ",\n";
        description << "}\n";
    }
    if (livery && livery->countries.is_array() && !livery->countries.empty()) {
        description << "countries = {";
        for (const auto& country : livery->countries)
            if (country.is_string())
                description << luaString(country.get<std::string>()) << ",";
        description << "}\n";
    }
    manifest["missing_textures"] = Json::array();
    if (resolver) {
        for (const auto& warning : resolver->warnings)
            manifest["warnings"].push_back(warning);
        auto& missing = resolver->missing;
        std::sort(missing.begin(), missing.end());
        missing.erase(std::unique(missing.begin(), missing.end()), missing.end());
        manifest["missing_textures"] = missing;
        if (!missing.empty())
            manifest["self_contained"] = false;
    }
    manifest["directory"] = pathString(staging.final);
    if (progress)
        progress("完成工程与 DCS 涂装配置");
    cancelled(cancel);
    writeJson(staging.directory / "project.edmpaint.json", manifest);
    if (!recovery)
        textFile(staging.directory / "description.lua", description.str());
    if (progress)
        progress("发布涂装目录");
    cancelled(cancel);
    fs::remove(staging.directory / "INCOMPLETE.txt");
    require(MoveFileExW(staging.directory.c_str(), staging.final.c_str(), MOVEFILE_WRITE_THROUGH) != 0,
            "无法发布涂装目录，暂存内容保持未完成状态");
    staging.published = true;
    return manifest;
}
} // namespace

Json savePaintProject(const Scene& scene, const PaintSnapshot& images, const fs::path& directory,
                      const std::string& name, std::shared_ptr<Livery> livery,
                      const fs::path& textureDirectory, Progress progress, const std::atomic_bool* cancel) {
    return save(scene, images, directory, name, std::move(livery), textureDirectory, std::move(progress),
                cancel, false);
}
Json savePaintRecovery(const Scene& scene, const PaintSnapshot& images, const fs::path& directory,
                       Progress progress, const std::atomic_bool* cancel, std::shared_ptr<Livery> livery,
                       const fs::path& textureDirectory) {
    return save(scene, images, directory, "未保存绘制恢复", std::move(livery), textureDirectory,
                std::move(progress), cancel, true);
}
namespace {
PaintSnapshot loadImages(const Scene& scene, const fs::path& project, const Json& manifest,
                         const std::atomic_bool* cancel, const PaintLoadOptions& options) {
    cancelled(cancel);
    require(options.maxExpandedBytes > 0 && options.maxExpandedBytes <= 1024ull * 1024 * 1024,
            "工程加载预算必须在 1 字节至 1 GiB 之间");
    require(manifest.value("format", "") == "EDM Studio Paint" && manifest.value("version", 0) == 2,
            "不是受支持的 EDM 绘制工程（需要版本 2 的完整材质身份）");
    require(manifest.at("scene_fingerprint").get<std::string>() == sceneFingerprint(scene, cancel),
            "绘制工程的模型 / UV / 材质身份与当前模型不匹配");
    require(manifest.at("textures").is_array() && !manifest["textures"].empty() &&
                manifest["textures"].size() <= scene.materials.size(),
            "绘制工程材质数量不匹配");
    const auto root = fs::weakly_canonical(project.parent_path());
    PaintSnapshot out;
    size_t totalBytes = 0;
    std::map<fs::path, std::pair<std::shared_ptr<const PaintImage>, std::string>> loaded;
    for (const auto& entry : manifest["textures"]) {
        cancelled(cancel);
        const int index = entry.at("material").get<int>();
        require(index >= 0 && index < int(scene.materials.size()) && !out.contains(index),
                "无效或重复的绘制材质索引");
        require(entry.at("material_name") == scene.materials[index].name &&
                    entry.at("material_identity") == materialIdentity(scene.materials[index]),
                "绘制工程与当前模型材质不匹配");
        const std::string file = entry.at("file").get<std::string>();
        const fs::path relative = wide(file);
        require(!relative.empty() && !relative.is_absolute() && !relative.has_root_name() &&
                    !relative.has_root_directory() && file.find(':') == std::string::npos &&
                    file.find('\0') == std::string::npos && lower(pathString(relative.extension())) == ".png",
                "工程图片必须为工程内的 PNG 相对路径");
        const auto path = fs::weakly_canonical(root / relative);
        require(within(path, root) && fs::is_regular_file(path), "工程图片路径超出工程目录或文件不存在");
        std::shared_ptr<const PaintImage> image;
        std::string checksum;
        if (auto existing = loaded.find(path); existing != loaded.end()) {
            image = existing->second.first;
            checksum = existing->second.second;
        } else {
            auto bytes = readFile(path, 256ull * 1024 * 1024);
            constexpr std::array<uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
            require(bytes.size() >= signature.size() &&
                        std::equal(signature.begin(), signature.end(), bytes.begin()),
                    "工程图片不是 PNG 文件");
            DirectX::TexMetadata metadata;
            ComApartment apartment;
            require(SUCCEEDED(DirectX::GetMetadataFromWICMemory(bytes.data(), bytes.size(),
                                                                DirectX::WIC_FLAGS_NONE, metadata)),
                    "工程 PNG 元数据损坏");
            require(metadata.width && metadata.height && metadata.width <= 8192 && metadata.height <= 8192,
                    "工程画布单边不能超过 8192 像素");
            const uint64_t expectedBytes = uint64_t(metadata.width) * metadata.height * 4;
            require(expectedBytes <= options.maxExpandedBytes - totalBytes,
                    "按材质展开的工程画布超过编辑预算");
            cancelled(cancel);
            image = std::make_shared<PaintImage>(PaintImage::load(ImageSource{path}, 0));
            checksum = imageFingerprint(*image);
            loaded[path] = {image, checksum};
        }
        // A deduplicated PNG still becomes a separate mutable canvas for every material in the UI.
        require(image->rgba.size() <= options.maxExpandedBytes - totalBytes,
                "按材质展开的工程画布超过编辑预算");
        totalBytes += image->rgba.size();
        require(entry.at("width") == image->width && entry.at("height") == image->height &&
                    entry.at("image_fingerprint").get<std::string>() == checksum,
                "工程图片尺寸或像素校验不匹配，文件可能已更改或损坏");
        out[index] = std::move(image);
    }
    cancelled(cancel);
    return out;
}
void appendWarnings(PaintProjectDocument& document, const Json& warnings) {
    if (warnings.is_array())
        for (const auto& warning : warnings)
            if (warning.is_string())
                document.warnings.push_back(warning.get<std::string>());
}
void restoreOriginalDiffuse(PaintProjectDocument& document, const Scene& scene, const Json& manifest,
                            const fs::path& directory, const std::atomic_bool* cancel) {
    if (!manifest.contains("original_diffuse")) {
        document.warnings.push_back("旧版工程未保存编辑前的漫反射贴图；取消“浏览已编辑的涂装”只能显示该工程已"
                                    "保存的成品，无法还原此前绘制。");
        return;
    }
    const auto& originals = manifest["original_diffuse"];
    require(originals.is_array() && originals.size() <= document.images.size(), "工程原图绑定数量无效");
    std::set<std::string> expected, restored;
    for (const auto& [material, image] : document.images)
        expected.insert(lower(scene.materials[material].name));
    for (const auto& original : originals) {
        cancelled(cancel);
        const int material = original.at("material").get<int>();
        require(document.images.contains(material) && original.at("material_name").is_string() &&
                    original["material_name"] == scene.materials[material].name,
                "工程原图绑定与已绘制材质不匹配");
        const auto& name = scene.materials[material].name;
        const auto key = std::make_pair(lower(name), 0);
        require(restored.insert(key.first).second, "工程原图材质绑定重复");
        const auto state = original.at("state").get<std::string>();
        if (state == "default") {
            document.livery->textures.erase(key);
            continue;
        }
        TextureOverride binding{name, "", 0, false};
        if (state == "empty") {
            binding.name = "empty";
            binding.common = true;
        } else if (state == "file") {
            const auto file = original.at("file").get<std::string>();
            const fs::path relative = wide(file);
            require(!relative.empty() && !relative.is_absolute() && !relative.has_root_name() &&
                        !relative.has_root_directory() && file.find(':') == std::string::npos &&
                        file.find('\0') == std::string::npos &&
                        lower(pathString(relative.extension())) == ".dds",
                    "工程原图必须为工程内的 DDS 相对路径");
            const auto path = fs::weakly_canonical(directory / relative);
            require(within(path, directory), "工程原图路径超出工程目录");
            if (fs::is_regular_file(path))
                binding.name = file;
            else
                document.warnings.push_back("工程原始漫反射贴图缺失：" + file +
                                            "；原图浏览保留缺图状态，绘制内容仍可使用。");
        } else
            require(state == "missing", "工程原图状态无效");
        // Keep an explicit blank local binding for missing originals. Removing the binding
        // would silently substitute a game/default texture (or the project's edited DDS).
        document.livery->textures[key] = std::move(binding);
    }
    require(restored == expected, "工程原图绑定未覆盖全部已绘制材质");
}
void restoreSnapshot(PaintProjectDocument& document, const Json& appearance) {
    require(appearance.is_object() && appearance.value("version", 0) == 1 && appearance.contains("livery"),
            "恢复工程的外观快照无效");
    document.restoreAppearance = true;
    document.textureDirectory = wide(appearance.at("texture_directory").get<std::string>());
    if (!document.textureDirectory.empty() && !fs::is_directory(document.textureDirectory))
        document.warnings.push_back("原额外贴图目录不存在：" + pathString(document.textureDirectory));
    const auto& saved = appearance.at("livery");
    if (saved.is_null())
        return; // Explicit default appearance, distinct from an old absent snapshot.
    require(saved.is_object(), "恢复工程涂装快照无效");
    auto livery = std::make_shared<Livery>();
    livery->path = wide(saved.at("path").get<std::string>());
    livery->entry = saved.at("entry").get<std::string>();
    livery->name = saved.at("name").get<std::string>();
    livery->unit = saved.at("unit").get<std::string>();
    livery->origin = saved.at("origin").get<std::string>();
    require(saved.at("textures").is_array() && saved["textures"].size() <= 100000, "恢复工程贴图绑定无效");
    for (const auto& row : saved["textures"]) {
        TextureOverride binding;
        binding.material = row.at("material").get<std::string>();
        binding.slot = row.at("slot").get<int>();
        binding.name = row.at("name").get<std::string>();
        binding.common = row.at("common").get<bool>();
        require(binding.slot >= 0 && binding.slot <= 100000, "恢复工程贴图槽位无效");
        require(
            livery->textures.emplace(std::make_pair(lower(binding.material), binding.slot), binding).second,
            "恢复工程贴图绑定重复");
    }
    require(saved.at("custom_args").is_object(), "恢复工程参数快照无效");
    for (auto it = saved["custom_args"].begin(); it != saved["custom_args"].end(); ++it) {
        size_t used = 0;
        int argument = std::stoi(it.key(), &used);
        require(used == it.key().size() && argument >= 0 && it.value().is_number(), "恢复工程参数编号无效");
        const double value = it.value().get<double>();
        require(std::isfinite(value), "恢复工程参数必须为有限数字");
        livery->args[argument] = value;
    }
    require(saved.at("countries").is_array(), "恢复工程国家列表无效");
    for (const auto& country : saved["countries"])
        require(country.is_string(), "恢复工程国家名称无效");
    livery->countries = saved["countries"];
    livery->evaluation = saved.value("evaluation", Json::object());
    if (saved.contains("warnings") && saved["warnings"].is_array())
        for (const auto& warning : saved["warnings"])
            if (warning.is_string())
                livery->warnings.push_back(warning.get<std::string>());
    if (livery->path.empty() || !fs::is_regular_file(livery->path))
        document.warnings.push_back("原涂装路径不存在，已恢复求值结果，但外部贴图可能不可用：" +
                                    pathString(livery->path));
    appendWarnings(document, livery->warnings);
    document.livery = std::move(livery);
}
} // namespace

PaintSnapshot loadPaintProject(const Scene& scene, const fs::path& project, const std::atomic_bool* cancel,
                               const PaintLoadOptions& options) {
    cancelled(cancel);
    return loadImages(scene, project, Json::parse(readFile(project, 2000000)), cancel, options);
}
PaintProjectDocument loadPaintDocument(const Scene& scene, const fs::path& project,
                                       const std::atomic_bool* cancel, const PaintLoadOptions& options) {
    cancelled(cancel);
    const auto manifest = Json::parse(readFile(project, 2000000));
    PaintProjectDocument document;
    document.images = loadImages(scene, project, manifest, cancel, options);
    appendWarnings(document, manifest.value("warnings", Json::array()));
    if (manifest.value("recovery_only", false)) {
        if (manifest.contains("appearance"))
            restoreSnapshot(document, manifest["appearance"]);
    } else {
        const auto directory = fs::weakly_canonical(project.parent_path());
        const auto description = fs::weakly_canonical(directory / "description.lua");
        require(within(description, directory), "工程 description.lua 路径超出工程目录");
        if (fs::is_regular_file(description)) {
            cancelled(cancel);
            auto livery = std::make_shared<Livery>(readLivery(description, "", "绘制工程"));
            for (const auto& [key, binding] : livery->textures)
                if (!binding.common) {
                    const auto relative = fs::path(wide(binding.name));
                    require(!relative.is_absolute() && !relative.has_root_name() &&
                                !relative.has_root_directory() &&
                                binding.name.find(':') == std::string::npos &&
                                binding.name.find('\0') == std::string::npos &&
                                within(fs::weakly_canonical(directory / relative), directory),
                            "工程涂装贴图引用超出工程目录");
                }
            document.livery = std::move(livery);
            document.textureDirectory = directory;
            document.restoreAppearance = true;
            appendWarnings(document, document.livery->warnings);
            for (const auto& dependency : manifest.value("dependencies", Json::array())) {
                const auto path =
                    fs::weakly_canonical(directory / wide(dependency.at("file").get<std::string>()));
                require(within(path, directory), "工程依赖贴图路径超出工程目录");
                if (!fs::is_regular_file(path))
                    document.warnings.push_back("工程依赖贴图缺失：" + pathString(path.filename()));
            }
            restoreOriginalDiffuse(document, scene, manifest, directory, cancel);
        } else
            document.warnings.push_back("工程 description.lua 缺失，已恢复绘制图像并保留当前外观。");
    }
    cancelled(cancel);
    return document;
}
} // namespace edm
