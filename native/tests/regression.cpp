#include "export.h"
#include <iostream>
#include <miniz.h>
#include <wincodec.h>
using namespace edm;
namespace {
int checks = 0;
void check(bool ok, const std::string& name) {
    checks++;
    require(ok, "Regression failed: " + name);
}
template <class F> void fails(F fn, const std::string& name) {
    bool failed = false;
    try {
        fn();
    } catch (...) {
        auto error = exceptionText();
        require(error.find("Lua worker crashed") == std::string::npos, error);
        failed = true;
    }
    check(failed, name);
}
void textFile(const fs::path& path, const std::string& text) {
    writeFile(path, {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
}
struct Glb {
    Json doc;
    std::vector<uint8_t> bytes;
    size_t binary = 0;
    explicit Glb(const fs::path& path) : bytes(readFile(path)) {
        uint32_t size = 0;
        std::memcpy(&size, bytes.data() + 12, 4);
        doc = Json::parse(bytes.begin() + 20, bytes.begin() + 20 + size);
        binary = 28 + size;
    }
    std::vector<float> floats(int accessor) const {
        auto& a = doc["accessors"][accessor];
        auto& v = doc["bufferViews"][a["bufferView"].get<int>()];
        size_t length = v["byteLength"], offset = v.value("byteOffset", size_t(0)) + binary;
        require(offset + length <= bytes.size(), "Invalid exported accessor range");
        std::vector<float> out(length / 4);
        std::memcpy(out.data(), bytes.data() + offset, length);
        return out;
    }
};
void png(const fs::path& path, std::array<uint8_t, 4> pixel) {
    DirectX::ScratchImage image;
    require(SUCCEEDED(image.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 1)), "Test image allocation");
    for (int i = 0; i < 16; i++)
        std::memcpy(image.GetPixels() + i * 4, pixel.data(), 4);
    DirectX::Blob blob;
    require(SUCCEEDED(DirectX::SaveToWICMemory(*image.GetImage(0, 0, 0), DirectX::WIC_FLAGS_NONE,
                                               GUID_ContainerFormatPng, blob)),
            "Test PNG encoding");
    writeFile(path, {static_cast<const uint8_t*>(blob.GetBufferPointer()), blob.GetBufferSize()});
}
void zip(const fs::path& path, const std::map<std::string, std::string>& entries) {
    mz_zip_archive archive{};
    require(mz_zip_writer_init_heap(&archive, 0, 0), "Test ZIP init");
    for (auto& [name, text] : entries)
        require(
            mz_zip_writer_add_mem(&archive, name.c_str(), text.data(), text.size(), MZ_DEFAULT_COMPRESSION),
            "Test ZIP entry");
    void* data = nullptr;
    size_t size = 0;
    require(mz_zip_writer_finalize_heap_archive(&archive, &data, &size), "Test ZIP finish");
    writeFile(path, {static_cast<uint8_t*>(data), size});
    mz_free(data);
    mz_zip_writer_end(&archive);
}
} // namespace
void runRegression() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    fs::path temp =
        fs::temp_directory_path() / (L"edm-native-tests-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(temp);
    fs::path fixtures = wide(EDM_TEST_FIXTURES);
    for (int version : {8, 10}) {
        auto scene = Scene::load(fixtures / ("animation-v" + std::to_string(version) + ".edm"));
        check(scene->version == version && scene->meshes.size() == 1 && scene->meshes[0].indices.size() == 36,
              "Sequential EDM version " + std::to_string(version));
        check(scene->limits.at(0) == std::pair<double, double>{-1, 1}, "Negative argument domain");
        auto& mesh = scene->meshes[0];
        auto p = scene->transformed(mesh, scene->evaluate({{0, -.5}}));
        double angle = -3.141592653589793 / 6;
        auto first = mesh.positions[0];
        V3 expected(first[0] * std::cos(angle) - first[1] * std::sin(angle),
                    1 + first[0] * std::sin(angle) + first[1] * std::cos(angle), first[2]);
        check((V3(p[0][0], p[0][1], p[0][2]) - expected).norm() < 1e-6, "Independent hinge math");
        auto hidden = scene->transformed(mesh, scene->evaluate({{40, .75}}));
        check(std::all_of(hidden.begin(), hidden.end(), [&](auto& v) { return v == hidden[0]; }),
              "Visibility upper endpoint");
        ExportOptions options;
        options.textures = false;
        options.baseline = {{40, .1}};
        auto path = temp / ("animation-v" + std::to_string(version) + ".glb");
        exportScene(*scene, path, options);
        Glb glb(path);
        check(glb.doc["animations"].size() == 2, "Independent animation clips");
        for (auto& clip : glb.doc["animations"]) {
            check(clip["channels"].size() == scene->tracks.size(), "Every clip resets other tracks");
            for (auto& sampler : clip["samplers"]) {
                auto times = glb.floats(sampler["input"]);
                check(std::adjacent_find(times.begin(), times.end(), std::greater_equal<float>()) ==
                          times.end(),
                      "Strictly increasing animation times");
            }
        }
        path.replace_extension(".gltf");
        exportScene(*scene, path, options);
        auto json = Json::parse(readFile(path));
        check(
            json["buffers"][0]["uri"].get<std::string>().starts_with("data:application/octet-stream;base64,"),
            "Single-file glTF");
    }
    auto raw = readFile(fixtures / "animation-v10.edm");
    for (int failure = 0; failure < 4; failure++) {
        auto corrupt = raw;
        if (failure == 0)
            corrupt.resize(2);
        if (failure == 1)
            corrupt.pop_back();
        if (failure == 2)
            corrupt.push_back(1);
        if (failure == 3)
            corrupt[0] = 'X';
        auto path = temp / "invalid.edm";
        writeFile(path, corrupt);
        fails([&] { Scene::load(path); }, "Reject malformed EDM");
    }
    check(Scene::load(fixtures / "visibility-v1.edm")->meshes.size() == 1, "C-130 visibility extension");
    fails([&] { Scene::load(fixtures / "visibility-unknown.edm"); }, "Unknown visibility layout");
    auto spots = Scene::load(fixtures / "spots-all-layouts.edm");
    check(spots->renderTypes["FakeSpotLightsNode"] == 3 && spots->meshes.size() == 1,
          "F-100D spot record boundaries");
    fails([&] { Scene::load(fixtures / "spots-unknown.edm"); }, "Unknown light layout");
    auto skin = Scene::load(fixtures / "skin-nan-packed.edm");
    auto& weighted = skin->meshes[0];
    check(weighted.joints[0][0] == 1 && weighted.weights[0][0] == .25f && weighted.weights[0][4] == .75f,
          "Packed NaN bytes and implicit fifth weight");
    auto moved = skin->transformed(weighted, skin->evaluate({{3, 1}}));
    check(moved[0] == F3{.5f, 0, 0}, "Skin palette index + 1");
    auto mirror = Scene::load(fixtures / "mirrored.edm");
    check(mirror->meshes[0].indices == std::vector<uint32_t>{0, 1, 2} &&
              mirror->meshes[0].extras.value("edm_winding_reversed", false),
          "Mirrored surface winding");
    auto scaled = Scene::load(fixtures / "scale-basis.edm");
    Mat q = rotation(V4(std::sin(.2), 0, 0, std::cos(.2)));
    Mat expected = q.transpose() * scaling(V3(2, 3, 4)) * q;
    check((scaled->defaultWorld[scaled->tails[0]] - expected).cwiseAbs().maxCoeff() < 1e-12,
          "Scale-basis multiplication order");
    auto groups = Scene::load(fixtures / "multi-parent.edm");
    check(groups->meshes.size() == 2 && groups->meshes[0].indices.size() == 3 &&
              groups->meshes[1].indices.size() == 3,
          "POSITION.w rigid groups");
    auto fixture = temp / "number.edm";
    writeFile(fixture, readFile(fixtures / "number.edm"));
    png(temp / "paint.png", {160, 170, 180, 255});
    png(temp / "paint_RoughMet.png", {17, 82, 193, 255});
    png(temp / "digits.png", {220, 220, 220, 255});
    auto number = Scene::load(fixture);
    check(number->numberArgs == std::vector<int>{32}, "NumberNode selectors");
    auto uv = textureUV(number->meshes[0], number->materials[0], 3, {{32, .4}});
    check(std::abs(uv[0][1] - .364) < 1e-7, "Number atlas UV");
    auto encoded = pngTexture(ImageSource{temp / "paint_RoughMet.png"});
    writeFile(temp / "orm-roundtrip.png", encoded);
    auto decoded = loadTexture(ImageSource{temp / "orm-roundtrip.png"}, 0);
    auto pixel = decoded->pixels.GetImage(0, 0, 0);
    check(pixel->pixels[0] == 17 && pixel->pixels[1] == 82 && pixel->pixels[2] == 193 &&
              pixel->pixels[3] == 255,
          "ORM channels and alpha preserved");
    ExportOptions options;
    options.arguments = std::vector<int>{32};
    options.baseline = {{32, .40000000001}};
    auto numberFile = temp / "number.glb";
    auto report = exportScene(*number, numberFile, options);
    check(report["embedded_diffuse_materials"] == 1 && report["embedded_roughmet_materials"] == 1 &&
              report["number_meshes"] == 1,
          "PBR and number export");
    Glb g(numberFile);
    auto& clip = g.doc["animations"][0];
    for (double v : {.05, .35, .85}) {
        int visible = 0;
        double chosen = -1;
        for (auto& channel : clip["channels"]) {
            auto& sampler = clip["samplers"][channel["sampler"].get<int>()];
            auto times = g.floats(sampler["input"]), scales = g.floats(sampler["output"]);
            check(std::adjacent_find(times.begin(), times.end(), std::greater_equal<float>()) == times.end(),
                  "Number times merge float32 collisions");
            auto it = std::upper_bound(times.begin(), times.end(), float(v * 3));
            size_t index = it == times.begin() ? 0 : size_t(it - times.begin() - 1);
            if (scales[index * 3] > .5) {
                visible++;
                auto name = g.doc["nodes"][channel["target"]["node"].get<int>()]["name"].get<std::string>();
                auto state = Json::parse(name.substr(name.rfind(" / ") + 3));
                chosen = state["32"];
            }
        }
        check(visible == 1 && std::abs(chosen - std::floor(v * 10) / 10) < 1e-9,
              "Exactly one STEP number state");
    }
    auto skinDir = temp / L"unit/涂装";
    fs::create_directories(skinDir);
    textFile(skinDir / "shared.lua", "return {prefix='body'}");
    textFile(skinDir / "description.lua", "local "
                                          "s=require('shared');livery={{s.prefix,13,s.prefix..'_RM',false}};"
                                          "name=_('演示');custom_args={[1001]=1/2}");
    auto livery = readLivery(skinDir);
    check(livery.name == "演示" && livery.args[1001] == .5 &&
              livery.evaluation["includes"][0] == "shared.lua",
          "Unicode livery and scoped require");
    auto pack = temp / "skin.zip";
    zip(pack, {{"skin/description.lua", "dofile('../shared.lua');name='ZIP'"},
               {"shared.lua", "livery={{'body',0,'diff',false}}"}});
    check(readLivery(pack).name == "ZIP", "ZIP Lua include");
    textFile(temp / "outside.lua", "livery={}");
    textFile(skinDir / "description.lua", "dofile('../../outside.lua')");
    fails([&] { readLivery(skinDir); }, "Lua include boundary");
    for (auto code : {"livery={};os.execute('test')", "livery={};debug.sethook()",
                      "livery={};require('socket')", "livery={};name=string.rep('x',64000000)",
                      "livery={};while true do end", "livery={};livery[1]=livery"}) {
        std::cout << "Lua case: " << code << std::endl;
        bool rejected = false;
        try {
            evaluateLua(code);
        } catch (...) {
            auto message = exceptionText();
            rejected = message.starts_with("动态 Lua 求值失败：");
            check(rejected, "Lua rejection must be a structured error, never a crash or timeout: " + message);
        }
        check(rejected, "Lua sandbox and quotas");
    }
    auto protectedError = evaluateLua("local ok=pcall(require,'missing'); livery={}; name=tostring(ok)");
    check(protectedError["environment"]["name"] == "false", "C++ include errors remain catchable Lua errors");
    auto nested = evaluateLua("livery={}; local t=livery; for i=1,30 do t.child={};t=t.child end");
    check(nested["environment"].contains("livery"), "Valid deep result reserves Lua API stack");
    auto start = Clock::now();
    fails([&] { evaluateLua("while true do pcall(function() while true do end end) end"); },
          "Hard Lua timeout");
    check(seconds(start) < 7, "Lua worker stops promptly");
    fails([&] { evaluateLua("livery={}", {}, "", {{"_G", 1}}); }, "Reserved Lua context key");
    std::cout << checks << " native regression checks passed\n";
    if (temp.parent_path() == fs::temp_directory_path())
        fs::remove_all(temp);
    CoUninitialize();
}
