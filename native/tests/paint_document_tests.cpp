#include "paint_document.h"
#include "export.h"
#include <iostream>
#include <miniz.h>

using namespace edm;
namespace {
int checks = 0;
void check(bool ok, const char* text) {
    ++checks;
    require(ok, std::string("Paint document regression: ") + text);
}
template <class F> void fails(F action, const char* text) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, text);
}
void textFile(const fs::path& path, const std::string& text) {
    writeFile(path, {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
}
void linearDDS(const fs::path& path, std::array<uint8_t, 4> color) {
    DirectX::ScratchImage image;
    require(SUCCEEDED(image.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 1)), "DDS test allocation");
    for (size_t i = 0; i < 64; ++i)
        std::copy(color.begin(), color.end(), image.GetPixels() + i * 4);
    DirectX::Blob dds;
    require(SUCCEEDED(DirectX::SaveToDDSMemory(image.GetImages(), image.GetImageCount(), image.GetMetadata(),
                                               DirectX::DDS_FLAGS_NONE, dds)),
            "DDS test encoding");
    writeFile(path, {static_cast<const uint8_t*>(dds.GetBufferPointer()), dds.GetBufferSize()});
}
Scene fixture(const fs::path& root) {
    Scene scene;
    scene.source = root / "fixture.edm";
    scene.version = 10;
    textFile(scene.source, "original EDM placeholder remains unchanged");
    Material material;
    material.name = "Body \"A\"\n中";
    material.shader = "test";
    material.format = {3, 3, 2};
    material.textures = {
        {0, "original"}, {1, "normal"}, {2, "original_RoughMet"}, {3, "procedural_not_a_DCS_decal"}};
    scene.materials.push_back(material);
    scene.nodes.resize(1);
    scene.order = {0};
    scene.staticLocal = {Mat::Identity()};
    scene.defaultWorld = scene.staticLocal;
    Mesh mesh;
    mesh.name = "test surface";
    mesh.positions = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
    mesh.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    mesh.uvs = {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.extras = Json::object();
    scene.meshes.push_back(mesh);
    linearDDS(root / "original.dds", {80, 90, 100, 255});
    linearDDS(root / "original_RoughMet.dds", {17, 81, 202, 255});
    PaintImage(8, 8, {128, 127, 255, 255}).savePNG(root / "normal.png");
    return scene;
}
fs::path folderOf(const Json& manifest) {
    return wide(manifest.at("directory").get<std::string>());
}
void noMarkers(const fs::path& root) {
    if (!fs::exists(root))
        return;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const auto name = pathString(entry.path().filename());
        check(name != "description.lua" && name != "project.edmpaint.json",
              "cancelled/failed operation leaves no completion marker");
    }
}
std::vector<uint8_t> glbImage(const fs::path& path, int material) {
    auto bytes = readFile(path);
    uint32_t jsonSize = 0;
    std::memcpy(&jsonSize, bytes.data() + 12, 4);
    const auto doc = Json::parse(bytes.begin() + 20, bytes.begin() + 20 + jsonSize);
    const int texture = doc["materials"][material]["pbrMetallicRoughness"]["baseColorTexture"]["index"];
    const int image = doc["textures"][texture]["source"], view = doc["images"][image]["bufferView"];
    const size_t start = size_t(28) + jsonSize + doc["bufferViews"][view].value("byteOffset", size_t(0));
    const size_t size = doc["bufferViews"][view]["byteLength"];
    check(start + size <= bytes.size(), "GLB embedded image range valid");
    return {bytes.begin() + start, bytes.begin() + start + size};
}
void tests(const fs::path& root) {
    fs::create_directories(root);
    auto scene = fixture(root);
    const auto sourceEDM = readFile(scene.source), sourceDDS = readFile(root / "original.dds"),
               sourceRM = readFile(root / "original_RoughMet.dds");
    auto image = std::make_shared<PaintImage>(16, 8, std::array<uint8_t, 4>{25, 155, 210, 72});
    image->rgba[5] = 23;
    PaintSnapshot snapshot{{0, image}};
    auto livery = std::make_shared<Livery>();
    livery->path = root / "description-source.lua";
    livery->args = {{38, .375}, {442, .4}};
    livery->countries = {"USA", "China"};
    textFile(livery->path, "original description remains unchanged\n");
    const auto sourceLua = readFile(livery->path);
    auto manifest = savePaintProject(scene, snapshot, root / "saved", "名称\"\\\n测试", livery, root);
    const auto folder = folderOf(manifest), project = folder / "project.edmpaint.json";
    check(manifest["version"] == 2 && !manifest["recovery_only"].get<bool>(),
          "full save produces version 2 project");
    check(fs::exists(folder / "description.lua") && fs::exists(project) &&
              !fs::exists(folder / "INCOMPLETE.txt"),
          "completed directory published with both markers");
    check(manifest["missing_textures"].empty(),
          "non-NumberNode raw slot 3 and absent slots do not produce missing noise");
    auto loaded = loadPaintProject(scene, project);
    check(loaded.size() == 1 && loaded[0]->rgba == image->rgba && loaded[0]->width == image->width,
          "PNG project roundtrip preserves full pixels and dimensions");
    auto editedDDS = PaintImage::load({folder / "paint_0.dds"}, 0);
    check(editedDDS.rgba == image->rgba, "DCS DDS retains edited RGBA pixels");
    DirectX::TexMetadata metadata;
    auto dds = readFile(folder / "paint_0.dds");
    check(SUCCEEDED(
              DirectX::GetMetadataFromDDSMemory(dds.data(), dds.size(), DirectX::DDS_FLAGS_NONE, metadata)) &&
              metadata.mipLevels > 1,
          "painted DCS DDS includes mipmaps");
    auto decodedLivery = readLivery(folder / "description.lua");
    const auto key = lower(scene.materials[0].name);
    check(decodedLivery.name == "名称\"\\\n测试" && decodedLivery.args == livery->args &&
              decodedLivery.countries == livery->countries,
          "Lua quoting and evaluated configuration roundtrip");
    check(decodedLivery.textures.contains({key, 0}) && decodedLivery.textures.contains({key, 13}) &&
              !decodedLivery.textures.contains({key, 2}) && !decodedLivery.textures.contains({key, 3}),
          "legacy RoughMet is emitted as DCS slot 13 and raw slot 3 is not DECAL");
    const auto roughName = decodedLivery.textures.at({key, 13}).name;
    check(readFile(folder / (roughName + ".dds")) == sourceRM,
          "source RoughMet DDS copied byte-for-byte without color conversion");
    const auto normalName = decodedLivery.textures.at({key, 1}).name;
    check(PaintImage::load({folder / (normalName + ".dds")}, 0).rgba ==
              PaintImage::load({root / "normal.png"}, 0).rgba,
          "non-DDS normal dependency converted losslessly to DDS");
    check(readFile(scene.source) == sourceEDM && readFile(root / "original.dds") == sourceDDS &&
              readFile(root / "original_RoughMet.dds") == sourceRM && readFile(livery->path) == sourceLua,
          "original EDM DDS RoughMet Lua inputs remain unchanged");
    auto relocated = scene;
    relocated.source = root / "moved" / "fixture.edm";
    check(loadPaintProject(relocated, project).size() == 1,
          "moving same model path does not invalidate its structural identity");
    auto wrongModel = scene;
    wrongModel.meshes[0].uvs[0][0][0] = .125f;
    fails([&] { loadPaintProject(wrongModel, project); },
          "same material names cannot bypass changed UV identity");
    wrongModel = scene;
    wrongModel.materials[0].shader = "different shader";
    fails([&] { loadPaintProject(wrongModel, project); }, "different material definition rejected");

    auto corrupt = manifest;
    auto tampered = folder / "tampered.edmpaint.json";
    const auto testManifest = [&](Json data) {
        writeJson(tampered, data);
        fails([&] { loadPaintProject(scene, tampered); }, "malformed or escaping project rejected");
    };
    corrupt["textures"][0]["file"] = "../outside.png";
    testManifest(corrupt);
    corrupt = manifest;
    corrupt["textures"][0]["file"] = pathString(root / "normal.png");
    testManifest(corrupt);
    corrupt = manifest;
    corrupt["textures"][0]["file"] = "paint_0.png:secret";
    testManifest(corrupt);
    corrupt = manifest;
    corrupt["textures"][0]["material_name"] = "impostor";
    testManifest(corrupt);
    corrupt = manifest;
    corrupt["textures"][0]["material"] = 99;
    testManifest(corrupt);
    corrupt = manifest;
    corrupt["textures"][0]["width"] = 999;
    testManifest(corrupt);
    corrupt = manifest;
    corrupt["textures"][0]["image_fingerprint"] = "changed";
    testManifest(corrupt);
    corrupt = manifest;
    corrupt["textures"].push_back(corrupt["textures"][0]);
    testManifest(corrupt);
    auto duplicate = scene;
    duplicate.materials.push_back(duplicate.materials[0]);
    duplicate.materials[1].name = "BODY \"A\"\n中";
    auto duplicateManifest = savePaintProject(duplicate, snapshot, root / "duplicates", "shared", {}, root);
    auto duplicateLoaded = loadPaintProject(duplicate, folderOf(duplicateManifest) / "project.edmpaint.json");
    check(duplicateLoaded.size() == 2 && duplicateLoaded[0] == duplicateLoaded[1],
          "DCS case-insensitive duplicate name shares one edited image and reload allocation");
    const PaintLoadOptions oneCanvasBudget{image->rgba.size()};
    const PaintLoadOptions twoCanvasBudget{image->rgba.size() * 2};
    check(loadPaintProject(scene, project, nullptr, oneCanvasBudget).size() == 1,
          "one canvas fits its exact expanded byte budget");
    fails(
        [&] {
            loadPaintProject(duplicate, folderOf(duplicateManifest) / "project.edmpaint.json", nullptr,
                             oneCanvasBudget);
        },
        "shared PNG cannot bypass expanded per-material canvas budget");
    check(loadPaintProject(duplicate, folderOf(duplicateManifest) / "project.edmpaint.json", nullptr,
                           twoCanvasBudget)
                  .size() == 2,
          "two shared PNG canvases fit only the sum of their expanded RGBA bytes");
    auto duplicateLivery = readLivery(folderOf(duplicateManifest) / "description.lua");
    check(duplicateLivery.textures.size() == 3 && !duplicateManifest["warnings"].empty(),
          "duplicate material emits only one DCS binding per slot and reports sharing");
    auto different = std::make_shared<PaintImage>(16, 8, std::array<uint8_t, 4>{255, 0, 0, 255});
    PaintSnapshot conflicting{{0, image}, {1, different}};
    fails([&] { savePaintProject(duplicate, conflicting, root / "conflicting", "invalid", {}, root); },
          "conflicting same-name painted images rejected before saving");
    check(!fs::exists(root / "conflicting"), "invalid snapshot creates no output folder");
    auto recoveredConflict = savePaintRecovery(duplicate, conflicting, root / "recovery");
    auto recovered = loadPaintProject(duplicate, folderOf(recoveredConflict) / "project.edmpaint.json");
    check(recovered[0]->rgba == image->rgba && recovered[1]->rgba == different->rgba,
          "recovery preserves different same-name canvases without DCS restrictions");
    check(!fs::exists(folderOf(recoveredConflict) / "description.lua") &&
              !fs::exists(folderOf(recoveredConflict) / "paint_0.dds") &&
              recoveredConflict["dependencies"].empty(),
          "fast recovery writes only PNG and identity manifest");
    duplicate.materials[1].textures[1].name = "normal_second";
    PaintImage(8, 8, {120, 125, 250, 255}).savePNG(root / "normal_second.png");
    auto ambiguous = savePaintProject(duplicate, snapshot, root / "ambiguous", "shared", {}, root);
    auto ambiguousLivery = readLivery(folderOf(ambiguous) / "description.lua");
    check(!ambiguousLivery.textures.contains({key, 1}) && !ambiguous["self_contained"].get<bool>(),
          "ambiguous duplicate default textures keep original model defaults");

    auto numberScene = scene;
    numberScene.meshes[0].extras["edm_type"] = "NumberNode";
    numberScene.materials[0].textures.back().name = "numbers";
    linearDDS(root / "numbers.dds", {220, 180, 10, 85});
    auto numberManifest =
        savePaintProject(numberScene, snapshot, root / "numbers-output", "numbers", {}, root);
    auto numberLivery = readLivery(folderOf(numberManifest) / "description.lua");
    check(numberLivery.textures.contains({key, 3}), "NumberNode material maps raw slot 3 to DCS DECAL");
    auto explicitDecal = std::make_shared<Livery>();
    explicitDecal->path = root / "description-source.lua";
    explicitDecal->textures[{key, 3}] = {scene.materials[0].name, "empty", 3, true};
    auto explicitManifest =
        savePaintProject(scene, snapshot, root / "explicit", "explicit", explicitDecal, root);
    auto explicitLoaded = readLivery(folderOf(explicitManifest) / "description.lua");
    check(explicitLoaded.textures.at({key, 3}).name == "empty" && explicitLoaded.textures.at({key, 3}).common,
          "explicit DCS DECAL empty override preserved even without NumberNode");
    const auto archivePath = root / "packed-livery.zip";
    mz_zip_archive archive{};
    require(mz_zip_writer_init_file(&archive, pathString(archivePath).c_str(), 0) != 0, "Create ZIP fixture");
    const std::string zipLua = "livery={}\n";
    require(mz_zip_writer_add_mem(&archive, "Livery/description.lua", zipLua.data(), zipLua.size(),
                                  MZ_BEST_SPEED) != 0 &&
                mz_zip_writer_add_mem(&archive, "Livery/packed_diffuse.dds", sourceDDS.data(),
                                      sourceDDS.size(), MZ_BEST_SPEED) != 0 &&
                mz_zip_writer_add_mem(&archive, "Livery/packed_RoughMet.dds", sourceRM.data(),
                                      sourceRM.size(), MZ_BEST_SPEED) != 0 &&
                mz_zip_writer_finalize_archive(&archive) != 0,
            "Write ZIP fixture");
    mz_zip_writer_end(&archive);
    auto zipLivery = std::make_shared<Livery>();
    zipLivery->path = archivePath;
    zipLivery->entry = "Livery/description.lua";
    zipLivery->textures[{key, 0}] = {scene.materials[0].name, "packed_diffuse", 0, false};
    zipLivery->textures[{key, 13}] = {scene.materials[0].name, "packed_RoughMet", 13, false};
    const auto zipBefore = readFile(archivePath);
    const auto zipManifest = savePaintProject(scene, snapshot, root / "zip-output", "zip", zipLivery, root);
    const auto zipSaved = readLivery(folderOf(zipManifest) / "description.lua");
    check(readFile(folderOf(zipManifest) / (zipSaved.textures.at({key, 13}).name + ".dds")) == sourceRM,
          "ZIP livery relative RoughMet copied byte-for-byte");
    check(readFile(folderOf(zipManifest) /
                   wide(zipManifest["original_diffuse"][0]["file"].get<std::string>())) == sourceDDS,
          "ZIP livery original diffuse is retained byte-for-byte separately from the painted DDS");
    check(readFile(archivePath) == zipBefore, "source livery ZIP remains unchanged");
    std::atomic_bool cancel = true;
    fails(
        [&] { savePaintProject(scene, snapshot, root / "cancelled-early", "cancel", {}, root, {}, &cancel); },
        "pre-cancelled save aborts before output");
    check(!fs::exists(root / "cancelled-early"), "pre-cancelled save creates no directory");
    cancel = false;
    fails(
        [&] {
            savePaintProject(
                scene, snapshot, root / "cancelled-late", "cancel", {}, root,
                [&](const std::string& progress) {
                    if (progress == "完成工程与 DCS 涂装配置")
                        cancel = true;
                },
                &cancel);
        },
        "late cancellation aborts before publishing metadata");
    noMarkers(root / "cancelled-late");
    cancel = false;
    fails(
        [&] {
            savePaintProject(
                scene, snapshot, root / "cancelled-publish", "cancel", {}, root,
                [&](const std::string& progress) {
                    if (progress == "发布涂装目录")
                        cancel = true;
                },
                &cancel);
        },
        "cancellation after marker writes cleans both markers before returning");
    noMarkers(root / "cancelled-publish");
    fails(
        [&] {
            savePaintProject(scene, snapshot, root / "failure", "fail", {}, root,
                             [](const std::string& progress) {
                                 if (progress == "完成工程与 DCS 涂装配置")
                                     throw std::runtime_error("test failure");
                             });
        },
        "failure before publishing leaves only explicit staging content");
    noMarkers(root / "failure");
    cancel = true;
    fails([&] { loadPaintProject(scene, project, &cancel); }, "project load responds to cancellation");

    auto document = loadPaintDocument(scene, project);
    check(document.restoreAppearance && document.livery && document.livery->args == livery->args &&
              document.livery->countries == livery->countries &&
              document.textureDirectory == fs::weakly_canonical(folder),
          "complete project restores its local livery arguments countries and texture directory");
    auto resaved = savePaintProject(scene, document.images, root / "resaved", "reopened", document.livery,
                                    document.textureDirectory);
    auto resavedDocument = loadPaintDocument(scene, folderOf(resaved) / "project.edmpaint.json");
    check(resavedDocument.livery->args == livery->args &&
              resavedDocument.livery->countries == livery->countries,
          "reopening and saving again retains evaluated animation arguments and countries");
    const auto resavedRM = resavedDocument.livery->textures.at({key, 13}).name;
    check(readFile(folderOf(resaved) / (resavedRM + ".dds")) == sourceRM,
          "reopening and saving again retains original RoughMet pixels and compression");

    const auto originalFile = manifest.at("original_diffuse").at(0).at("file").get<std::string>();
    check(manifest["original_diffuse"].size() == 1 && manifest["original_diffuse"][0]["state"] == "file" &&
              readFile(folder / wide(originalFile)) == sourceDDS,
          "full project preserves the exact original DDS independently of edit pixels");
    TextureResolver originalResolver(scene.source, document.textureDirectory, document.livery);
    auto originalSource = originalResolver.material(scene.materials[0]);
    check(originalSource && originalSource->bytes() == sourceDDS &&
              document.images.at(0)->rgba == image->rgba,
          "reopened base resolves original DDS while the paint snapshot retains edited pixels");
    check(decodedLivery.textures.at({key, 0}).name == "paint_0" &&
              document.livery->textures.at({key, 0}).name == originalFile,
          "DCS description retains baked edits while the editor base livery uses the original diffuse");
    TextureResolver resavedResolver(scene.source, resavedDocument.textureDirectory, resavedDocument.livery);
    auto resavedSource = resavedResolver.material(scene.materials[0]);
    check(resavedSource && resavedSource->bytes() == sourceDDS &&
              resavedDocument.images.at(0)->rgba == image->rgba,
          "resaving a reopened project does not promote its painted DDS into the original baseline");

    const auto portableFolder = root / "portable-project";
    fs::copy(folder, portableFolder, fs::copy_options::recursive);
    auto portableScene = scene;
    portableScene.source = root / "unavailable-game" / "fixture.edm";
    const auto portableDocument = loadPaintDocument(portableScene, portableFolder / "project.edmpaint.json");
    TextureResolver portableResolver(portableScene.source, portableDocument.textureDirectory,
                                     portableDocument.livery);
    auto portableOriginal = portableResolver.material(portableScene.materials[0]);
    check(portableOriginal && within(portableOriginal->path, portableFolder) &&
              portableOriginal->bytes() == sourceDDS,
          "relocated project restores its original diffuse locally without the original model directory");

    auto oldManifest = manifest;
    oldManifest.erase("original_diffuse");
    const auto oldProject = folder / "legacy.edmpaint.json";
    writeJson(oldProject, oldManifest);
    const auto legacyDocument = loadPaintDocument(scene, oldProject);
    check(legacyDocument.livery->textures.at({key, 0}).name == "paint_0" &&
              std::any_of(
                  legacyDocument.warnings.begin(), legacyDocument.warnings.end(),
                  [](const auto& warning) { return warning.find("无法还原此前绘制") != std::string::npos; }),
          "older projects retain their baked appearance and explain that earlier edits cannot be recovered");

    const auto originalTampered = folder / "original-tampered.edmpaint.json";
    auto brokenOriginal = manifest;
    brokenOriginal["original_diffuse"][0]["file"] = "missing-original.dds";
    writeJson(originalTampered, brokenOriginal);
    const auto missingOriginal = loadPaintDocument(scene, originalTampered);
    TextureResolver missingOriginalResolver(scene.source, root, missingOriginal.livery);
    check(!missingOriginalResolver.material(scene.materials[0]) &&
              missingOriginal.livery->textures.at({key, 0}).name.empty() &&
              !missingOriginal.livery->textures.at({key, 0}).common &&
              missingOriginal.images.at(0)->rgba == image->rgba && !missingOriginal.warnings.empty(),
          "missing original copy stays unresolved instead of falling back to valid model or painted DDS");
    const auto rejectOriginal = [&](Json value) {
        writeJson(originalTampered, value);
        fails([&] { loadPaintDocument(scene, originalTampered); }, "malformed original binding rejected");
    };
    for (const auto& file : std::vector<std::string>{"../outside.dds", pathString(root / "original.dds"),
                                                     "original.dds:stream", "paint_0.png"}) {
        auto bad = manifest;
        bad["original_diffuse"][0]["file"] = file;
        rejectOriginal(std::move(bad));
    }
    brokenOriginal = manifest;
    brokenOriginal["original_diffuse"][0]["material"] = 99;
    rejectOriginal(brokenOriginal);
    brokenOriginal = manifest;
    brokenOriginal["original_diffuse"][0]["material_name"] = "wrong material";
    rejectOriginal(brokenOriginal);
    brokenOriginal = manifest;
    brokenOriginal["original_diffuse"][0]["state"] = "guess_default";
    rejectOriginal(brokenOriginal);
    brokenOriginal = manifest;
    brokenOriginal["original_diffuse"].push_back(brokenOriginal["original_diffuse"][0]);
    rejectOriginal(brokenOriginal);
    brokenOriginal = manifest;
    brokenOriginal["original_diffuse"] = Json::array();
    rejectOriginal(brokenOriginal);

    auto emptyLivery = std::make_shared<Livery>(*livery);
    emptyLivery->textures[{key, 0}] = {scene.materials[0].name, "empty", 0, true};
    TextureResolver emptyBeforeResolver(scene.source, root, emptyLivery);
    const auto emptyBefore = emptyBeforeResolver.material(scene.materials[0]);
    require(bool(emptyBefore), "Common empty texture must resolve to a file or transparent fallback");
    const auto emptyManifest =
        savePaintProject(scene, snapshot, root / "empty-original", "empty", emptyLivery, root);
    const auto emptyDocument = loadPaintDocument(scene, folderOf(emptyManifest) / "project.edmpaint.json");
    TextureResolver emptyResolver(scene.source, root, emptyDocument.livery);
    const auto emptySource = emptyResolver.material(scene.materials[0]);
    check(emptyManifest["original_diffuse"][0]["state"] == (emptyBefore->empty ? "empty" : "file") &&
              emptySource &&
              PaintImage::load(*emptySource, 0).rgba == PaintImage::load(*emptyBefore, 0).rgba &&
              emptyDocument.images.at(0)->rgba == image->rgba,
          "common empty original retains the exact displayed pixels whether it resolves to a game DDS or "
          "transparent fallback");
    const auto emptyResaved = savePaintProject(scene, emptyDocument.images, root / "empty-resaved", "empty",
                                               emptyDocument.livery, emptyDocument.textureDirectory);
    check(emptyResaved["original_diffuse"][0]["state"] == emptyManifest["original_diffuse"][0]["state"],
          "common empty original survives reopening and saving again");
    linearDDS(root / "transparent-original.dds", {0, 0, 0, 0});
    emptyLivery->textures.at({key, 0}) = {scene.materials[0].name, "transparent-original", 0, false};
    const auto transparentManifest =
        savePaintProject(scene, snapshot, root / "transparent-original", "transparent", emptyLivery, root);
    const auto transparentDocument =
        loadPaintDocument(scene, folderOf(transparentManifest) / "project.edmpaint.json");
    TextureResolver transparentResolver(scene.source, root, transparentDocument.livery);
    const auto transparentSource = transparentResolver.material(scene.materials[0]);
    check(transparentSource && transparentSource->bytes() == readFile(root / "transparent-original.dds") &&
              PaintImage::load(*transparentSource, 0).rgba == std::vector<uint8_t>(8 * 8 * 4, 0),
          "transparent original DDS retains zero alpha and exact bytes when edited overrides are hidden");

    auto missingLivery = std::make_shared<Livery>(*livery);
    missingLivery->textures[{key, 0}] = {scene.materials[0].name, "does-not-exist", 0, false};
    const auto missingManifest =
        savePaintProject(scene, snapshot, root / "missing-original", "missing", missingLivery, root);
    const auto missingDocumentBase =
        loadPaintDocument(scene, folderOf(missingManifest) / "project.edmpaint.json");
    TextureResolver missingResolver(scene.source, root, missingDocumentBase.livery);
    check(missingManifest["original_diffuse"][0]["state"] == "missing" &&
              !missingResolver.material(scene.materials[0]) && !missingManifest["self_contained"].get<bool>(),
          "explicit missing original never substitutes the available model default");
    const auto missingResaved =
        savePaintProject(scene, missingDocumentBase.images, root / "missing-resaved", "missing",
                         missingDocumentBase.livery, missingDocumentBase.textureDirectory);
    check(missingResaved["original_diffuse"][0]["state"] == "missing",
          "explicit missing original survives reopening and saving again");
    missingLivery->textures.at({key, 0}).name.clear();
    const auto blankManifest =
        savePaintProject(scene, snapshot, root / "blank-original", "blank", missingLivery, root);
    check(blankManifest["original_diffuse"][0]["state"] == "missing",
          "explicit blank binding stays missing instead of exposing the model default");

    auto noDiffuse = scene;
    noDiffuse.materials[0].textures.erase(noDiffuse.materials[0].textures.begin());
    const auto noDiffuseManifest =
        savePaintProject(noDiffuse, snapshot, root / "no-diffuse", "none", {}, root);
    const auto noDiffuseDocument =
        loadPaintDocument(noDiffuse, folderOf(noDiffuseManifest) / "project.edmpaint.json");
    TextureResolver noDiffuseResolver(noDiffuse.source, root, noDiffuseDocument.livery);
    check(noDiffuseManifest["original_diffuse"][0]["state"] == "default" &&
              !noDiffuseDocument.livery->textures.contains({key, 0}) &&
              !noDiffuseResolver.material(noDiffuse.materials[0]),
          "material with no original diffuse returns to its absent model default");

    auto ambiguousDiffuse = duplicate;
    ambiguousDiffuse.materials[1].textures[0].name = "normal_second";
    const auto ambiguousDiffuseManifest =
        savePaintProject(ambiguousDiffuse, snapshot, root / "ambiguous-diffuse", "ambiguous", {}, root);
    const auto ambiguousDiffuseDocument =
        loadPaintDocument(ambiguousDiffuse, folderOf(ambiguousDiffuseManifest) / "project.edmpaint.json");
    TextureResolver ambiguousDiffuseResolver(scene.source, root, ambiguousDiffuseDocument.livery);
    const auto firstOriginal = ambiguousDiffuseResolver.material(ambiguousDiffuse.materials[0]);
    const auto secondOriginal = ambiguousDiffuseResolver.material(ambiguousDiffuse.materials[1]);
    check(
        ambiguousDiffuseManifest["original_diffuse"][0]["state"] == "default" &&
            !ambiguousDiffuseDocument.livery->textures.contains({key, 0}) &&
            !ambiguousDiffuseManifest["self_contained"].get<bool>() && firstOriginal && secondOriginal &&
            firstOriginal->bytes() == sourceDDS && secondOriginal->path.filename() == "normal_second.png",
        "ambiguous same-name diffuse bindings retain distinct model defaults with a self-contained warning");

    const auto reopenedRecovery = savePaintRecovery(scene, document.images, root / "reopened-recovery", {},
                                                    nullptr, document.livery, document.textureDirectory);
    const auto reopenedRecoveryDocument =
        loadPaintDocument(scene, folderOf(reopenedRecovery) / "project.edmpaint.json");
    TextureResolver reopenedRecoveryResolver(scene.source, reopenedRecoveryDocument.textureDirectory,
                                             reopenedRecoveryDocument.livery);
    const auto recoveredOriginal = reopenedRecoveryResolver.material(scene.materials[0]);
    check(recoveredOriginal && recoveredOriginal->bytes() == sourceDDS &&
              reopenedRecoveryDocument.images.at(0)->rgba == image->rgba &&
              !reopenedRecovery.contains("original_diffuse"),
          "recovery after reopening retains original base bindings and separate edited pixels without extra "
          "DDS copies");

    const auto dynamicPath = root / "dynamic-recovery.lua";
    const std::string dynamicText =
        "assert(EDM_STUDIO.guard=='active', 'must not re-evaluate recovery Lua')\n"
        "livery={{" +
        Json(scene.materials[0].name).dump() +
        ",13,'original_RoughMet',true}}\n"
        "name='evaluated recovery'; custom_args={[38]=EDM_STUDIO.pose}; countries={'USA'}\n";
    textFile(dynamicPath, dynamicText);
    auto dynamicLivery = std::make_shared<Livery>(
        readLivery(dynamicPath, "", "test", "test-unit", {{"guard", "active"}, {"pose", .625}}));
    const auto dynamicBefore = readFile(dynamicPath);
    auto appearanceRecovery =
        savePaintRecovery(scene, snapshot, root / "appearance-recovery", {}, nullptr, dynamicLivery, root);
    auto appearanceDocument =
        loadPaintDocument(scene, folderOf(appearanceRecovery) / "project.edmpaint.json");
    check(appearanceDocument.restoreAppearance && appearanceDocument.livery &&
              appearanceDocument.livery->args == dynamicLivery->args &&
              appearanceDocument.livery->countries == dynamicLivery->countries &&
              appearanceDocument.livery->textures.at({key, 13}).name == "original_RoughMet" &&
              appearanceDocument.livery->path == fs::absolute(dynamicPath) &&
              appearanceDocument.textureDirectory == fs::absolute(root),
          "recovery restores evaluated texture bindings args countries source and extra directory without "
          "re-running dynamic Lua");
    check(readFile(dynamicPath) == dynamicBefore,
          "recovery snapshot and reopen leave original dynamic Lua unchanged");
    auto defaultRecovery = savePaintRecovery(scene, snapshot, root / "default-recovery");
    auto defaultDocument = loadPaintDocument(scene, folderOf(defaultRecovery) / "project.edmpaint.json");
    check(defaultDocument.restoreAppearance && !defaultDocument.livery &&
              defaultDocument.textureDirectory.empty(),
          "null livery snapshot explicitly restores default appearance and clears extra directory");
    auto oldRecovery = appearanceRecovery;
    oldRecovery.erase("appearance");
    const auto oldPath = folderOf(appearanceRecovery) / "old-recovery.json";
    writeJson(oldPath, oldRecovery);
    check(!loadPaintDocument(scene, oldPath).restoreAppearance,
          "older recovery without appearance fields preserves current appearance");
    auto missingRecovery = appearanceRecovery;
    missingRecovery["appearance"]["livery"]["path"] =
        pathString(root / "missing-original" / "description.lua");
    missingRecovery["appearance"]["texture_directory"] = pathString(root / "missing-textures");
    const auto missingPath = folderOf(appearanceRecovery) / "missing-recovery.json";
    writeJson(missingPath, missingRecovery);
    const auto missingDocument = loadPaintDocument(scene, missingPath);
    check(missingDocument.restoreAppearance && missingDocument.livery->args == dynamicLivery->args &&
              missingDocument.warnings.size() >= 2,
          "missing original livery and texture directory produce explicit warnings while retaining evaluated "
          "state");
    auto zipRecovery =
        savePaintRecovery(scene, snapshot, root / "zip-recovery", {}, nullptr, zipLivery, root);
    const auto zipDocument = loadPaintDocument(scene, folderOf(zipRecovery) / "project.edmpaint.json");
    check(zipDocument.restoreAppearance && zipDocument.livery->path == fs::absolute(zipLivery->path) &&
              zipDocument.livery->entry == zipLivery->entry &&
              zipDocument.livery->textures.at({key, 13}).name == "packed_RoughMet",
          "recovery retains ZIP livery path entry and evaluated local texture binding");

    auto tooWide = std::make_shared<PaintImage>(8193, 1, std::array<uint8_t, 4>{10, 20, 30, 255});
    fails([&] { savePaintRecovery(scene, {{0, tooWide}}, root / "too-wide-save"); },
          "saving refuses canvas wider than the editor 8192 pixel limit");
    tooWide->savePNG(folder / "too-wide.png");
    auto oversized = manifest;
    oversized["textures"][0]["file"] = "too-wide.png";
    oversized["textures"][0]["width"] = 8193;
    oversized["textures"][0]["height"] = 1;
    testManifest(oversized);
    const auto descriptionBefore = readFile(folder / "description.lua");
    textFile(folder / "description.lua", "livery={{'bad',0,'../outside',false}}\n");
    fails([&] { loadPaintDocument(scene, project); },
          "complete project appearance rejects escaping local texture references");
    writeFile(folder / "description.lua", descriptionBefore);

    ExportOptions options;
    options.arguments = std::vector<int>{};
    options.textureDirectory = root;
    const auto overrideBytes = image->pngBytes();
    options.diffuseOverrides[0] = overrideBytes;
    exportScene(scene, root / "painted.glb", options);
    check(glbImage(root / "painted.glb", 0) == overrideBytes,
          "GLB embeds exact edited PNG bytes instead of original diffuse");
    check(readFile(scene.source) == sourceEDM && readFile(root / "original.dds") == sourceDDS &&
              readFile(livery->path) == sourceLua,
          "GLB export and project operations preserve originals");
    std::cout << "Paint document: " << checks << " checks passed\n";
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc >= 4 && std::wstring_view(argv[1]) == L"--lua-worker")
        return luaWorker(argv[2], argv[3]);
    try {
        ComRuntime imageRuntime;
        const auto root = fs::current_path() / "validation" /
                          ("paint-document-tests-" + std::to_string(GetCurrentProcessId()));
        tests(root);
        return 0;
    } catch (...) {
        std::cerr << exceptionText() << "\n";
        return 1;
    }
}
