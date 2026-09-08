#include "export.h"
#include <iostream>
#include <limits>
#include <sstream>
#include <wincodec.h>

// Kept explicit so this independently-built test can also be linked during integration.
namespace edm {
Json exportObjScene(const Scene&, const fs::path&, const ExportOptions&, Progress, const std::atomic_bool*);
}
using namespace edm;
namespace {
int checks = 0;
void check(bool condition, const std::string& message) {
    ++checks;
    require(condition, message);
}
template <class F> void rejects(F action, const std::string& message) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, message);
}
void closeTo(double actual, double expected, const std::string& message, double tolerance = 1e-6) {
    check(std::abs(actual - expected) <= tolerance, message);
}
std::string text(const fs::path& path) {
    auto bytes = readFile(path);
    return {bytes.begin(), bytes.end()};
}
void saveText(const fs::path& path, const std::string& value) {
    writeFile(path, {reinterpret_cast<const uint8_t*>(value.data()), value.size()});
}
void graph(Scene& scene) {
    scene.staticLocal.clear();
    scene.order.clear();
    for (size_t i = 0; i < scene.nodes.size(); ++i) {
        scene.staticLocal.push_back(scene.nodes[i].local());
        scene.order.push_back(int(i));
    }
    scene.defaultWorld = scene.evaluate({});
}
Scene fixture(const fs::path& root) {
    Scene scene;
    scene.source = root / "fixture" / "Shapes" / "sample.edm";
    scene.nodes.resize(1);
    scene.nodes[0].name = "Plane root";
    scene.materials.resize(1);
    scene.materials[0].name = "机翼 材质\nnewmtl forged";
    scene.materials[0].uniforms = {{"diffuseColor", {.25, .5, .75}}, {"opacityValue", .6}};
    Mesh mesh;
    mesh.name = "左机翼\nf 999 999 999";
    mesh.positions = {{{0, 0, 0}}, {{1, 0, 0}}, {{0, 1, 0}}};
    mesh.normals = {{{0, 0, 1}}, {{0, 0, 1}}, {{0, 0, 1}}};
    mesh.uvs = {{{{0, 0}}, {{1, 0}}, {{0, 1}}}};
    mesh.indices = {0, 1, 2};
    mesh.extras = Json::object();
    scene.meshes.push_back(mesh);
    graph(scene);
    return scene;
}
struct Obj {
    std::vector<V3> vertices, normals;
    std::vector<F2> uv;
    std::vector<std::array<size_t, 3>> faces;
    std::vector<std::string> materialNames;
    fs::path mtl;
    explicit Obj(const fs::path& path) {
        std::istringstream input(text(path));
        std::string line;
        while (std::getline(input, line)) {
            std::istringstream fields(line);
            fields.imbue(std::locale::classic());
            std::string kind;
            fields >> kind;
            if (kind == "v" || kind == "vn") {
                V3 point;
                check(bool(fields >> point[0] >> point[1] >> point[2]), "OBJ vector contains 3 numbers");
                check(point.allFinite(), "OBJ vectors are finite");
                (kind == "v" ? vertices : normals).push_back(point);
            } else if (kind == "vt") {
                F2 value;
                check(bool(fields >> value[0] >> value[1]), "OBJ UV contains 2 numbers");
                uv.push_back(value);
            } else if (kind == "f") {
                std::array<size_t, 3> face;
                for (auto& index : face) {
                    std::string vertex;
                    check(bool(fields >> vertex), "OBJ triangle contains 3 corners");
                    std::replace(vertex.begin(), vertex.end(), '/', ' ');
                    std::istringstream corner(vertex);
                    size_t vt, vn;
                    check(bool(corner >> index >> vt >> vn), "OBJ corner references position/UV/normal");
                    check(index > 0 && index <= vertices.size() && vt > 0 && vt <= uv.size() && vn > 0 &&
                              vn <= normals.size(),
                          "All OBJ indices reference already-written valid elements");
                    check(index == vt && vt == vn, "OBJ independent streams retain matching corners");
                    --index;
                }
                faces.push_back(face);
            } else if (kind == "mtllib") {
                std::string name;
                fields >> name;
                check(name.find_first_of(" \t\\") == std::string::npos, "Portable MTL relative filename");
                mtl = path.parent_path() / wide(name);
            } else if (kind == "usemtl") {
                std::string name;
                fields >> name;
                materialNames.push_back(name);
            }
        }
        check(fs::is_regular_file(mtl), "Referenced MTL exists");
    }
    void winding() const {
        for (auto [a, b, c] : faces) {
            V3 face = (vertices[b] - vertices[a]).cross(vertices[c] - vertices[a]);
            check(face.dot(normals[a] + normals[b] + normals[c]) > 0,
                  "Exported face winding agrees with exported world-space normals");
        }
    }
};
Json output(const Scene& scene, const fs::path& path, const ExportOptions& options = {},
            Progress progress = {}, const std::atomic_bool* cancel = nullptr) {
    return exportObjScene(scene, path, options, std::move(progress), cancel);
}
std::map<std::string, std::vector<uint8_t>> snapshot(const fs::path& root) {
    std::map<std::string, std::vector<uint8_t>> files;
    for (const auto& item : fs::recursive_directory_iterator(root))
        if (item.is_regular_file())
            files[pathString(fs::relative(item.path(), root))] = readFile(item.path());
    return files;
}
std::vector<uint8_t> png(std::array<uint8_t, 4> color) {
    DirectX::ScratchImage image;
    require(SUCCEEDED(image.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, 2, 2, 1, 1)), "Create PNG fixture");
    for (size_t i = 0; i < 4; ++i)
        std::copy(color.begin(), color.end(), image.GetPixels() + i * 4);
    DirectX::Blob blob;
    require(SUCCEEDED(DirectX::SaveToWICMemory(*image.GetImage(0, 0, 0), DirectX::WIC_FLAGS_NONE,
                                               GUID_ContainerFormatPng, blob)),
            "Encode PNG fixture");
    auto* bytes = static_cast<const uint8_t*>(blob.GetBufferPointer());
    return {bytes, bytes + blob.GetBufferSize()};
}
std::map<std::string, fs::path> maps(const Obj& obj) {
    std::istringstream lines(text(obj.mtl));
    std::string line;
    std::map<std::string, fs::path> result;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string kind, path;
        fields >> kind >> path;
        if (kind == "map_Kd" || kind == "map_d") {
            auto relative = fs::path(wide(path));
            check(relative.is_relative() && path.find("..") == std::string::npos,
                  "MTL texture path is a portable relative path");
            auto resolved = obj.mtl.parent_path() / relative;
            check(fs::is_regular_file(resolved), "MTL texture reference resolves");
            result[kind] = resolved;
        }
    }
    return result;
}
void basicAndPose(const fs::path& root) {
    auto scene = fixture(root);
    ExportOptions options;
    options.textures = false;
    Track track;
    track.node = 0;
    track.arg = 7;
    track.keys = {{0, V4(2, 3, 4, 0)}, {1, V4(10, 3, 4, 0)}};
    scene.tracks.push_back(track);
    scene.limits[7] = {0, 1};
    options.livery = std::make_shared<Livery>();
    options.livery->args[7] = .25;
    auto path = root / L"飞机 模型.OBJ";
    auto report = output(scene, path, options);
    Obj obj(path);
    check(obj.vertices.size() == 3 && obj.faces.size() == 1, "Static OBJ triangle exported");
    closeTo(obj.vertices[0].x(), 4, "Livery argument selects pose");
    closeTo(obj.vertices[0].y(), 3, "World-space translation retains metres");
    closeTo(obj.uv[0][1], 1, "Top-left UV becomes OBJ bottom-left convention");
    closeTo(obj.uv[2][1], 0, "UV V flip preserves texture orientation");
    check(text(obj.mtl).find("Kd 0.25 0.5 0.75") != std::string::npos, "MTL diffuse color preserved");
    auto opacityStart = text(obj.mtl).find("\nd ");
    check(opacityStart != std::string::npos, "MTL includes scalar opacity");
    closeTo(std::stod(text(obj.mtl).substr(opacityStart + 3)), .6, "MTL preserves scalar material opacity");
    check(text(obj.mtl).find("\nnewmtl forged") == std::string::npos, "Material names cannot inject MTL");
    check(report["exported_arguments"].empty(), "OBJ report never claims animation export");
    check(report["warnings"].dump().find("OBJ") != std::string::npos, "OBJ animation limitation is reported");
    check(fs::is_regular_file(fs::path(wide(report["report_path"].get<std::string>()))),
          "Unique report file is written");
    obj.winding();
    options.baseline[7] = .75;
    output(scene, path, options);
    Obj overridePose(path);
    closeTo(overridePose.vertices[0].x(), 8, "User baseline overrides livery argument");
    check(fs::is_regular_file(obj.mtl), "Successful overwrite preserves old sidecars for external users");
}
void normalsAndSkin(const fs::path& root) {
    auto scene = fixture(root);
    scene.meshes[0].positions[2][2] = 1;
    for (auto& normal : scene.meshes[0].normals)
        normal = {0, -float(std::sqrt(.5)), float(std::sqrt(.5))};
    scene.nodes[0].s = V3(-2, 3, .5);
    graph(scene);
    ExportOptions options;
    options.textures = false;
    auto path = root / "mirrored.obj";
    auto report = output(scene, path, options);
    Obj reflected(path);
    V3 expected(0, -1. / 3, 2);
    expected.normalize();
    check((reflected.normals[0] - expected).norm() < 1e-6,
          "Nonuniform scale uses inverse-transpose normal matrix");
    check(report["reversed_triangles"] == 1, "Mirrored rigid transform reverses winding");
    reflected.winding();

    auto skin = fixture(root);
    skin.nodes.resize(3);
    skin.nodes[1].t = V3(2, 0, 0);
    skin.nodes[2].t = V3(6, 0, 0);
    auto& mesh = skin.meshes[0];
    mesh.joints.resize(3, {0, 0, 0, 0, 1, 0, 0, 0});
    mesh.weights.resize(3, {.25f, 0, 0, 0, .75f, 0, 0, 0});
    mesh.skinNodes = {1, 2};
    mesh.inverseBind = {Mat::Identity(), translation(V3(-1, 0, 0))};
    graph(skin);
    output(skin, root / "skinned.obj", options);
    Obj skinned(root / "skinned.obj");
    closeTo(skinned.vertices[0].x(), 4.25, "Skin blends world palette with inverse bind and all weights");
    skinned.winding();
    for (size_t i = 1; i < skin.nodes.size(); ++i)
        skin.nodes[i].s.x() = -1;
    graph(skin);
    output(skin, root / "skinned-mirrored.obj", options);
    Obj skinnedMirrored(root / "skinned-mirrored.obj");
    skinnedMirrored.winding();

    auto hidden = fixture(root);
    hidden.nodes.resize(2);
    hidden.nodes[1].s = V3::Zero();
    hidden.meshes.push_back(hidden.meshes[0]);
    hidden.meshes[1].node = 1;
    graph(hidden);
    auto visibleReport = output(hidden, root / "visible.obj", options);
    Obj visible(root / "visible.obj");
    check(visible.faces.size() == 1 && visibleReport["skipped_hidden_or_degenerate_meshes"] == 1,
          "Visibility gates omit collapsed hidden mesh");
    hidden.nodes[0].s = V3::Zero();
    graph(hidden);
    rejects([&] { output(hidden, root / "all-hidden.obj", options); },
            "All-hidden scene reports no visible geometry");
    check(!fs::exists(root / "all-hidden.obj"), "All-hidden export does not publish an empty model");
}
void texturesAndNumbers(const fs::path& root) {
    auto scene = fixture(root);
    auto painted = png({13, 57, 199, 81});
    ExportOptions options;
    options.diffuseOverrides[0] = painted;
    auto path = root / L"已绘制 涂装.obj";
    auto report = output(scene, path, options);
    Obj obj(path);
    auto textures = maps(obj);
    check(textures.contains("map_Kd") && textures.contains("map_d"),
          "Painted diffuse and separate portable opacity maps exported");
    check(readFile(textures.at("map_Kd")) == painted,
          "Full-resolution edited PNG is preserved byte-for-byte");
    auto opacity = loadTexture({textures.at("map_d")}, 0);
    auto* alpha = opacity->pixels.GetImage(0, 0, 0);
    closeTo(alpha->pixels[0], 81, "Opacity map contains PNG alpha as luminance", 0);
    check(text(obj.mtl).find("Kd 1 1 1") != std::string::npos, "Textured material is not incorrectly tinted");
    check(report["exported_textures"] == 1, "Texture count is reported");

    auto skinDirectory = root / L"涂装 素材";
    fs::create_directories(skinDirectory);
    saveText(skinDirectory / "description.lua", "livery={}");
    auto atlas = png({221, 71, 31, 140});
    writeFile(skinDirectory / L"编号 图集.png", atlas);
    options.livery = std::make_shared<Livery>();
    options.livery->path = skinDirectory / "description.lua";
    options.livery->textures[{lower(scene.materials[0].name), 3}] = {scene.materials[0].name, "编号 图集.png",
                                                                     3, false};
    auto& mesh = scene.meshes[0];
    mesh.selectors = {0, 0, 0};
    mesh.numbers = {{31, 32, 2, 3}};
    scene.materials[0].textures.push_back({3, "original_atlas"});
    options.livery->args = {{31, .1}, {32, .2}};
    options.baseline[31] = .3;
    scene.materials[0].textures[0].matrix(0, 3) = .05;
    scene.materials[0].uniforms["decalShift"] = {.01, .02};
    auto numberedPath = root / "numbered.obj";
    auto numberReport = output(scene, numberedPath, options);
    Obj numbered(numberedPath);
    auto numberedTextures = maps(numbered);
    check(readFile(numberedTextures.at("map_Kd")) == atlas,
          "Dynamic registration uses selected livery atlas, not edited diffuse");
    closeTo(numbered.uv[0][0], .66, "Number UV combines texture transform, decal shift and current argument");
    closeTo(numbered.uv[0][1], .38, "Number V combines livery argument and OBJ V convention");
    closeTo(numbered.vertices[0].z(), .0002, "Number layer receives small surface offset");
    check(numberReport["number_meshes"] == 1, "Current dynamic number mesh reported");
    check(numbered.materialNames[0].find("_number") != std::string::npos,
          "Dynamic registration has its own material");
}
void invalidAndAtomic(const fs::path& root) {
    fs::create_directories(root);
    auto scene = fixture(root);
    ExportOptions options;
    options.textures = false;
    auto path = root / "existing.obj";
    output(scene, path, options);
    auto original = snapshot(root);
    std::atomic_bool cancel = true;
    rejects([&] { output(scene, path, options, {}, &cancel); }, "Pre-cancelled OBJ export is rejected");
    check(snapshot(root) == original, "Pre-cancel preserves all files");
    cancel = false;
    rejects([&] { output(scene, path, options, [&](const auto&) { cancel = true; }, &cancel); },
            "Mid-export cancellation is observed before publication");
    check(snapshot(root) == original, "Cancellation removes only new sidecars and keeps old model intact");
    rejects([&] { output(scene, path, options, [&](const auto&) { throw std::runtime_error("callback"); }); },
            "Progress callback failure aborts transaction");
    check(snapshot(root) == original, "Callback exception leaves existing OBJ and all old sidecars intact");
    auto badIndex = scene;
    badIndex.meshes[0].indices[0] = 999;
    rejects([&] { output(badIndex, path, options); }, "Reject out-of-range triangle index");
    auto badUV = scene;
    badUV.meshes[0].uvs[0].pop_back();
    rejects([&] { output(badUV, path, options); }, "Reject inconsistent UV counts");
    auto badNormal = scene;
    badNormal.meshes[0].normals[0][2] = std::numeric_limits<float>::quiet_NaN();
    rejects([&] { output(badNormal, path, options); }, "Reject nonfinite normals");
    auto badGraph = scene;
    badGraph.nodes[0].parent = 0;
    rejects([&] { output(badGraph, path, options); }, "Reject graph cycle before evaluating");
    auto badSelector = scene;
    badSelector.meshes[0].selectors = {9, 9, 9};
    rejects([&] { output(badSelector, path, options); }, "Reject invalid dynamic number selector");
    auto badArgs = options;
    badArgs.baseline[1] = std::numeric_limits<double>::infinity();
    rejects([&] { output(scene, path, badArgs); }, "Reject nonfinite baseline argument");
    auto badSkin = scene;
    badSkin.meshes[0].joints.resize(3, {1, 0, 0, 0, 0, 0, 0, 0});
    badSkin.meshes[0].weights.resize(3, {1, 0, 0, 0, 0, 0, 0, 0});
    badSkin.meshes[0].skinNodes = {0};
    badSkin.meshes[0].inverseBind = {Mat::Identity()};
    rejects([&] { output(badSkin, path, options); }, "Reject out-of-range weighted skin joint");
    for (auto& joint : badSkin.meshes[0].joints)
        joint[0] = 0;
    badSkin.meshes[0].weights[0][0] = .5f;
    rejects([&] { output(badSkin, path, options); }, "Reject unnormalized skin weights");
    auto badPng = options;
    badPng.textures = true;
    badPng.diffuseOverrides[0] = {137, 'P', 'N', 'G', 13, 10, 26, 10};
    rejects([&] { output(scene, path, badPng); }, "Reject truncated PNG with valid signature");
    check(snapshot(root) == original, "Every validation failure preserves previous OBJ bundle");
    auto directoryTarget = root / "directory.obj";
    fs::create_directory(directoryTarget);
    rejects([&] { output(scene, directoryTarget, options); }, "Failed final publication is reported");
    check(snapshot(root) == original && fs::is_directory(directoryTarget),
          "Failed final publication cleans staged sidecars and preserves destination directory");
    auto wrongExtension = root / "wrong.fbx";
    rejects([&] { output(scene, wrongExtension, options); }, "OBJ writer rejects another format extension");
}
} // namespace
int wmain() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        ComRuntime imageRuntime;
        ComApartment apartment;
        auto root = fs::current_path() / "validation" / "export-obj-tests" /
                    std::to_wstring(Clock::now().time_since_epoch().count());
        fs::create_directories(root);
        basicAndPose(root / "pose");
        normalsAndSkin(root / "skin");
        texturesAndNumbers(root / "textures");
        invalidAndAtomic(root / "atomic");
        std::cout << "OBJ export tests passed: " << checks << " checks\n";
        return 0;
    } catch (...) {
        std::cerr << exceptionText() << '\n';
        return 1;
    }
}
