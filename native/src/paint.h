#pragma once
#include "assets.h"
#include <limits>

namespace edm {

// Editing uses a separate RGBA copy. Loading, painting and undo never modify a source texture.
// PSD layers are deliberately unsupported; export a flattened PNG/TGA from the template editor.
struct PaintImage {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
    PaintImage() = default;
    PaintImage(uint32_t width, uint32_t height, std::array<uint8_t, 4> color = {255, 255, 255, 255});
    static PaintImage load(const ImageSource& source, size_t maxSize = 8192);
    void validate() const;
    std::vector<uint8_t> pngBytes() const;
    void savePNG(const fs::path& path) const;
    void saveDDS(const fs::path& path) const;
    std::shared_ptr<TextureImage> texture(bool generateMips = false) const;
};

struct PaintRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0; // Exclusive upper bounds, suitable for D3D11_BOX.
    bool empty() const {
        return x1 <= x0 || y1 <= y0;
    }
};
struct PaintLayerInfo {
    uint64_t id = 0;
    std::string name;
    bool visible = true;
    float opacity = 1;
    bool preserveAlpha = true;
    bool operator==(const PaintLayerInfo&) const = default;
};
struct PaintLayerTile {
    uint32_t x = 0, y = 0; // Pixel origin, aligned to 64; edge tiles are clipped to the base image.
    PaintImage image;
};
struct PaintLayerSnapshot {
    PaintLayerInfo info;
    std::vector<PaintLayerTile> tiles;
};
struct PaintCanvasLayers {
    PaintImage base;
    std::vector<PaintLayerSnapshot> layers; // Bottom to top.
    uint64_t activeLayer = 0;
};

// Sparse tile history: a drag is one undo step, irrespective of its number of stamps.
// The most recent operation remains undoable even if it alone exceeds the soft history budget.
class PaintCanvas {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    explicit PaintCanvas(PaintImage image, size_t historyBudget = 128ull * 1024 * 1024);
    ~PaintCanvas();
    PaintCanvas(PaintCanvas&&) noexcept;
    PaintCanvas& operator=(PaintCanvas&&) noexcept;
    PaintCanvas(const PaintCanvas&) = delete;
    PaintCanvas& operator=(const PaintCanvas&) = delete;
    const PaintImage& image() const;
    uint64_t revision() const;
    size_t historyBytes() const;
    std::optional<PaintRect> takeDirtyRect();
    void beginStroke(std::string label = "画笔");
    bool endStroke();
    void cancelStroke() noexcept;
    // Preparation allocates snapshots/history capacity without changing either history stack.
    // After all canvases have prepared successfully a batch can commit without allocating or throwing.
    bool prepareStroke();
    uint64_t preparedStrokeToken() const noexcept;
    uint64_t commitPreparedStroke() noexcept; // 0 denotes a prepared empty operation.
    uint64_t undoToken() const noexcept;
    uint64_t redoToken() const noexcept;
    bool prepareUndo(uint64_t token);
    bool prepareRedo(uint64_t token);
    bool commitPreparedUndo(uint64_t token) noexcept;
    bool commitPreparedRedo(uint64_t token) noexcept;
    bool strokeActive() const;
    bool canUndo() const;
    bool canRedo() const;
    bool undo();
    bool redo();
    void enableLayers(const std::vector<PaintLayerInfo>& layers, uint64_t active);
    bool layered() const;
    std::vector<PaintLayerInfo> layers() const;
    uint64_t activeLayer() const;
    void selectLayer(uint64_t id);
    // Layer structure/content changes join the current stroke's undo transaction.
    void setLayers(const std::vector<PaintLayerInfo>& layers, uint64_t active);
    void duplicateLayer(uint64_t source, uint64_t destination); // Both IDs must already exist.
    void clearLayer(uint64_t id);
    void replaceLayerImage(uint64_t id, const PaintImage& image);
    // Synchronize global metadata without discarding the canvas's existing undo/redo tokens.
    void syncLayers(const std::vector<PaintLayerInfo>& layers, uint64_t active);
    // Expanded archive pixel budget: immutable base + composite + every logical layer tile.
    // Unlike storageBytes(), this counts shared duplicate tiles once per layer and excludes history.
    size_t layerSnapshotBytes() const;
    PaintCanvasLayers layerSnapshot() const;
    void restoreLayers(PaintCanvasLayers layers);
    size_t storageBytes() const;
    // color is straight sRGB RGBA, all components in [0,1]. Alpha masks are preserved by default.
    // Requires an active stroke. Returns true only when the stored pixel changes.
    // In layered mode this always accumulates source-over RGBA in the active visible layer;
    // preserveAlpha is ignored, and PaintLayerInfo::preserveAlpha controls final compositing.
    bool blendPixel(int x, int y, const F4& color, float coverage = 1, bool preserveAlpha = true);
};

struct PaintTriangle {
    std::array<V3, 3> world, normals;
    std::array<F2, 3> uv;
    uint32_t mesh = 0, index = 0; // index is the triangle ordinal within its mesh.
    int material = 0;
    bool hasUV = false;
    V3 normal() const;
};
struct PaintHit {
    V3 position = V3::Zero(), normal = V3::UnitY(), barycentric = V3::Zero();
    F2 uv{};
    uint32_t primitive = 0, mesh = 0, triangle = 0;
    int material = 0;
    double distance = 0;
};
struct PaintUVInfo {
    size_t coveredPixels = 0, sharedPixels = 0, repeatedTriangles = 0, degenerateTriangles = 0;
    bool shared() const {
        return sharedPixels != 0 || repeatedTriangles != 0;
    }
};

// A reusable BVH over the current animated pose. Rebuild after changing model, arguments, or
// attachment visibility. Hidden attachments are excluded from both painting and occlusion;
// camera motion, brush strokes and texture changes do not rebuild it.
class PaintSurface {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    PaintSurface(std::shared_ptr<const Scene> scene, const Args& args = {}, Progress progress = {},
                 const std::atomic_bool* cancel = nullptr, bool attachmentsVisible = true);
    ~PaintSurface();
    PaintSurface(PaintSurface&&) noexcept;
    PaintSurface& operator=(PaintSurface&&) noexcept;
    PaintSurface(const PaintSurface&) = delete;
    PaintSurface& operator=(const PaintSurface&) = delete;
    const Scene& scene() const;
    size_t triangleCount() const;
    PaintTriangle triangle(uint32_t primitive) const;
    std::optional<PaintHit> raycast(const V3& origin, const V3& direction, int material = -1,
                                    double maxDistance = std::numeric_limits<double>::infinity()) const;
    std::vector<uint32_t> querySphere(const V3& center, double radius, int material = -1) const;
    std::pair<V3, V3> bounds(int material = -1) const;
    PaintUVInfo analyzeUV(int material, uint32_t resolution = 256,
                          const std::atomic_bool* cancel = nullptr) const;
};

struct PaintRasterOptions {
    // Unwrapped UV bounds {minimum U, minimum V, maximum U, maximum V}; useful for local brush stamps.
    std::optional<std::array<double, 4>> uvBounds;
    uint64_t maxPixels = 128ull * 1024 * 1024;
    uint32_t maxRepeatTiles = 1024;
    const std::atomic_bool* cancel = nullptr;
};
using PaintRasterCallback = std::function<void(int x, int y, const V3& barycentric)>;
// Samples texel centres, wraps UVs as the renderer does, and reports barycentric coordinates.
// A callback can occur more than once for shared/repeated UV; the caller chooses the compositing rule.
// Throws before scanning an excessive UV extent and polls cancellation while rasterizing.
uint64_t rasterizePaintTriangle(const PaintTriangle& triangle, uint32_t width, uint32_t height,
                                const PaintRasterCallback& callback, const PaintRasterOptions& options = {});

struct PaintBrush {
    double radius = .1; // Radius in world units, independent of texture resolution and UV island size.
    float hardness = .65f, opacity = 1;
    F4 color{.9f, .1f, .08f, 1};
    bool preserveAlpha = true, visibleOnly = true;
    uint64_t maxPixels = 256ull * 1024; // Bounds work for an interactive stamp; reduce radius on overflow.
};
struct PaintStats {
    size_t triangles = 0, pixels = 0, sharedUVSamples = 0;
};
// Paints across UV seams by rasterizing nearby world triangles; unrelated UV islands stay untouched.
// The caller begins/ends the stroke. eye enables exact occlusion checks against the current pose.
// targetMaterials overrides targetMaterial when nonempty; shared texels blend once across the union.
PaintStats paintBrush(PaintCanvas& canvas, const PaintSurface& surface, const PaintHit& hit,
                      const PaintBrush& brush, const std::optional<V3>& eye = {},
                      const std::atomic_bool* cancel = nullptr, int targetMaterial = -1,
                      const std::vector<int>& targetMaterials = {});

} // namespace edm
