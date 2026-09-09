#include "export.h"
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <wincodec.h>

namespace edm {
namespace {
void cancelled(const std::atomic_bool* cancel) {
    if (cancel && cancel->load())
        throw std::runtime_error("Cancelled");
}
std::string oneLine(std::string value) {
    for (char& c : value)
        if (static_cast<unsigned char>(c) < 32 || c == 127)
            c = ' ';
    return value;
}
std::string uniqueName() {
    GUID id;
    require(SUCCEEDED(CoCreateGuid(&id)), "Cannot allocate OBJ export identifier");
    std::ostringstream text;
    text << "edm_obj_" << std::hex << std::setfill('0');
    for (const auto byte : std::span(reinterpret_cast<const unsigned char*>(&id), sizeof(id)))
        text << std::setw(2) << unsigned(byte);
    return text.str();
}
// Each transaction owns a newly-created directory and a newly-created MTL. Existing
// exports are never removed: replacing the OBJ is the final, atomic publication step.
struct Transaction {
    fs::path directory, material;
    std::vector<fs::path> files;
    bool published = false, ownsMaterial = false;
    explicit Transaction(const fs::path& parent) {
        for (int attempt = 0; attempt < 16; ++attempt) {
            auto name = uniqueName();
            auto candidate = parent / wide(name);
            auto mtl = parent / wide(name + ".mtl");
            if (fs::exists(mtl))
                continue;
            if (fs::create_directory(candidate)) {
                directory = std::move(candidate);
                material = std::move(mtl);
                HANDLE file = CreateFileW(material.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file == INVALID_HANDLE_VALUE) {
                    std::error_code error;
                    fs::remove(directory, error);
                    directory.clear();
                    continue;
                }
                CloseHandle(file);
                ownsMaterial = true;
                return;
            }
        }
        throw std::runtime_error("Cannot create unique OBJ sidecars");
    }
    ~Transaction() {
        if (!published) {
            std::error_code error;
            if (ownsMaterial)
                fs::remove(material, error);
            // Remove only our own files, never unrelated content placed in the folder.
            if (!directory.empty()) {
                for (const auto& file : files)
                    fs::remove(file, error);
                fs::remove(directory, error);
            }
        }
    }
};
void validateScene(const Scene& scene, const std::atomic_bool* cancel) {
    require(scene.staticLocal.size() == scene.nodes.size() && scene.order.size() == scene.nodes.size(),
            "OBJ: invalid scene graph arrays");
    std::vector<bool> seen(scene.nodes.size(), false);
    for (int node : scene.order) {
        require(node >= 0 && size_t(node) < scene.nodes.size() && !seen[node],
                "OBJ: invalid scene graph order");
        int parent = scene.nodes[node].parent;
        require(parent == -1 || (parent >= 0 && size_t(parent) < seen.size() && seen[parent]),
                "OBJ: invalid scene graph parent");
        require(scene.staticLocal[node].allFinite(), "OBJ: nonfinite node transform");
        seen[node] = true;
    }
    for (const auto& track : scene.tracks) {
        require(track.node >= 0 && size_t(track.node) < scene.nodes.size(), "OBJ: invalid animation node");
        require(track.visibility || !track.keys.empty(), "OBJ: empty animation track");
        for (size_t i = 0; i < track.keys.size(); ++i)
            require(track.keys[i].value.allFinite() && std::isfinite(track.keys[i].time) &&
                        (!i || track.keys[i - 1].time < track.keys[i].time),
                    "OBJ: invalid animation key");
    }
    for (const auto& mesh : scene.meshes) {
        cancelled(cancel);
        require(mesh.node >= 0 && size_t(mesh.node) < scene.nodes.size() && mesh.material >= 0 &&
                    size_t(mesh.material) < scene.materials.size(),
                "OBJ: invalid mesh node or material");
        require(mesh.normals.size() == mesh.positions.size() && mesh.indices.size() % 3 == 0,
                "OBJ: invalid vertex or triangle array");
        for (size_t i = 0; i < mesh.positions.size(); ++i) {
            if (!(i % 4096))
                cancelled(cancel);
            for (int axis = 0; axis < 3; ++axis)
                require(std::isfinite(mesh.positions[i][axis]) && std::isfinite(mesh.normals[i][axis]),
                        "OBJ: nonfinite position or normal");
        }
        for (auto index : mesh.indices)
            require(index < mesh.positions.size(), "OBJ: triangle index out of range");
        for (const auto& channel : mesh.uvs) {
            require(channel.size() == mesh.positions.size(), "OBJ: UV count does not match vertices");
            for (const auto& uv : channel)
                require(std::isfinite(uv[0]) && std::isfinite(uv[1]), "OBJ: nonfinite UV");
        }
        if (!mesh.selectors.empty()) {
            require(mesh.selectors.size() == mesh.positions.size(), "OBJ: invalid number selector count");
            for (int selector : mesh.selectors)
                require(selector >= 0 && size_t(selector) < mesh.numbers.size(),
                        "OBJ: number selector out of range");
        }
        for (const auto& control : mesh.numbers)
            require(std::isfinite(control.su) && std::isfinite(control.sv),
                    "OBJ: nonfinite number atlas multiplier");
        if (mesh.skinned()) {
            require(mesh.joints.size() == mesh.positions.size() &&
                        mesh.weights.size() == mesh.positions.size() && !mesh.skinNodes.empty() &&
                        mesh.inverseBind.size() == mesh.skinNodes.size(),
                    "OBJ: invalid skin arrays");
            for (size_t i = 0; i < mesh.skinNodes.size(); ++i)
                require(mesh.skinNodes[i] >= 0 && size_t(mesh.skinNodes[i]) < scene.nodes.size() &&
                            mesh.inverseBind[i].allFinite(),
                        "OBJ: invalid skin joint or inverse bind");
            for (size_t i = 0; i < mesh.positions.size(); ++i) {
                double sum = 0;
                for (size_t k = 0; k < 8; ++k) {
                    double weight = mesh.weights[i][k];
                    require(std::isfinite(weight) && weight >= 0 &&
                                (weight == 0 || mesh.joints[i][k] < mesh.skinNodes.size()),
                            "OBJ: invalid skin weight or joint index");
                    sum += weight;
                }
                require(std::abs(sum - 1) < 1e-4, "OBJ: skin weights are not normalized");
            }
        } else
            require(mesh.weights.empty() && mesh.skinNodes.empty() && mesh.inverseBind.empty(),
                    "OBJ: incomplete skin data");
    }
}
Eigen::Matrix3d normalMatrix(const Mat& matrix) {
    const Eigen::Matrix3d linear = matrix.block<3, 3>(0, 0);
    if (std::abs(linear.determinant()) <= 1e-20)
        return Eigen::Matrix3d::Zero();
    return linear.inverse().transpose();
}
struct PosedMesh {
    std::vector<V3> positions, normals;
    std::vector<std::array<uint32_t, 3>> triangles;
    size_t reversed = 0;
};
PosedMesh poseMesh(const Mesh& mesh, const std::vector<Mat>& world, const std::atomic_bool* cancel) {
    PosedMesh out;
    if (!mesh.skinned() && world[mesh.node].block<3, 3>(0, 0).cwiseAbs().maxCoeff() < 1e-20)
        return out;
    std::vector<Mat> palette;
    if (mesh.skinned())
        for (size_t i = 0; i < mesh.skinNodes.size(); ++i)
            palette.push_back(world[mesh.skinNodes[i]] * mesh.inverseBind[i]);
    else
        palette.push_back(world[mesh.node]);
    std::vector<Eigen::Matrix3d> normals;
    for (const auto& matrix : palette) {
        require(matrix.allFinite(), "OBJ: nonfinite skin transform");
        normals.push_back(normalMatrix(matrix));
    }
    out.positions.resize(mesh.positions.size());
    out.normals.resize(mesh.positions.size());
    for (size_t i = 0; i < mesh.positions.size(); ++i) {
        if (!(i % 4096))
            cancelled(cancel);
        auto p = mesh.positions[i], n = mesh.normals[i];
        V4 point(p[0], p[1], p[2], 1), transformed = V4::Zero();
        V3 normal(n[0], n[1], n[2]), transformedNormal = V3::Zero();
        if (mesh.skinned()) {
            for (size_t k = 0; k < 8; ++k)
                if (mesh.weights[i][k] > 0) {
                    transformed += palette[mesh.joints[i][k]] * point * mesh.weights[i][k];
                    transformedNormal += normals[mesh.joints[i][k]] * normal * mesh.weights[i][k];
                }
        } else {
            transformed = palette[0] * point;
            transformedNormal = normals[0] * normal;
        }
        require(transformed.allFinite() && transformedNormal.allFinite(),
                "OBJ: nonfinite transformed vertex");
        out.positions[i] = transformed.head<3>();
        out.normals[i] = transformedNormal;
        if (out.normals[i].squaredNorm() > 1e-30)
            out.normals[i].normalize();
    }
    const bool mirrored = !mesh.skinned() && palette[0].block<3, 3>(0, 0).determinant() < 0;
    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
        if (!(i % 12288))
            cancelled(cancel);
        uint32_t a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];
        V3 face = (out.positions[b] - out.positions[a]).cross(out.positions[c] - out.positions[a]);
        require(face.allFinite(), "OBJ: nonfinite transformed triangle");
        if (face.squaredNorm() <= 1e-40)
            continue; // Visibility gates also collapse skinned triangles.
        const bool reverse =
            mirrored || (mesh.skinned() && face.dot(out.normals[a] + out.normals[b] + out.normals[c]) < 0);
        if (reverse) {
            std::swap(b, c);
            face = -face;
            ++out.reversed;
        }
        out.triangles.push_back({a, b, c});
        for (auto index : {a, b, c})
            if (out.normals[index].squaredNorm() <= 1e-30)
                out.normals[index] = face.normalized();
    }
    for (auto& normal : out.normals)
        if (normal.squaredNorm() <= 1e-30)
            normal = V3::UnitY();
    return out;
}
struct PngMaps {
    std::vector<uint8_t> alpha;
};
PngMaps validatePng(std::span<const uint8_t> bytes, const std::atomic_bool* cancel) {
    constexpr std::array<uint8_t, 8> magic{137, 'P', 'N', 'G', 13, 10, 26, 10};
    require(bytes.size() >= magic.size() && std::equal(magic.begin(), magic.end(), bytes.begin()),
            "OBJ: invalid PNG texture");
    ComApartment apartment;
    DirectX::TexMetadata metadata;
    require(SUCCEEDED(DirectX::GetMetadataFromWICMemory(bytes.data(), bytes.size(), DirectX::WIC_FLAGS_NONE,
                                                        metadata)) &&
                metadata.width > 0 && metadata.height > 0 && metadata.width <= 32768 &&
                metadata.height <= 32768,
            "OBJ: invalid PNG dimensions");
    DirectX::ScratchImage decoded, converted;
    require(SUCCEEDED(DirectX::LoadFromWICMemory(bytes.data(), bytes.size(), DirectX::WIC_FLAGS_FORCE_RGB,
                                                 nullptr, decoded)),
            "OBJ: cannot decode PNG pixels");
    const auto* image = decoded.GetImage(0, 0, 0);
    require(image != nullptr, "OBJ: missing PNG pixels");
    if (image->format != DXGI_FORMAT_R8G8B8A8_UNORM && image->format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        require(SUCCEEDED(DirectX::Convert(*image, DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT, 0,
                                           converted)),
                "OBJ: cannot convert PNG pixels");
        image = converted.GetImage(0, 0, 0);
    }
    bool alpha = false;
    for (size_t y = 0; y < image->height && !alpha; ++y) {
        if (!(y % 64))
            cancelled(cancel);
        for (size_t x = 0; x < image->width; ++x)
            if (image->pixels[y * image->rowPitch + x * 4 + 3] != 255) {
                alpha = true;
                break;
            }
    }
    PngMaps result;
    if (alpha) {
        DirectX::ScratchImage opacity;
        require(SUCCEEDED(opacity.Initialize2D(DXGI_FORMAT_R8_UNORM, image->width, image->height, 1, 1)),
                "OBJ: cannot allocate opacity map");
        auto* target = opacity.GetImage(0, 0, 0);
        for (size_t y = 0; y < image->height; ++y) {
            if (!(y % 64))
                cancelled(cancel);
            for (size_t x = 0; x < image->width; ++x)
                target->pixels[y * target->rowPitch + x] = image->pixels[y * image->rowPitch + x * 4 + 3];
        }
        DirectX::Blob blob;
        require(SUCCEEDED(DirectX::SaveToWICMemory(*target, DirectX::WIC_FLAGS_NONE, GUID_ContainerFormatPng,
                                                   blob)),
                "OBJ: cannot encode opacity map");
        auto* begin = static_cast<const uint8_t*>(blob.GetBufferPointer());
        result.alpha.assign(begin, begin + blob.GetBufferSize());
    }
    return result;
}
} // namespace

Json exportObjScene(const Scene& scene, const fs::path& path, const ExportOptions& options, Progress progress,
                    const std::atomic_bool* cancel) {
    const auto start = Clock::now();
    cancelled(cancel);
    require(lower(pathString(path.extension())) == ".obj", "OBJ output must have .obj extension");
    validateScene(scene, cancel);
    Args baseline = scene.defaultArgs;
    if (options.livery)
        for (auto [argument, value] : options.livery->args)
            baseline[argument] = value;
    for (auto [arg, value] : options.baseline)
        baseline[arg] = value;
    for (auto [arg, value] : baseline)
        require(std::isfinite(value), "OBJ: nonfinite baseline argument");
    auto world = scene.evaluate(baseline);
    for (const auto& matrix : world)
        require(matrix.allFinite(), "OBJ: nonfinite evaluated transform");
    fs::path output = fs::absolute(path), parent = output.parent_path();
    fs::create_directories(parent);
    Transaction transaction(parent);
    auto staged = transaction.directory / "model.tmp";
    transaction.files.push_back(staged);
    std::ofstream obj(staged, std::ios::binary), mtl(transaction.material, std::ios::binary);
    require(bool(obj) && bool(mtl), "Cannot create OBJ/MTL output");
    obj.imbue(std::locale::classic());
    mtl.imbue(std::locale::classic());
    obj << std::setprecision(17);
    mtl << std::setprecision(9);
    obj << "# EDM Studio Wavefront OBJ\n# Units: metres; +Y up; static current pose\n"
           "# Animation and skinning are baked into vertex positions.\nmtllib "
        << pathString(transaction.material.filename()) << "\n";
    mtl << "# EDM Studio Wavefront MTL\n# Diffuse and opacity only; DCS shader effects are not reproduced.\n";
    std::unique_ptr<TextureResolver> resolver;
    if (options.textures)
        resolver = std::make_unique<TextureResolver>(scene.source, options.textureDirectory, options.livery);
    std::map<std::pair<int, bool>, std::string> materials;
    std::map<std::string, std::pair<std::string, std::string>> textureCache;
    size_t vertices = 0, triangles = 0, meshes = 0, hidden = 0, reversed = 0, numberMeshes = 0;
    size_t diffuseCount = 0, textureCount = 0;
    auto materialFor = [&](int index, bool number) {
        auto key = std::make_pair(index, number);
        if (materials.contains(key))
            return materials.at(key);
        cancelled(cancel);
        const auto& material = scene.materials[index];
        auto name = "material_" + std::to_string(index) + (number ? "_number" : "");
        auto color = materialColor(material);
        for (float component : color)
            require(std::isfinite(component), "OBJ: nonfinite material color");
        std::pair<std::string, std::string> maps;
        if (resolver) {
            auto edited = options.diffuseOverrides.find(index);
            const bool isEdited = !number && edited != options.diffuseOverrides.end();
            auto source =
                isEdited ? std::optional<ImageSource>{} : resolver->material(material, number ? 3 : 0);
            auto imageKey = isEdited ? "painted:" + std::to_string(index) : source ? source->key() : "";
            if (!imageKey.empty()) {
                if (textureCache.contains(imageKey))
                    maps = textureCache.at(imageKey);
                else {
                    std::vector<uint8_t> png;
                    if (isEdited)
                        png = edited->second;
                    else
                        png = pngTexture(*source);
                    cancelled(cancel);
                    auto decoded = validatePng(png, cancel);
                    const auto leaf = "texture_" + std::to_string(textureCount++);
                    const auto relative = pathString(transaction.directory.filename()) + "/" + leaf;
                    auto texturePath = transaction.directory / wide(leaf + ".png");
                    transaction.files.push_back(texturePath);
                    writeFile(texturePath, png);
                    maps.first = relative + ".png";
                    if (!decoded.alpha.empty()) {
                        auto opacityPath = transaction.directory / wide(leaf + "_opacity.png");
                        transaction.files.push_back(opacityPath);
                        writeFile(opacityPath, decoded.alpha);
                        maps.second = relative + "_opacity.png";
                    }
                    textureCache[imageKey] = maps;
                }
            }
        }
        if (!maps.first.empty()) {
            color[0] = color[1] = color[2] = 1;
            ++diffuseCount;
        }
        if (number)
            color[3] = 1;
        mtl << "\n# " << oneLine(material.name) << (number ? " / current registration" : "") << "\nnewmtl "
            << name << "\nKa 0 0 0\nKd " << color[0] << ' ' << color[1] << ' ' << color[2]
            << "\nKs 0 0 0\nNs 0\nd " << color[3] << "\nillum 2\n";
        if (!maps.first.empty())
            mtl << "map_Kd " << maps.first << '\n';
        if (!maps.second.empty() && (number || materialAlphaMode(material) != MaterialAlphaMode::Opaque))
            mtl << "map_d " << maps.second << '\n';
        materials[key] = name;
        return name;
    };
    for (size_t mi = 0; mi < scene.meshes.size(); ++mi) {
        cancelled(cancel);
        const auto& mesh = scene.meshes[mi];
        auto posed = poseMesh(mesh, world, cancel);
        if (posed.triangles.empty()) {
            ++hidden;
            continue;
        }
        bool number = !mesh.selectors.empty();
        auto uv = textureUV(mesh, scene.materials[mesh.material], number ? 3 : 0, baseline);
        require(uv.size() == posed.positions.size(), "OBJ: mapped UV count does not match vertices");
        for (const auto& value : uv)
            require(std::isfinite(value[0]) && std::isfinite(value[1]), "OBJ: nonfinite mapped UV");
        auto material = materialFor(mesh.material, number);
        obj << "\n# " << oneLine(mesh.name) << "\no mesh_" << mi << "\nusemtl " << material << "\ns 1\n";
        for (size_t i = 0; i < posed.positions.size(); ++i) {
            if (!(i % 4096))
                cancelled(cancel);
            V3 point = posed.positions[i];
            if (number)
                point += posed.normals[i] * .0002;
            obj << "v " << point[0] << ' ' << point[1] << ' ' << point[2] << '\n';
        }
        // DCS/glTF textures have top-left V=0; OBJ convention uses bottom-left V=0.
        for (size_t i = 0; i < uv.size(); ++i) {
            if (!(i % 4096))
                cancelled(cancel);
            obj << "vt " << uv[i][0] << ' ' << 1.0 - uv[i][1] << '\n';
        }
        for (size_t i = 0; i < posed.normals.size(); ++i) {
            if (!(i % 4096))
                cancelled(cancel);
            const auto& normal = posed.normals[i];
            obj << "vn " << normal[0] << ' ' << normal[1] << ' ' << normal[2] << '\n';
        }
        for (size_t i = 0; i < posed.triangles.size(); ++i) {
            if (!(i % 4096))
                cancelled(cancel);
            const auto& face = posed.triangles[i];
            obj << 'f';
            for (uint32_t index : face) {
                size_t global = vertices + index + 1;
                obj << ' ' << global << '/' << global << '/' << global;
            }
            obj << '\n';
        }
        require(bool(obj) && bool(mtl), "Cannot write OBJ/MTL output");
        vertices += posed.positions.size();
        triangles += posed.triangles.size();
        reversed += posed.reversed;
        numberMeshes += number;
        ++meshes;
        if (progress)
            progress("导出 OBJ 网格 " + std::to_string(mi + 1) + " / " + std::to_string(scene.meshes.size()));
    }
    require(meshes > 0, "OBJ: 当前姿态没有可见的三角网格");
    obj.close();
    mtl.close();
    require(!obj.fail() && !mtl.fail(), "Cannot finish OBJ/MTL output");
    Json report = scene.summary(), args = Json::object();
    for (auto [arg, value] : baseline)
        args[std::to_string(arg)] = value;
    auto warnings = scene.warnings;
    warnings.push_back(
        "OBJ 不支持动画或骨骼；仅导出当前参数对应的静态姿态。需要动画请使用 FBX 或 GLB/glTF。");
    warnings.push_back("OBJ/MTL 仅保留漫反射与透明度；RoughMet、双面设置和 DCS 专用着色器无法完整映射。");
    auto reportPath = transaction.directory / "report.json";
    transaction.files.push_back(reportPath);
    report.update({{"format", "obj"},
                   {"output", pathString(path)},
                   {"report_path", pathString(reportPath)},
                   {"material_file", pathString(transaction.material.filename())},
                   {"texture_directory", pathString(transaction.directory.filename())},
                   {"exported_meshes", meshes},
                   {"exported_vertices", vertices},
                   {"exported_triangles", triangles},
                   {"skipped_hidden_or_degenerate_meshes", hidden},
                   {"reversed_triangles", reversed},
                   {"number_meshes", numberMeshes},
                   {"exported_materials", materials.size()},
                   {"embedded_diffuse_materials", diffuseCount},
                   {"exported_textures", textureCount},
                   {"exported_arguments", Json::array()},
                   {"baseline_arguments", args},
                   {"animation_encoding", "static current pose only"},
                   {"units", "metres"},
                   {"up_axis", "Y"},
                   {"warnings", warnings},
                   {"missing_textures", resolver ? Json(resolver->missing) : Json::array()},
                   {"texture_warnings", resolver ? Json(resolver->warnings) : Json::array()},
                   {"resolved_textures", resolver ? resolver->resolved : Json::object()},
                   {"livery", options.livery ? options.livery->metadata() : Json()},
                   {"export_seconds", seconds(start)}});
    writeJson(reportPath, report);
    cancelled(cancel);
    require(MoveFileExW(staged.c_str(), output.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
            "Cannot publish OBJ output; existing export was preserved");
    transaction.published = true;
    return report;
}
} // namespace edm
