#include "assets.h"
#include <iostream>
#include <miniz.h>

using namespace edm;
namespace {
using Bytes = std::vector<uint8_t>;
int checks = 0, failures = 0;
std::string currentCase;
void check(bool condition, const std::string& description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL [" << currentCase << "] " << description << '\n';
    }
}
template <class F> void run(const char* name, F action) {
    currentCase = name;
    try {
        action();
    } catch (...) {
        ++failures;
        std::cerr << "FAIL [" << name << "] " << exceptionText() << '\n';
    }
}
Bytes textBytes(const std::string& text) {
    return {text.begin(), text.end()};
}
void file(const fs::path& path, const Bytes& bytes) {
    fs::create_directories(path.parent_path());
    writeFile(path, bytes);
}
Bytes dds(std::array<uint8_t, 4> color) {
    DirectX::ScratchImage image;
    require(SUCCEEDED(image.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 1)),
            "Cannot allocate resolver fixture DDS");
    for (size_t i = 0; i < 16; ++i)
        std::copy(color.begin(), color.end(), image.GetPixels() + i * 4);
    DirectX::Blob blob;
    require(SUCCEEDED(DirectX::SaveToDDSMemory(image.GetImages(), image.GetImageCount(), image.GetMetadata(),
                                               DirectX::DDS_FLAGS_NONE, blob)),
            "Cannot encode resolver fixture DDS");
    const auto* bytes = static_cast<const uint8_t*>(blob.GetBufferPointer());
    return {bytes, bytes + blob.GetBufferSize()};
}
void zip(const fs::path& path, const std::map<std::string, Bytes>& files) {
    mz_zip_archive archive{};
    require(mz_zip_writer_init_heap(&archive, 0, 0), "Cannot initialize resolver fixture ZIP");
    void* data = nullptr;
    try {
        for (const auto& [name, bytes] : files)
            require(mz_zip_writer_add_mem(&archive, name.c_str(), bytes.data(), bytes.size(),
                                          MZ_DEFAULT_COMPRESSION),
                    "Cannot add resolver fixture ZIP entry");
        size_t size = 0;
        require(mz_zip_writer_finalize_heap_archive(&archive, &data, &size),
                "Cannot finish resolver fixture ZIP");
        fs::create_directories(path.parent_path());
        writeFile(path, {static_cast<const uint8_t*>(data), size});
        mz_free(data);
        data = nullptr;
        mz_zip_writer_end(&archive);
    } catch (...) {
        mz_free(data);
        mz_zip_writer_end(&archive);
        throw;
    }
}
struct Fixture {
    fs::path root, unit, source;
    Material material;
    Bytes a = dds({17, 31, 47, 255}), b = dds({59, 71, 83, 255}), other = dds({101, 113, 127, 255});
    Bytes rough = dds({7, 129, 241, 255}), physical = dds({149, 163, 179, 255}),
          fallback = dds({191, 211, 223, 255});
    explicit Fixture(const fs::path& directory)
        : root(directory), unit(root / "unit"), source(root / "model/Shapes/test.edm") {
        file(source, textBytes("Synthetic model path; no EDM parser is used by this resolver test."));
        file(source.parent_path() / "Textures/fixture_default.dds", fallback);
        file(source.parent_path() / "Textures/fixture_default_roughmet.dds", fallback);
        material.name = "F100 Wing";
        material.textures = {{0, "fixture_default"}, {13, "fixture_default_roughmet"}};
    }
    std::shared_ptr<Livery> selected(const fs::path& path, const std::string& entry = {}) const {
        auto livery = std::make_shared<Livery>();
        livery->path = fs::absolute(path);
        livery->entry = entry;
        livery->name = "Synthetic selected skin";
        return livery;
    }
    void overrideName(const std::shared_ptr<Livery>& livery, const std::string& name, int slot) const {
        livery->textures[{lower(material.name), slot}] = {material.name, name, slot, false};
    }
    void expect(TextureResolver& resolver, const std::shared_ptr<Livery>& livery, const std::string& name,
                const Bytes& expected, const fs::path& path, const std::string& entry = {},
                int slot = 0) const {
        overrideName(livery, name, slot);
        const auto missing = resolver.missing.size();
        auto image = resolver.material(material, slot);
        check(image.has_value(), "Resolve '" + name + "' in slot " + std::to_string(slot));
        if (!image)
            return;
        check(!image->empty, "Resolved local texture is a real image, not a transparent placeholder");
        check(image->bytes() == expected, "Read exact expected DDS bytes for '" + name + "'");
        std::error_code error;
        check(fs::equivalent(image->path, path, error) && !error,
              "Selected the intended physical file or ZIP for '" + name + "'");
        check(image->entry == entry, "Selected the intended archive-relative entry for '" + name + "'");
        check(resolver.missing.size() == missing, "Successful explicit override is not reported missing");
    }
    void absent(TextureResolver& resolver, const std::shared_ptr<Livery>& livery, const std::string& name,
                int slot = 0) const {
        overrideName(livery, name, slot);
        const auto missing = resolver.missing.size();
        auto image = resolver.material(material, slot);
        check(!image, "Unresolvable or rejected local reference stays missing: '" + name + "'");
        check(resolver.missing.size() == missing + 1,
              "Missing override is reported instead of falling back: '" + name + "'");
        if (image)
            check(image->bytes() != fallback,
                  "Missing local override cannot silently substitute the model's default DDS");
    }
};
void siblingArchives(const fs::path& root) {
    Fixture f(root);
    auto a = f.unit / "A.zip", b = f.unit / "B.ZiP", unicode = f.unit / L"涂装 B 包.ZIP";
    zip(a, {{"description.lua", textBytes("livery={}")}, {"wing.dds", f.a}, {"Local Folder/local.dds", f.a}});
    zip(b, {{"WiNg.DdS", f.b}, {"WING_RoughMet.DDS", f.rough}, {"Nested Folder/图 案.dds", f.rough}});
    zip(unicode, {{"翼 面.DdS", f.physical}});
    zip(f.unit / "0 Other Skin.zip", {{"wing.dds", f.other}, {"图 案.dds", f.other}});
    auto selected = f.selected(a, "description.lua");
    TextureResolver resolver(f.source, {}, selected);
    f.expect(resolver, selected, "../b/wing", f.b, b, "WiNg.DdS");
    f.expect(resolver, selected, R"(..\B\WiNg.DDS)", f.b, b, "WiNg.DdS");
    f.expect(resolver, selected, "../B/./wing.DdS", f.b, b, "WiNg.DdS");
    f.expect(resolver, selected, "../B/inner/../wing", f.b, b, "WiNg.DdS");
    f.expect(resolver, selected, "../b/wing_roughmet", f.rough, b, "WING_RoughMet.DDS", 13);
    f.expect(resolver, selected, R"(..\涂装 b 包\翼 面)", f.physical, unicode, "翼 面.DdS");
    f.expect(resolver, selected, "../B/Nested Folder/图 案", f.rough, b, "Nested Folder/图 案.dds");
    f.expect(resolver, selected, "wing", f.a, a, "wing.dds");
    f.expect(resolver, selected, R"(Local Folder\local)", f.a, a, "Local Folder/local.dds");
    f.absent(resolver, selected, "../Missing Skin/wing");
    f.absent(resolver, selected, "../B/Missing Folder/wing");
    f.absent(resolver, selected, "../B/absent_roughmet", 13);
    f.absent(resolver, selected, "not_a_texture");
    // Establish that the default actually is resolvable; negative checks cannot pass merely because
    // their fallback bait happens to be unavailable in the synthetic model's common texture roots.
    TextureResolver commonResolver(f.source);
    auto common = commonResolver.material(f.material);
    check(common && common->bytes() == f.fallback,
          "Synthetic model's default diffuse fallback bait exists and is readable");
}
void physicalDescription(const fs::path& root) {
    Fixture f(root);
    auto a = f.unit / "A/description.lua", b = f.unit / "B.zip";
    file(a, textBytes("livery={}"));
    file(a.parent_path() / "wing.dds", f.a);
    zip(b, {{"wing.dds", f.b}, {"wing_roughmet.dds", f.rough}});
    auto selected = f.selected(a);
    TextureResolver resolver(f.source, {}, selected);
    f.expect(resolver, selected, "../B/wing", f.b, b, "wing.dds");
    f.expect(resolver, selected, R"(..\b\WING_ROUGHMET.DDS)", f.rough, b, "wing_roughmet.dds", 13);
    f.expect(resolver, selected, "wing", f.a, a.parent_path() / "wing.dds");
}
void internalArchivePriority(const fs::path& root) {
    Fixture f(root);
    auto pack = f.unit / "Selected Pack.zip";
    zip(pack, {{"Skins/A/description.lua", textBytes("livery={}")},
               {"Skins/A/wing.dds", f.a},
               {"Skins/B/wing.dds", f.b},
               {"Skins/B/wing_RoughMet.dds", f.rough}});
    zip(f.unit / "B.zip", {{"wing.dds", f.other}, {"wing_RoughMet.dds", f.other}});
    file(f.unit / "B/wing.dds", f.physical);
    auto selected = f.selected(pack, "Skins/A/description.lua");
    TextureResolver resolver(f.source, {}, selected);
    f.expect(resolver, selected, "../B/wing", f.b, pack, "Skins/B/wing.dds");
    f.expect(resolver, selected, R"(..\B\wing_RoughMet)", f.rough, pack, "Skins/B/wing_RoughMet.dds", 13);
    f.expect(resolver, selected, "wing", f.a, pack, "Skins/A/wing.dds");
    f.absent(resolver, selected, "../C/wing");
}
void physicalDirectoryPriority(const fs::path& root) {
    Fixture f(root);
    auto archive = f.unit / "A.zip", physicalDescription = f.unit / "A/description.lua";
    zip(archive, {{"description.lua", textBytes("livery={}")}, {"wing.dds", f.a}});
    file(physicalDescription, textBytes("livery={}"));
    file(f.unit / "B/wing.dds", f.physical);
    file(f.unit / "B/wing_roughmet.dds", f.rough);
    zip(f.unit / "B.zip", {{"wing.dds", f.b}, {"wing_roughmet.dds", f.b}});
    for (const auto& selected : {f.selected(archive, "description.lua"), f.selected(physicalDescription)}) {
        TextureResolver resolver(f.source, {}, selected);
        f.expect(resolver, selected, "../B/wing", f.physical, f.unit / "B/wing.dds");
        f.expect(resolver, selected, "../B/wing_roughmet", f.rough, f.unit / "B/wing_roughmet.dds", {}, 13);
    }
}
void boundaryRejection(const fs::path& root) {
    Fixture f(root);
    auto archive = f.unit / "A.zip", physical = f.unit / "A/description.lua";
    file(physical, textBytes("livery={}"));
    file(f.root / "outside/secret.dds", f.other);
    zip(archive, {{"description.lua", textBytes("livery={}")},
                  {"secret.dds", f.a},
                  {"../../outside/secret.dds", f.b}});
    zip(f.root / "outside.zip", {{"secret.dds", f.other}});
    std::vector<std::string> bad{pathString(f.root / "outside/secret.dds"),
                                 "C:secret",
                                 R"(C:\secret)",
                                 R"(\\server\share\secret)",
                                 "//server/share/secret",
                                 "/outside/secret",
                                 "../../outside/secret",
                                 R"(..\..\outside\secret)",
                                 "../B/../../outside/secret",
                                 "../../../outside/secret",
                                 "file:///outside/secret"};
    for (const auto& selected : {f.selected(archive, "description.lua"), f.selected(physical)}) {
        TextureResolver resolver(f.source, {}, selected);
        for (const auto& name : bad)
            f.absent(resolver, selected, name);
        f.absent(resolver, selected, "../../outside/secret", 13);
    }
}
} // namespace
int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        ComRuntime runtime;
        const auto temp = fs::weakly_canonical(fs::temp_directory_path());
        const auto root = temp / (L"edm-texture-resolver-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                  std::to_wstring(GetTickCount64()));
        fs::create_directories(root);
        run("sibling archives, spelling and exact paths", [&] { siblingArchives(root / "zip-siblings"); });
        run("physical description references sibling ZIP",
            [&] { physicalDescription(root / "physical-description"); });
        run("same archive keeps priority", [&] { internalArchivePriority(root / "internal-priority"); });
        run("physical sibling directory before ZIP",
            [&] { physicalDirectoryPriority(root / "directory-priority"); });
        run("local unit boundary and no fallback", [&] { boundaryRejection(root / "boundary"); });
        std::cout << "Texture resolver: " << checks << " checks, " << failures << " failures\n";
        if (!failures) {
            const auto resolved = fs::weakly_canonical(root);
            require(resolved.parent_path() == temp &&
                        resolved.filename().wstring().starts_with(L"edm-texture-resolver-"),
                    "Refusing to clean a resolver fixture outside its temporary directory");
            std::error_code error;
            fs::remove_all(resolved, error);
            if (error)
                std::cout << "Temporary fixture cleanup deferred: " << pathString(root) << '\n';
        } else
            std::cout << "Failure fixtures: " << pathString(root) << '\n';
        return failures ? 1 : 0;
    } catch (...) {
        std::cerr << exceptionText() << '\n';
        return 2;
    }
}
