#include "export_internal.h"
#include "attachment.h"
#include "paint_document.h"
#include "animation_analysis.h"
#include <ufbx.h>
#include <iostream>
#include <sstream>

using namespace edm;
namespace {
size_t checks = 0;
void check(bool value, const char* message) {
    require(value, std::string("LOD regression: ") + message);
    ++checks;
}
template <class F> void rejects(F action, const char* message) {
    bool caught = false;
    try {
        action();
    } catch (const std::exception&) {
        caught = true;
    }
    check(caught, message);
}
// Independently serialized original EDM v8 fixture, with no game content.
struct Bytes {
    std::vector<uint8_t> data{'E', 'D', 'M', 8, 0};
    template <class T> void value(T v) {
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        data.insert(data.end(), p, p + sizeof(v));
    }
    void u(uint32_t v) {
        value(v);
    }
    void d(double v) {
        value(v);
    }
    void str(const std::string& s) {
        u(uint32_t(s.size()));
        data.insert(data.end(), s.begin(), s.end());
    }
    void base(const std::string& name) {
        str(name);
        u(0);
        u(0);
    }
    void matrix() {
        for (int i = 0; i < 16; ++i)
            d(i % 5 == 0 ? 1. : 0.);
    }
    void animation() {
        matrix();
        for (double v : {0., 0., 0., 0., 0., 0., 1., 0., 0., 0., 1., 1., 1., 1.})
            d(v);
        u(1);
        u(7);
        u(2);
        for (double v : {0., 0., 0., 0., 1., 2., 0., 0.})
            d(v);
        u(0);
        u(0);
    }
};
std::vector<uint8_t> fixture(int malformed = 0) {
    Bytes b;
    b.u(0);
    b.u(0);
    b.str("model::RootNode");
    b.base("LOD selection fixture");
    b.value(uint8_t(0));
    for (int i = 0; i < 18; ++i)
        b.d(0);
    b.u(2);
    for (bool skin : {false, true}) {
        b.u(3);
        b.str("VERTEX_FORMAT");
        b.u(skin ? 22 : 5);
        for (int i = 0; i < (skin ? 22 : 5); ++i)
            b.value(uint8_t(i == 0 ? 4 : i == 1 ? 3 : i == 4 ? 2 : skin && i == 21 ? 4 : 0));
        b.str("MATERIAL_NAME");
        b.str("def_material");
        b.str("NAME");
        b.str(skin ? "Skin material" : "Body material");
    }
    b.u(0);
    b.u(0);
    b.u(15);
    for (int i = 0; i < 15; ++i) {
        if (i == 1 || i == 4 || i == 7) {
            b.str("model::LodNode");
            b.base("Distance group");
            const int levels = (malformed == 1 && i == 1) ? 1 : 2;
            b.u(levels);
            for (int j = 0; j < levels; ++j) {
                bool nearest = i == 1 ? j == 1 : j == 0;
                const bool empty = i == 1 && (malformed == 5 || (malformed == 4 && j == 0));
                b.d(empty ? 0. : malformed == 2 && i == 1 ? -1. : nearest ? 0. : 10000.);
                b.d(empty ? 0. : nearest ? 10000. : 1000000.);
            }
        } else if (i == 10) {
            b.str("model::ArgVisibilityNode");
            b.base("Active appearance");
            b.u(1);
            b.u(38);
            b.u(1);
            b.d(-.25);
            b.d(.5);
        } else if (i == 11 || i == 14) {
            b.str("model::ArgAnimationNode");
            b.base("Independent hinge");
            b.animation();
        } else {
            const bool bone = i == 0 || i == 2 || i == 3 || i == 12 || i == 13;
            b.str(bone ? "model::Bone" : "model::TransformNode");
            // Misleading names deliberately prove that names do not select LODs.
            b.base(i == 2 ? "LOD0" : i == 3 ? "Low quality" : "Unnamed part");
            b.matrix();
            if (bone)
                b.matrix();
        }
    }
    const std::vector<int> parents{-1, 0, 1, 1, 3, 4, 4, 0, 7, 7, 3, 10, 3, 2, 2};
    for (int i = 0; i < 15; ++i)
        b.value(int32_t(malformed == 3 && i == 0 ? 14 : parents[i]));
    b.u(1);
    b.str("RENDER_NODES");
    b.u(4);
    auto geometry = [&](bool skin, const std::vector<int>& parts) {
        b.u(uint32_t(parts.size() * 3));
        b.u(skin ? 13 : 9);
        for (size_t i = 0; i < parts.size(); ++i) {
            const float z = (parts[i] == 2 || parts[i] == 6 || parts[i] == 9 || parts[i] == 14) ? 1.f : 0.f;
            for (auto xy : {F2{0, 0}, F2{1, 0}, F2{0, 1}}) {
                b.value(xy[0]);
                b.value(xy[1]);
                b.value(z);
                if (skin)
                    b.u(0);
                else
                    b.value(float(i));
                for (float v : {0.f, 0.f, 1.f, xy[0], xy[1]})
                    b.value(v);
                if (skin)
                    for (float v : {1.f, 0.f, 0.f, 0.f})
                        b.value(v);
            }
        }
        b.value(uint8_t(1));
        b.u(uint32_t(parts.size() * 3));
        b.u(5);
        for (uint16_t i = 0; i < parts.size() * 3; ++i)
            b.value(i);
    };
    b.str("model::RenderNode");
    b.base("Batched alternatives");
    b.u(0);
    b.u(0);
    const std::vector<int> parts{2, 3, 5, 6, 8, 9, 11, 14};
    b.u(uint32_t(parts.size()));
    for (int p : parts) {
        b.u(p);
        b.u(0);
        b.value(int32_t(-1));
    }
    geometry(false, parts);
    for (auto bones : {std::pair{3, 12}, std::pair{2, 13}, std::pair{0, 13}}) {
        b.str("model::SkinNode");
        b.base(bones.first == 3 ? "Selected skin" : "Excluded skin");
        b.u(0);
        b.u(1);
        b.u(2);
        b.u(bones.first);
        b.u(bones.second);
        b.u(0);
        geometry(true, {bones.first});
    }
    return b.data;
}
size_t active(const Scene& s) {
    return std::count_if(s.meshes.begin(), s.meshes.end(), [](const Mesh& m) { return m.lodActive; });
}
void fbxCheck(const fs::path& path, size_t meshes, bool pure) {
    ufbx_error error{};
    ufbx_load_opts options{};
    auto* s = ufbx_load_file(pathString(path).c_str(), &options, &error);
    require(s != nullptr, "Independent FBX import failed");
    check(s->meshes.count == meshes, "FBX contains only selected LOD geometry");
    if (pure)
        check(!s->bones.count && !s->anim_stacks.count, "Pure FBX has no rig or animation");
    ufbx_free_scene(s);
}
void run(const fs::path& dir) {
    const auto source = dir / "lod.edm";
    writeFile(source, fixture());
    auto scene = Scene::load(source);
    check(scene->meshes.size() == 11 && active(*scene) == 5, "Keep all source IDs but activate five parts");
    check(scene->summary()["lod_excluded_meshes"] == 6, "LOD diagnostics reflect omitted geometry");
    check(scene->nodes[scene->heads[1]].extras["edm_lod_selected_level"] == 1,
          "Nearest range need not be first child");
    const std::vector<bool> expected{false, true, true, false, true, false, true, false, true, false, false};
    auto world = scene->evaluate({{7, 1.}});
    for (size_t i = 0; i < expected.size(); ++i) {
        const auto& m = scene->meshes[i];
        check(m.lodActive == expected[i], "Nested, independent, batched and skinned LOD selection");
        check(m.indices.size() == 3 && m.uvs.size() == 1,
              "Original triangles and UVs retained for project identity");
    }
    check(std::abs(scene->transformed(scene->meshes[6], world)[0][0] - 2.f) < 1e-6,
          "Selected animation still moves by two metres");
    check(world[scene->tails[14]].block<3, 3>(0, 0).norm() == 0,
          "Animation cannot reveal an excluded branch");
    PaintSurface surface(scene, {});
    check(surface.triangleCount() == 5, "Paint BVH excludes inactive LODs including shared-root skin");
    auto hit = surface.raycast(V3(.2, .2, 2), V3(0, 0, -1));
    check(hit.has_value() && expected.at(hit->mesh), "Inactive nearer surfaces do not occlude picking");
    PaintSurface hidden(scene, {{38, .75}});
    check(hidden.triangleCount() == 4, "LOD selection preserves argument visibility");
    const auto analysis = analyzeAnimations(*scene);
    for (const auto& [argument, finding] : analysis.arguments)
        for (auto index : finding.meshes)
            check(expected.at(index), "Animation inference ignores low LODs");
    ExportOptions opts;
    opts.textures = false;
    for (bool pure : {false, true}) {
        opts.modelOnly = pure;
        auto payload = buildExportPayload(*scene, opts, {}, nullptr);
        check(payload.document["meshes"].size() == 5, "glTF payload excludes every lower LOD");
        if (pure)
            check(!payload.document.contains("animations") && !payload.document.contains("skins"),
                  "Pure export remains static meshes");
        else
            check(payload.document.contains("animations") && payload.document["skins"].size() == 1,
                  "Selected skin and animations are exported");
        for (const auto* ext : {"fbx", "glb", "gltf", "obj"}) {
            const auto output = dir / (std::string(pure ? "pure." : "animated.") + ext);
            exportScene(*scene, output, opts);
            if (std::string_view(ext) == "fbx")
                fbxCheck(output, 5, pure);
            if (std::string_view(ext) == "obj") {
                std::ifstream input(output);
                std::string line;
                size_t faces = 0;
                while (std::getline(input, line))
                    faces += line.starts_with("f ");
                check(faces == 5, "OBJ contains exactly the selected five triangles");
            }
        }
    }
    opts.modelOnly = true;
    opts.baseline = {{38, .75}};
    const auto frozen = buildExportPayload(*scene, opts, {}, nullptr);
    check(frozen.document["meshes"].size() == 4, "Static snapshot still omits hidden appearance variants");
    // A legacy all-LOD project has identical mesh indices, raw buffers and assembly ranges.
    auto legacy = *scene;
    for (auto& m : legacy.meshes)
        m.lodActive = true;
    PaintImage image;
    image.width = 2;
    image.height = 2;
    image.rgba.assign(16, 255);
    PaintSnapshot images{{0, std::make_shared<PaintImage>(image)}};
    auto saved = savePaintRecovery(legacy, images, dir / "paint");
    auto loaded = loadPaintProject(*scene, fs::path(wide(saved.at("directory").get<std::string>())) /
                                               "project.edmpaint.json");
    check(loaded.at(0)->rgba == image.rgba, "Saved all-LOD paint project remains compatible");
    Node connector;
    connector.name = "Pylon";
    connector.parent = 0;
    connector.t = V3(10, 0, 0);
    connector.extras = {{"edm_type", "Connector"}};
    const int target = int(scene->nodes.size());
    scene->nodes.push_back(connector);
    scene->staticLocal.push_back(connector.local());
    scene->order.push_back(target);
    scene->defaultWorld = scene->evaluate({});
    auto attached = attachScene(*scene, *Scene::load(source), target);
    check(attached->meshes.size() == 22 && active(*attached) == 10,
          "Independent attachment preserves LOD masks and mesh ranges");
    check(PaintSurface(attached, {}).triangleCount() == 10, "Attached selected surfaces are paintable");
    check(PaintSurface(attached, {}, {}, nullptr, false).triangleCount() == 5,
          "Hidden attachment cannot obstruct paint");
    opts.baseline.clear();
    auto combined = buildExportPayload(*attached, opts, {}, nullptr);
    check(combined.document["meshes"].size() == 10,
          "Export includes only selected LOD of host and attachment");
    auto detached = detachScene(*attached, target);
    check(active(*detached.scene) == 5 && detached.scene->meshes.size() == 11,
          "Detaching restores source LOD masks and indices");
    writeFile(dir / "empty-range.edm", fixture(4));
    check(active(*Scene::load(dir / "empty-range.edm")) == 5,
          "Empty distance intervals cannot override the nearest usable LOD");
    for (int error : {1, 2, 3, 5}) {
        auto bad = dir / ("invalid-" + std::to_string(error) + ".edm");
        writeFile(bad, fixture(error));
        rejects([&] { Scene::load(bad); }, "Ambiguous ranges, invalid distances and cycles fail clearly");
    }
}
} // namespace
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        ComRuntime runtime;
        ComApartment apartment;
        if (argc == 3 && std::string_view(argv[1]) == "--write-fixture") {
            writeFile(wide(argv[2]), fixture());
            return 0;
        }
        const auto dir =
            fs::temp_directory_path() / ("edm-lod-tests-" + std::to_string(GetCurrentProcessId()));
        fs::create_directories(dir);
        run(dir);
        fs::remove_all(dir);
        std::cout << "LOD selection: " << checks << " checks passed\n";
        return 0;
    } catch (...) {
        std::cerr << exceptionText() << '\n';
        return 1;
    }
}
