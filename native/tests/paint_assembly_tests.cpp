#include "attachment.h"
#include "paint_assembly.h"
#include <iostream>

using namespace edm;
namespace {
int checks = 0;
void check(bool value, const char* message) {
    require(value, std::string("Paint assembly restoration: ") + message);
    ++checks;
}
template <class Function> void fails(Function action, const char* message) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, message);
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
    // Extend our small original v8 animation fixture with actual EDM connector records. This
    // exercises the real parser/reloader and never packages or depends on a DCS game model.
    auto bytes = readFile(fs::path(wide(EDM_TEST_FIXTURES)) / "animation-v8.edm");
    std::vector<uint8_t> category;
    text(category, "RENDER_NODES");
    const auto found = std::search(bytes.begin(), bytes.end(), category.begin(), category.end());
    require(found != bytes.end() && found - bytes.begin() >= 4, "Fixture render category missing");
    const auto offset = size_t(found - bytes.begin()) - 4;
    require(bytes[offset] == 1 && bytes[offset + 1] == 0 && bytes[offset + 2] == 0 && bytes[offset + 3] == 0,
            "Fixture category layout changed");
    bytes[offset] = 2;
    text(bytes, "CONNECTORS");
    u32(bytes, 3);
    for (const auto* name : {"Pylon1", "Point01", "AttachPoint"}) {
        text(bytes, "model::Connector");
        text(bytes, name);
        u32(bytes, 0); // version
        u32(bytes, 0); // properties
        u32(bytes, 0); // source node parent
        u32(bytes, 0); // reserved connector field
    }
    writeFile(path, bytes);
}
int ownedPoint(const Scene& scene, int owner) {
    for (int index = 0; index < int(scene.nodes.size()); ++index) {
        const auto& node = scene.nodes[index];
        if (isConnector(node) && node.name == "Point01" && node.extras.value("edm_attachment", -1) == owner)
            return index;
    }
    throw std::runtime_error("Fixture rack Point01 missing");
}
void tests(const fs::path& root) {
    fs::create_directories(root);
    const auto aircraftFile = root / "aircraft.edm", rackFile = root / "rack.edm",
               weaponFile = root / "weapon.edm";
    for (const auto& file : {aircraftFile, rackFile, weaponFile})
        connectorFixture(file);
    auto aircraft = Scene::load(aircraftFile), rack = Scene::load(rackFile), weapon = Scene::load(weaponFile);
    check(aircraft->connectorCount == 3, "The native parser reads the original connector fixture");
    aircraft->defaultArgs = {{0, .2}, {40, .1}};
    auto assembly = attachScene(*aircraft, *rack, findConnector(*aircraft, "Pylon1"), {{0, -.25}, {40, .4}});
    assembly = attachScene(*assembly, *weapon, ownedPoint(*assembly, 0), {{0, .65}, {40, .3}});
    const auto metadata = paintAssemblyMetadata(*assembly);
    check(metadata["attachments"][1]["target_name"] == "Point01",
          "The saved assembly includes a nested rack Point");
    auto image = std::make_shared<PaintImage>(8, 8, std::array<uint8_t, 4>{22, 44, 88, 91});
    PaintSnapshot images{{0, image}, {1, image}, {2, image}};
    auto saved = savePaintRecovery(*assembly, images, root / "projects");
    const fs::path folder = wide(saved.at("directory").get<std::string>());
    const auto project = folder / "project.edmpaint.json";
    auto matched = restorePaintAssembly(assembly, project);
    check(matched == assembly,
          "Opening an already matching composition preserves the scene pointer and GPU geometry");

    auto fresh = Scene::load(aircraftFile);
    auto restored = restorePaintAssembly(fresh, folder);
    check(restored != fresh && fresh->attachments.empty() && restored->attachments.size() == 2,
          "A project folder restores its rack and weapon without changing the current aircraft");
    check(
        paintAssemblyMetadata(*restored) == metadata,
        "Restoration preserves exact target indices, duplicate connector names, argument maps and defaults");
    auto document = loadPaintDocument(*restored, project);
    check(document.images.size() == 3 && document.images.at(2)->rgba == image->rgba,
          "The rebuilt assembly passes full saved geometry/material/image fingerprint validation");
    check(restorePaintAssembly(restored, folder) == restored,
          "Repeated project opening does not append duplicate weapons");
    for (auto [argument, value] : assembly->defaultArgs)
        check(restored->defaultArgs.contains(argument) && restored->defaultArgs.at(argument) == value,
              "Both host and independently remapped child default arguments survive reopening");
    const auto expectedWorld = assembly->evaluate({}), actualWorld = restored->evaluate({});
    check(expectedWorld.size() == actualWorld.size(), "Restored animation graph has the original node count");
    for (size_t node = 0; node < actualWorld.size(); ++node)
        check((actualWorld[node] - expectedWorld[node]).cwiseAbs().maxCoeff() < 1e-12,
              "Restored default pose matches the complete original aircraft/rack/weapon assembly");

    auto different = attachScene(*fresh, *weapon, findConnector(*fresh, "Pylon1"));
    const auto unchanged = paintAssemblyMetadata(*different);
    check(paintAssemblyMetadata(*restorePaintAssembly(different, project)) == metadata &&
              paintAssemblyMetadata(*different) == unchanged,
          "A different current assembly is replaced atomically rather than appended to or mutated");

    const auto invalid = folder / "invalid.edmpaint.json";
    auto tampered = saved;
    for (auto& attachment : tampered["assembly"]["attachments"])
        attachment["source"] =
            pathString(fs::relative(fs::path(wide(attachment["source"].get<std::string>())), folder));
    writeJson(invalid, tampered);
    check(paintAssemblyMetadata(*restorePaintAssembly(fresh, invalid)) == metadata,
          "Relative source EDM paths resolve against the project folder without searching by filename");
    tampered = saved;
    tampered["assembly"]["attachments"][0]["source"] = pathString(root / "missing-store.edm");
    writeJson(invalid, tampered);
    fails([&] { restorePaintAssembly(assembly, invalid); },
          "Missing source EDMs are reported even when the current model is already loaded");
    check(paintAssemblyMetadata(*assembly) == metadata,
          "Missing-file failure leaves the existing combination untouched");
    tampered = saved;
    tampered["assembly"]["attachments"][1]["target_name"] = "Pylon1";
    writeJson(invalid, tampered);
    fails([&] { restorePaintAssembly(fresh, invalid); },
          "A valid target index with the wrong saved connector name is rejected");
    tampered = saved;
    tampered["assembly"]["attachments"][0]["target_node"] = 0;
    writeJson(invalid, tampered);
    fails([&] { restorePaintAssembly(fresh, invalid); },
          "A normal graph node cannot masquerade as a mounting connector");
    tampered = saved;
    tampered["assembly"]["attachments"][0]["argument_map"]["0"] = 999;
    writeJson(invalid, tampered);
    fails([&] { loadPaintDocument(*restorePaintAssembly(fresh, invalid), invalid); },
          "Changed argument remapping fails the full project fingerprint before publication");
    tampered = saved;
    tampered["assembly"]["default_args"]["0"] = "invalid";
    writeJson(invalid, tampered);
    fails([&] { restorePaintAssembly(fresh, invalid); },
          "Malformed default values cannot silently alter the assembled pose");
    tampered = saved;
    tampered["assembly"]["attachments"][1]["mesh_count"] = 1234;
    writeJson(invalid, tampered);
    fails([&] { restorePaintAssembly(fresh, invalid); },
          "Changed child geometry layout metadata is rejected");
    check(fresh->attachments.empty() && different->attachments.size() == 1 &&
              paintAssemblyMetadata(*assembly) == metadata,
          "Every failed rebuild preserves all previously supplied scenes");

    std::atomic_bool cancel = true;
    fails([&] { restorePaintAssembly(fresh, project, {}, &cancel); },
          "A pre-cancelled project does not begin reloading models");
    cancel = false;
    bool reachedChild = false;
    fails(
        [&] {
            restorePaintAssembly(
                fresh, project,
                [&](const std::string& message) {
                    if (message.find("恢复外挂") != std::string::npos) {
                        reachedChild = true;
                        cancel = true;
                    }
                },
                &cancel);
        },
        "Cancellation while restoring a child stops the entire assembly operation");
    check(reachedChild && fresh->attachments.empty(),
          "Mid-rebuild cancellation leaves the currently loaded model intact");

    auto singleSaved = savePaintRecovery(*fresh, {{0, image}}, root / "legacy");
    const fs::path singleFolder = wide(singleSaved.at("directory").get<std::string>());
    singleSaved.erase("assembly");
    writeJson(singleFolder / "project.edmpaint.json", singleSaved);
    auto single = restorePaintAssembly(assembly, singleFolder);
    check(single->attachments.empty() && single->defaultArgs.empty(),
          "A pre-assembly paint project restores the aircraft without leftover external stores");
    check(loadPaintDocument(*single, singleFolder / "project.edmpaint.json").images.size() == 1,
          "Legacy version-2 painting projects still pass complete fingerprint checks");
}
} // namespace

int wmain() {
    try {
        ComRuntime runtime;
        const auto root =
            fs::temp_directory_path() / (L"edm-paint-assembly-" + std::to_wstring(GetCurrentProcessId()));
        tests(root);
        std::cout << "Paint assembly restoration: " << checks << " checks passed\n";
        return 0;
    } catch (...) {
        std::cerr << exceptionText() << '\n';
        return 1;
    }
}
