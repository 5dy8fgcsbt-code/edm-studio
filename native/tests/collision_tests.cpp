#include "export.h"
#include "paint_decal.h"
#include <ufbx.h>
#include <iostream>
#include <limits>
#include <sstream>

using namespace edm;
namespace {
size_t checks = 0;
void check(bool value, const char* message) {
    require(value, std::string("Collision EDM regression: ") + message);
    ++checks;
}
template <class Function> void rejects(Function action, const char* message) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, message);
}
struct Bytes {
    std::vector<uint8_t> data;
    int version;
    const std::vector<std::string> strings{"model::RootNode",
                                           "model::TransformNode",
                                           "model::ArgAnimationNode",
                                           "model::ArgVisibilityNode",
                                           "model::ShellNode",
                                           "model::RenderNode",
                                           "SHELL_NODES",
                                           "RENDER_NODES",
                                           "VERTEX_FORMAT",
                                           "MATERIAL_NAME",
                                           "NAME",
                                           "def_material",
                                           "Unused visual material"};
    template <class T> void value(T v) {
        const auto* first = reinterpret_cast<const uint8_t*>(&v);
        data.insert(data.end(), first, first + sizeof(v));
    }
    void u(uint32_t v) {
        value(v);
    }
    void d(double v) {
        value(v);
    }
    void literal(const std::string& text) {
        u(uint32_t(text.size()));
        data.insert(data.end(), text.begin(), text.end());
    }
    void str(const std::string& text) {
        if (version == 8) {
            literal(text);
            return;
        }
        auto it = std::find(strings.begin(), strings.end(), text);
        require(it != strings.end(), "Fixture string table is incomplete");
        u(uint32_t(it - strings.begin()));
    }
    void base(const char* name) {
        literal(name);
        u(0);
        u(0);
    }
    void matrix(const V3& offset = V3::Zero()) {
        Mat transform = Mat::Identity();
        transform.block<3, 1>(0, 3) = offset;
        for (int index = 0; index < 16; ++index)
            d(transform.data()[index]);
    }
    explicit Bytes(int v) : version(v) {
        data = {'E', 'D', 'M'};
        value(uint16_t(version));
        if (version == 10) {
            std::string table;
            for (const auto& text : strings) {
                table += text;
                table += '\0';
            }
            literal(table);
        }
    }
};
struct Fixture {
    int version = 8, indexWidth = 2;
    bool originalMaterial = false, visualMesh = false, secondShell = false, shellInRenderCategory = false;
    uint32_t parent = 2, stride = 3;
    std::vector<uint8_t> format = [] {
        std::vector<uint8_t> f(26);
        f[0] = 3;
        return f;
    }();
    std::vector<float> vertices{0, 0, 0, 2, 0, 0, 0, 3, 0};
    std::vector<uint32_t> indices{0, 1, 2};
};
void geometry(Bytes& out, const Fixture& fixture) {
    require(fixture.stride && fixture.vertices.size() % fixture.stride == 0, "Invalid test vertex rows");
    out.u(uint32_t(fixture.vertices.size() / fixture.stride));
    out.u(fixture.stride);
    for (float component : fixture.vertices)
        out.value(component);
    out.value(uint8_t(fixture.indexWidth == 2 ? 1 : 2));
    out.u(uint32_t(fixture.indices.size()));
    out.u(5); // Triangle index primitive descriptor used by original EDM writers.
    for (uint32_t index : fixture.indices)
        if (fixture.indexWidth == 2)
            out.value(uint16_t(index));
        else
            out.u(index);
}
std::vector<uint8_t> fixtureBytes(const Fixture& fixture) {
    // Original, tiny EDM fixture based on the documented ShellNode record:
    // base, parent, its OWN vertex format, vertices, indices. No DCS assets are copied.
    Bytes out(fixture.version);
    out.u(0);
    out.u(0);
    out.str("model::RootNode");
    out.base("Original collision fixture");
    if (fixture.version == 8)
        out.value(uint8_t(0));
    for (int i = 0; i < 18; ++i)
        out.d(0);
    out.u(fixture.originalMaterial || fixture.visualMesh ? 1 : 0);
    if (fixture.originalMaterial || fixture.visualMesh) {
        out.u(3);
        out.str("VERTEX_FORMAT");
        out.u(5);
        for (uint8_t component : {4, 3, 0, 0, 2})
            out.value(component);
        out.str("MATERIAL_NAME");
        out.str("def_material");
        out.str("NAME");
        out.str("Unused visual material");
    }
    out.u(0);
    out.u(0);
    out.u(4);
    out.str("model::TransformNode");
    out.base("Host transform");
    out.matrix(V3(4, 5, 6));
    out.str("model::ArgAnimationNode");
    out.base("Shell hinge");
    out.matrix();
    for (double v : {0., 0., 0., 0., 0., 0., 1., 0., 0., 0., 1., 1., 1., 1.})
        out.d(v);
    out.u(1);
    out.u(7);
    out.u(2);
    for (double v : {0., 0., 0., 0., 1., 2., 0., 0.})
        out.d(v);
    out.u(0);
    out.u(0);
    out.str("model::ArgVisibilityNode");
    out.base("Shell visibility");
    out.u(1);
    out.u(38);
    out.u(1);
    out.d(0);
    out.d(.5);
    out.str("model::TransformNode");
    out.base("Separate shell transform");
    out.matrix(V3(-2, 1, 0));
    for (int32_t parent : {-1, 0, 1, 0})
        out.value(parent);
    out.u(fixture.visualMesh ? 2 : 1);
    if (fixture.visualMesh) {
        out.str("RENDER_NODES");
        out.u(1);
        out.str("model::RenderNode");
        out.base("Visible body");
        out.u(0);
        out.u(0);
        out.u(1);
        out.u(0);
        out.value(int32_t(-1));
        Fixture visual;
        visual.stride = 9;
        visual.vertices = {0, 0, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 1, 1, 0, 0, 3, 0, 0, 0, 0, 1, 0, 1};
        geometry(out, visual);
    }
    out.str(fixture.shellInRenderCategory ? "RENDER_NODES" : "SHELL_NODES");
    out.u(fixture.secondShell ? 2 : 1);
    const auto shell = [&](const char* name, uint32_t parent) {
        out.str("model::ShellNode");
        out.base(name);
        out.u(parent);
        out.u(uint32_t(fixture.format.size()));
        for (uint8_t component : fixture.format)
            out.value(component);
        geometry(out, fixture);
    };
    shell("Hull A", fixture.parent);
    if (fixture.secondShell)
        shell("Hull B", 3);
    return out.data;
}
std::shared_ptr<Scene> loadFixture(const fs::path& root, const std::string& name, const Fixture& fixture) {
    const auto file = root / (name + ".edm");
    writeFile(file, fixtureBytes(fixture));
    return Scene::load(file);
}
V3 vec(const F3& p) {
    return {p[0], p[1], p[2]};
}
void pointSet(std::vector<V3> actual, const std::vector<V3>& expected, const char* message) {
    check(actual.size() == expected.size(), "Triangle retains its original corner count");
    for (const auto& point : expected) {
        auto found = std::find_if(actual.begin(), actual.end(),
                                  [&](const V3& value) { return (value - point).norm() < 2e-5; });
        check(found != actual.end(), message);
        actual.erase(found);
    }
}
std::vector<V3> corners(const Scene& scene, const Mesh& mesh, const Args& args = {}) {
    const auto vertices = scene.transformed(mesh, scene.evaluate(args));
    std::vector<V3> result;
    for (uint32_t index : mesh.indices)
        result.push_back(vec(vertices.at(index)));
    return result;
}
const Mesh& hull(const Scene& scene, const std::string& name) {
    auto found = std::find_if(scene.meshes.begin(), scene.meshes.end(),
                              [&](const Mesh& mesh) { return mesh.name == name; });
    require(found != scene.meshes.end(), "Collision mesh name was not preserved");
    return *found;
}
void loading(const fs::path& root) {
    for (auto [version, width] : {std::pair{8, 2}, std::pair{10, 4}}) {
        Fixture fixture;
        fixture.version = version;
        fixture.indexWidth = width;
        fixture.secondShell = true;
        auto scene = loadFixture(root, "shell-" + std::to_string(version), fixture);
        check(scene->collisionOnly() && scene->collisionCount == 2 && scene->meshes.size() == 2,
              "A collision-only EDM with no source materials loads both separated shells");
        check(scene->materials.size() == 2, "Each shell receives an independent synthetic material");
        for (const auto& mesh : scene->meshes) {
            const auto& material = scene->materials.at(mesh.material);
            check(mesh.extras.value("edm_is_collision", false) &&
                      scene->nodes.at(mesh.node).extras.value("edm_is_collision", false) &&
                      material.extras.value("edm_is_collision", false),
                  "Collision provenance is retained on mesh, graph node and synthetic material");
            check(mesh.uvs.empty() && material.textures.empty() && material.culling == 1,
                  "Shells are untextured and double-sided rather than borrowing a visual material");
            const auto color = materialColor(material);
            check(std::all_of(color.begin(), color.end(),
                              [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; }) &&
                      color[3] == 1 && std::max({color[0], color[1], color[2]}) > .1f,
                  "Generated shell colors are finite, visible and opaque");
            check(mesh.normals.size() == mesh.positions.size(),
                  "Position-only shells gain renderable normals");
            for (const auto& normal : mesh.normals)
                check(vec(normal).allFinite() && std::abs(vec(normal).norm() - 1) < 1e-5,
                      "Generated collision normals are finite and normalized");
        }
        pointSet(corners(*scene, hull(*scene, "Hull A")), {{4, 5, 6}, {6, 5, 6}, {4, 8, 6}},
                 "Shell uses its source parent transform");
        pointSet(corners(*scene, hull(*scene, "Hull A"), {{7, .5}}), {{5, 5, 6}, {7, 5, 6}, {5, 8, 6}},
                 "Shell follows its parent animation at an intermediate value");
        pointSet(corners(*scene, hull(*scene, "Hull B")), {{2, 6, 6}, {4, 6, 6}, {2, 9, 6}},
                 "Separated shells retain their different source parents");
        const auto hidden = corners(*scene, hull(*scene, "Hull A"), {{38, .75}});
        check((hidden[1] - hidden[0]).cross(hidden[2] - hidden[0]).squaredNorm() < 1e-20,
              "Parent visibility gates hide collision triangles");
        check(scene->limits.contains(7) && scene->limits.contains(38),
              "Collision animation arguments remain available to the viewer and exporters");
    }
    Fixture ownFormat;
    ownFormat.originalMaterial = true;
    ownFormat.stride = 8;
    ownFormat.format[1] = 3;
    ownFormat.format[4] = 2;
    ownFormat.vertices = {0, 0, 0, 0, 0, 1, .1f, .2f, 2, 0, 0, 0, 0, 1, .8f, .2f, 0, 3, 0, 0, 0, 1, .1f, .9f};
    auto independent = loadFixture(root, "own-format", ownFormat);
    check(
        independent->materials.size() == 1 && independent->materials[0].name != "Unused visual material" &&
            independent->meshes[0].uvs.empty(),
        "Unused material stride and embedded UV channels do not turn a shell into textured visual geometry");
    pointSet(corners(*independent, independent->meshes[0]), {{4, 5, 6}, {6, 5, 6}, {4, 8, 6}},
             "Shell positions follow its own eight-float vertex layout");
    Fixture inRender;
    inRender.shellInRenderCategory = true;
    check(loadFixture(root, "shell-in-render-category", inRender)->collisionOnly(),
          "A ShellNode in the render category remains a collision-only fallback");
    Fixture visual;
    visual.visualMesh = true;
    visual.secondShell = true;
    auto body = loadFixture(root, "body-and-shells", visual);
    check(!body->collisionOnly() && body->meshes.size() == 1 && body->meshes[0].name == "Visible body" &&
              body->collisionCount == 2,
          "A normal visual model retains its single visual mesh while source shell statistics remain "
          "available");
}

void paintExclusion(const fs::path& root) {
    auto scene = loadFixture(root, "unpaintable-shell", {});
    PaintSurface surface(scene);
    PaintCanvas canvas(PaintImage(16, 16, {0, 0, 0, 91}));
    const auto before = canvas.image().rgba;
    PaintHit hit;
    hit.position = V3(4.5, 5.5, 6);
    hit.normal = V3::UnitZ();
    hit.material = 0;
    PaintBrush brush;
    brush.radius = 10;
    canvas.beginStroke();
    check(paintBrush(canvas, surface, hit, brush).pixels == 0 && !canvas.endStroke(),
          "A visible shell without UVs cannot receive direct brush pixels");
    SurfaceDecalOptions decal;
    decal.center = V3(5, 6, 6);
    decal.width = decal.height = 10;
    decal.depth = 1;
    decal.material = 0;
    PaintImage red(1, 1, {255, 0, 0, 255});
    check(findDecalMaterials(surface, decal).materials.empty() &&
              !applySurfaceDecal(surface, canvas, red, decal).changed,
          "Automatic surface decals do not create a canvas target for collision geometry");
    CameraProjectionOptions projection;
    projection.material = 0;
    projection.eye = V3(5, 6, 20);
    projection.viewProjection = Mat::Identity();
    projection.viewProjection(0, 0) = projection.viewProjection(1, 1) = .25;
    projection.viewProjection(2, 2) = .02;
    projection.viewProjection(0, 3) = -1;
    projection.viewProjection(1, 3) = -1.5;
    check(findProjectionMaterials(surface, projection).materials.empty() &&
              !applyCameraProjection(surface, canvas, red, projection).changed &&
              canvas.image().rgba == before,
          "An in-view collision triangle remains unpaintable by camera projection");
}

std::vector<uint8_t> decode64(std::string_view text) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t bits = 0;
    int available = 0;
    std::vector<uint8_t> result;
    for (char c : text) {
        if (c == '=')
            break;
        auto digit = alphabet.find(c);
        require(digit != std::string_view::npos, "Invalid exported base64");
        bits = (bits << 6) | uint32_t(digit);
        available += 6;
        if (available >= 8) {
            available -= 8;
            result.push_back(uint8_t(bits >> available));
        }
    }
    return result;
}
struct Gltf {
    Json doc;
    std::vector<uint8_t> binary;
    explicit Gltf(const fs::path& path) {
        auto bytes = readFile(path);
        if (path.extension() == ".gltf") {
            doc = Json::parse(bytes);
            const auto uri = doc.at("buffers")[0].at("uri").get<std::string>();
            const auto comma = uri.find(',');
            require(comma != std::string::npos && uri.substr(0, comma).ends_with(";base64"),
                    "Missing glTF buffer");
            binary = decode64(std::string_view(uri).substr(comma + 1));
        } else {
            const auto word = [&](size_t offset) {
                require(offset + 4 <= bytes.size(), "Truncated GLB header");
                uint32_t value;
                std::memcpy(&value, bytes.data() + offset, 4);
                return value;
            };
            check(word(0) == 0x46546c67 && word(4) == 2 && word(8) == bytes.size(),
                  "GLB header describes the real file");
            const auto length = word(12);
            require(word(16) == 0x4e4f534a && size_t(length) + 28 <= bytes.size(), "Invalid GLB JSON chunk");
            doc = Json::parse(bytes.begin() + 20, bytes.begin() + 20 + length);
            require(word(24 + length) == 0x004e4942, "Missing GLB binary chunk");
            binary.assign(bytes.begin() + 28 + length, bytes.end());
        }
    }
    std::vector<float> floats(int accessor) const {
        const auto& a = doc.at("accessors").at(accessor);
        const auto& view = doc.at("bufferViews").at(a.at("bufferView").get<int>());
        require(a.at("componentType") == 5126, "Expected float accessor");
        const auto type = a.at("type").get<std::string>();
        const size_t components = type == "SCALAR" ? 1 : type == "VEC3" ? 3 : 4;
        const size_t count = a.at("count").get<size_t>() * components;
        const size_t start = view.value("byteOffset", size_t(0)) + a.value("byteOffset", size_t(0));
        require(start <= binary.size() && count <= (binary.size() - start) / 4,
                "Exported accessor exceeds binary buffer");
        std::vector<float> result(count);
        std::memcpy(result.data(), binary.data() + start, count * 4);
        return result;
    }
};
void inspectGltf(const fs::path& path) {
    Gltf file(path);
    check(file.doc["meshes"].size() == 1 && file.doc["animations"].size() == 2,
          "glTF container retains a shell and both parent animations");
    const auto& primitive = file.doc["meshes"][0]["primitives"][0];
    check(primitive.value("mode", 4) == 4 && !primitive["attributes"].contains("TEXCOORD_0"),
          "glTF collision primitives remain triangles without fabricated UV attributes");
    auto positions = file.floats(primitive["attributes"]["POSITION"]);
    std::vector<V3> points;
    for (size_t i = 0; i < positions.size(); i += 3)
        points.emplace_back(positions[i], positions[i + 1], positions[i + 2]);
    pointSet(points, {{0, 0, 0}, {2, 0, 0}, {0, 3, 0}},
             "glTF accessor bytes contain the original shell triangle");
    check(file.doc["materials"][primitive["material"].get<int>()].value("doubleSided", false),
          "glTF keeps the synthetic shell material double-sided");
    std::vector<int> parents(file.doc["nodes"].size(), -1);
    int meshNode = -1;
    for (int i = 0; i < int(parents.size()); ++i) {
        const auto& node = file.doc["nodes"][i];
        if (node.contains("mesh") && node["mesh"] == 0)
            meshNode = i;
        for (const auto& child : node.value("children", Json::array()))
            parents.at(child.get<int>()) = i;
    }
    require(meshNode >= 0, "Exported shell lacks a scene instance");
    std::set<int> ancestors;
    for (int n = meshNode; n >= 0; n = parents[n])
        require(ancestors.insert(n).second, "Exported node cycle");
    bool moves = false, hides = false;
    for (const auto& animation : file.doc["animations"])
        for (const auto& channel : animation["channels"]) {
            if (!ancestors.contains(channel["target"]["node"].get<int>()))
                continue;
            const auto& sampler = animation["samplers"][channel["sampler"].get<int>()];
            const auto output = file.floats(sampler["output"]);
            const auto input = file.floats(sampler["input"]);
            check(!input.empty() && input.front() == 0 && input.back() == 2,
                  "Collision clip samplers use the selected seconds duration");
            if (animation["name"] == "Argument 007" && channel["target"]["path"] == "translation")
                moves |= output.size() >= 6 && output[0] == 0 && output[output.size() - 3] == 2;
            if (animation["name"] == "Argument 038" && channel["target"]["path"] == "scale")
                hides |= sampler.value("interpolation", "") == "STEP" && output[0] == 1 && output.back() == 0;
        }
    check(moves && hides, "glTF translation and STEP visibility samplers target actual shell ancestors");
}
using Fbx = std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)>;
std::vector<V3> fbxCorners(const ufbx_scene& scene) {
    std::vector<V3> points;
    for (const auto* node : scene.nodes)
        if (node->mesh) {
            check(node->mesh->num_faces == 1 && node->mesh->num_indices == 3,
                  "FBX contains one real triangle");
            check(!node->mesh->vertex_uv.exists, "FBX collision mesh does not fabricate a UV layer");
            for (size_t i = 0; i < node->mesh->num_indices; ++i) {
                const auto local = ufbx_get_vertex_vec3(&node->mesh->vertex_position, i);
                const auto world = ufbx_transform_position(&node->geometry_to_world, local);
                points.emplace_back(world.x, world.y, world.z);
            }
        }
    return points;
}
void exports(const fs::path& root) {
    Fixture fixture;
    fixture.version = 10;
    fixture.indexWidth = 4;
    auto scene = loadFixture(root, "export-shell", fixture);
    ExportOptions options;
    options.textures = false;
    options.duration = 2;
    options.arguments = std::vector<int>{7, 38};
    for (const auto* extension : {".glb", ".gltf"}) {
        const auto path = root / (std::string("shell") + extension);
        exportScene(*scene, path, options);
        inspectGltf(path);
    }
    options.baseline = {{7, .5}, {38, 0}};
    const auto objPath = root / "shell.obj";
    exportScene(*scene, objPath, options);
    const auto objBytes = readFile(objPath);
    std::istringstream obj(std::string(objBytes.begin(), objBytes.end()));
    std::string line, materialFile;
    std::vector<V3> vertices;
    size_t faces = 0;
    while (std::getline(obj, line)) {
        std::istringstream fields(line);
        std::string kind;
        fields >> kind;
        if (kind == "v") {
            V3 p;
            require(bool(fields >> p[0] >> p[1] >> p[2]), "Malformed OBJ vertex");
            vertices.push_back(p);
        }
        if (kind == "f")
            ++faces;
        if (kind == "mtllib")
            fields >> materialFile;
    }
    check(faces == 1 && !materialFile.empty(), "OBJ emits one posed collision triangle with its MTL");
    pointSet(vertices, {{5, 5, 6}, {7, 5, 6}, {5, 8, 6}}, "OBJ bakes the selected collision parent pose");
    const auto mtlBytes = readFile(objPath.parent_path() / wide(materialFile));
    const std::string mtl(mtlBytes.begin(), mtlBytes.end());
    check(mtl.find("Kd ") != std::string::npos && mtl.find("map_Kd") == std::string::npos,
          "OBJ uses a color material without phantom collision textures");
    options.baseline.clear();
    const auto fbxPath = root / "shell.fbx";
    exportScene(*scene, fbxPath, options);
    ufbx_load_opts load{};
    load.strict = true;
    load.target_unit_meters = 1;
    ufbx_error error{};
    Fbx fbx(ufbx_load_file(pathString(fbxPath).c_str(), &load, &error), ufbx_free_scene);
    check(bool(fbx) && fbx->anim_stacks.count == 2,
          "Independent ufbx imports both collision animation stacks");
    pointSet(fbxCorners(*fbx), {{4, 5, 6}, {6, 5, 6}, {4, 8, 6}},
             "FBX baseline preserves the shell's parent transform");
    for (const auto* name : {"Argument 007", "Argument 038"}) {
        auto* stack = ufbx_find_anim_stack(fbx.get(), name);
        require(stack != nullptr, "Collision FBX animation stack missing");
        ufbx_evaluate_opts eval{};
        Fbx pose(ufbx_evaluate_scene(fbx.get(), stack->anim, 1.5, &eval, &error), ufbx_free_scene);
        require(bool(pose), "Independent FBX animation evaluation failed");
        const auto points = fbxCorners(*pose);
        if (std::string(name) == "Argument 007")
            pointSet(points, {{5.5, 5, 6}, {7.5, 5, 6}, {5.5, 8, 6}},
                     "FBX evaluates the shell's moving parent independently");
        else
            check(points.size() == 3 &&
                      (points[1] - points[0]).cross(points[2] - points[0]).squaredNorm() < 1e-18,
                  "FBX parent visibility removes the shell area in an independently evaluated pose");
    }
    Fixture visual;
    visual.visualMesh = true;
    visual.secondShell = true;
    auto normal = loadFixture(root, "normal-export", visual);
    const auto normalPath = root / "normal.glb";
    ExportOptions visualOptions;
    visualOptions.textures = false;
    visualOptions.arguments = std::vector<int>{};
    exportScene(*normal, normalPath, visualOptions);
    check(Gltf(normalPath).doc["meshes"].size() == 1,
          "Normal visual export does not acquire collision triangles");
}

void malformed(const fs::path& root) {
    const auto fail = [&](Fixture fixture, const char* label) {
        rejects([&] { loadFixture(root, label, fixture); }, label);
    };
    Fixture fixture;
    fixture.parent = 9999;
    fail(fixture, "invalid-parent");
    fixture = {};
    fixture.format[0] = 4;
    fail(fixture, "format-stride-mismatch");
    fixture = {};
    fixture.format[0] = 2;
    fixture.format[1] = 1;
    fail(fixture, "missing-three-component-position");
    fixture = {};
    fixture.vertices[0] = std::numeric_limits<float>::infinity();
    fail(fixture, "nonfinite-position");
    fixture = {};
    fixture.vertices.insert(fixture.vertices.end(), {std::numeric_limits<float>::quiet_NaN(), 0, 0});
    fail(fixture, "nonfinite-unreferenced-vertex");
    fixture = {};
    fixture.indices[2] = 3;
    fail(fixture, "out-of-range-index");
    fixture = {};
    fixture.indices.pop_back();
    fail(fixture, "nontriangle-index-count");
    fixture = {};
    fixture.vertices.clear();
    fixture.indices.clear();
    fail(fixture, "empty-shell");
    auto bytes = fixtureBytes({});
    bytes.pop_back();
    const auto path = root / "truncated.edm";
    writeFile(path, bytes);
    rejects([&] { Scene::load(path); }, "Truncated shell indices cannot escape the mapped file");
}
} // namespace

int wmain() {
    try {
        ComRuntime runtime;
        const auto root =
            fs::temp_directory_path() / (L"edm-collision-tests-" + std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(root);
        loading(root);
        paintExclusion(root);
        exports(root);
        malformed(root);
        std::cout << "Collision EDM: " << checks << " checks passed\n";
        return 0;
    } catch (...) {
        std::cerr << exceptionText() << '\n';
        return 1;
    }
}
