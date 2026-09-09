#include "attachment.h"
#include "paint_assembly.h"
#include <iostream>

using namespace edm;
namespace {
int checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    require(condition, std::string("Detachment regression: ") + message);
}
template <class Function> void fails(Function function, const char* message) {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, message);
}
Scene fixture(const fs::path& path) {
    Scene scene;
    scene.source = path;
    scene.version = 10;
    auto node = [&](std::string name, int parent, V3 position, bool connector = false) {
        Node value;
        value.name = std::move(name);
        value.parent = parent;
        value.t = position;
        value.extras["edm_node"] = scene.nodes.size();
        if (connector) {
            value.extras["edm_type"] = "Connector";
            ++scene.connectorCount;
        }
        scene.nodes.push_back(value);
    };
    node("Root", -1, {.5, 1, .3});
    node("AttachPoint", 0, {.2, 0, 0}, true);
    node("Pylon1", 0, {1, 0, 0}, true);
    node("Pylon2", 0, {-1, 0, 0}, true);
    node("Point01", 0, {0, .4, .3}, true);
    node("Motion", 0, V3::Zero());
    node("Skin", -1, V3::Zero());
    scene.tracks.push_back({5, 3, Channel::Position, {{0, V4(0, 0, 0, 0)}, {1, V4(0, .5, 0, 0)}}});
    scene.limits = {{3, {0, 1}}, {8, {0, 1}}, {9, {0, 1}}};
    scene.defaultArgs = {{3, .2}, {8, .3}};
    scene.numberArgs = {8, 9};
    for (int index = 0; index < 2; ++index) {
        Material material;
        material.name = pathString(path.stem()) + "_" + std::to_string(index);
        material.source = path;
        material.textures.push_back({0, material.name});
        scene.materials.push_back(material);
        Mesh mesh;
        mesh.name = material.name;
        mesh.node = index ? 6 : 5;
        mesh.material = index;
        mesh.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        mesh.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
        mesh.indices = {0, 1, 2};
        mesh.uvs = {{{0, 0}, {1, 0}, {0, 1}}};
        mesh.extras = {{"edm_render_index", index}, {"edm_parent", mesh.node}};
        if (index) {
            mesh.skinNodes = {0, 5};
            mesh.inverseBind = {Mat::Identity(), translation({0, -.2, 0})};
            mesh.joints = {{0, 1}, {1, 0}, {0, 1}};
            mesh.weights = {{.5f, .5f}, {.25f, .75f}, {.75f, .25f}};
        } else {
            mesh.numbers = {{8, 9, .5f, .25f}};
            mesh.selectors = {0, 0, 0};
            mesh.extras["edm_properties"]["number_controls"] = {{8, .5, 9, .25}};
        }
        scene.meshes.push_back(std::move(mesh));
    }
    for (int index = 0; index < int(scene.nodes.size()); ++index) {
        scene.order.push_back(index);
        scene.heads.push_back(index);
        scene.tails.push_back(index);
        scene.staticLocal.push_back(scene.nodes[index].local());
    }
    scene.sourceNodes = int(scene.nodes.size());
    scene.collisionCount = 2;
    scene.renderTypes = {{"RenderNode", 1}, {"SkinNode", 1}};
    scene.defaultWorld = scene.evaluate({});
    return scene;
}
int point(const Scene& scene, int owner) {
    for (int index = 0; index < int(scene.nodes.size()); ++index)
        if (isConnector(scene.nodes[index]) && scene.nodes[index].name == "Point01" &&
            scene.nodes[index].extras.value("edm_attachment", -1) == owner)
            return index;
    throw std::runtime_error("Point missing");
}
void compareSurvivors(const Scene& before, const DetachResult& result) {
    const auto& after = *result.scene;
    for (int pose = 0; pose < 7; ++pose) {
        Args args = before.defaultArgs;
        for (auto [argument, limits] : before.limits)
            args[argument] = std::fmod(pose * .17 + argument * .13, 1.);
        const auto oldWorld = before.evaluate(args), newWorld = after.evaluate(args);
        for (size_t node = 0; node < before.nodes.size(); ++node)
            if (result.nodeMap[node] >= 0)
                check((oldWorld[node] - newWorld[result.nodeMap[node]]).cwiseAbs().maxCoeff() < 1e-12,
                      "every surviving animated node keeps its world pose");
        for (size_t mesh = 0; mesh < before.meshes.size(); ++mesh)
            if (result.meshMap[mesh] >= 0) {
                const auto& current = after.meshes[result.meshMap[mesh]];
                const auto oldPositions = before.transformed(before.meshes[mesh], oldWorld);
                const auto newPositions = after.transformed(current, newWorld);
                check(oldPositions == newPositions, "surviving rigid/skinned vertices are byte-identical");
                check(textureUV(before.meshes[mesh], before.materials[before.meshes[mesh].material], 3,
                                args) == textureUV(current, after.materials[current.material], 3, args),
                      "surviving dynamic-number arguments and atlas selectors retain their values");
            }
    }
    for (int argument : result.removedArguments)
        check(!after.limits.contains(argument) && !after.defaultArgs.contains(argument) &&
                  std::find(after.numberArgs.begin(), after.numberArgs.end(), argument) ==
                      after.numberArgs.end(),
              "removed payload parameters leave no track limits, defaults or number controls");
}
void core(const fs::path& root) {
    const auto base = fixture(root / "aircraft.edm"), rack = fixture(root / "rack.edm"),
               middle = fixture(root / "middle.edm"), leaf = fixture(root / "leaf.edm");
    auto assembly = attachScene(base, rack, findConnector(base, "Pylon1"));
    assembly = attachScene(*assembly, middle, findConnector(base, "Pylon2"));
    assembly = attachScene(*assembly, leaf, point(*assembly, 0));
    const auto original = paintAssemblyMetadata(*assembly);
    const auto detached = detachScene(*assembly, findConnector(base, "Pylon2"));
    check(detached.removedCount == 1 && detached.scene->attachments.size() == 2 &&
              detached.attachmentMap == std::vector<int>({0, -1, 1}),
          "middle attachment removed without its siblings");
    check(detached.materialMap[0] == 0 && detached.materialMap[1] == 1 && detached.materialMap[4] == -1 &&
              detached.materialMap[6] == 4,
          "base materials keep their prefix and surviving paint keys compact predictably");
    check(detached.scene->attachments[1].argumentMap == assembly->attachments[2].argumentMap,
          "surviving attachment preserves original argument IDs, including gaps");
    check(detached.scene->sourceNodes == base.sourceNodes * 3 && detached.scene->collisionCount == 6 &&
              detached.scene->renderTypes.at("RenderNode") == 3 && detached.scene->connectorCount == 12,
          "source node, render, collision and connector counts exclude the removed object");
    compareSurvivors(*assembly, detached);
    check(paintAssemblyMetadata(*assembly) == original, "detachment never changes the input scene");
    const auto onlyLeaf = detachScene(*assembly, point(*assembly, 0));
    check(onlyLeaf.removedCount == 1 && onlyLeaf.scene->attachments[0].source == rack.source,
          "leaf removal retains its parent rack");
    compareSurvivors(*assembly, onlyLeaf);
    const auto cascade = detachScene(*assembly, findConnector(base, "Pylon1"));
    check(cascade.removedCount == 2 && cascade.scene->attachments.size() == 1 &&
              cascade.scene->attachments[0].source == middle.source,
          "unloading a rack recursively removes all descendants and preserves its sibling");
    compareSurvivors(*assembly, cascade);
    auto shared = attachScene(*assembly, leaf, findConnector(base, "Pylon1"));
    const auto allDirect = detachScene(*shared, findConnector(base, "Pylon1"));
    check(allDirect.removedCount == 3, "one connector unloads all direct payload roots plus descendants");
    auto again = attachScene(*detached.scene, middle, findConnector(base, "Pylon2"));
    check(again->attachments.size() == 3 &&
              again->attachments[1].argumentMap == assembly->attachments[2].argumentMap,
          "a new attachment after unloading does not renumber surviving controls");
    const auto empty = detachScene(base, findConnector(base, "Pylon1"));
    check(empty.removedCount == 0 && empty.scene->meshes.size() == base.meshes.size(),
          "an unoccupied connector is a no-op");
    fails([&] { detachScene(*assembly, 0); }, "nonconnector unload target rejected");
    fails([&] { detachScene(*assembly, -1); }, "negative unload target rejected");
    std::map<int, int> forced{{3, 1003}, {8, 1008}, {9, 1009}};
    auto restoredMap = attachScene(base, rack, 2, {}, forced);
    check(restoredMap->attachments[0].argumentMap == forced, "saved sparse parameter maps restore exactly");
    fails([&] { attachScene(base, rack, 2, {}, std::map<int, int>{{3, 1003}}); },
          "incomplete forced map rejected");
    fails([&] { attachScene(base, rack, 2, {}, std::map<int, int>{{3, 3}, {8, 1008}, {9, 1009}}); },
          "forced map cannot drive an existing host argument");
    fails([&] { attachScene(base, rack, 2, {}, std::map<int, int>{{3, 1003}, {8, 1003}, {9, 1009}}); },
          "two child parameters cannot share a forced ID");
    // Only materials still present in the compact scene may contribute to the unified description.lua.
    PaintSnapshot painted;
    auto image = std::make_shared<PaintImage>(4, 4, std::array<uint8_t, 4>{24, 72, 144, 255});
    for (int material = 0; material < int(detached.scene->materials.size()); ++material)
        painted[material] = image;
    auto saved = savePaintProject(*detached.scene, painted, root / "livery", "remaining stores");
    const auto directory = fs::path(wide(saved.at("directory").get<std::string>()));
    const auto lua = readLivery(directory / "description.lua");
    check(lua.textures.size() == detached.scene->materials.size(),
          "unified Lua has exactly the surviving material bindings");
    for (const auto& [key, binding] : lua.textures)
        check(binding.material.find("middle") == std::string::npos &&
                  fs::is_regular_file(directory / wide(binding.name + ".dds")),
              "deleted object has no Lua binding and every remaining texture exists");
}
void u32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (int byte = 0; byte < 4; ++byte)
        bytes.push_back(uint8_t(value >> (byte * 8)));
}
void text(std::vector<uint8_t>& bytes, const std::string& value) {
    u32(bytes, uint32_t(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}
void connectorFixture(const fs::path& path) {
    auto bytes = readFile(fs::path(wide(EDM_TEST_FIXTURES)) / "animation-v8.edm");
    std::vector<uint8_t> category;
    text(category, "RENDER_NODES");
    const auto found = std::search(bytes.begin(), bytes.end(), category.begin(), category.end());
    require(found != bytes.end() && found - bytes.begin() >= 4, "Missing fixture category");
    bytes[size_t(found - bytes.begin()) - 4] = 2;
    text(bytes, "CONNECTORS");
    u32(bytes, 4);
    for (const auto* name : {"Pylon1", "Pylon2", "Point01", "AttachPoint"}) {
        text(bytes, "model::Connector");
        text(bytes, name);
        for (int field = 0; field < 4; ++field)
            u32(bytes, 0);
    }
    writeFile(path, bytes);
}
void reopen(const fs::path& root) {
    const auto host = root / "real-host.edm", store = root / "real-store.edm";
    connectorFixture(host);
    connectorFixture(store);
    auto base = Scene::load(host), child = Scene::load(store);
    auto all = attachScene(*base, *child, findConnector(*base, "Pylon1"));
    all = attachScene(*all, *child, findConnector(*base, "Pylon2"));
    all = attachScene(*all, *child, point(*all, 0), {{0, .65}, {40, .3}});
    const auto removed = detachScene(*all, findConnector(*base, "Pylon2"));
    auto image = std::make_shared<PaintImage>(4, 4, std::array<uint8_t, 4>{56, 112, 224, 255});
    PaintSnapshot images;
    for (int material = 0; material < int(removed.scene->materials.size()); ++material)
        images[material] = image;
    auto saved = savePaintRecovery(*removed.scene, images, root / "recovery");
    const auto folder = fs::path(wide(saved.at("directory").get<std::string>()));
    auto restored = restorePaintAssembly(Scene::load(host), folder);
    check(paintAssemblyMetadata(*restored) == paintAssemblyMetadata(*removed.scene),
          "saved assembly restores compact indices and sparse argument IDs after middle removal");
    const auto document = loadPaintDocument(*restored, folder / "project.edmpaint.json");
    check(document.images.size() == images.size() && document.images.rbegin()->second->rgba == image->rgba,
          "surviving edited canvases pass the full geometry/material/assembly fingerprint after reopening");
    const auto appended = attachScene(*restored, *child, findConnector(*base, "Pylon2"));
    check(appended->attachments.size() == 3 &&
              appended->attachments[1].argumentMap == restored->attachments[1].argumentMap,
          "restored sparse parameter namespaces allow a subsequent attachment");
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    if (argc >= 4 && std::wstring_view(argv[1]) == L"--lua-worker")
        return luaWorker(argv[2], argv[3]);
    try {
        ComRuntime runtime;
        const auto root =
            fs::current_path() / "validation" / ("detachment-tests-" + std::to_string(GetCurrentProcessId()));
        fs::create_directories(root);
        core(root);
        reopen(root);
        std::cout << "Detachment: " << checks << " checks passed\n";
        return 0;
    } catch (...) {
        std::cerr << exceptionText() << '\n';
        return 1;
    }
}
