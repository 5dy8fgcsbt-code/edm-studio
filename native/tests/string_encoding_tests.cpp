#include "paint_document.h"
#include <iostream>

using namespace edm;
namespace {
int checks = 0;
void check(bool value, const std::string& description) {
    ++checks;
    require(value, "EDM string encoding: " + description);
}
struct Writer {
    int version;
    std::vector<uint8_t> bytes;
    std::vector<std::string> strings;
    template <class T> void pod(T value) {
        const auto* p = reinterpret_cast<const uint8_t*>(&value);
        bytes.insert(bytes.end(), p, p + sizeof(value));
    }
    void literal(const std::string& text) {
        pod(uint32_t(text.size()));
        bytes.insert(bytes.end(), text.begin(), text.end());
    }
    void str(const std::string& text) {
        if (version == 8)
            literal(text);
        else {
            auto found = std::find(strings.begin(), strings.end(), text);
            require(found != strings.end(), "test writer string not interned");
            pod(uint32_t(found - strings.begin()));
        }
    }
    void base(const std::string& name) {
        literal(name); // Both EDM versions retain literal node names.
        pod(uint32_t(0));
        pod(uint32_t(0));
    }
};
void fixture(const fs::path& path, int version, bool utf8Encoding) {
    const std::string cyrillic = utf8Encoding ? "\xD0\xA1" : "\xD1";
    const auto material = "AIM_120" + cyrillic, texture = "paint_" + cyrillic;
    Writer out{version};
    out.strings = {"model::RootNode",  "NAME",       "MATERIAL_NAME", "TEXTURES", "model::Node",
                   "model::Connector", "CONNECTORS", material,        texture,    "model::default"};
    out.bytes = {'E', 'D', 'M'};
    out.pod(uint16_t(version));
    if (version == 10) {
        std::string table;
        for (const auto& text : out.strings) {
            table += text;
            table += '\0';
        }
        out.literal(table);
    }
    out.pod(uint32_t(0)); // Lookup tables.
    out.pod(uint32_t(0));
    out.str("model::RootNode");
    out.base("AsciiRoot");
    if (version == 8)
        out.pod(uint8_t(0));
    for (int i = 0; i < 18; ++i)
        out.pod(double(0));
    out.pod(uint32_t(1)); // One material, including both literal and interned string fields.
    out.pod(uint32_t(3));
    out.str("NAME");
    out.str(material);
    out.str("MATERIAL_NAME");
    out.str("model::default");
    out.str("TEXTURES");
    out.pod(uint32_t(1));
    out.pod(uint32_t(0));
    out.pod(int32_t(0));
    out.str(texture);
    for (int i = 0; i < 16; ++i)
        out.pod(uint8_t(0));
    for (int i = 0; i < 16; ++i)
        out.pod(float(i % 5 == 0 ? 1 : 0));
    out.pod(uint64_t(0));
    out.pod(uint32_t(1));
    out.str("model::Node");
    out.base("AsciiParent");
    out.pod(int32_t(-1));
    out.pod(uint32_t(1));
    out.str("CONNECTORS");
    out.pod(uint32_t(1));
    out.str("model::Connector");
    out.base("Point_" + cyrillic);
    out.pod(uint32_t(0));
    out.pod(uint32_t(0));
    writeFile(path, out.bytes);
}
void tests(const fs::path& root) {
    for (int version : {8, 10})
        for (bool isUtf8 : {false, true}) {
            const auto directory = root / ("v" + std::to_string(version) + (isUtf8 ? "-utf8" : "-cp1251"));
            const auto source = directory / "sample.edm";
            fixture(source, version, isUtf8);
            const auto doc = parseEdm(source);
            const std::string cyrillic = "\xD0\xA1";
            check(doc.version == version && doc.materials.size() == 1 && doc.connectors.size() == 1,
                  "valid minimal material/connector EDM fixture parses");
            check(doc.materials[0].name == "AIM_120" + cyrillic,
                  "material is exact UTF-8 AIM_120C with Cyrillic U+0421");
            check(doc.materials[0].texture(0)->name == "paint_" + cyrillic,
                  "non-ASCII texture names retain their Unicode identity");
            check(doc.connectors[0].name == "Point_" + cyrillic,
                  "literal connector names support the same encoding as material names");
            check(doc.nodes[0].name == "AsciiParent" && doc.materials[0].shader == "model::default",
                  "ASCII node and shader names are unchanged");
            Scene scene;
            scene.source = source;
            scene.materials = doc.materials;
            scene.version = version;
            const auto texture = directory / wide("paint_" + cyrillic + ".dds");
            PaintImage(4, 4, {35, 120, 230, 255}).saveDDS(texture);
            const auto saved = exportLiveryAssets(scene, {}, directory / "export", "encoded material");
            const auto livery =
                readLivery(fs::path(wide(saved.at("directory").get<std::string>())) / "description.lua");
            check(livery.textures.contains({lower("AIM_120" + cyrillic), 0}) &&
                      saved["missing_textures"].empty(),
                  "generated DCS Lua addresses the original material without mojibake");
        }
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc >= 4 && std::wstring_view(argv[1]) == L"--lua-worker")
        return luaWorker(argv[2], argv[3]);
    try {
        ComRuntime runtime;
        const auto root = fs::current_path() / "validation" /
                          ("string-encoding-tests-" + std::to_string(GetCurrentProcessId()));
        tests(root);
        std::cout << "EDM string encoding: " << checks << " checks passed\n";
        return 0;
    } catch (...) {
        std::cerr << exceptionText() << '\n';
        return 1;
    }
}
