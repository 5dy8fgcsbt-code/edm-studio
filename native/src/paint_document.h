#pragma once
#include "paint.h"

namespace edm {
using PaintSnapshot = std::map<int, std::shared_ptr<const PaintImage>>;
using PaintLayerSnapshotMap = std::map<int, std::shared_ptr<const PaintCanvasLayers>>;
inline constexpr size_t paintProjectByteBudget = 2ull * 1024 * 1024 * 1024;
inline constexpr size_t paintProjectManifestBytes = 64ull * 1024 * 1024;
struct PaintLoadOptions {
    // v2 expands per material; v3 counts each explicitly shared canvas once.
    size_t maxExpandedBytes = paintProjectByteBudget;
};
struct PaintProjectDocument {
    PaintSnapshot images;
    PaintLayerSnapshotMap layerCanvases;
    std::vector<PaintLayerInfo> layers;
    uint64_t activeLayer = 0;
    bool restoreAppearance = false;
    // restoreAppearance=true and livery=nullptr explicitly restores the model's default appearance.
    std::shared_ptr<Livery> livery;
    fs::path textureDirectory;
    Json assembly = Json::object(); // Sources, connector targets and argument maps for reattaching EDMs.
    std::vector<std::string> warnings;
};
// Saves a new, self-contained folder. Original game/livery/template files are never overwritten.
Json savePaintProject(const Scene& scene, const PaintSnapshot& images, const fs::path& directory,
                      const std::string& name, std::shared_ptr<Livery> livery = {},
                      const fs::path& textureDirectory = {}, Progress progress = {},
                      const std::atomic_bool* cancel = nullptr,
                      const PaintLayerSnapshotMap& layerCanvases = {});
// Export a self-contained DCS livery even if no materials have been painted.
// Both entries create a unique child directory; existing files are never overwritten.
Json exportLiveryAssets(const Scene& scene, const PaintSnapshot& images, const fs::path& directory,
                        const std::string& name, std::shared_ptr<Livery> livery = {},
                        const fs::path& textureDirectory = {}, Progress progress = {},
                        const std::atomic_bool* cancel = nullptr);
Json exportLiveryAssetsFromPng(const Scene& scene, const std::map<int, std::vector<uint8_t>>& images,
                               const fs::path& directory, const std::string& name,
                               std::shared_ptr<Livery> livery = {}, const fs::path& textureDirectory = {},
                               Progress progress = {}, const std::atomic_bool* cancel = nullptr);
Json paintAssemblyMetadata(const Scene& scene);
PaintSnapshot loadPaintProject(const Scene& scene, const fs::path& project,
                               const std::atomic_bool* cancel = nullptr,
                               const PaintLoadOptions& options = {});
PaintProjectDocument loadPaintDocument(const Scene& scene, const fs::path& project,
                                       const std::atomic_bool* cancel = nullptr,
                                       const PaintLoadOptions& options = {});
// Fast PNG-only recovery, using the same verified project format. It is not a DCS livery.
Json savePaintRecovery(const Scene& scene, const PaintSnapshot& images, const fs::path& directory,
                       Progress progress = {}, const std::atomic_bool* cancel = nullptr,
                       std::shared_ptr<Livery> livery = {}, const fs::path& textureDirectory = {},
                       const PaintLayerSnapshotMap& layerCanvases = {});
} // namespace edm
