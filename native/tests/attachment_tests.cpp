#include "attachment.h"
#include "export.h"
#include <ufbx.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

using namespace edm;
namespace {
size_t checks = 0;
void check(bool condition, const std::string& message) {
    ++checks;
    require(condition, "Attachment regression: " + message);
}
void expectNear(const Mat& actual, const Mat& expected, const std::string& message) {
    check(actual.allFinite() && (actual - expected).cwiseAbs().maxCoeff() < 1e-9, message);
}
void expectNear(const F3& actual, const V3& expected, const std::string& message) {
    check((V3(actual[0], actual[1], actual[2]) - expected).norm() < 4e-5, message);
}
template <class Function> void fails(Function function, const std::string& message) {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, message);
}
V4 angle(const V3& axis, double radians) {
    Eigen::Quaterniond rotation(Eigen::AngleAxisd(radians, axis.normalized()));
    return rotation.coeffs();
}
int node(Scene& scene, std::string name, int parent, V3 position = V3::Zero(), V4 quaternion = V4(0, 0, 0, 1),
         V3 scale = V3::Ones(), bool connector = false) {
    Node value;
    value.name = std::move(name);
    value.parent = parent;
    value.t = position;
    value.q = quaternion;
    value.s = scale;
    if (connector) {
        value.extras["edm_type"] = "Connector";
        ++scene.connectorCount;
    }
    scene.nodes.push_back(std::move(value));
    return int(scene.nodes.size()) - 1;
}
void finish(Scene& scene) {
    scene.order.clear();
    scene.staticLocal.clear();
    // Fixtures intentionally append parents before children, with multiple roots for skins.
    for (int index = 0; index < int(scene.nodes.size()); ++index) {
        scene.order.push_back(index);
        scene.staticLocal.push_back(scene.nodes[index].local());
    }
    scene.defaultWorld = scene.evaluate({});
}
Mesh triangle(int owner, std::string name) {
    Mesh mesh;
    mesh.name = std::move(name);
    mesh.node = owner;
    mesh.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    mesh.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    mesh.indices = {0, 1, 2};
    mesh.uvs = {{{0, 0}, {1, 0}, {0, 1}}};
    mesh.extras = {{"edm_render_index", 0}, {"edm_parent", 0}};
    return mesh;
}
Scene aircraft() {
    Scene scene;
    scene.source = "aircraft.edm";
    scene.version = 10;
    node(scene, "Aircraft", -1, {12, -3, 8}, angle({0, 1, 0}, .53));
    node(scene, "Pylon rotation", 0);
    node(scene, "Pylon1", 1, {1, -.4, 2}, angle({1, 1, 0}, .23), V3::Ones(), true);
    node(scene, "Pylon2", 0, {-1, -.4, 2}, angle({0, 1, 1}, -.37), V3::Ones(), true);
    scene.tracks.push_back({1, 7, Channel::Rotation, {{0, angle({0, 1, 0}, 0)}, {1, angle({0, 1, 0}, 1.7)}}});
    scene.limits[7] = {0, 1};
    scene.defaultArgs[7] = .25;
    Material material;
    material.name = "body";
    material.source = scene.source;
    material.animatedUniforms = {{"reserved", {{"argument", 80}, {"keys", Json::array()}}}};
    scene.materials.push_back(material);
    scene.meshes.push_back(triangle(0, "aircraft body"));
    scene.sourceNodes = 2;
    scene.heads = {0, 1};
    scene.tails = {0, 1};
    finish(scene);
    return scene;
}
Scene payload() {
    Scene scene;
    scene.source = "payload.edm";
    scene.version = 10;
    node(scene, "Payload", -1, {-1, 2, .3}, angle({1, 0, 0}, .41), {2, .6, -1.3});
    node(scene, "AttachPoint", 0, {.7, -.2, .6}, angle({1, 1, 1}, -.77), V3::Ones(), true);
    node(scene, "Animated fin", 0);
    node(scene, "Point01", 0, {1, 0, -.2}, angle({0, 0, 1}, .19), V3::Ones(), true);
    node(scene, "Skin root", -1);
    node(scene, "Bone", 0, {.3, .2, .1});
    scene.nodes[2].extras = {{"edm_node", 1}, {"edm_argument", 7}, {"edm_channel", "translation"}};
    scene.tracks.push_back({2, 7, Channel::Position, {{0, V4(0, 0, 0, 0)}, {1, V4(0, 0, 2, 0)}}});
    scene.limits[7] = {0, 1};
    Material material;
    material.name = "payload body";
    material.uniforms = {{"diffuseShift", {0., 0.}}};
    material.animatedUniforms = {{"opacityValue", {{"argument", 80}, {"keys", Json::array()}}}};
    scene.materials.push_back(material);
    auto rigid = triangle(2, "payload rigid");
    rigid.numbers = {{8, 9, .25f, .5f}};
    rigid.selectors = {0, 0, 0};
    rigid.extras = {
        {"edm_render_index", 0},
        {"edm_parent", 1},
        {"edm_damage_argument", 10},
        {"edm_properties", {{"number_controls", {{8, .25, 9, .5}}}, {"custom", {{"argument", 80}}}}}};
    scene.meshes.push_back(rigid);
    auto skin = triangle(4, "payload skinned");
    skin.skinNodes = {0, 5};
    skin.inverseBind = {translation({-.1, .3, 0}), translation({.3, 0, -.1})};
    skin.joints = {{0, 1}, {1, 0}, {1, 0}};
    skin.weights = {{.6f, .4f}, {.75f, .25f}, {.2f, .8f}};
    scene.meshes.push_back(skin);
    scene.numberArgs = {8, 9};
    scene.limits[8] = scene.limits[9] = {0, 1};
    scene.sourceNodes = 2;
    scene.heads = {0, 2};
    scene.tails = {0, 2};
    finish(scene);
    return scene;
}
void alignAndFollow() {
    const auto base = aircraft(), child = payload();
    const Args defaults{{7, .8}, {8, .4}, {9, .6}};
    const auto combined = attachScene(base, child, 2, defaults);
    check(combined->attachments.size() == 1, "attachment record exists");
    const auto& record = combined->attachments.front();
    const int offset = int(base.nodes.size()) + 2;
    check(record.root == int(base.nodes.size()) && record.attachNode == offset + 1 &&
              record.targetNode == 2 && record.nodeCount == int(child.nodes.size()) + 2 &&
              record.meshBegin == int(base.meshes.size()) && record.meshCount == int(child.meshes.size()) &&
              record.materialBegin == int(base.materials.size()) &&
              record.materialCount == int(child.materials.size()),
          "append ranges and attachment mount indices are complete");
    check(record.argumentMap.size() == 5 && record.argumentMap.at(7) != 7 && record.argumentMap.at(80) != 80,
          "all track, number, damage and material arguments use isolated indices");
    check(combined->defaultArgs.at(record.argumentMap.at(7)) == .8 &&
              combined->defaultArgs.at(7) == base.defaultArgs.at(7),
          "child default pose retained independently of host defaults");
    check(combined->materials.back().source == child.source &&
              combined->materials.front().source == base.source,
          "material texture ownership survives composition");
    check(combined->nodes[offset + 2].extras.at("edm_argument") == record.argumentMap.at(7) &&
              combined->nodes[offset + 2].extras.at("edm_node") == 3 &&
              combined->meshes[1].extras.at("edm_parent") == 3 &&
              combined->meshes[1].extras.at("edm_damage_argument") == record.argumentMap.at(10) &&
              combined->meshes[1].extras.at("edm_render_index") == 1,
          "source-node and argument metadata references are remapped");
    check(combined->materials.back().animatedUniforms.at("opacityValue").at("argument") ==
                  record.argumentMap.at(80) &&
              combined->meshes[1].extras.at("edm_properties").at("custom").at("argument") ==
                  record.argumentMap.at(80) &&
              combined->meshes[1].extras.at("edm_properties").at("number_controls")[0][0] ==
                  record.argumentMap.at(8),
          "Lua argument metadata and number controls refer to their independent child arguments");
    check(combined->heads == std::vector<int>({0, 1, offset, offset + 2}) &&
              combined->tails == combined->heads,
          "source head/tail lookup retains host entries and offsets children");
    check(combined->meshes[1].selectors == child.meshes[0].selectors &&
              combined->meshes[1].numbers[0].u == record.argumentMap.at(8),
          "number selectors preserve atlas control selection while remapping control arguments");
    Args textureArgs = combined->defaultArgs;
    const auto beforeUV = textureUV(child.meshes[0], child.materials[0], 3, defaults);
    const auto afterUV = textureUV(combined->meshes[1], combined->materials[1], 3, textureArgs);
    check(beforeUV == afterUV, "number-atlas UVs remain identical at remapped default arguments");
    for (int index = 0; index < int(base.nodes.size()); ++index) {
        check(combined->nodes[index].name == base.nodes[index].name &&
                  combined->nodes[index].parent == base.nodes[index].parent &&
                  combined->nodes[index].extras == base.nodes[index].extras,
              "host node indices, parents and metadata unchanged");
        expectNear(combined->staticLocal[index], base.staticLocal[index], "host local transforms unchanged");
    }
    const auto mount = child.evaluate(defaults)[1];
    for (int pose = 0; pose <= 12; ++pose) {
        const double value = pose / 12.;
        Args hostArgs{{7, value}};
        const auto hostWorld = base.evaluate(hostArgs);
        Args combinedArgs{{7, value}, {record.argumentMap.at(7), 1 - value}};
        const auto world = combined->evaluate(combinedArgs);
        Args childArgs = defaults;
        childArgs[7] = 1 - value;
        const auto localWorld = child.evaluate(childArgs);
        const Mat placement = hostWorld[2] * mount.inverse();
        expectNear(world[record.attachNode], hostWorld[2],
                   "full mount position and orientation follow host animation");
        for (size_t index = 0; index < child.nodes.size(); ++index)
            expectNear(world[offset + index], placement * localWorld[index],
                       "two-node affine factorization preserves rotation, shear, reflection and all roots");
        for (size_t mesh = 0; mesh < child.meshes.size(); ++mesh) {
            const auto original = child.transformed(child.meshes[mesh], localWorld);
            const auto attached = combined->transformed(combined->meshes[1 + mesh], world);
            for (size_t vertex = 0; vertex < original.size(); ++vertex) {
                const auto& point = original[vertex];
                const V4 expected = placement * V4(point[0], point[1], point[2], 1);
                expectNear(attached[vertex], expected.head<3>(),
                           "rigid and multi-weight skinned vertices transform exactly once");
            }
        }
    }
    for (size_t joint = 0; joint < child.meshes.back().inverseBind.size(); ++joint)
        expectNear(combined->meshes.back().inverseBind[joint], child.meshes.back().inverseBind[joint],
                   "skin inverse bind remains in source model coordinates");
    check(base.attachments.empty() && child.attachments.empty() && base.nodes.size() == 4 &&
              child.nodes.size() == 6 && child.materials[0].source.empty(),
          "composition does not mutate either source scene");
    const auto explicitDefault = combined->evaluate(combined->defaultArgs);
    for (size_t index = 0; index < combined->nodes.size(); ++index)
        expectNear(combined->defaultWorld[index], explicitDefault[index],
                   "cached default world includes child defaults");
}
void nestedAndRepeated() {
    const auto base = aircraft(), child = payload();
    const auto rack = attachScene(base, child, findConnector(base, "Pylon1"));
    const int rackPoint = findConnector(*rack, "Point01");
    const auto weapon = attachScene(*rack, child, rackPoint);
    check(weapon->attachments.size() == 2 && weapon->attachments[1].targetNode == rackPoint,
          "Point01 mounts payloads with the same semantics as Pylon connectors");
    const auto world = weapon->evaluate({{7, .72}});
    expectNear(world[weapon->attachments[1].attachNode], world[rackPoint],
               "nested payload mount follows the rack point");
    check(weapon->nodes[rackPoint].extras.at("edm_attachment") == 0 &&
              weapon->nodes[weapon->attachments[1].attachNode].extras.at("edm_attachment") == 1,
          "nested payload owner annotations remain separate");
    for (auto [argument, remapped] : weapon->attachments[1].argumentMap) {
        bool unique = remapped != 7 && remapped != 80;
        for (auto [previous, mapped] : weapon->attachments[0].argumentMap)
            unique &= mapped != remapped;
        check(unique, "each attachment has an independent parameter namespace");
    }
    fails([&] { findConnector(*weapon, "Point01"); }, "ambiguous duplicate Point names require node index");
    check(findConnector(*weapon, "#" + std::to_string(rackPoint)) == rackPoint,
          "explicit node index selects duplicate connectors");
    const auto repeated = attachScene(*weapon, child, rackPoint);
    check(repeated->attachments.size() == 3 && repeated->nodes.size() == weapon->nodes.size() + 8,
          "loading an identical EDM again appends a separate instance");
    for (size_t index = 0; index < weapon->nodes.size(); ++index) {
        check(repeated->nodes[index].parent == weapon->nodes[index].parent &&
                  repeated->nodes[index].extras == weapon->nodes[index].extras,
              "repeated load retains every existing index and ownership annotation");
        expectNear(repeated->defaultWorld[index], weapon->defaultWorld[index],
                   "repeated load retains previous mount poses");
    }
    // Compose an already assembled rack and missile as one child of another aircraft.
    const auto assembled = attachScene(child, child, findConnector(child, "Point01"));
    const auto whole = attachScene(base, *assembled, 3);
    check(whole->attachments.size() == 2 && whole->attachments[0].attachNode >= 0 &&
              whole->attachments[1].source == child.source,
          "a preassembled child selects its own AttachPoint instead of its payload's AttachPoint");
    const int remapOffset = int(base.nodes.size()) + 2;
    check(whole->attachments[1].root == assembled->attachments[0].root + remapOffset &&
              whole->attachments[1].targetNode == assembled->attachments[0].targetNode + remapOffset &&
              whole->attachments[1].meshBegin == assembled->attachments[0].meshBegin + 1,
          "preassembled nested attachment records remap all ranges and node indices");
    for (auto [original, remapped] : assembled->attachments[0].argumentMap)
        check(whole->attachments[1].argumentMap.at(original) ==
                  whole->attachments[0].argumentMap.at(remapped),
              "nested original-to-composite parameter mapping is preserved");
    const auto combinedWorld = whole->evaluate({});
    expectNear(combinedWorld[whole->attachments[0].attachNode], combinedWorld[3],
               "assembled child mount aligns to selected host pylon");
    expectNear(combinedWorld[whole->attachments[1].attachNode],
               combinedWorld[whole->attachments[1].targetNode],
               "assembled payload keeps its internal mount alignment");
    check(whole->meshes.back().extras.at("edm_attachment") == 1 &&
              whole->materials.back().extras.at("edm_attachment") == 1,
          "preassembled child mesh and material ownership remaps to nested record");
}
void hiding() {
    const auto base = aircraft(), child = payload();
    const auto first = attachScene(base, child, 2);
    const auto scene = attachScene(*first, child, findConnector(*first, "Point01"));
    const Args args{{7, .83}};
    const auto visible = scene->evaluate(args), hidden = scene->evaluate(args, false);
    for (size_t index = 0; index < base.nodes.size(); ++index)
        expectNear(hidden[index], visible[index], "hiding attachments preserves every aircraft transform");
    for (size_t index = base.nodes.size(); index < hidden.size(); ++index)
        check(hidden[index].block<3, 3>(0, 0).cwiseAbs().maxCoeff() < 1e-12,
              "hide flag collapses every attachment root, bone and child connector");
    for (size_t index = 1; index < scene->meshes.size(); ++index) {
        const auto points = scene->transformed(scene->meshes[index], hidden);
        for (const auto& point : points)
            expectNear(point, V3(points[0][0], points[0][1], points[0][2]),
                       "hidden rigid and skinned surfaces have no remaining triangle area");
    }
    const auto restored = scene->evaluate(args);
    for (size_t index = 0; index < visible.size(); ++index)
        expectNear(restored[index], visible[index], "showing attachments restores the exact prior pose");
    check(scene->summary().at("attachments").size() == 2, "inspection reports attachment metadata");
}
void invalidInputs() {
    const auto base = aircraft();
    auto child = payload();
    fails([&] { attachScene(base, child, -1); }, "negative target rejected");
    fails([&] { attachScene(base, child, int(base.nodes.size())); }, "out of range target rejected");
    fails([&] { attachScene(base, child, 0); }, "ordinary model node is not a connector");
    fails([&] { findConnector(base, "Point01"); }, "missing connector rejected");
    for (const auto* name : {"#-1", "#999999999999999999999999999", "#0", "#2bad", "#"})
        fails([&] { findConnector(base, name); }, "malformed or nonconnector index rejected");
    fails([&] { attachScene(base, child, 2, {{7, std::numeric_limits<double>::quiet_NaN()}}); },
          "nonfinite attachment default rejected");
    node(child, "AttachPoint", 0, V3::Zero(), V4(0, 0, 0, 1), V3::Ones(), true);
    finish(child);
    fails([&] { attachScene(base, child, 2); }, "multiple own AttachPoints rejected without guessing");
    child = payload();
    child.nodes[0].s[0] = 0;
    finish(child);
    fails([&] { attachScene(base, child, 2); }, "singular AttachPoint cannot be inverted");
    child = payload();
    child.staticLocal[1](0, 0) = std::numeric_limits<double>::infinity();
    fails([&] { attachScene(base, child, 2); }, "nonfinite source matrix rejected");
    child = payload();
    child.nodes[0].parent = 2;
    fails([&] { attachScene(base, child, 2); }, "cyclic or invalid scene order rejected before evaluation");
    child = payload();
    child.meshes.back().skinNodes[0] = 999;
    fails([&] { attachScene(base, child, 2); }, "invalid skin bone references rejected");
    child = payload();
    child.nodes[1].name = "OtherMount";
    const auto fallback = attachScene(base, child, 2);
    check(fallback->attachments[0].attachNode == -1 && !fallback->warnings.empty(),
          "missing AttachPoint uses the model origin with an explicit warning");
    const auto childWorld = child.evaluate({});
    const auto hostWorld = base.evaluate({});
    for (size_t index = 0; index < child.nodes.size(); ++index)
        expectNear(fallback->defaultWorld[base.nodes.size() + 2 + index], hostWorld[2] * childWorld[index],
                   "origin fallback retains source root placement");
}
// The reference below derives the fixture's mount/skin formula directly. It deliberately
// does not call attachScene's evaluator or transformed(), so export checks are independent.
std::vector<V3> analyticVertices(const Scene& source, size_t meshIndex, double hostArg, double childArg) {
    const Mat host = translation({12, -3, 8}) * rotation(angle({0, 1, 0}, .53));
    const Mat target = host * rotation(angle({0, 1, 0}, 1.7 * hostArg)) * translation({1, -.4, 2}) *
                       rotation(angle({1, 1, 0}, .23));
    const Mat child = translation({-1, 2, .3}) * rotation(angle({1, 0, 0}, .41)) * scaling({2, .6, -1.3});
    const Mat mount = child * translation({.7, -.2, .6}) * rotation(angle({1, 1, 1}, -.77));
    const Mat placement = target * mount.inverse();
    const auto& mesh = source.meshes.at(meshIndex);
    std::vector<V3> output;
    for (size_t vertex = 0; vertex < mesh.positions.size(); ++vertex) {
        const auto& position = mesh.positions[vertex];
        const V4 input(position[0], position[1], position[2], 1);
        V4 result;
        if (meshIndex == 0)
            result = host * input;
        else if (meshIndex == 1)
            result = placement * child * translation({0, 0, childArg * 2}) * input;
        else {
            const std::array<Mat, 2> palette{
                Mat(placement * child * translation({-.1, .3, 0})),
                Mat(placement * child * translation({.3, .2, .1}) * translation({.3, 0, -.1}))};
            result.setZero();
            for (size_t weight = 0; weight < 8; ++weight)
                if (mesh.weights[vertex][weight] != 0)
                    result += palette[mesh.joints[vertex][weight]] * input * mesh.weights[vertex][weight];
        }
        output.push_back(result.head<3>());
    }
    return output;
}
struct GlbReader {
    Json document;
    std::vector<uint8_t> binary;
    explicit GlbReader(const fs::path& path) {
        const auto file = readFile(path);
        auto word = [&](size_t position) {
            require(position + 4 <= file.size(), "GLB chunk boundary");
            uint32_t value;
            std::memcpy(&value, file.data() + position, 4);
            return value;
        };
        check(word(0) == 0x46546c67 && word(4) == 2 && word(8) == file.size(), "valid actual GLB header");
        const size_t jsonSize = word(12);
        check(word(16) == 0x4e4f534a && 20 + jsonSize + 8 <= file.size(), "valid GLB JSON chunk");
        document = Json::parse(file.begin() + 20, file.begin() + 20 + jsonSize);
        const size_t offset = 20 + jsonSize, length = word(offset);
        check(word(offset + 4) == 0x004e4942 && offset + 8 + length == file.size(), "valid GLB BIN chunk");
        binary.assign(file.begin() + offset + 8, file.end());
    }
    double scalar(size_t accessor, size_t element, size_t component = 0) const {
        const auto& description = document.at("accessors").at(accessor);
        const auto& view = document.at("bufferViews").at(description.at("bufferView").get<size_t>());
        const auto type = description.at("type").get<std::string>();
        const size_t dimensions = type == "MAT4"   ? 16
                                  : type == "VEC4" ? 4
                                  : type == "VEC3" ? 3
                                  : type == "VEC2" ? 2
                                                   : 1;
        const int storage = description.at("componentType").get<int>();
        const size_t bytes = storage == 5123 ? 2 : 4;
        require((storage == 5123 || storage == 5125 || storage == 5126) && component < dimensions &&
                    element < description.at("count").get<size_t>(),
                "GLB accessor bounds/type");
        const size_t offset = view.value("byteOffset", size_t(0)) +
                              description.value("byteOffset", size_t(0)) +
                              element * view.value("byteStride", bytes * dimensions) + bytes * component;
        require(offset + bytes <= binary.size(), "GLB binary accessor boundary");
        if (storage == 5123) {
            uint16_t value;
            std::memcpy(&value, binary.data() + offset, 2);
            return value;
        }
        if (storage == 5125) {
            uint32_t value;
            std::memcpy(&value, binary.data() + offset, 4);
            return value;
        }
        float value;
        std::memcpy(&value, binary.data() + offset, 4);
        return value;
    }
    std::vector<Mat> world(int argument = -1, double time = 0) const {
        auto nodes = document.at("nodes");
        if (argument >= 0) {
            const Json* animation = nullptr;
            for (const auto& item : document.at("animations"))
                if (item.at("extras").at("edm_argument") == argument)
                    animation = &item;
            require(animation, "Missing GLB argument animation");
            for (const auto& channel : animation->at("channels")) {
                const auto& sampler = animation->at("samplers").at(channel.at("sampler").get<size_t>());
                const auto input = sampler.at("input").get<size_t>(),
                           output = sampler.at("output").get<size_t>();
                const auto count = document.at("accessors").at(input).at("count").get<size_t>();
                require(count > 0, "Empty GLB animation sampler");
                size_t first = 0;
                while (first + 1 < count && scalar(input, first + 1) <= time)
                    ++first;
                const size_t second = std::min(first + 1, count - 1);
                const double begin = scalar(input, first), end = scalar(input, second);
                double blend = begin == end ? 0 : std::clamp((time - begin) / (end - begin), 0., 1.);
                if (sampler.value("interpolation", "LINEAR") == "STEP")
                    blend = 0;
                const auto path = channel.at("target").at("path").get<std::string>();
                const size_t dimensions = path == "rotation" ? 4 : 3;
                V4 a = V4::Zero(), b = V4::Zero(), result;
                for (size_t component = 0; component < dimensions; ++component) {
                    a[component] = scalar(output, first, component);
                    b[component] = scalar(output, second, component);
                }
                if (path == "rotation") {
                    const Eigen::Quaterniond qa(a[3], a[0], a[1], a[2]), qb(b[3], b[0], b[1], b[2]);
                    result = qa.normalized().slerp(blend, qb.normalized()).coeffs();
                } else
                    result = a * (1 - blend) + b * blend;
                auto& destination = nodes[channel.at("target").at("node").get<size_t>()][path];
                destination = Json::array();
                for (size_t component = 0; component < dimensions; ++component)
                    destination.push_back(result[component]);
            }
        }
        std::vector<int> parents(nodes.size(), -1), state(nodes.size());
        for (size_t parent = 0; parent < nodes.size(); ++parent)
            if (nodes[parent].contains("children"))
                for (const auto& child : nodes[parent].at("children")) {
                    const size_t index = child.get<size_t>();
                    require(index < nodes.size() && parents[index] == -1,
                            "GLB node has invalid/multiple parents");
                    parents[index] = int(parent);
                }
        std::vector<Mat> result(nodes.size());
        std::function<void(size_t)> evaluate = [&](size_t index) {
            if (state[index] == 2)
                return;
            require(state[index] == 0, "GLB parent cycle");
            state[index] = 1;
            const auto& item = nodes[index];
            const auto t = item.value("translation", std::vector<double>{0, 0, 0});
            const auto q = item.value("rotation", std::vector<double>{0, 0, 0, 1});
            const auto s = item.value("scale", std::vector<double>{1, 1, 1});
            Eigen::Affine3d local = Eigen::Translation3d(t[0], t[1], t[2]) *
                                    Eigen::Quaterniond(q[3], q[0], q[1], q[2]).normalized() *
                                    Eigen::Scaling(s[0], s[1], s[2]);
            result[index] = local.matrix();
            if (parents[index] >= 0) {
                evaluate(size_t(parents[index]));
                result[index] = result[parents[index]] * result[index];
            }
            state[index] = 2;
        };
        for (size_t index = 0; index < nodes.size(); ++index)
            evaluate(index);
        return result;
    }
    std::vector<V3> vertices(int node, const std::vector<Mat>& poses) const {
        const auto& owner = document.at("nodes").at(node);
        const auto& attributes =
            document.at("meshes").at(owner.at("mesh").get<size_t>()).at("primitives").at(0).at("attributes");
        const auto positionAccessor = attributes.at("POSITION").get<size_t>();
        const auto count = document.at("accessors").at(positionAccessor).at("count").get<size_t>();
        std::vector<V3> result;
        for (size_t vertex = 0; vertex < count; ++vertex) {
            V4 position(scalar(positionAccessor, vertex, 0), scalar(positionAccessor, vertex, 1),
                        scalar(positionAccessor, vertex, 2), 1),
                output = V4::Zero();
            if (owner.contains("skin")) {
                const auto& skin = document.at("skins").at(owner.at("skin").get<size_t>());
                for (int set = 0; attributes.contains("WEIGHTS_" + std::to_string(set)); ++set)
                    for (size_t weight = 0; weight < 4; ++weight) {
                        const auto jointAccessor =
                            attributes.at("JOINTS_" + std::to_string(set)).get<size_t>();
                        const auto weightAccessor =
                            attributes.at("WEIGHTS_" + std::to_string(set)).get<size_t>();
                        const double amount = scalar(weightAccessor, vertex, weight);
                        if (amount == 0)
                            continue;
                        const size_t joint = size_t(scalar(jointAccessor, vertex, weight));
                        Mat inverse;
                        for (size_t component = 0; component < 16; ++component)
                            inverse.data()[component] =
                                scalar(skin.at("inverseBindMatrices").get<size_t>(), joint, component);
                        output +=
                            poses.at(skin.at("joints").at(joint).get<size_t>()) * inverse * position * amount;
                    }
            } else
                output = poses.at(node) * position;
            result.push_back(output.head<3>());
        }
        return result;
    }
};
void exportRoundtrip() {
    ComRuntime runtime;
    const auto root = fs::current_path() / "validation" /
                      ("attachment-export-tests-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(root);
    const auto scene = attachScene(aircraft(), payload(), 2, {{7, .8}});
    const int childArgument = scene->attachments[0].argumentMap.at(7);
    ExportOptions options;
    options.textures = false;
    options.arguments = std::vector<int>{7, childArgument};
    options.duration = 2;
    const auto glb = root / "attachment.glb", fbx = root / "attachment.fbx";
    exportScene(*scene, glb, options);
    exportScene(*scene, fbx, options);
    const GlbReader gltf(glb);
    using Imported = std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)>;
    ufbx_load_opts loadOptions{};
    loadOptions.strict = true;
    loadOptions.evaluate_skinning = true;
    loadOptions.target_unit_meters = 1;
    ufbx_error error{};
    Imported imported(ufbx_load_file(pathString(fbx).c_str(), &loadOptions, &error), ufbx_free_scene);
    char detail[1024]{};
    ufbx_format_error(detail, sizeof(detail), &error);
    check(bool(imported), std::string("independent strict FBX attachment load: ") + detail);
    check(imported->anim_stacks.count == 2 && imported->metadata.warnings.count == 0,
          "FBX retains independent host/payload animations with no structural warning");
    double maximum = 0;
    auto compare = [&](const ufbx_scene& fbxPose, int argument, double time, double hostValue,
                       double childValue) {
        const auto gltfPose = gltf.world(argument, time);
        for (size_t meshIndex = 0; meshIndex < scene->meshes.size(); ++meshIndex) {
            const auto& mesh = scene->meshes[meshIndex];
            const auto expected = analyticVertices(*scene, meshIndex, hostValue, childValue);
            const auto actualGlb = gltf.vertices(mesh.node, gltfPose);
            check(actualGlb.size() == expected.size(), "GLB attachment vertex count");
            const auto name = scene->nodes[mesh.node].name + " / geometry 0";
            const auto* fbxNode = ufbx_find_node(&fbxPose, name.c_str());
            check(fbxNode && fbxNode->mesh && fbxNode->mesh->num_indices == mesh.indices.size(),
                  "FBX attachment mesh/triangle count");
            const auto& geometry = *fbxNode->mesh;
            const auto& positions =
                geometry.skinned_position.exists ? geometry.skinned_position : geometry.vertex_position;
            for (size_t vertex = 0; vertex < expected.size(); ++vertex) {
                const double difference = (actualGlb[vertex] - expected[vertex]).norm();
                maximum = std::max(maximum, difference);
                check(difference < 2e-5, "independently decoded GLB matches analytical mounted vertex");
            }
            for (size_t corner = 0; corner < mesh.indices.size(); ++corner) {
                auto position = ufbx_get_vertex_vec3(&positions, corner);
                if (!geometry.skinned_position.exists || geometry.skinned_is_local)
                    position = ufbx_transform_position(&fbxNode->geometry_to_world, position);
                const double difference =
                    (V3(position.x, position.y, position.z) - expected[mesh.indices[corner]]).norm();
                maximum = std::max(maximum, difference);
                check(difference < 2e-5,
                      "independent FBX skinning/hierarchy matches analytical mounted vertex");
            }
        }
    };
    compare(*imported, -1, 0, .25, .8);
    for (int argument : {7, childArgument}) {
        std::ostringstream name;
        name << "Argument " << std::setw(3) << std::setfill('0') << argument;
        const auto* stack = ufbx_find_anim_stack(imported.get(), name.str().c_str());
        check(stack && stack->anim, "FBX mounted argument stack exists");
        for (int sample = 0; sample <= 8; ++sample) {
            const double value = sample / 8., time = value * options.duration;
            ufbx_evaluate_opts evaluate{};
            evaluate.evaluate_skinning = true;
            Imported pose(ufbx_evaluate_scene(imported.get(), stack->anim, time, &evaluate, &error),
                          ufbx_free_scene);
            check(bool(pose), "independent FBX attachment animation evaluation");
            compare(*pose, argument, time, argument == 7 ? value : .25,
                    argument == childArgument ? value : .8);
        }
    }
    std::cout << "Attachment GLB/FBX analytical round-trip: 19 poses, maximum " << maximum << " m\n";
}
} // namespace
int main() {
    try {
        alignAndFollow();
        nestedAndRepeated();
        hiding();
        invalidInputs();
        exportRoundtrip();
        std::cout << "Attachments: " << checks << " checks passed\n";
        return 0;
    } catch (...) {
        std::cerr << exceptionText() << '\n';
        return 1;
    }
}
