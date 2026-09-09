#include "paint_assembly.h"
#include <iostream>

using namespace edm;
namespace {
int checks = 0;
void check(bool value, const std::string& description) {
    ++checks;
    require(value, "Layer document: " + description);
}
template <class F> void rejects(F operation, const std::string& description) {
    bool failed = false;
    try {
        operation();
    } catch (const std::exception&) {
        failed = true;
    }
    check(failed, description);
}
bool same(const PaintImage& a, const PaintImage& b) {
    return a.width == b.width && a.height == b.height && a.rgba == b.rgba;
}
void compare(const PaintCanvasLayers& a, const PaintCanvasLayers& b) {
    check(same(a.base, b.base), "immutable original base pixels roundtrip exactly");
    check(a.activeLayer == b.activeLayer && a.layers.size() == b.layers.size(),
          "active layer and empty layers survive");
    for (size_t i = 0; i < a.layers.size(); ++i) {
        check(a.layers[i].info == b.layers[i].info, "global layer ordering and all properties survive");
        check(a.layers[i].tiles.size() == b.layers[i].tiles.size(), "sparse tile counts survive");
        for (size_t j = 0; j < a.layers[i].tiles.size(); ++j) {
            const auto& x = a.layers[i].tiles[j];
            const auto& y = b.layers[i].tiles[j];
            check(x.x == y.x && x.y == y.y && same(x.image, y.image), "hidden and edge tile pixels survive");
        }
    }
}
std::shared_ptr<PaintCanvasLayers> fixtureLayers(bool alternate = false) {
    auto result = std::make_shared<PaintCanvasLayers>();
    result->base = PaintImage(65, 67,
                              alternate ? std::array<uint8_t, 4>{90, 130, 200, 72}
                                        : std::array<uint8_t, 4>{30, 60, 100, 170});
    result->activeLayer = 9;
    result->layers = {
        {{7, "机身笔触", true, .75f, true}, {{0, 0, PaintImage(64, 64, {180, 20, 40, 100})}}},
        {{9, "隐藏贴花", false, .5f, false}, {{0, 0, PaintImage(64, 64, {15, 230, 70, 220})}}},
        {{11, "尚未绘制", true, 1, true}, {}},
        {{13, "边缘像素", true, .4f, false}, {{64, 64, PaintImage(1, 3, {240, 100, 10, 150})}}}};
    return result;
}
std::shared_ptr<const PaintImage> composite(const PaintCanvasLayers& layers) {
    PaintCanvas canvas(PaintImage(1, 1));
    canvas.restoreLayers(layers);
    return std::make_shared<PaintImage>(canvas.image());
}
Scene fixtureScene(const fs::path& root) {
    Scene scene;
    scene.source = root / "fixture.edm";
    scene.version = 10;
    for (int i = 0; i < 2; ++i) {
        Material material;
        material.name = "LayerMaterial_" + std::to_string(i);
        scene.materials.push_back(material);
    }
    scene.nodes.resize(1);
    scene.staticLocal = {Mat::Identity()};
    scene.defaultWorld = scene.staticLocal;
    scene.order = {0};
    Mesh mesh;
    mesh.name = "Layer test triangle";
    mesh.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    mesh.indices = {0, 1, 2};
    mesh.uvs = {{{0, 0}, {1, 0}, {0, 1}}};
    mesh.extras = Json::object();
    scene.meshes = {mesh};
    return scene;
}
void noPublishedMarkers(const fs::path& root) {
    if (!fs::exists(root))
        return;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const auto name = entry.path().filename();
        check(name != "description.lua" && name != "project.edmpaint.json",
              "failed save publishes no completed project markers");
    }
}
void tests(const fs::path& root) {
    auto scene = fixtureScene(root);
    auto first = fixtureLayers(), second = fixtureLayers(true);
    PaintLayerSnapshotMap snapshots{{0, first}, {1, second}};
    PaintSnapshot images{{0, composite(*first)}, {1, composite(*second)}};
    const auto manifest = savePaintRecovery(scene, images, root / "recovery", {}, nullptr, {}, {}, snapshots);
    const fs::path directory = wide(manifest.at("directory").get<std::string>());
    const auto project = directory / "project.edmpaint.json";
    check(manifest["version"] == 3 && manifest["recovery_only"] == true &&
              !fs::exists(directory / "description.lua"),
          "layer recovery is v3 and keeps recovery semantics");
    auto loaded = loadPaintDocument(scene, project);
    check(loaded.layerCanvases.size() == 2 && loaded.layers.size() == 4 && loaded.activeLayer == 9,
          "v3 restores per-material canvases and global empty-layer metadata");
    compare(*first, *loaded.layerCanvases.at(0));
    compare(*second, *loaded.layerCanvases.at(1));
    check(same(*images.at(0), *loaded.images.at(0)) && same(*images.at(1), *loaded.images.at(1)),
          "flattened export pixels remain exact");
    auto revealed = *loaded.layerCanvases.at(0);
    revealed.layers[1].info.visible = true;
    check(!same(*composite(revealed), *loaded.images.at(0)),
          "hidden layer pixels remain available for revealing later");
    auto withoutLayers = revealed;
    for (auto& layer : withoutLayers.layers)
        layer.tiles.clear();
    check(same(*composite(withoutLayers), first->base),
          "deleting all layer content restores the exact base RGBA");
    check(restorePaintAssembly(std::make_shared<Scene>(scene), project)->source == scene.source,
          "assembly restoration accepts v3 before canvas loading");
    PaintLoadOptions exact;
    exact.maxExpandedBytes = manifest["layer_state"]["expanded_bytes"].get<size_t>();
    check(loadPaintDocument(scene, project, nullptr, exact).layerCanvases.size() == 2,
          "exact budget includes base composite and sparse tiles");
    --exact.maxExpandedBytes;
    rejects([&] { loadPaintDocument(scene, project, nullptr, exact); },
            "one byte below true expanded budget is rejected");

    const auto complete =
        savePaintProject(scene, images, root / "complete", "Layered skin", {}, {}, {}, nullptr, snapshots);
    const fs::path completeDir = wide(complete.at("directory").get<std::string>());
    auto completeDoc = loadPaintDocument(scene, completeDir / "project.edmpaint.json");
    compare(*first, *completeDoc.layerCanvases.at(0));
    check(readLivery(completeDir / "description.lua").textures.size() == 2 &&
              same(PaintImage::load({completeDir / "paint_0.dds"}), *images.at(0)),
          "full v3 project keeps flattened DCS DDS and one shared description");
    const auto legacy = savePaintRecovery(scene, images, root / "legacy");
    auto legacyDoc = loadPaintDocument(scene, fs::path(wide(legacy.at("directory").get<std::string>())) /
                                                  "project.edmpaint.json");
    check(legacy["version"] == 2 && legacyDoc.layerCanvases.empty() && legacyDoc.layers.empty() &&
              legacyDoc.activeLayer == 0 && same(*legacyDoc.images.at(0), *images.at(0)),
          "existing v2 flattened projects load without fabricated layers");

    PaintSnapshot aliases{{0, images.at(0)}, {1, images.at(0)}};
    PaintLayerSnapshotMap aliasSnapshots{{0, first}, {1, first}};
    const auto alias =
        savePaintRecovery(scene, aliases, root / "aliases", {}, nullptr, {}, {}, aliasSnapshots);
    const auto aliasProject =
        fs::path(wide(alias.at("directory").get<std::string>())) / "project.edmpaint.json";
    const auto aliasDoc = loadPaintDocument(scene, aliasProject);
    check(alias["layer_state"]["canvases"].size() == 1 &&
              aliasDoc.layerCanvases.at(0) == aliasDoc.layerCanvases.at(1),
          "shared immutable snapshot writes one base and tile set");
    exact.maxExpandedBytes = alias["layer_state"]["expanded_bytes"].get<size_t>();
    check(exact.maxExpandedBytes * 2 == manifest["layer_state"]["expanded_bytes"].get<size_t>() &&
              loadPaintDocument(scene, aliasProject, nullptr, exact).layerCanvases.size() == 2 &&
              aliasDoc.images.at(0) == aliasDoc.images.at(1),
          "explicit alias charges base composite and tiles once at the exact byte budget");
    --exact.maxExpandedBytes;
    rejects([&] { loadPaintDocument(scene, aliasProject, nullptr, exact); },
            "one byte below shared canvas payload is rejected");
    auto independentCopy = std::make_shared<PaintCanvasLayers>(*first);
    const auto identical = savePaintRecovery(scene, aliases, root / "identical-independent", {}, nullptr, {},
                                             {}, {{0, first}, {1, independentCopy}});
    const auto identicalProject =
        fs::path(wide(identical.at("directory").get<std::string>())) / "project.edmpaint.json";
    const auto identicalDoc = loadPaintDocument(scene, identicalProject);
    check(identical["layer_state"]["canvases"].size() == 2 &&
              identicalDoc.layerCanvases.at(0) != identicalDoc.layerCanvases.at(1) &&
              identical["layer_state"]["expanded_bytes"] == manifest["layer_state"]["expanded_bytes"],
          "equal-content snapshots remain independently editable and count separate payloads");
    exact.maxExpandedBytes = alias["layer_state"]["expanded_bytes"].get<size_t>();
    rejects([&] { loadPaintDocument(scene, identicalProject, nullptr, exact); },
            "equal PNG pixels cannot bypass independent canvas expanded accounting");

    size_t badIndex = 0;
    auto bad = [&](Json changed, const std::string& description) {
        const auto path = directory / ("invalid-" + std::to_string(++badIndex) + ".json");
        writeJson(path, changed);
        rejects([&] { loadPaintDocument(scene, path); }, description);
    };
    auto mutate = [&](const std::function<void(Json&)>& change, const std::string& description) {
        auto edited = manifest;
        change(edited);
        bad(std::move(edited), description);
    };
    mutate([](Json& j) { j.erase("layer_state"); }, "v3 missing layer state is not silently flattened");
    mutate([](Json& j) { j["layer_state"]["version"] = 99; }, "unknown layer-state version is rejected");
    mutate([](Json& j) { j["layer_state"]["active_layer"] = 999; }, "unknown active layer is rejected");
    mutate([](Json& j) { j["layer_state"]["layers"][1]["id"] = 7; },
           "duplicate global layer identifiers are rejected");
    mutate([](Json& j) { j["layer_state"]["layers"][0]["opacity"] = 1.001; }, "invalid opacity is rejected");
    mutate([](Json& j) { j["layer_state"]["layers"][0]["name"] = "bad\nname"; },
           "control characters in names are rejected");
    mutate([](Json& j) { j["layer_state"]["canvases"][1]["materials"] = {0}; },
           "duplicate material ownership is rejected");
    mutate([](Json& j) { j["layer_state"]["canvases"][0]["materials"] = {4294967296ull}; },
           "material integer truncation cannot bypass identity checks");
    mutate([](Json& j) { j["layer_state"]["canvases"][0]["layers"][0]["id"] = 9; },
           "per-canvas layer ordering must match global descriptors");
    mutate(
        [](Json& j) {
            auto& tiles = j["layer_state"]["canvases"][0]["layers"][0]["tiles"];
            tiles.push_back(tiles[0]);
        },
        "duplicate and overlapping sparse tiles are rejected");
    mutate([](Json& j) { j["layer_state"]["canvases"][0]["layers"][0]["tiles"][0]["x"] = 1; },
           "unaligned sparse tile origin is rejected");
    mutate([](Json& j) { j["layer_state"]["canvases"][0]["layers"][0]["tiles"][0]["y"] = -64; },
           "negative sparse tile origin is rejected");
    mutate([](Json& j) { j["layer_state"]["canvases"][0]["layers"][3]["tiles"][0]["width"] = 64; },
           "edge tile must use the clipped dimensions");
    mutate([](Json& j) { j["layer_state"]["canvases"][0]["base"]["width"] = 8193; },
           "oversized base image is rejected before decoding");
    for (const auto& path : {"../outside.png", "C:/outside.png", "C:outside.png", "//server/share/file.png"})
        mutate([&](Json& j) { j["layer_state"]["canvases"][0]["base"]["file"] = path; },
               "absolute or escaping image paths are rejected");
    mutate(
        [](Json& j) {
            j["layer_state"]["canvases"][0]["layers"][1]["tiles"][0]["png_sha256"] = std::string(64, '0');
        },
        "hidden tile SHA-256 is verified even when the layer is invisible");
    mutate([](Json& j) { j["layer_state"]["canvases"][0]["base"]["image_fingerprint"] = "incorrect"; },
           "base pixel checksum is verified");
    mutate([](Json& j) { j["layer_state"]["expanded_bytes"] = 1; }, "false memory declaration is rejected");
    mutate([](Json& j) { j["layer_state"]["layers"][1]["visible"] = true; },
           "stored flattened image must match recomposed layers");
    const auto baseFile =
        directory / wide(manifest["layer_state"]["canvases"][0]["base"]["file"].get<std::string>());
    const auto originalPng = readFile(baseFile);
    auto corruptedPng = originalPng;
    corruptedPng.back() ^= 1;
    writeFile(baseFile, corruptedPng);
    rejects([&] { loadPaintDocument(scene, project); }, "PNG byte corruption is rejected before decoding");
    writeFile(baseFile, originalPng);
    auto invalidSnapshot = std::make_shared<PaintCanvasLayers>(*second);
    invalidSnapshot->layers[0].info.name = "Different layer";
    rejects(
        [&] {
            savePaintRecovery(scene, images, root / "failed-global", {}, nullptr, {}, {},
                              {{0, first}, {1, invalidSnapshot}});
        },
        "saving rejects inconsistent cross-material layer descriptions");
    noPublishedMarkers(root / "failed-global");
    rejects(
        [&] {
            savePaintRecovery(scene, images, root / "failed-composite", {}, nullptr, {}, {}, aliasSnapshots);
        },
        "saving a shared snapshot with different flattened outputs is rejected");
    noPublishedMarkers(root / "failed-composite");
    std::atomic_bool cancel{false};
    rejects(
        [&] {
            savePaintRecovery(
                scene, images, root / "cancelled",
                [&](const std::string& text) {
                    if (text.find("保存完整图层") != std::string::npos)
                        cancel = true;
                },
                &cancel, {}, {}, snapshots);
        },
        "cancellation during layer persistence is honored");
    noPublishedMarkers(root / "cancelled");
    cancel = true;
    rejects([&] { loadPaintDocument(scene, project, &cancel); },
            "cancelled loads do not publish partial layer documents");
    std::cout << "Layer document: " << checks << " checks passed\n";
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc >= 4 && std::wstring_view(argv[1]) == L"--lua-worker")
        return luaWorker(argv[2], argv[3]);
    try {
        ComRuntime runtime;
        const auto root = fs::current_path() / "validation" /
                          ("paint-layer-document-tests-" + std::to_string(GetCurrentProcessId()));
        fs::create_directories(root);
        tests(root);
        return 0;
    } catch (...) {
        std::cerr << exceptionText() << '\n';
        return 1;
    }
}
