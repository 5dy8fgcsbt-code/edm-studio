#include "paint_batch.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>

thread_local long allocationCountdown = -1;
void* operator new(std::size_t size) {
    if (allocationCountdown >= 0 && allocationCountdown-- == 0) {
        allocationCountdown = -1;
        throw std::bad_alloc();
    }
    if (void* result = std::malloc(size ? size : 1))
        return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}
using namespace edm;
namespace {
int checks = 0;
void check(bool condition, const char* label) {
    ++checks;
    require(condition, std::string("Paint layers: ") + label);
}
template <class F> void fails(F action, const char* label) {
    bool threw = false;
    try {
        action();
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, label);
}
std::vector<PaintLayerInfo> initial() {
    return {{1, "颜色"}};
}
std::shared_ptr<PaintCanvas> canvas(uint32_t width = 129, uint32_t height = 65) {
    auto result = std::make_shared<PaintCanvas>(PaintImage(width, height, {20, 40, 60, 37}));
    result->enableLayers(initial(), 1);
    return result;
}
std::array<uint8_t, 4> pixel(const PaintImage& image, uint32_t x = 0, uint32_t y = 0) {
    std::array<uint8_t, 4> result;
    std::copy_n(image.rgba.data() + (size_t(y) * image.width + x) * 4, 4, result.begin());
    return result;
}
void sparsePixelsAndAlpha() {
    auto c = canvas();
    check(c->layered() && c->activeLayer() == 1 && c->layers() == initial(), "initial metadata");
    check(c->storageBytes() == 2 * c->image().rgba.size(), "empty layers allocate no pixel tiles");
    auto original = c->image().rgba;
    c->takeDirtyRect();
    c->beginStroke();
    check(c->blendPixel(128, 64, {1, 0, 0, .5f}, 1, false), "layer paint ignores flat alpha policy");
    check(c->blendPixel(128, 64, {0, 0, 1, .5f}), "second source-over dab");
    check(c->endStroke(), "pixel stroke committed");
    auto shot = c->layerSnapshot();
    check(shot.base.rgba == original && shot.layers[0].tiles.size() == 1,
          "immutable base and single sparse edge tile");
    const auto& tile = shot.layers[0].tiles[0];
    check(c->layerSnapshotBytes() == 2 * original.size() + 4,
          "archive budget counts base, composite and logical clipped tile");
    check(tile.x == 128 && tile.y == 64 && tile.image.width == 1 && tile.image.height == 1,
          "pixel origins and clipped edge dimensions");
    auto stored = pixel(tile.image);
    check(stored[0] >= 84 && stored[0] <= 86 && stored[2] >= 169 && stored[2] <= 171 && stored[3] == 192,
          "transparent layer stores straight source-over RGBA");
    check(pixel(c->image(), 128, 64)[3] == 37, "layer preserveAlpha protects lower alpha");
    auto dirty = c->takeDirtyRect();
    check(dirty && dirty->x0 == 128 && dirty->y0 == 64 && dirty->x1 == 129 && dirty->y1 == 65,
          "brush dirty rectangle remains local");
    auto painted = c->image().rgba;
    check(c->undo() && c->image().rgba == original && c->layerSnapshot().layers[0].tiles.empty(),
          "undo removes created sparse tile");
    check(c->redo() && c->image().rgba == painted, "redo restores sparse tile");
    auto layers = c->layers();
    layers[0].preserveAlpha = false;
    c->beginStroke();
    c->setLayers(layers, 1);
    c->endStroke();
    check(pixel(c->image(), 128, 64)[3] >= 200, "layer alpha property recomposes output alpha");
    c->undo();
    check(pixel(c->image(), 128, 64)[3] == 37, "metadata undo restores alpha policy");
}
void metadataAndCow() {
    auto c = canvas(128, 128);
    c->beginStroke();
    c->blendPixel(5, 6, {1, 0, 0, 1});
    c->endStroke();
    auto a = c->image().rgba;
    auto layers = c->layers();
    layers.push_back({2, "副本", false});
    size_t before = c->storageBytes();
    c->beginStroke();
    c->setLayers(layers, 2);
    c->duplicateLayer(1, 2);
    c->endStroke();
    check(c->storageBytes() == before, "duplicating a hidden layer shares all existing pixel storage");
    check(c->layerSnapshotBytes() == 2 * c->image().rgba.size() + 2 * 64 * 64 * 4,
          "archive budget expands shared duplicate tiles independently");
    auto shot = c->layerSnapshot();
    check(shot.layers[1].tiles[0].image.rgba == shot.layers[0].tiles[0].image.rgba,
          "duplicate retains exact sparse pixels");
    c->beginStroke();
    fails([&] { c->blendPixel(5, 6, {0, 1, 0, 1}); }, "hidden layer refuses painting");
    c->cancelStroke();
    layers[1].visible = true;
    layers[1].opacity = 0;
    c->beginStroke();
    c->setLayers(layers, 2);
    c->blendPixel(5, 6, {0, 0, 1, 1});
    c->endStroke();
    check(c->image().rgba == a, "zero-opacity painting changes layer without changing composite");
    shot = c->layerSnapshot();
    check(pixel(shot.layers[0].tiles[0].image, 5, 6)[0] == 255 &&
              pixel(shot.layers[1].tiles[0].image, 5, 6)[2] == 255,
          "copy-on-write edit does not change source layer");
    layers[1].opacity = .5f;
    c->beginStroke();
    c->setLayers(layers, 2);
    c->endStroke();
    auto mixed = pixel(c->image(), 5, 6);
    check(mixed[0] == 128 && mixed[2] == 128, "layer opacity mixes over lower layer");
    std::swap(layers[0], layers[1]);
    c->beginStroke();
    c->setLayers(layers, 1);
    c->endStroke();
    check(pixel(c->image(), 5, 6)[0] == 255, "bottom-to-top reorder changes overlap");
    c->undo();
    check(c->activeLayer() == 2 && pixel(c->image(), 5, 6) == mixed, "undo restores active id and order");
    c->redo();
    c->beginStroke();
    c->setLayers({layers[0]}, 2);
    c->endStroke();
    check(c->layers().size() == 1, "delete layer removes metadata");
    c->undo();
    check(c->layers().size() == 2 && pixel(c->image(), 5, 6)[0] == 255,
          "undo deletion restores full layer pixels");
    c->beginStroke();
    c->clearLayer(1);
    c->endStroke();
    check(c->layerSnapshot().layers[1].tiles.empty(), "clear layer drops current pixels");
    c->undo();
    check(pixel(c->image(), 5, 6)[0] == 255, "undo clear restores source pixels");
}
void templatesAndRestore() {
    auto c = canvas();
    auto base = c->image().rgba;
    PaintImage image(129, 65, {10, 20, 30, 0});
    const size_t pos = (size_t(64) * 129 + 128) * 4;
    image.rgba[pos] = 200;
    image.rgba[pos + 3] = 255;
    c->beginStroke();
    c->replaceLayerImage(1, image);
    c->endStroke();
    auto snapshot = c->layerSnapshot();
    check(snapshot.layers[0].tiles.size() == 1 && snapshot.base.rgba == base,
          "template stores only nontransparent tile support and preserves base");
    auto expected = c->image().rgba;
    PaintCanvas restored(PaintImage(1, 1));
    restored.restoreLayers(snapshot);
    check(restored.image().rgba == expected && restored.layers() == c->layers() && !restored.canUndo(),
          "layer snapshot restores dimensions, pixels and metadata without history");
    check(c->undo() && c->image().rgba == base && c->redo() && c->image().rgba == expected,
          "template replacement undo/redo");
    auto bad = snapshot;
    bad.layers[0].tiles[0].x = 1;
    fails([&] { restored.restoreLayers(bad); }, "unaligned restored tile rejected");
    bad = snapshot;
    bad.layers[0].tiles.push_back(bad.layers[0].tiles[0]);
    fails([&] { restored.restoreLayers(bad); }, "duplicate tile rejected");
    bad = snapshot;
    bad.layers[0].tiles[0].image = PaintImage(64, 64);
    fails([&] { restored.restoreLayers(bad); }, "incorrect clipped dimensions rejected");
    check(restored.image().rgba == expected, "invalid restore leaves old canvas intact");
    c->beginStroke();
    c->replaceLayerImage(1, PaintImage(129, 65, {0, 0, 0, 0}));
    c->cancelStroke();
    check(c->image().rgba == expected, "cancel template operation restores complete prior layer");
}
void validation() {
    auto c = canvas();
    fails([&] { c->enableLayers(initial(), 1); }, "cannot re-enable layered canvas");
    fails([&] { c->setLayers(initial(), 1); }, "metadata requires transaction");
    fails([&] { c->replaceLayerImage(1, PaintImage(129, 65)); }, "template requires transaction");
    auto original = c->layers();
    auto bad = original;
    bad[0].opacity = std::numeric_limits<float>::quiet_NaN();
    fails([&] { c->syncLayers(bad, 1); }, "nonfinite opacity rejected");
    bad = original;
    bad[0].id = UINT64_MAX;
    fails([&] { c->syncLayers(bad, UINT64_MAX); }, "exhausted layer id rejected");
    bad = original;
    bad[0].name = std::string("bad\0name", 8);
    fails([&] { c->syncLayers(bad, 1); }, "embedded nul rejected");
    bad = original;
    bad[0].name = "\xC0\xAF";
    fails([&] { c->syncLayers(bad, 1); }, "invalid UTF-8 rejected");
    bad = original;
    bad.push_back(bad[0]);
    fails([&] { c->syncLayers(bad, 1); }, "duplicate id rejected");
    bad.clear();
    for (uint64_t id = 1; id <= 65; ++id)
        bad.push_back({id, "x"});
    fails([&] { c->syncLayers(bad, 1); }, "more than 64 layers rejected");
    check(c->layers() == original, "failed validation does not publish metadata");
    c->beginStroke();
    fails([&] { c->duplicateLayer(1, 1); }, "self duplication rejected");
    fails([&] { c->duplicateLayer(1, 2); }, "missing duplicate target rejected");
    fails([&] { c->syncLayers(initial(), 1); }, "sync cannot modify active stroke");
    fails([&] { c->replaceLayerImage(1, PaintImage(1, 1)); }, "template size mismatch rejected");
    c->cancelStroke();
}
void controlAndLazySync() {
    auto anchor = canvas(1, 1), a = canvas(4, 4);
    PaintBatchHistory batch;
    auto expanded = initial();
    expanded.push_back({2, "贴花"});
    batch.begin();
    batch.touchControl(anchor).setLayers(expanded, 2);
    batch.touch(3, a).setLayers(expanded, 2);
    check(batch.activeMaterials() == std::vector<int>{3}, "control omitted from active materials");
    check(batch.commit() && batch.changedMaterials() == std::vector<int>{3},
          "control omitted from changed materials");
    auto b = canvas(4, 4);
    b->syncLayers(expanded, 2);
    batch.begin();
    batch.touch(7, b).blendPixel(1, 1, {1, 0, 0, 1});
    batch.commit();
    check(batch.undo(), "undo lazy canvas brush");
    uint64_t redoToken = b->redoToken();
    check(batch.undo() && anchor->layers() == initial(), "undo global creation restores anchor");
    b->syncLayers(anchor->layers(), anchor->activeLayer());
    check(b->layers() == initial() && b->redoToken() == redoToken, "sync lazy canvas preserves redo token");
    check(batch.redo(), "redo global creation");
    b->syncLayers(anchor->layers(), anchor->activeLayer());
    check(batch.redo() && pixel(b->image(), 1, 1)[0] == 255,
          "redo lazy canvas painting after layout synchronization");
    batch.remapMaterials({-1, -1, -1, -1, -1, -1, -1, -1}, {});
    check(batch.canUndo() && batch.undo() && anchor->layers() == initial(),
          "removing every material still retains independent control undo");
    check(batch.changedMaterials().empty() && batch.redo(),
          "control-only remapped redo has no material notifications");
    PaintBatchHistory empty;
    empty.begin();
    empty.touchControl(anchor).setLayers(initial(), 1);
    check(empty.commit() && empty.changedMaterials().empty(),
          "metadata-only operation is a real history step");
    check(empty.undo() && empty.redo(), "metadata-only undo and redo");
    empty.begin();
    empty.touchControl(anchor);
    fails([&] { empty.touchControl(a); }, "second distinct control rejected");
    check(!empty.active() && !anchor->strokeActive(), "control ownership error cancels all active strokes");
}
void noAllocationPublication() {
    auto c = canvas();
    c->beginStroke();
    c->blendPixel(3, 4, {1, 0, 0, 1});
    auto infos = c->layers();
    infos.push_back({2, "clone", false});
    c->setLayers(infos, 2);
    c->duplicateLayer(1, 2);
    check(c->prepareStroke(), "layer stroke prepared");
    allocationCountdown = 0;
    uint64_t token = c->commitPreparedStroke();
    bool noAllocation = allocationCountdown == 0;
    allocationCountdown = -1;
    check(token && noAllocation, "commit publication performs no allocations");
    check(c->prepareUndo(token), "layer undo prepared");
    allocationCountdown = 0;
    bool done = c->commitPreparedUndo(token);
    noAllocation = allocationCountdown == 0;
    allocationCountdown = -1;
    check(done && noAllocation, "undo publication performs no allocations");
    check(c->prepareRedo(token), "layer redo prepared");
    allocationCountdown = 0;
    done = c->commitPreparedRedo(token);
    noAllocation = allocationCountdown == 0;
    allocationCountdown = -1;
    check(done && noAllocation, "redo publication performs no allocations");
    c->beginStroke();
    c->clearLayer(1);
    allocationCountdown = 0;
    c->cancelStroke();
    noAllocation = allocationCountdown == 0;
    allocationCountdown = -1;
    check(noAllocation && c->undoToken() == token,
          "cancel publication performs no allocations and retains history");
}
void failureRollback() {
    bool completed = false;
    int injected = 0;
    for (long fail = 0; fail < 180 && !completed; ++fail) {
        auto c = canvas(65, 65);
        c->beginStroke();
        c->blendPixel(1, 1, {1, 0, 0, 1});
        c->endStroke();
        auto pixels = c->image().rgba;
        auto token = c->undoToken();
        auto infos = c->layers();
        infos[0].visible = false;
        allocationCountdown = fail;
        try {
            c->syncLayers(infos, 1);
            completed = true;
        } catch (const std::bad_alloc&) {
            ++injected;
        }
        allocationCountdown = -1;
        if (!completed)
            check(c->image().rgba == pixels && c->layers() == initial() && c->undoToken() == token,
                  "sync allocation failure preserves pixels, metadata and token");
    }
    check(completed && injected > 8, "sync allocation injection reached every publication preflight");
    completed = false;
    injected = 0;
    for (long fail = 0; fail < 180 && !completed; ++fail) {
        auto anchor = canvas(1, 1), c = canvas(65, 65);
        PaintBatchHistory batch;
        auto infos = initial();
        infos.push_back({2, "layer"});
        batch.begin();
        batch.touchControl(anchor).setLayers(infos, 2);
        batch.touch(0, c).setLayers(infos, 2);
        c->blendPixel(64, 64, {0, 1, 0, 1});
        allocationCountdown = fail;
        try {
            batch.commit();
            completed = true;
        } catch (const std::bad_alloc&) {
            ++injected;
        }
        allocationCountdown = -1;
        if (!completed)
            check(!batch.active() && !c->strokeActive() && c->layers() == initial() &&
                      anchor->layers() == initial() && pixel(c->image(), 64, 64)[1] == 40,
                  "batch prepare failure rolls back control, layers and pixels together");
    }
    check(completed && injected > 4, "batch allocation injection reaches successful commit");
}
void largeSparseCanvas() {
    const auto start = Clock::now();
    auto c = canvas(4096, 4096);
    auto infos = c->layers();
    for (uint64_t id = 2; id <= 8; ++id)
        infos.push_back({id, "empty", false});
    c->syncLayers(infos, 1);
    const size_t baseBytes = 2 * c->image().rgba.size();
    check(c->storageBytes() == baseBytes, "eight empty 4K layers require only base and composite storage");
    const auto brushStart = Clock::now();
    c->beginStroke();
    for (int x = 10; x < 110; ++x)
        c->blendPixel(x, 3, {1, 0, 0, .5f});
    c->endStroke();
    const double brushSeconds = seconds(brushStart);
    const size_t paintedBytes = c->storageBytes();
    check(paintedBytes - baseBytes == 2 * 64 * 64 * 4 * 3,
          "4K hundred-pixel stroke allocates only two sparse/history tiles");
    c->beginStroke();
    for (uint64_t id = 2; id <= 8; ++id)
        c->duplicateLayer(1, id);
    c->endStroke();
    check(c->storageBytes() == paintedBytes,
          "seven duplicates of sparse 4K layer allocate no additional pixel buffers");
    std::cout << "4K sparse layers: " << c->storageBytes() << " bytes, " << paintedBytes - baseBytes
              << " bytes above base/composite; 100 dabs " << brushSeconds * 1000
              << " ms; setup and duplicates " << seconds(start) * 1000 << " ms\n";
}
} // namespace
int main() {
    try {
        sparsePixelsAndAlpha();
        metadataAndCow();
        templatesAndRestore();
        validation();
        controlAndLazySync();
        noAllocationPublication();
        failureRollback();
        largeSparseCanvas();
        std::cout << "Paint layers: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        allocationCountdown = -1;
        std::cerr << e.what() << '\n';
        return 1;
    }
}
