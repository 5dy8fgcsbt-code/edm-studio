#pragma once
#include "assets.h"
namespace edm {
struct ExportOptions {
    std::optional<std::vector<int>> arguments;
    double duration = 3;
    bool textures = true;
    fs::path textureDirectory;
    std::shared_ptr<Livery> livery;
    Args baseline;
    std::map<int, std::vector<uint8_t>> diffuseOverrides; // Baked, full-resolution PNG editing snapshots.
};
Json exportScene(const Scene& scene, const fs::path& path, const ExportOptions& options = {},
                 Progress progress = {}, const std::atomic_bool* cancel = nullptr);
} // namespace edm
