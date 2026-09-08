#pragma once
#include "model.h"
#include <DirectXTex.h>
namespace edm {
struct ImageSource {
    fs::path path;
    std::string entry;
    bool empty = false;
    std::string key() const {
        return empty ? "<transparent>" : pathString(path) + (entry.empty() ? "" : "::" + entry);
    }
    std::vector<uint8_t> bytes(size_t limit = 256ull * 1024 * 1024) const;
};
std::vector<std::string> zipEntries(const fs::path& path);
std::vector<uint8_t> zipRead(const fs::path& path, const std::string& entry, size_t limit);
struct TextureOverride {
    std::string material, name;
    int slot = 0;
    bool common = false;
};
struct Livery {
    fs::path path;
    std::string entry, name, unit, origin = "手动选择";
    std::map<std::pair<std::string, int>, TextureOverride> textures;
    Args args;
    Json countries = Json::array(), evaluation;
    std::vector<std::string> warnings;
    std::string identifier() const {
        return pathString(path) + (entry.empty() ? "" : "::" + entry);
    }
    Json metadata() const;
};
Json evaluateLua(const std::string& text, const fs::path& path = {}, const std::string& entry = {},
                 const Json& context = Json::object());
int luaWorker(const fs::path& request, const fs::path& response);
Livery readLivery(fs::path path, std::string entry = "", std::string origin = "手动选择",
                  std::string unit = "", const Json& context = Json::object());
std::vector<fs::path> installationPaths(const fs::path& source = {});
fs::path modulePath(const fs::path& source);
struct Catalog {
    std::vector<Livery> liveries;
    std::vector<fs::path> roots, installs;
    std::vector<std::string> warnings;
};
Catalog discoverLiveries(const fs::path& source, const std::vector<fs::path>& extras = {},
                         Progress progress = {}, const std::atomic_bool* cancel = nullptr);
class TextureResolver {
    using Index = std::map<std::string, ImageSource>;
    fs::path source;
    std::shared_ptr<Livery> livery;
    Index index, local;
    std::vector<fs::path> pending;
    void put(Index& index, const std::string& name, const ImageSource& image);
    void archive(const fs::path& path, Index& into, const std::string& relative = "");
    void directory(const fs::path& path, Index& into, bool recursive = true);
    std::optional<ImageSource> localImage(const std::string& name);

  public:
    std::vector<std::string> missing, warnings;
    Json resolved = Json::object();
    std::vector<fs::path> roots;
    TextureResolver(const fs::path& source, const fs::path& extra = {}, std::shared_ptr<Livery> livery = {});
    std::optional<ImageSource> common(const std::string& name);
    std::optional<ImageSource> material(const Material& mat, int slot = 0);
};
struct TextureImage {
    DirectX::ScratchImage pixels;
    size_t firstMip = 0;
    bool srgb = false;
    size_t bytes() const;
};
std::shared_ptr<TextureImage> loadTexture(const ImageSource& image, size_t maxSize = 4096);
std::vector<uint8_t> pngTexture(const ImageSource& image);
} // namespace edm
