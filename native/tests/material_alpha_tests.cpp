#include "export_internal.h"
#include "paint_document.h"
#include <ufbx.h>
#include <iostream>
#include <sstream>

using namespace edm;
namespace {
size_t checks = 0;
void check(bool condition, const std::string& description) {
    ++checks;
    require(condition, "Material alpha regression: " + description);
}
std::string text(ufbx_string value) {
    return {value.data, value.length};
}
std::string text(const fs::path& path) {
    const auto bytes = readFile(path);
    return {bytes.begin(), bytes.end()};
}
using Imported = std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)>;
Imported loadFbx(const fs::path& path) {
    ufbx_load_opts options{};
    options.strict = true;
    options.force_single_thread_ascii_parsing = true;
    options.index_error_handling = UFBX_INDEX_ERROR_HANDLING_ABORT_LOADING;
    ufbx_error error{};
    Imported scene(ufbx_load_file(pathString(path).c_str(), &options, &error), ufbx_free_scene);
    char message[2048]{};
    ufbx_format_error(message, sizeof(message), &error);
    check(bool(scene), std::string("strict FBX load: ") + message);
    return scene;
}
std::vector<uint8_t> imageBytes(const ExportPayload& payload, const Json& material) {
    const auto texture = material.at("pbrMetallicRoughness").at("baseColorTexture").at("index").get<size_t>();
    const auto image = payload.document.at("textures").at(texture).at("source").get<size_t>();
    const auto viewIndex = payload.document.at("images").at(image).at("bufferView").get<size_t>();
    const auto& view = payload.document.at("bufferViews").at(viewIndex);
    const size_t begin = view.value("byteOffset", size_t(0)), size = view.at("byteLength").get<size_t>();
    require(begin <= payload.buffer.size() && size <= payload.buffer.size() - begin,
            "PNG buffer view bounds");
    return {payload.buffer.begin() + begin, payload.buffer.begin() + begin + size};
}
std::vector<uint8_t> imageBytes(const ufbx_texture* texture) {
    require(texture != nullptr, "FBX texture connection missing");
    auto content = texture->content;
    if (!content.size && texture->video)
        content = texture->video->content;
    require(content.size != 0, "FBX embedded PNG missing");
    const auto* begin = static_cast<const uint8_t*>(content.data);
    return {begin, begin + content.size};
}
bool containsOriginalVideo(const ufbx_scene& scene, const std::vector<uint8_t>& original) {
    for (const auto* video : scene.videos) {
        const auto content = video->content;
        if (content.size == original.size() && content.data &&
            std::memcmp(content.data, original.data(), original.size()) == 0)
            return true;
    }
    return false;
}
void opaquePng(const fs::path& root, size_t index, const std::vector<uint8_t>& exported,
               const std::vector<uint8_t>& original) {
    constexpr std::array<uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    check(exported.size() >= 33 && std::equal(signature.begin(), signature.end(), exported.begin()) &&
              exported[8] == 0 && exported[9] == 0 && exported[10] == 0 && exported[11] == 13 &&
              std::memcmp(exported.data() + 12, "IHDR", 4) == 0 && exported[24] == 8 && exported[25] == 2,
          "FBX opaque diffuse is an actual RGB PNG, with no alpha channel for importers to connect");
    const auto exportedPath = root / ("verify-opaque-" + std::to_string(index) + ".png");
    const auto originalPath = root / ("verify-original-" + std::to_string(index) + ".png");
    writeFile(exportedPath, exported);
    writeFile(originalPath, original);
    const auto rgb = PaintImage::load({exportedPath}, 0);
    const auto rgba = PaintImage::load({originalPath}, 0);
    check(rgb.width == rgba.width && rgb.height == rgba.height && rgb.rgba.size() == rgba.rgba.size(),
          "opaque RGB variant retains source dimensions");
    bool opaqueAlpha = true;
    for (size_t pixel = 0; pixel < rgba.rgba.size() / 4; ++pixel) {
        const size_t offset = pixel * 4;
        check(rgb.rgba[offset] == rgba.rgba[offset] && rgb.rgba[offset + 1] == rgba.rgba[offset + 1] &&
                  rgb.rgba[offset + 2] == rgba.rgba[offset + 2],
              "opaque RGB variant preserves unassociated source color at pixel " + std::to_string(pixel));
        opaqueAlpha &= rgb.rgba[offset + 3] == 255;
    }
    check(opaqueAlpha, "independent PNG decoder supplies fully opaque alpha for the RGB variant");
}
struct MtlMaterial {
    double opacity = -1;
    fs::path diffuse, alpha;
};
std::map<std::string, MtlMaterial> loadMtl(const fs::path& obj) {
    fs::path mtl;
    std::istringstream geometry(text(obj));
    std::string line;
    while (std::getline(geometry, line)) {
        std::istringstream fields(line);
        std::string key, value;
        fields >> key >> value;
        if (key == "mtllib")
            mtl = obj.parent_path() / wide(value);
    }
    require(fs::is_regular_file(mtl), "OBJ MTL dependency missing");
    std::map<std::string, MtlMaterial> result;
    std::string current;
    std::istringstream input(text(mtl));
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string key, value;
        fields >> key;
        if (key == "newmtl")
            fields >> current;
        else if (key == "d")
            fields >> result[current].opacity;
        else if (key == "map_Kd" || key == "map_d") {
            fields >> value;
            auto path = fs::weakly_canonical(mtl.parent_path() / wide(value));
            require(within(path, mtl.parent_path()) && fs::is_regular_file(path),
                    "OBJ image dependency bounds");
            (key == "map_Kd" ? result[current].diffuse : result[current].alpha) = path;
        }
    }
    return result;
}
struct Case {
    int mode;
    double opacity;
};
const std::vector<Case> cases{{0, 0}, {0, .6}, {1, 0}, {1, .6}, {2, .6}, {3, .6}, {4, .6}, {5, .6}, {6, .6}};
Scene fixture(const fs::path& root) {
    Scene scene;
    scene.version = 10;
    scene.source = root / "alpha-fixture.edm";
    for (size_t index = 0; index < cases.size(); ++index) {
        Material material;
        material.name = "alpha_case_" + std::to_string(index);
        material.shader = "def_material";
        material.blending = cases[index].mode;
        material.uniforms = {{"opacityValue", cases[index].opacity}};
        material.textures = {{0, "shared-source-alpha"}};
        material.format = {3, 3, 2};
        scene.materials.push_back(material);
        Node node;
        node.name = "alpha surface " + std::to_string(index);
        node.t = V3(double(index) * 2, 0, 0);
        scene.nodes.push_back(node);
        scene.order.push_back(int(index));
        scene.staticLocal.push_back(node.local());
        Mesh mesh;
        mesh.name = node.name;
        mesh.node = mesh.material = int(index);
        mesh.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        mesh.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
        mesh.indices = {0, 1, 2};
        mesh.uvs = {{{0, 0}, {1, 0}, {0, 1}}};
        mesh.extras = Json::object();
        scene.meshes.push_back(mesh);
    }
    scene.defaultWorld = scene.evaluate({});
    return scene;
}
void modes() {
    for (int mode = 0; mode <= 6; ++mode) {
        Material material;
        material.blending = mode;
        const auto expected = mode == 0   ? MaterialAlphaMode::Opaque
                              : mode == 2 ? MaterialAlphaMode::Mask
                                          : MaterialAlphaMode::Blend;
        for (double opacity : {0.0, .6}) {
            material.uniforms["opacityValue"] = opacity;
            check(materialAlphaMode(material) == expected,
                  "DCS blend mode classification " + std::to_string(mode));
            check(std::abs(materialColor(material)[3] - (mode == 0 ? 1 : opacity)) < 1e-6,
                  "opaque ignores scalar opacity while genuine alpha modes retain it");
        }
        material.uniforms.erase("opacityValue");
        check(materialColor(material)[3] == 1, "unspecified opacity defaults to one");
    }
}
void exportedMaterials(const fs::path& root, const Scene& scene, const ExportOptions& options,
                       const std::vector<uint8_t>& transparentPng, const std::vector<uint8_t>& mixedPng) {
    const auto payload = buildExportPayload(scene, options);
    check(payload.document.at("materials").size() == cases.size(), "all material alpha cases retained");
    for (size_t index = 0; index < cases.size(); ++index) {
        const auto& material = payload.document.at("materials").at(index);
        const auto& item = cases[index];
        const auto mode = item.mode == 0 ? "OPAQUE" : item.mode == 2 ? "MASK" : "BLEND";
        check(material.value("alphaMode", "OPAQUE") == mode, "glTF alpha mode follows DCS material flags");
        check(std::abs(material.at("pbrMetallicRoughness").at("baseColorFactor").at(3).get<double>() -
                       (item.mode == 0 ? 1 : item.opacity)) < 1e-6,
              "glTF opaque alpha zero does not hide a body and translucent scalar opacity remains effective");
        check(imageBytes(payload, material) == (index == 0 ? transparentPng : mixedPng),
              "glTF preserves original or painted PNG bytes including zero alpha");
        if (item.mode == 2)
            check(material.at("alphaCutoff") == .5, "DCS alpha-test exports MASK with cutoff 0.5");
    }
    for (const auto* extension : {".glb", ".gltf"}) {
        const auto path = root / (std::string("alpha-materials") + extension);
        exportScene(scene, path, options);
        auto bytes = readFile(path);
        Json document;
        if (std::string_view(extension) == ".glb") {
            uint32_t size;
            require(bytes.size() >= 20, "GLB header bounds");
            std::memcpy(&size, bytes.data() + 12, 4);
            require(size <= bytes.size() - 20, "GLB JSON bounds");
            document = Json::parse(bytes.begin() + 20, bytes.begin() + 20 + size);
        } else
            document = Json::parse(bytes);
        check(document.at("materials") == payload.document.at("materials"),
              "actual GLB/glTF export uses the verified shared alpha material definitions");
    }
    const auto obj = root / "alpha-materials.obj";
    exportScene(scene, obj, options);
    const auto mtl = loadMtl(obj);
    for (size_t index = 0; index < cases.size(); ++index) {
        const auto key = "material_" + std::to_string(index);
        check(mtl.contains(key), "OBJ material entry exists");
        const auto& material = mtl.at(key);
        check(std::abs(material.opacity - (cases[index].mode == 0 ? 1 : cases[index].opacity)) < 1e-6,
              "OBJ scalar dissolve ignores opacity for opaque materials only");
        check(!material.diffuse.empty() &&
                  readFile(material.diffuse) == (index == 0 ? transparentPng : mixedPng),
              "OBJ keeps source PNG alpha bytes without destructive conversion");
        check(material.alpha.empty() == (cases[index].mode == 0),
              "OBJ opaque omits map_d while mask/blend keep map_d including shared-texture cache reuse");
        if (!material.alpha.empty()) {
            const auto image = PaintImage::load({material.alpha}, 0);
            check(image.rgba[0] == 0 && image.rgba[4] == 64 && image.rgba[8] == 128 && image.rgba[12] == 255,
                  "OBJ opacity-map luminance preserves source alpha samples");
        }
    }
    const auto fbx = root / "alpha-materials.fbx";
    exportScene(scene, fbx, options);
    auto imported = loadFbx(fbx);
    check(containsOriginalVideo(*imported, transparentPng),
          "FBX preserves exact original all-zero-alpha RGBA PNG as an embedded Video resource");
    check(containsOriginalVideo(*imported, mixedPng),
          "FBX preserves exact original fractional-alpha RGBA PNG as an embedded Video resource");
    std::vector<const ufbx_texture*> diffuseTextures;
    for (size_t index = 0; index < cases.size(); ++index) {
        const auto name = scene.materials[index].name;
        ufbx_material* material = nullptr;
        for (auto* candidate : imported->materials)
            if (text(candidate->name) == name)
                material = candidate;
        check(material != nullptr, "FBX material found by independent importer");
        const auto* diffuse = material->fbx.diffuse_color.texture;
        check(diffuse != nullptr, "FBX diffuse connection remains available for every alpha mode");
        diffuseTextures.push_back(diffuse);
        const auto& original = index == 0 ? transparentPng : mixedPng;
        if (cases[index].mode == 0)
            opaquePng(root, index, imageBytes(diffuse), original);
        else
            check(imageBytes(diffuse) == original,
                  "FBX mask/blend diffuse retains exact original RGBA PNG bytes");
        check((material->fbx.transparency_color.texture != nullptr) == (cases[index].mode != 0),
              "FBX opaque has no TransparentColor texture connection; mask/blend retain it");
        if (cases[index].mode != 0)
            check(imageBytes(material->fbx.transparency_color.texture) == original,
                  "FBX mask/blend transparency connection uses the original RGBA image");
        const auto* opacity = ufbx_find_prop(&material->props, "Opacity");
        check(opacity &&
                  std::abs(opacity->value_real - (cases[index].mode == 0 ? 1 : cases[index].opacity)) < 1e-6,
              "FBX Opacity property preserves genuine transparency only");
        const auto* mode = ufbx_find_prop(&material->props, "EDM_AlphaMode");
        check(mode && text(mode->value_str) == (cases[index].mode == 0   ? "OPAQUE"
                                                : cases[index].mode == 2 ? "MASK"
                                                                         : "BLEND"),
              "FBX mode metadata agrees with actual alpha connection policy");
    }
    check(diffuseTextures.at(1) != diffuseTextures.at(2),
          "one shared source image routes opaque RGB and blended RGBA through separate FBX textures");
}
void registration(const fs::path& root, const Scene& original, const std::vector<uint8_t>& atlasPng) {
    auto scene = original;
    scene.materials.resize(1);
    scene.meshes.resize(1);
    scene.materials[0].blending = 0;
    scene.materials[0].uniforms["opacityValue"] = 0;
    scene.materials[0].textures.push_back({3, "number-alpha-atlas"});
    scene.meshes[0].numbers = {{32, -1, 1, 0}};
    scene.meshes[0].selectors = {0, 0, 0};
    scene.meshes[0].extras["edm_type"] = "NumberNode";
    scene.numberArgs = {32};
    scene.limits[32] = {0, 1};
    writeFile(root / "number-alpha-atlas.png", atlasPng);
    ExportOptions options;
    options.arguments = std::vector<int>{};
    options.textureDirectory = root;
    options.baseline = {{32, .4}};
    const auto payload = buildExportPayload(scene, options);
    const auto& material = payload.document.at("materials").back();
    check(material.at("alphaMode") == "MASK" && material.at("alphaCutoff") == .1,
          "registration remains independently cut out at 0.1 despite an opaque base mode");
    check(material.at("pbrMetallicRoughness").at("baseColorFactor").at(3) == 1,
          "registration ignores base opacityValue zero");
    check(imageBytes(payload, material) == atlasPng, "registration atlas original alpha remains intact");
    const auto obj = root / "number-alpha.obj";
    exportScene(scene, obj, options);
    const auto materials = loadMtl(obj);
    const auto& number = materials.at("material_0_number");
    check(number.opacity == 1 && !number.alpha.empty(),
          "OBJ registration keeps map_d while base opacity zero is ignored");
    check(readFile(number.diffuse) == atlasPng, "OBJ registration retains exact atlas bytes");
    const auto fbx = root / "number-alpha.fbx";
    exportScene(scene, fbx, options);
    const auto imported = loadFbx(fbx);
    ufbx_material* registration = nullptr;
    for (auto* material : imported->materials)
        if (text(material->name).ends_with(" / registration"))
            registration = material;
    check(registration && registration->fbx.transparency_color.texture &&
              imageBytes(registration->fbx.diffuse_color.texture) == atlasPng &&
              imageBytes(registration->fbx.transparency_color.texture) == atlasPng,
          "FBX registration keeps atlas alpha connection regardless of base material mode");
}
void editing(const fs::path& root, const Scene& scene, const PaintImage& source,
             const std::vector<uint8_t>& sourcePng) {
    PaintCanvas canvas(source);
    canvas.beginStroke();
    check(canvas.blendPixel(0, 0, {1, 0, 0, .5f}, 1, true) && canvas.endStroke(),
          "paint on opaque-model canvas still records RGB edits");
    check(canvas.image().rgba[3] == 0,
          "preserve-alpha paint does not overwrite opaque material's stored alpha mask");
    check(canvas.undo() && canvas.image().rgba == source.rgba,
          "paint undo restores every original RGBA byte");
    canvas.beginStroke();
    check(canvas.blendPixel(0, 0, {1, 0, 0, .5f}, 1, false) && canvas.endStroke(),
          "explicit alpha-editing mode remains available");
    check(canvas.image().rgba[0] == 255 && canvas.image().rgba[1] == 0 && canvas.image().rgba[2] == 0 &&
              canvas.image().rgba[3] == 128,
          "source-over onto zero stored alpha uses raw destination alpha rather than opaque display alpha");
    check(canvas.undo() && canvas.image().rgba == source.rgba,
          "alpha-edit undo restores original transparent pixels");
    auto image = std::make_shared<PaintImage>(canvas.image());
    const auto manifest =
        savePaintProject(scene, {{0, image}}, root / "paint-project", "alpha-preservation", {}, root);
    const auto project =
        fs::path(wide(manifest.at("directory").get<std::string>())) / "project.edmpaint.json";
    const auto loaded = loadPaintDocument(scene, project);
    check(loaded.images.at(0)->rgba == source.rgba,
          "saving/reopening paint preserves raw alpha including zero");
    check(source.pngBytes() == sourcePng && source.rgba[3] == 0,
          "render/export alpha policy does not mutate the original in-memory PNG or source canvas");
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc >= 4 && std::wstring_view(argv[1]) == L"--lua-worker")
        return luaWorker(argv[2], argv[3]);
    try {
        ComRuntime runtime;
        const auto root = fs::current_path() / "validation" /
                          ("material-alpha-tests-" + std::to_string(GetCurrentProcessId()));
        fs::create_directories(root);
        auto scene = fixture(root);
        PaintImage zero(4, 4, {37, 97, 181, 0}), mixed(4, 4, {47, 157, 211, 255});
        for (size_t pixel = 0; pixel < 16; ++pixel) {
            for (size_t channel = 0; channel < 3; ++channel) {
                zero.rgba[pixel * 4 + channel] = uint8_t(zero.rgba[channel] + pixel * (13 - 4 * channel));
                mixed.rgba[pixel * 4 + channel] = uint8_t(mixed.rgba[channel] + pixel * (7 + 3 * channel));
            }
            mixed.rgba[pixel * 4 + 3] = std::array<uint8_t, 4>{0, 64, 128, 255}[pixel % 4];
        }
        const auto zeroPng = zero.pngBytes(), mixedPng = mixed.pngBytes();
        writeFile(root / "shared-source-alpha.png", mixedPng);
        ExportOptions options;
        options.arguments = std::vector<int>{};
        options.textureDirectory = root;
        options.diffuseOverrides[0] = zeroPng;
        options.diffuseOverrides[3] = mixedPng;
        modes();
        exportedMaterials(root, scene, options, zeroPng, mixedPng);
        registration(root, scene, mixedPng);
        editing(root, scene, zero, zeroPng);
        check(readFile(root / "shared-source-alpha.png") == mixedPng &&
                  readFile(root / "number-alpha-atlas.png") == mixedPng,
              "source PNG files retain every byte after all export and paint operations");
        std::cout << "Material alpha: " << checks << " checks passed\n";
        return 0;
    } catch (...) {
        std::cerr << exceptionText() << '\n';
        return 1;
    }
}
