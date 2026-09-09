#include "export.h"
#include "paint_document.h"
#include <iostream>

using namespace edm;
namespace {
int checks = 0;
void check(bool condition, const std::string& message) {
    ++checks;
    require(condition, "Attachment assets: " + message);
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
void textFile(const fs::path& path, const std::string& value) {
    writeFile(path, {reinterpret_cast<const uint8_t*>(value.data()), value.size()});
}
fs::path folder(const Json& manifest) {
    return wide(manifest.at("directory").get<std::string>());
}
Scene fixture(const fs::path& root) {
    Scene scene;
    scene.source = root / "Aircraft" / "Shapes" / "aircraft.edm";
    const auto child = root / "Weapon" / "Shapes" / "weapon.edm";
    for (const auto& source : {scene.source, child}) {
        fs::create_directories(source.parent_path());
        fs::create_directories(source.parent_path().parent_path() / "Textures");
        textFile(source, "The EDM source is never modified by texture or export operations.");
    }
    scene.nodes.resize(4);
    scene.nodes[0].name = "Aircraft root";
    scene.nodes[1].name = "Pylon1";
    scene.nodes[1].parent = 0;
    scene.nodes[1].t = V3(2, 0, 0);
    scene.nodes[1].extras = {{"edm_type", "Connector"}};
    scene.nodes[2].name = "Weapon root";
    scene.nodes[2].parent = 1;
    scene.nodes[3].name = "AttachPoint";
    scene.nodes[3].parent = 2;
    scene.nodes[3].extras = {{"edm_type", "Connector"}};
    for (int i = 2; i < 4; ++i)
        scene.nodes[i].extras["edm_attachment"] = 0;
    for (int i = 0; i < 2; ++i) {
        Material material;
        material.source = i == 0 ? scene.source : child;
        material.name = i == 0 ? "AircraftSkin" : "WeaponSkin";
        material.textures = {{0, "body"}, {13, "body_RoughMet"}};
        if (i)
            material.extras["edm_attachment"] = 0;
        scene.materials.push_back(material);
        Mesh mesh;
        mesh.name = material.name;
        mesh.node = i == 0 ? 0 : 2;
        mesh.material = i;
        mesh.positions = {{{0, 0, 0}}, {{1, 0, 0}}, {{0, 1, 0}}};
        mesh.normals = {{{0, 0, 1}}, {{0, 0, 1}}, {{0, 0, 1}}};
        mesh.uvs = {{{{0, 0}}, {{1, 0}}, {{0, 1}}}};
        mesh.indices = {0, 1, 2};
        mesh.extras = Json::object();
        if (i)
            mesh.extras["edm_attachment"] = 0;
        scene.meshes.push_back(mesh);
        const auto textures = material.source.parent_path().parent_path() / "Textures";
        PaintImage(8, 8,
                   i ? std::array<uint8_t, 4>{25, 90, 180, 255} : std::array<uint8_t, 4>{190, 40, 30, 255})
            .saveDDS(textures / "body.dds");
        PaintImage(8, 8,
                   i ? std::array<uint8_t, 4>{255, 110, 80, 255} : std::array<uint8_t, 4>{255, 220, 10, 255})
            .saveDDS(textures / "body_RoughMet.dds");
    }
    SceneAttachment attachment;
    attachment.source = child;
    attachment.targetNode = 1;
    attachment.root = 2;
    attachment.attachNode = 3;
    attachment.nodeBegin = 2;
    attachment.nodeCount = 2;
    attachment.materialBegin = 1;
    attachment.materialCount = 1;
    attachment.meshBegin = 1;
    attachment.meshCount = 1;
    attachment.argumentMap = {{42, 10001}};
    scene.attachments.push_back(attachment);
    scene.defaultArgs = {{10001, .375}};
    Track track;
    track.node = 2;
    track.arg = 10001;
    track.keys = {{0, V4::Zero()}, {1, V4(0, 1, 0, 0)}};
    scene.tracks.push_back(track);
    scene.limits = {{10001, {0, 1}}};
    for (int i = 0; i < int(scene.nodes.size()); ++i) {
        scene.staticLocal.push_back(scene.nodes[i].local());
        scene.order.push_back(i);
    }
    scene.defaultWorld = scene.evaluate({});
    return scene;
}
void verifyDependencies(const Scene& scene, const Json& manifest) {
    const auto dir = folder(manifest);
    auto livery = std::make_shared<Livery>(readLivery(dir / "description.lua"));
    TextureResolver original(scene.source), copied(scene.source, {}, livery);
    for (const auto& material : scene.materials)
        for (int slot : {0, 13}) {
            auto before = original.material(material, slot), after = copied.material(material, slot);
            check(before && after && before->bytes() == after->bytes(),
                  "each model's original DDS and RoughMet bytes survive the shared description");
            check(after && within(after->path, dir),
                  "shared description resolves into its own export directory");
        }
    check(livery->textures.size() == 4, "one description contains aircraft and weapon texture bindings");
    check(manifest["self_contained"].get<bool>() && manifest["missing_textures"].empty(),
          "original texture bundle is self-contained");
}
void tests(const fs::path& root) {
    auto scene = fixture(root);
    const auto aircraftOriginal = readFile(scene.source),
               weaponOriginal = readFile(scene.attachments[0].source);
    const auto brokenArchive =
        scene.attachments[0].source.parent_path().parent_path() / "Textures" / "bad.zip";
    textFile(brokenArchive, "corrupt archive fixture");
    TextureResolver resolver(scene.source);
    auto aircraft = resolver.material(scene.materials[0]), weapon = resolver.material(scene.materials[1]);
    check(aircraft && weapon && aircraft->key() != weapon->key() && aircraft->bytes() != weapon->bytes(),
          "same texture basename resolves against each EDM source");
    const auto roots = resolver.roots.size();
    for (int repeat = 0; repeat < 8; ++repeat)
        check(resolver.material(scene.materials[1])->key() == weapon->key(),
              "per-source cache remains stable");
    check(resolver.roots.size() == roots && resolver.resolved.size() == 2 && resolver.missing.empty(),
          "cached material lookups do not duplicate source roots or diagnostic records");
    check(resolver.warnings.size() == 1 &&
              resolver.warnings.front().starts_with(pathString(scene.attachments[0].source)),
          "child archive warnings are merged once and identify their EDM source");
    fs::remove(brokenArchive);
    auto absentLivery = std::make_shared<Livery>();
    absentLivery->path = root / "empty-livery" / "description.lua";
    fs::create_directories(absentLivery->path.parent_path());
    absentLivery->textures[{"weaponskin", 0}] = {"WeaponSkin", "absent-image", 0, false};
    TextureResolver absent(scene.source, {}, absentLivery);
    check(!absent.material(scene.materials[1]) && absent.missing.size() == 1 &&
              absent.missing.front().starts_with(pathString(scene.attachments[0].source)),
          "unresolved child livery textures are reported in the parent resolver with their source");
    auto commonLivery = std::make_shared<Livery>();
    for (const auto& material : scene.materials)
        commonLivery->textures[{lower(material.name), 0}] = {material.name, "body", 0, true};
    TextureResolver common(scene.source, {}, commonLivery);
    check(common.material(scene.materials[0])->key() == aircraft->key() &&
              common.material(scene.materials[1])->key() == weapon->key(),
          "common=true overrides retain the model-specific search scope");

    auto manifest = exportLiveryAssets(scene, {}, root / "export", "Aircraft and weapon");
    verifyDependencies(scene, manifest);
    check(manifest["assets_only"].get<bool>() && manifest["textures"].empty(),
          "unpainted export has no fabricated editing canvases");
    check(manifest["assembly"]["attachments"][0]["target_name"] == "Pylon1" &&
              manifest["assembly"]["attachments"][0]["argument_map"]["42"] == 10001 &&
              manifest["assembly"]["default_args"]["10001"] == .375,
          "attachment hierarchy and isolated argument defaults are retained");
    rejects([&] { savePaintProject(scene, {}, root / "empty-paint", "empty"); },
            "normal paint saving still rejects an empty editing project");

    auto image = std::make_shared<PaintImage>(8, 8, std::array<uint8_t, 4>{10, 220, 100, 190});
    auto paintManifest = savePaintProject(scene, {{1, image}}, root / "paint", "weapon edit");
    auto document = loadPaintDocument(scene, folder(paintManifest) / "project.edmpaint.json");
    check(document.images.size() == 1 && document.images.at(1)->rgba == image->rgba &&
              document.assembly == paintManifest["assembly"],
          "weapon paint and the assembly metadata reopen without changing material indices");
    TextureResolver reopened(scene.source, document.textureDirectory, document.livery);
    check(reopened.material(scene.materials[0])->bytes() == aircraft->bytes() &&
              reopened.material(scene.materials[1])->bytes() == weapon->bytes(),
          "original appearance after reopening includes both aircraft and weapon");
    auto changedTarget = scene;
    changedTarget.attachments[0].targetNode = 0;
    rejects([&] { loadPaintDocument(changedTarget, folder(paintManifest) / "project.edmpaint.json"); },
            "same meshes attached to a different connector cannot silently reuse a paint identity");

    const auto luaPath = root / "dynamic" / "description.lua";
    fs::create_directories(luaPath.parent_path());
    image->saveDDS(luaPath.parent_path() / "weapon_skin.dds");
    textFile(luaPath, "local prefix='weapon'\nlocal function skin() return prefix..'_skin' end\n"
                      "livery={{'WeaponSkin',0,skin(),false}}\ncustom_args={[42]=3/8}\n"
                      "countries={'USA'}\nname='dynamic weapon'\n");
    auto dynamic = std::make_shared<Livery>(readLivery(luaPath));
    auto dynamicManifest = exportLiveryAssets(scene, {}, root / "dynamic-export", "dynamic weapon", dynamic);
    auto baked = readLivery(folder(dynamicManifest) / "description.lua");
    check(baked.args == dynamic->args && baked.countries == dynamic->countries &&
              dynamicManifest["evaluated_appearance"]["livery"]["evaluation"] == dynamic->evaluation,
          "dynamic Lua's evaluated bindings, arguments, countries and evaluation record are retained");
    TextureResolver dynamicResolved(scene.source, {}, std::make_shared<Livery>(baked));
    check(PaintImage::load(*dynamicResolved.material(scene.materials[1])).rgba == image->rgba,
          "evaluated child livery pixels are copied into the shared description");

    auto collision = scene;
    collision.materials[1].name = collision.materials[0].name;
    rejects([&] { exportLiveryAssets(collision, {}, root / "collision", "collision"); },
            "same material name with different source textures cannot be silently flattened");
    rejects([&] { savePaintProject(collision, {{1, image}}, root / "partial-collision", "collision"); },
            "painting only one of two conflicting named materials is rejected explicitly");
    auto shared = scene;
    shared.materials[1].name = shared.materials[0].name;
    const auto childTextures = shared.materials[1].source.parent_path().parent_path() / "Textures";
    for (auto name : {"body.dds", "body_RoughMet.dds"})
        writeFile(childTextures / name,
                  readFile(shared.source.parent_path().parent_path() / "Textures" / name));
    auto sharedManifest = exportLiveryAssets(shared, {}, root / "shared", "shared");
    check(readLivery(folder(sharedManifest) / "description.lua").textures.size() == 2,
          "identical bytes from different source paths may share one DCS material binding");
    // Restore the distinct weapon texture so format tests still exercise source separation.
    PaintImage(8, 8, {25, 90, 180, 255}).saveDDS(childTextures / "body.dds");
    PaintImage(8, 8, {255, 110, 80, 255}).saveDDS(childTextures / "body_RoughMet.dds");

    ExportOptions options;
    options.arguments = std::vector<int>{10001};
    options.diffuseOverrides[1] = image->pngBytes();
    for (const auto& extension : {".obj", ".glb", ".gltf", ".fbx"}) {
        const auto path = root / (std::string("assembly") + extension);
        auto report = exportScene(scene, path, options);
        check(fs::file_size(path) > 0 && report.contains("livery_assets_directory") &&
                  fs::is_regular_file(wide(report.at("description_lua").get<std::string>())),
              std::string(extension) + " includes a DCS livery sidecar");
        auto exported = readLivery(wide(report.at("description_lua").get<std::string>()));
        TextureResolver copied(scene.source, {}, std::make_shared<Livery>(exported));
        check(PaintImage::load(*copied.material(scene.materials[1])).rgba == image->rgba &&
                  copied.material(scene.materials[0])->bytes() == aircraft->bytes(),
              std::string(extension) + " exports painted weapon pixels and unpainted aircraft DDS");
        check(report["baseline_arguments"]["10001"] == .375,
              std::string(extension) + " preserves the attached model's isolated baseline");
        auto reportFile = path;
        reportFile.replace_extension(".report.json");
        check(Json::parse(readFile(reportFile))["description_lua"] == report["description_lua"],
              "written report identifies the shared livery directory");
    }
    const auto protectedPath = root / "existing.glb";
    textFile(protectedPath, "previous successful export");
    const auto protectedBytes = readFile(protectedPath);
    rejects([&] { exportScene(collision, protectedPath, ExportOptions{}); },
            "livery conflict is detected before publishing the replacement model");
    check(readFile(protectedPath) == protectedBytes, "failed livery validation preserves an existing model");
    options.textures = false;
    auto geometry = exportScene(scene, root / "geometry.glb", options);
    check(!geometry.contains("livery_assets_directory"),
          "textures=false does not create a DCS texture bundle");
    check(readFile(scene.source) == aircraftOriginal &&
              readFile(scene.attachments[0].source) == weaponOriginal,
          "all operations leave original EDM files unchanged");
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc >= 4 && std::wstring_view(argv[1]) == L"--lua-worker")
        return luaWorker(argv[2], argv[3]);
    try {
        ComRuntime runtime;
        const auto root = fs::current_path() / "validation" /
                          ("attachment-assets-tests-" + std::to_string(GetCurrentProcessId()));
        fs::create_directories(root);
        tests(root);
        std::cout << "Attachment assets: " << checks << " checks passed\n";
        return 0;
    } catch (...) {
        std::cerr << exceptionText() << '\n';
        return 1;
    }
}
