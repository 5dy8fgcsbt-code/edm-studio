#include "attachment.h"
#include "export.h"
#include "paint.h"
#include <ufbx.h>
#include <iostream>
#include <sstream>

using namespace edm;
namespace {
size_t checks = 0;
double maximumError = 0;
void check(bool value, const std::string& message) {
    ++checks;
    require(value, "Model-only regression: " + message);
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
std::string string(ufbx_string value) {
    return {value.data, value.length};
}
V4 angle(const V3& axis, double radians) {
    return Eigen::Quaterniond(Eigen::AngleAxisd(radians, axis.normalized())).coeffs();
}
// Analytic reference transforms use Eigen directly, never Scene::evaluate(),
// Scene::transformed(), textureUV(), or an intermediate exporter payload.
Mat affine(V3 t, V3 axis = V3::UnitZ(), double radians = 0, V3 scale = V3::Ones()) {
    return (Eigen::Translation3d(t) * Eigen::AngleAxisd(radians, axis.normalized()) *
            Eigen::Scaling(scale)).matrix();
}
int node(Scene& scene, std::string name, int parent = -1, V3 t = V3::Zero(),
         V3 axis = V3::UnitZ(), double radians = 0, V3 scale = V3::Ones(), bool connector = false) {
    Node n;
    n.name = std::move(name);
    n.parent = parent;
    n.t = t;
    n.q = angle(axis, radians);
    n.s = scale;
    n.extras = {{"edm_node", scene.nodes.size()}, {"custom_test_metadata", "must be removed"}};
    if (connector) {
        n.extras["edm_type"] = "Connector";
        ++scene.connectorCount;
    }
    scene.nodes.push_back(std::move(n));
    return int(scene.nodes.size()) - 1;
}
void finish(Scene& scene) {
    scene.order.clear();
    scene.staticLocal.clear();
    for (int i = 0; i < int(scene.nodes.size()); ++i) {
        require(scene.nodes[i].parent < i, "Topological analytic fixture");
        scene.order.push_back(i);
        scene.staticLocal.push_back(scene.nodes[i].local());
    }
    scene.defaultWorld = scene.evaluate({});
    scene.sourceNodes = int(scene.nodes.size());
}
Material material(std::string name) {
    Material mat;
    mat.name = std::move(name);
    mat.shader = "test-only-edm-shader";
    mat.blending = 1;
    mat.format = {3, 3, 2, 2};
    mat.uvChannels = {1};
    mat.uniforms = {{"diffuseColor", {.25, .5, .75}}, {"opacityValue", .7},
                    {"diffuseShift", {.03, -.02}}};
    mat.animatedUniforms = {{"private", {{"argument", 90}}}};
    mat.extras = {{"edm_test", "material"}};
    TextureRef ref{0, "fixture-diffuse"};
    ref.matrix(0, 0) = .7;
    ref.matrix(0, 1) = .15;
    ref.matrix(1, 0) = -.1;
    ref.matrix(1, 1) = .8;
    ref.matrix(0, 3) = .12;
    ref.matrix(1, 3) = -.08;
    mat.textures.push_back(ref);
    return mat;
}
Mesh triangle(std::string name, int owner, int materialIndex = 0) {
    Mesh mesh;
    mesh.name = std::move(name);
    mesh.node = owner;
    mesh.material = materialIndex;
    mesh.positions = {{0, 0, 0}, {1, 0, .25f}, {0, 1, .5f}};
    const V3 normal = V3(-.25, -.5, 1).normalized();
    mesh.normals.assign(3, F3{float(normal.x()), float(normal.y()), float(normal.z())});
    mesh.indices = {0, 1, 2};
    mesh.uvs = {{{0, 0}, {1, 0}, {0, 1}}, {{.05f, .1f}, {.8f, .2f}, {.15f, .95f}}};
    mesh.extras = {{"edm_render_index", owner}, {"edm_type", "RenderableNode"},
                   {"private_metadata", "remove me"}};
    return mesh;
}
void rotationTrack(Scene& scene, int owner, int argument, V3 axis, double radians) {
    scene.tracks.push_back({owner, argument, Channel::Rotation,
                            {{0, angle(axis, 0)}, {1, angle(axis, radians)}}});
    scene.limits[argument] = {0, 1};
}
struct Corner {
    V3 position, normal;
    Eigen::Vector2d uv;
};
using Triangle = std::array<Corner, 3>;
using Geometry = std::vector<Triangle>;
Eigen::Vector2d diffuseUv(const Mesh& mesh, size_t vertex) {
    const auto uv = mesh.uvs[1][vertex];
    return {.7 * uv[0] + .15 * uv[1] + .15, -.1 * uv[0] + .8 * uv[1] - .1};
}
Triangle rigidReference(const Mesh& mesh, const Mat& matrix) {
    Triangle result;
    const auto normalMatrix = matrix.block<3, 3>(0, 0).inverse().transpose().eval();
    for (size_t i = 0; i < 3; ++i) {
        const auto p = mesh.positions[i], n = mesh.normals[i];
        result[i] = {(matrix * V4(p[0], p[1], p[2], 1)).head<3>(),
                     (normalMatrix * V3(n[0], n[1], n[2])).normalized(), diffuseUv(mesh, i)};
    }
    return result;
}
struct Fixture {
    Scene scene;
    ExportOptions options;
    Geometry expected;
    std::vector<uint8_t> painted;
    bool registration = false;
};
Fixture geometryFixture(const fs::path& root) {
    Fixture fixture;
    auto& source = fixture.scene;
    source.source = root / "inputs" / "analytic.edm";
    source.version = 10;
    source.materials = {material("visible coating"), material("unused coating"), material("hidden coating")};
    const int base = node(source, "Root helper", -1, {4, -2, 3}, V3::UnitY(), .31, {2, .6, 1.3});
    const int rotor = node(source, "Animated rotation helper", base);
    rotationTrack(source, rotor, 1, V3::UnitZ(), 1.1);
    const int rigid = node(source, "Sheared surface", rotor, {.4, -.7, .2}, V3::UnitX(), .27);
    source.meshes.push_back(triangle("Sheared surface", rigid));
    const int mirrored = node(source, "Mirrored surface", rotor, {-3, 1, .5}, V3::UnitY(), -.23,
                              {-1.2, .8, 1.4});
    source.meshes.push_back(triangle("Mirrored surface", mirrored));
    const int skinRoot = node(source, "Skeleton helper", -1, {-7, 2, 1}, V3::UnitY(), -.31,
                              {1.15, .75, 1.3});
    std::vector<int> bones;
    for (int i = 0; i < 8; ++i) {
        const int offset = node(source, "Bone offset " + std::to_string(i), skinRoot,
                                {.3 * i, -.1 * i, .2 * i});
        const int bone = node(source, "Bone " + std::to_string(i), offset);
        rotationTrack(source, bone, 4, V3::UnitY(), .1 + .04 * i);
        bones.push_back(bone);
    }
    const int skinOwner = node(source, "Eight-weight surface", -1, {99, -88, 77});
    auto skin = triangle("Eight-weight surface", skinOwner);
    skin.skinNodes = bones;
    for (int i = 0; i < 8; ++i)
        skin.inverseBind.push_back(affine({-.3 * i, .1 * i, -.2 * i}));
    skin.joints.resize(3);
    skin.weights.resize(3);
    constexpr std::array<float, 8> weights{1, 2, 3, 4, 5, 6, 7, 4};
    for (size_t vertex = 0; vertex < 3; ++vertex)
        for (size_t joint = 0; joint < 8; ++joint) {
            skin.joints[vertex][joint] = uint16_t(joint);
            skin.weights[vertex][joint] = weights[(joint + vertex * 3) % 8] / 32;
        }
    source.meshes.push_back(skin);
    const int hidden = node(source, "Hidden zero-scale helper", base, {2, 4, 1}, V3::UnitZ(), 0,
                            V3::Zero());
    source.meshes.push_back(triangle("Hidden degenerate surface", hidden, 2));
    const int mount = node(source, "Pylon1", rotor, {1.3, -.4, 2}, V3::UnitX(), .23,
                           V3::Ones(), true);
    node(source, "Unused empty connector", base, V3::Zero(), V3::UnitY(), 0, V3::Ones(), true);
    source.defaultArgs = {{1, .2}, {4, .15}};
    finish(source);

    Scene payload;
    payload.source = root / "inputs" / "payload.edm";
    payload.version = 10;
    payload.materials.push_back(material("payload coating"));
    node(payload, "Payload root", -1, {-1, 2, .3}, V3::UnitX(), .41, {2, .6, -1.3});
    node(payload, "AttachPoint", 0, {.7, -.2, .6}, {1, 1, 1}, -.77, V3::Ones(), true);
    node(payload, "Payload surface", 0, {1, .5, -.6}, V3::UnitZ(), .37, {.8, 1.4, .9});
    payload.meshes.push_back(triangle("Payload surface", 2));
    finish(payload);
    source = *attachScene(source, payload, mount);

    auto& options = fixture.options;
    options.modelOnly = true;
    options.duration = 1.7;
    options.arguments = std::vector<int>{1, 4}; // Pure export must override explicit animated selection.
    options.livery = std::make_shared<Livery>();
    options.livery->args = {{1, .4}, {4, .375}};
    options.baseline = {{1, .6}, {4, .7}};
    options.textureDirectory = root / "inputs";
    fixture.painted = PaintImage(4, 4, {47, 171, 93, 179}).pngBytes();
    options.diffuseOverrides[0] = fixture.painted;
    options.diffuseOverrides[1] = PaintImage(4, 4, {255, 0, 0, 255}).pngBytes();
    options.diffuseOverrides[2] = PaintImage(4, 4, {0, 0, 255, 255}).pngBytes();
    options.diffuseOverrides[3] = fixture.painted;

    const Mat host = affine({4, -2, 3}, V3::UnitY(), .31, {2, .6, 1.3});
    const Mat turning = host * affine(V3::Zero(), V3::UnitZ(), 1.1 * .6);
    const Mat sheared = turning * affine({.4, -.7, .2}, V3::UnitX(), .27);
    check(std::abs(sheared.block<3, 1>(0, 0).dot(sheared.block<3, 1>(0, 1))) > .1,
          "fixture truly contains world shear, not only a decomposable local TRS");
    fixture.expected.push_back(rigidReference(source.meshes[0], sheared));
    fixture.expected.push_back(rigidReference(source.meshes[1],
        turning * affine({-3, 1, .5}, V3::UnitY(), -.23, {-1.2, .8, 1.4})));
    const Mat skeleton = affine({-7, 2, 1}, V3::UnitY(), -.31, {1.15, .75, 1.3});
    Triangle deformed;
    for (size_t vertex = 0; vertex < 3; ++vertex) {
        V4 position = V4::Zero();
        V3 normal = V3::Zero();
        const auto p = skin.positions[vertex], n = skin.normals[vertex];
        for (int i = 0; i < 8; ++i) {
            const Mat matrix = skeleton * affine({.3 * i, -.1 * i, .2 * i}) *
                               affine(V3::Zero(), V3::UnitY(), (.1 + .04 * i) * .7) *
                               affine({-.3 * i, .1 * i, -.2 * i});
            const double weight = weights[(i + vertex * 3) % 8] / 32.;
            position += matrix * V4(p[0], p[1], p[2], 1) * weight;
            normal += matrix.block<3, 3>(0, 0).inverse().transpose() * V3(n[0], n[1], n[2]) * weight;
        }
        deformed[vertex] = {position.head<3>(), normal.normalized(), diffuseUv(skin, vertex)};
    }
    fixture.expected.push_back(deformed);
    const Mat childRoot = affine({-1, 2, .3}, V3::UnitX(), .41, {2, .6, -1.3});
    const Mat attachPoint = childRoot * affine({.7, -.2, .6}, {1, 1, 1}, -.77);
    const Mat target = turning * affine({1.3, -.4, 2}, V3::UnitX(), .23);
    fixture.expected.push_back(rigidReference(payload.meshes[0], target * attachPoint.inverse() *
        childRoot * affine({1, .5, -.6}, V3::UnitZ(), .37, {.8, 1.4, .9})));
    return fixture;
}

std::map<std::string, std::vector<uint8_t>> snapshot(const fs::path& root) {
    std::map<std::string, std::vector<uint8_t>> result;
    if (fs::exists(root))
        for (const auto& item : fs::recursive_directory_iterator(root))
            if (item.is_regular_file())
                result[pathString(fs::relative(item.path(), root))] = readFile(item.path());
    return result;
}
Json sourceSnapshot(const Scene& scene) {
    Json result = {{"summary", scene.summary()}, {"defaultArgs", scene.defaultArgs},
                    {"nodes", Json::array()}, {"meshes", Json::array()}, {"materials", Json::array()},
                    {"staticLocal", Json::array()}, {"defaultWorld", Json::array()}};
    for (const auto& item : scene.nodes)
        result["nodes"].push_back({item.name, item.parent, arrayJson(item.t), arrayJson(item.q),
                                   arrayJson(item.s), item.extras});
    for (const auto& item : scene.meshes) {
        Json binds = Json::array();
        for (const auto& bind : item.inverseBind)
            binds.push_back(matJson(bind));
        result["meshes"].push_back({item.name, item.node, item.material, item.positions, item.normals,
                                    item.indices, item.uvs, item.joints, item.weights, item.skinNodes,
                                    item.selectors, binds, item.extras});
    }
    for (const auto& item : scene.materials)
        result["materials"].push_back({item.name, item.shader, item.uniforms, item.animatedUniforms, item.extras});
    for (const auto& value : scene.staticLocal)
        result["staticLocal"].push_back(matJson(value));
    for (const auto& value : scene.defaultWorld)
        result["defaultWorld"].push_back(matJson(value));
    return result;
}
void cleanJson(const Json& value) {
    if (value.is_object())
        for (auto item = value.begin(); item != value.end(); ++item) {
            check(item.key() != "extras" && !lower(item.key()).starts_with("edm_"),
                  "no extras or EDM custom metadata anywhere in pure glTF");
            cleanJson(item.value());
        }
    else if (value.is_array())
        for (const auto& item : value)
            cleanJson(item);
}
std::vector<uint8_t> decode64(std::string_view input) {
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> result;
    uint32_t value = 0;
    int bits = 0;
    for (char c : input) {
        if (c == '=')
            break;
        const auto digit = alphabet.find(c);
        require(digit != std::string::npos, "Invalid fixture output base64");
        value = (value << 6) | uint32_t(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(uint8_t(value >> bits));
        }
    }
    return result;
}
void bundle(const fs::path& directory, const std::set<fs::path>& required) {
    for (const auto& file : required)
        check(fs::is_regular_file(file), "each referenced model resource exists");
    for (const auto& item : fs::recursive_directory_iterator(directory))
        if (item.is_regular_file())
            check(required.contains(fs::weakly_canonical(item.path())),
                  "output contains only model and required resources, no JSON reports/Lua/orphan files: " +
                      pathString(item.path().filename()));
}
struct Gltf {
    Json doc;
    std::vector<uint8_t> binary;
    fs::path directory;
    std::set<fs::path> required;
    std::vector<uint8_t> uri(const std::string& value) {
        if (value.starts_with("data:")) {
            const auto comma = value.find(',');
            require(comma != std::string::npos && value.substr(0, comma).ends_with(";base64"),
                    "glTF resource data URI is base64");
            return decode64(std::string_view(value).substr(comma + 1));
        }
        const auto file = fs::weakly_canonical(directory / wide(value));
        check(within(file, directory) && fs::path(wide(value)).is_relative(), "bounded relative glTF resource");
        required.insert(file);
        return readFile(file);
    }
    explicit Gltf(const fs::path& path) : directory(fs::weakly_canonical(path.parent_path())) {
        required.insert(fs::weakly_canonical(path));
        const auto file = readFile(path);
        if (path.extension() == ".gltf") {
            doc = Json::parse(file);
            binary = uri(doc.at("buffers").at(0).at("uri").get<std::string>());
        } else {
            auto word = [&](size_t offset) {
                require(offset + 4 <= file.size(), "GLB word bounds");
                uint32_t value;
                std::memcpy(&value, file.data() + offset, 4);
                return value;
            };
            check(word(0) == 0x46546c67 && word(4) == 2 && word(8) == file.size(), "GLB 2.0 header");
            const size_t length = word(12), offset = 20 + length;
            check(word(16) == 0x4e4f534a && offset + 8 <= file.size(), "GLB JSON chunk bounds");
            doc = Json::parse(file.begin() + 20, file.begin() + offset);
            check(word(offset + 4) == 0x004e4942 && offset + 8 + word(offset) == file.size(),
                  "GLB BIN chunk bounds");
            binary.assign(file.begin() + offset + 8, file.end());
        }
        check(doc.at("buffers").size() == 1 && doc.at("buffers").at(0).at("byteLength") <= binary.size(),
              "one valid glTF geometry buffer");
    }
    double scalar(size_t accessor, size_t element, size_t component = 0) const {
        const auto& a = doc.at("accessors").at(accessor);
        const auto& v = doc.at("bufferViews").at(a.at("bufferView").get<size_t>());
        const auto type = a.at("type").get<std::string>();
        const size_t dimensions = type == "VEC3" ? 3 : type == "VEC2" ? 2 : 1;
        const int storage = a.at("componentType").get<int>();
        const size_t bytes = storage == 5121 ? 1 : storage == 5123 ? 2 : 4;
        require((storage == 5121 || storage == 5123 || storage == 5125 || storage == 5126) &&
                    component < dimensions && element < a.at("count").get<size_t>(), "glTF accessor bounds");
        const size_t offset = v.value("byteOffset", size_t(0)) + a.value("byteOffset", size_t(0)) +
                              element * v.value("byteStride", bytes * dimensions) + bytes * component;
        require(offset + bytes <= binary.size(), "glTF buffer bounds");
        if (storage == 5126) {
            float value;
            std::memcpy(&value, binary.data() + offset, 4);
            return value;
        }
        uint32_t value = 0;
        std::memcpy(&value, binary.data() + offset, bytes);
        return value;
    }
    std::vector<std::vector<uint8_t>> images() {
        std::vector<std::vector<uint8_t>> result;
        for (const auto& item : doc.value("images", Json::array())) {
            if (item.contains("uri"))
                result.push_back(uri(item.at("uri").get<std::string>()));
            else {
                const auto& view = doc.at("bufferViews").at(item.at("bufferView").get<size_t>());
                const size_t offset = view.value("byteOffset", size_t(0)), length = view.at("byteLength");
                require(offset + length <= binary.size(), "Embedded glTF image bounds");
                result.emplace_back(binary.begin() + offset, binary.begin() + offset + length);
            }
        }
        return result;
    }
    Geometry geometry() const {
        cleanJson(doc);
        check(!doc.contains("skins") && !doc.contains("animations"), "pure glTF has no skins or animations");
        const auto& nodes = doc.at("nodes");
        const auto& roots = doc.at("scenes").at(doc.value("scene", 0)).at("nodes");
        check(roots.size() == nodes.size(), "all pure glTF nodes are direct scene roots");
        std::set<int> uniqueRoots;
        for (const auto& root : roots)
            uniqueRoots.insert(root.get<int>());
        check(uniqueRoots.size() == nodes.size(), "pure glTF roots are unique");
        Geometry output;
        std::set<size_t> materials, meshes, textures, images;
        for (size_t ni = 0; ni < nodes.size(); ++ni) {
            const auto& owner = nodes[ni];
            check(uniqueRoots.contains(int(ni)) && owner.contains("mesh") && !owner.contains("children") &&
                      !owner.contains("skin") && !owner.contains("camera"),
                  "glTF contains only mesh nodes, no helpers/connectors/skin parents");
            if (owner.contains("matrix"))
                for (size_t i = 0; i < 16; ++i)
                    check(std::abs(owner.at("matrix").at(i).get<double>() - (i % 5 == 0 ? 1 : 0)) < 1e-12,
                          "glTF world-space mesh node matrix is identity");
            for (const auto* field : {"translation", "rotation", "scale"})
                if (owner.contains(field)) {
                    const std::vector<double> expected = std::string_view(field) == "rotation"
                        ? std::vector<double>{0, 0, 0, 1} : std::string_view(field) == "scale"
                        ? std::vector<double>{1, 1, 1} : std::vector<double>{0, 0, 0};
                    check(owner.at(field).get<std::vector<double>>() == expected,
                          "glTF world-space mesh node TRS is identity");
                }
            const size_t mesh = owner.at("mesh");
            meshes.insert(mesh);
            for (const auto& primitive : doc.at("meshes").at(mesh).at("primitives")) {
                check(primitive.value("mode", 4) == 4 && !primitive.contains("targets"),
                      "pure glTF triangle primitive without morph targets");
                const auto& attributes = primitive.at("attributes");
                for (auto item = attributes.begin(); item != attributes.end(); ++item)
                    check(!item.key().starts_with("JOINTS_") && !item.key().starts_with("WEIGHTS_"),
                          "pure glTF has no skin vertex attributes");
                const size_t material = primitive.at("material");
                materials.insert(material);
                const auto& pbr = doc.at("materials").at(material).at("pbrMetallicRoughness");
                size_t channel = 0;
                if (pbr.contains("baseColorTexture")) {
                    const auto& texture = pbr.at("baseColorTexture");
                    channel = texture.value("texCoord", size_t(0));
                    textures.insert(texture.at("index").get<size_t>());
                }
                const size_t positions = attributes.at("POSITION"), normals = attributes.at("NORMAL"),
                             uv = attributes.at("TEXCOORD_" + std::to_string(channel)),
                             indices = primitive.at("indices");
                const size_t count = doc.at("accessors").at(indices).at("count");
                check(count > 0 && count % 3 == 0, "pure glTF retains nonempty triangle geometry");
                std::set<size_t> usedVertices;
                for (size_t corner = 0; corner < count; corner += 3) {
                    Triangle triangle;
                    for (size_t k = 0; k < 3; ++k) {
                        const size_t vertex = size_t(scalar(indices, corner + k));
                        usedVertices.insert(vertex);
                        triangle[k] = {{scalar(positions, vertex, 0), scalar(positions, vertex, 1),
                                        scalar(positions, vertex, 2)},
                                       {scalar(normals, vertex, 0), scalar(normals, vertex, 1),
                                        scalar(normals, vertex, 2)},
                                       {scalar(uv, vertex, 0), scalar(uv, vertex, 1)}};
                    }
                    output.push_back(triangle);
                }
                check(usedVertices.size() == doc.at("accessors").at(positions).at("count"),
                      "no unreferenced or collapsed hidden vertices survive glTF compaction");
            }
        }
        for (size_t texture : textures)
            images.insert(doc.at("textures").at(texture).at("source").get<size_t>());
        check(meshes.size() == doc.at("meshes").size() && materials.size() == doc.at("materials").size(),
              "no unreferenced pure glTF meshes or materials");
        check(textures.size() == doc.value("textures", Json::array()).size() &&
                  images.size() == doc.value("images", Json::array()).size(),
              "only textures/images used by surviving surfaces are retained");
        return output;
    }
};

using Imported = std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)>;
Imported loadFbx(const fs::path& path) {
    ufbx_load_opts options{};
    options.strict = true;
    options.force_single_thread_ascii_parsing = true;
    options.index_error_handling = UFBX_INDEX_ERROR_HANDLING_ABORT_LOADING;
    options.target_unit_meters = 1;
    ufbx_error error{};
    Imported scene(ufbx_load_file(pathString(path).c_str(), &options, &error), ufbx_free_scene);
    char detail[4096]{};
    ufbx_format_error(detail, sizeof(detail), &error);
    check(bool(scene), "strict independent ufbx load: " + std::string(detail));
    check(scene->metadata.warnings.count == 0, "strict ufbx reports no malformed-file warnings");
    return scene;
}
Geometry fbxGeometry(const ufbx_scene& scene) {
    check(scene.bones.count == 0 && scene.skin_deformers.count == 0 && scene.skin_clusters.count == 0 &&
              scene.anim_stacks.count == 0 && scene.anim_layers.count == 0 && scene.anim_curves.count == 0 &&
              scene.anim_values.count == 0 && scene.blend_deformers.count == 0,
          "pure FBX has zero bones, skin clusters/deformers, morphs, animation stacks/layers/curves/values");
    for (const auto* element : scene.elements)
        for (const auto& property : element->props.props) {
            const auto name = string(property.name);
            // The shared FBX writer marks KString properties as user-defined, including
            // these two standard texture properties that are required for material UVs.
            const bool textureProperty = name == "Path" || name == "UVSet";
            check((!(property.flags & UFBX_PROP_FLAG_USER_DEFINED) || textureProperty) &&
                      !lower(name).starts_with("edm_"),
                  "pure FBX carries no EDM/private custom properties beyond required texture fields");
        }
    Geometry output;
    std::set<const ufbx_material*> materials;
    size_t models = 0;
    for (const auto* owner : scene.nodes) {
        if (owner->is_root)
            continue; // ufbx's implicit format root is not an exported helper Model.
        ++models;
        check(owner->mesh && !owner->bone && owner->parent == scene.root_node && owner->children.count == 0,
              "every FBX Model is a direct-root mesh with no helper, connector, or bone models");
        const auto& mesh = *owner->mesh;
        check(mesh.skin_deformers.count == 0 && mesh.vertex_normal.exists && mesh.vertex_uv.exists,
              "FBX mesh has ordinary normals/UVs and no skin");
        check(owner->materials.count == 1, "pure FBX surface retains its required material");
        auto* mat = owner->materials.data[0];
        materials.insert(mat);
        const auto* diffuse = mat->fbx.diffuse_color.texture;
        if (!diffuse)
            diffuse = mat->pbr.base_color.texture;
        const ufbx_vertex_vec2* uv = &mesh.vertex_uv;
        if (diffuse)
            for (const auto& layer : mesh.uv_sets)
                if (string(layer.name) == string(diffuse->uv_set))
                    uv = &layer.vertex_uv;
        const auto normalMatrix = ufbx_matrix_for_normals(&owner->geometry_to_world);
        for (const auto& face : mesh.faces) {
            check(face.num_indices == 3, "pure FBX retains triangle faces");
            Triangle triangle;
            for (size_t k = 0; k < 3; ++k) {
                const size_t corner = face.index_begin + k;
                const auto p = ufbx_transform_position(&owner->geometry_to_world,
                    ufbx_get_vertex_vec3(&mesh.vertex_position, corner));
                const auto n = ufbx_transform_direction(&normalMatrix,
                    ufbx_get_vertex_vec3(&mesh.vertex_normal, corner));
                const auto t = ufbx_get_vertex_vec2(uv, corner);
                triangle[k] = {{p.x, p.y, p.z}, V3(n.x, n.y, n.z).normalized(), {t.x, 1 - t.y}};
            }
            output.push_back(triangle);
        }
    }
    check(models == scene.meshes.count && models + 1 == scene.nodes.count,
          "FBX model inventory consists exactly of ordinary mesh models");
    check(materials.size() == scene.materials.count, "unused/hidden FBX materials are removed");
    return output;
}
std::vector<std::vector<uint8_t>> fbxImages(const ufbx_scene& scene, const fs::path& path,
                                          std::set<fs::path>& required) {
    std::vector<std::vector<uint8_t>> output;
    for (const auto* texture : scene.textures) {
        if (texture->type != UFBX_TEXTURE_FILE)
            continue;
        auto content = texture->content;
        if (!content.size && texture->video)
            content = texture->video->content;
        if (content.size) {
            const auto* begin = static_cast<const uint8_t*>(content.data);
            output.emplace_back(begin, begin + content.size);
        } else {
            const auto relative = fs::path(wide(string(texture->relative_filename)));
            const auto image = fs::weakly_canonical(path.parent_path() / relative);
            check(relative.is_relative() && within(image, path.parent_path()), "bounded FBX texture path");
            required.insert(image);
            output.push_back(readFile(image));
        }
    }
    return output;
}
void compare(const Geometry& actual, const Geometry& expected, const std::string& label) {
    check(actual.size() == expected.size(), label + ": exact visible triangle count excludes hidden degenerates");
    std::vector<bool> used(expected.size());
    for (const auto& triangle : actual) {
        const V3 face = (triangle[1].position - triangle[0].position).cross(
                            triangle[2].position - triangle[0].position);
        check(face.allFinite() && face.norm() > 1e-8 &&
                  face.dot(triangle[0].normal + triangle[1].normal + triangle[2].normal) > 0,
              label + ": mirrored winding agrees with finite nondegenerate world normals");
        bool matched = false;
        for (size_t candidate = 0; candidate < expected.size() && !matched; ++candidate) {
            if (used[candidate])
                continue;
            std::array<int, 3> permutation{0, 1, 2};
            do {
                bool close = true;
                double error = 0;
                for (size_t k = 0; k < 3; ++k) {
                    const auto& a = triangle[k];
                    const auto& b = expected[candidate][permutation[k]];
                    error = std::max(error, (a.position - b.position).norm());
                    close &= a.position.allFinite() && a.normal.allFinite() && a.uv.allFinite() &&
                             (a.position - b.position).norm() < 3e-5 &&
                             (a.normal - b.normal).norm() < 3e-5 && (a.uv - b.uv).norm() < 2e-6 &&
                             std::abs(a.normal.norm() - 1) < 2e-6;
                }
                if (close) {
                    used[candidate] = true;
                    matched = true;
                    maximumError = std::max(maximumError, error);
                    break;
                }
            } while (std::next_permutation(permutation.begin(), permutation.end()));
        }
        check(matched, label + ": triangle positions, inverse-transpose normals, and selected UVs match analytic reference");
    }
}
void roundtrip(const Fixture& fixture, const fs::path& root, const std::string& stem) {
    const auto before = sourceSnapshot(fixture.scene);
    for (const auto* extension : {".glb", ".gltf", ".fbx"}) {
        const auto directory = root / (stem + extension + "-bundle");
        fs::create_directories(directory);
        const auto path = directory / (stem + extension);
        exportScene(fixture.scene, path, fixture.options);
        std::vector<std::vector<uint8_t>> images;
        if (path.extension() == ".fbx") {
            const auto imported = loadFbx(path);
            compare(fbxGeometry(*imported), fixture.expected, stem + extension);
            for (const auto* material : imported->materials) {
                const auto* opacity = ufbx_find_prop(&material->props, "Opacity");
                check(opacity && std::abs(opacity->value_real - (fixture.registration ? 1 : .7)) < 1e-6 &&
                          material->fbx.diffuse_color.texture && material->fbx.transparency_color.texture,
                      "pure FBX keeps visible material opacity and connected color/alpha textures");
            }
            std::set<fs::path> required{fs::weakly_canonical(path)};
            images = fbxImages(*imported, path, required);
            bundle(directory, required);
        } else {
            Gltf imported(path);
            compare(imported.geometry(), fixture.expected, stem + extension);
            for (const auto& material : imported.doc.at("materials"))
                check(material.value("alphaMode", "OPAQUE") == (fixture.registration ? "MASK" : "BLEND") &&
                          std::abs(material.at("pbrMetallicRoughness").at("baseColorFactor").at(3)
                                       .get<double>() - (fixture.registration ? 1 : .7)) < 1e-6,
                      "pure glTF keeps opacity/registration cutout material behavior");
            images = imported.images();
            bundle(directory, imported.required);
        }
        check(!images.empty() && std::find(images.begin(), images.end(), fixture.painted) != images.end(),
              "required painted diffuse/number atlas remains connected with exact PNG contents");
        for (const auto& image : images)
            check(image == fixture.painted, "unused and hidden textures are pruned from pure output");
        check(sourceSnapshot(fixture.scene) == before, "export leaves current source scene and cached pose unchanged");
    }
}
Fixture numberFixture(const fs::path& root) {
    Fixture fixture;
    fixture.registration = true;
    fixture.scene.source = root / "inputs" / "numbers.edm";
    fixture.scene.version = 10;
    fixture.scene.materials.push_back(material("registration"));
    auto& mat = fixture.scene.materials[0];
    mat.uvChannels.resize(4, 0);
    mat.uniforms["decalShift"] = {.01, .02};
    TextureRef atlas{3, "registration-atlas"};
    atlas.matrix(0, 3) = .05;
    mat.textures.push_back(atlas);
    node(fixture.scene, "Number helper", -1, {2, -1, .5}, V3::UnitY(), .29, {1.7, .6, 1.2});
    node(fixture.scene, "Number surface", 0, {.2, .3, -.4}, V3::UnitZ(), .41);
    auto mesh = triangle("Number surface", 1);
    mesh.selectors = {0, 0, 0};
    mesh.numbers = {{31, 32, 2, 3}};
    fixture.scene.meshes.push_back(mesh);
    fixture.scene.numberArgs = {31, 32};
    fixture.scene.limits = {{31, {0, 1}}, {32, {0, 1}}};
    finish(fixture.scene);
    fixture.options.modelOnly = true;
    fixture.options.arguments = std::vector<int>{31, 32};
    fixture.options.livery = std::make_shared<Livery>();
    fixture.options.livery->args = {{31, .1}, {32, .2}};
    fixture.options.baseline = {{31, .3}};
    fixture.options.textureDirectory = root / "inputs";
    fixture.painted = PaintImage(20, 20, {199, 81, 47, 170}).pngBytes();
    fs::create_directories(fixture.options.textureDirectory);
    writeFile(fixture.options.textureDirectory / "registration-atlas.png", fixture.painted);
    fixture.options.diffuseOverrides[0] = PaintImage(4, 4, {61, 111, 201, 255}).pngBytes();
    const Mat world = affine({2, -1, .5}, V3::UnitY(), .29, {1.7, .6, 1.2}) *
                      affine({.2, .3, -.4}, V3::UnitZ(), .41);
    auto expected = rigidReference(mesh, world);
    for (size_t i = 0; i < 3; ++i) {
        const auto p = mesh.positions[i], n = mesh.normals[i];
        expected[i].position = (world * V4(double(p[0]) + n[0] * .0002,
            double(p[1]) + n[1] * .0002, double(p[2]) + n[2] * .0002, 1)).head<3>();
        expected[i].uv = {mesh.uvs[0][i][0] + .05 + .01 + .3 * 2,
                           mesh.uvs[0][i][1] + .02 + .2 * 3};
    }
    fixture.expected.push_back(expected);
    return fixture;
}
void staticCompatibility(const Fixture& fixture, const fs::path& root) {
    auto options = fixture.options;
    options.modelOnly = false;
    options.textures = false;
    options.arguments = std::vector<int>{};
    options.diffuseOverrides.clear();
    options.livery.reset();
    for (const auto* extension : {".glb", ".gltf", ".fbx"}) {
        const auto path = root / (std::string("legacy-static") + extension);
        exportScene(fixture.scene, path, options);
        if (path.extension() == ".fbx") {
            const auto imported = loadFbx(path);
            check(imported->bones.count > 0 && imported->skin_deformers.count > 0 &&
                      imported->anim_stacks.count == 0,
                  "legacy --static keeps the skin/skeleton and omits animation clips");
        } else {
            const Gltf imported(path);
            check(imported.doc.contains("skins") && !imported.doc.at("skins").empty() &&
                      imported.doc.value("animations", Json::array()).empty(),
                  "arguments={} with modelOnly=false retains legacy static skin metadata");
        }
    }
}
void emptyAndAtomic(const Fixture& fixture, const fs::path& root) {
    for (const auto* extension : {".glb", ".gltf", ".fbx"}) {
        const auto directory = root / (std::string("atomic") + extension);
        fs::create_directories(directory);
        const auto path = directory / (std::string("existing") + extension);
        exportScene(fixture.scene, path, fixture.options);
        const auto original = snapshot(directory);
        std::atomic_bool cancel = true;
        rejects([&] { exportScene(fixture.scene, path, fixture.options, {}, &cancel); },
                "pre-cancelled pure export is rejected");
        check(snapshot(directory) == original, "pre-cancel preserves previous model and all resources");
        cancel = false;
        rejects([&] { exportScene(fixture.scene, path, fixture.options,
                                  [&](const auto&) { cancel = true; }, &cancel); },
                "mid-export cancellation is observed before publication");
        check(snapshot(directory) == original, "mid-export cancel preserves previous model and all resources");
        rejects([&] { exportScene(fixture.scene, path, fixture.options,
                                  [&](const auto&) { throw std::runtime_error("progress callback failure"); }); },
                "pure export aborts on progress callback failure");
        check(snapshot(directory) == original, "callback failure leaves no partial sidecars or changed model");
        Scene empty;
        empty.source = fixture.scene.source;
        auto options = fixture.options;
        options.arguments.reset();
        options.textures = false;
        options.diffuseOverrides.clear();
        rejects([&] { exportScene(empty, path, options); }, "empty selection reports no exportable model");
        auto hidden = fixture.scene;
        hidden.meshes = {fixture.scene.meshes[3]};
        rejects([&] { exportScene(hidden, path, options); }, "all-hidden selection reports no visible triangles");
        check(snapshot(directory) == original, "empty/all-hidden rejection preserves previous output bundle");
        const auto absent = directory / (std::string("never-created") + extension);
        rejects([&] { exportScene(hidden, absent, options); }, "all-hidden new export is rejected");
        check(!fs::exists(absent) && snapshot(directory) == original,
              "all-hidden new export leaves no misleading empty model or sidecars");
    }
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc >= 4 && std::wstring_view(argv[1]) == L"--lua-worker")
        return luaWorker(argv[2], argv[3]);
    try {
        ComRuntime runtime;
        ComApartment apartment;
        const auto root = fs::current_path() / "validation" / "export-model-only-tests" /
                          std::to_wstring(Clock::now().time_since_epoch().count());
        fs::create_directories(root / "inputs");
        const auto fixture = geometryFixture(root);
        roundtrip(fixture, root, "posed");
        roundtrip(numberFixture(root), root, "numbers");
        staticCompatibility(fixture, root);
        emptyAndAtomic(fixture, root);
        std::cout << "Model-only export: " << checks << " checks passed; maximum analytic world error "
                  << maximumError << " m\n";
        return 0;
    } catch (...) {
        std::cerr << exceptionText() << '\n';
        return 1;
    }
}
