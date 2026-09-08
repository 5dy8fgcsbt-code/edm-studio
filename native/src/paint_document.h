#pragma once
#include "paint.h"

namespace edm {
using PaintSnapshot = std::map<int, std::shared_ptr<const PaintImage>>;
struct PaintLoadOptions {
    // The UI expands each material into an independent canvas, even when PNG files are shared.
    size_t maxExpandedBytes = 1024ull * 1024 * 1024;
};
struct PaintProjectDocument {
    PaintSnapshot images;
    bool restoreAppearance = false;
    // restoreAppearance=true and livery=nullptr explicitly restores the model's default appearance.
    std::shared_ptr<Livery> livery;
    fs::path textureDirectory;
    std::vector<std::string> warnings;
};
// Saves a new, self-contained folder. Original game/livery/template files are never overwritten.
Json savePaintProject(const Scene& scene, const PaintSnapshot& images, const fs::path& directory,
                      const std::string& name, std::shared_ptr<Livery> livery = {},
                      const fs::path& textureDirectory = {}, Progress progress = {},
                      const std::atomic_bool* cancel = nullptr);
PaintSnapshot loadPaintProject(const Scene& scene, const fs::path& project,
                               const std::atomic_bool* cancel = nullptr,
                               const PaintLoadOptions& options = {});
PaintProjectDocument loadPaintDocument(const Scene& scene, const fs::path& project,
                                       const std::atomic_bool* cancel = nullptr,
                                       const PaintLoadOptions& options = {});
// Fast PNG-only recovery, using the same verified project format. It is not a DCS livery.
Json savePaintRecovery(const Scene& scene, const PaintSnapshot& images, const fs::path& directory,
                       Progress progress = {}, const std::atomic_bool* cancel = nullptr,
                       std::shared_ptr<Livery> livery = {}, const fs::path& textureDirectory = {});
} // namespace edm
