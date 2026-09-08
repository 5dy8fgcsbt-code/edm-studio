#include "paint_batch.h"
#include <cstdlib>
#include <iostream>
#include <new>

// Deterministic failure injection exercises every allocation in prepare/undo preflight. It is armed
// only around the operation under test and disarms itself before throwing, so error handling can run.
thread_local long paintAllocationCountdown = -1;
void* operator new(std::size_t size) {
    if (paintAllocationCountdown >= 0 && paintAllocationCountdown-- == 0) {
        paintAllocationCountdown = -1;
        throw std::bad_alloc();
    }
    if (void* pointer = std::malloc(size ? size : 1))
        return pointer;
    throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept {
    std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete[](void* pointer) noexcept {
    ::operator delete(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
    ::operator delete(pointer);
}

using namespace edm;
namespace {
int checks = 0;
void check(bool condition, const char* description) {
    ++checks;
    require(condition, std::string("Paint batch regression: ") + description);
}
template <class F> void fails(F action, const char* description) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, description);
}
std::shared_ptr<PaintCanvas> canvas(uint32_t width = 16, uint32_t height = 8, size_t budget = 128 * 1024) {
    return std::make_shared<PaintCanvas>(PaintImage(width, height), budget);
}
void paint(PaintCanvas& canvas, int x = 1, const F4& color = {1, 0, 0, 1}) {
    canvas.blendPixel(x, 1, color);
}
void batchesAndAliases() {
    auto a = canvas(), b = canvas(32, 16), untouched = canvas();
    auto beforeA = a->image().rgba, beforeB = b->image().rgba, beforeUntouched = untouched->image().rgba;
    PaintBatchHistory batch;
    batch.begin("two material seam");
    paint(batch.touch(2, a));
    paint(batch.touch(5, b));
    batch.touch(7, a);
    check(batch.activeMaterials() == std::vector<int>{2, 5, 7}, "All touched material aliases are visible");
    check(!batch.canUndo() && !batch.canRedo(), "Active operation cannot be undone halfway");
    check(batch.commit() && batch.changedMaterials() == std::vector<int>{2, 5, 7},
          "One commit records two canvases and all aliases");
    auto afterA = a->image().rgba, afterB = b->image().rgba;
    auto tokenA = a->undoToken(), tokenB = b->undoToken();
    check(tokenA && tokenB && batch.canUndo(), "Each committed canvas has a history token");
    check(batch.undo() && a->image().rgba == beforeA && b->image().rgba == beforeB,
          "One batch undo restores every material");
    check(a->redoToken() == tokenA && b->redoToken() == tokenB, "Undo preserves exact operation tokens");
    check(batch.redo() && a->image().rgba == afterA && b->image().rgba == afterB,
          "One batch redo restores every material");
    check(untouched->image().rgba == beforeUntouched && !untouched->canUndo(),
          "Untouched material remains unchanged and gains no history");
    batch.begin();
    paint(batch.touch(2, a), 3, {0, 1, 0, 1});
    paint(batch.touch(5, b), 4, {0, 0, 1, 1});
    batch.cancel();
    check(a->image().rgba == afterA && b->image().rgba == afterB && batch.canUndo(),
          "Cancel rolls back all active materials while preserving previous batch history");
    check(batch.changedMaterials().empty(), "Cancel clears material change report");
    batch.undo();
    batch.begin("empty stroke");
    batch.touch(2, a);
    batch.touch(5, b);
    check(!batch.commit() && batch.canRedo() && !a->strokeActive() && !b->strokeActive(),
          "Empty multi-canvas operation retains redo and closes all strokes");
    batch.redo();
    batch.begin("one changed material");
    batch.touch(2, a);
    paint(batch.touch(5, b), 6, {0, 1, 0, 1});
    batch.commit();
    check(batch.changedMaterials() == std::vector<int>{5}, "Changed materials exclude no-op canvases");
    batch.clear();
    check(!batch.canUndo() && !batch.canRedo() && a->canUndo(),
          "Clearing coordinator history does not destroy canvas-local history");
}
void staleHistoryProtection() {
    auto a = canvas(16, 8, 1), b = canvas();
    PaintBatchHistory batch;
    batch.begin();
    paint(batch.touch(0, a));
    paint(batch.touch(1, b));
    batch.commit();
    auto groupToken = a->undoToken();
    a->beginStroke("independent edit");
    paint(*a, 4, {0, 0, 1, 1});
    a->endStroke();
    auto external = a->image().rgba, originalB = b->image().rgba;
    check(a->undoToken() != groupToken && !batch.canUndo() && !batch.undo(),
          "Evicted/covered batch token cannot undo a different canvas operation");
    check(a->image().rgba == external && b->image().rgba == originalB,
          "Batch preflight prevents partially undoing unaffected group members");
    a->undo();
    check(a->undoToken() == 0 && !batch.canUndo(),
          "History budget eviction remains detectable after external undo");

    auto c = canvas(), d = canvas();
    PaintBatchHistory redo;
    redo.begin();
    paint(redo.touch(0, c));
    paint(redo.touch(1, d));
    redo.commit();
    redo.undo();
    d->beginStroke();
    paint(*d, 7, {0, 1, 0, 1});
    d->endStroke();
    auto unchangedC = c->image().rgba, changedD = d->image().rgba;
    check(!redo.canRedo() && !redo.redo() && c->image().rgba == unchangedC && d->image().rgba == changedD,
          "Invalidated redo branch cannot partially replay a material group");

    auto replaced = canvas();
    PaintBatchHistory replacement;
    replacement.begin();
    paint(replacement.touch(0, replaced));
    replacement.commit();
    auto oldToken = replaced->undoToken();
    *replaced = PaintCanvas(PaintImage(16, 8));
    replaced->beginStroke();
    paint(*replaced);
    replaced->endStroke();
    check(replaced->undoToken() != oldToken && !replacement.canUndo(),
          "Replacing a canvas implementation cannot reuse a stale batch token");
}
void prepareAndFailureAtomicity() {
    auto prepared = canvas();
    prepared->beginStroke();
    paint(*prepared);
    check(prepared->prepareStroke() && prepared->preparedStrokeToken() != 0 && prepared->undoToken() == 0,
          "Prepare allocates a token without exposing a committed operation");
    fails([&] { paint(*prepared, 2); }, "Prepared transaction cannot be mutated before commit");
    auto token = prepared->preparedStrokeToken();
    paintAllocationCountdown = 0;
    auto committed = prepared->commitPreparedStroke();
    paintAllocationCountdown = -1;
    check(committed == token && prepared->undoToken() == token,
          "Prepared commit publishes exact token without further allocation");
    check(!prepared->prepareUndo(token + 1), "Incorrect undo token is rejected");
    check(prepared->prepareUndo(token), "Exact token can prepare undo");
    paintAllocationCountdown = 0;
    bool undone = prepared->commitPreparedUndo(token);
    paintAllocationCountdown = -1;
    check(undone, "Prepared undo is allocation free");
    check(prepared->prepareRedo(token), "Exact token can prepare redo");
    paintAllocationCountdown = 0;
    bool redone = prepared->commitPreparedRedo(token);
    paintAllocationCountdown = -1;
    check(redone, "Prepared redo is allocation free");

    size_t commitFailures = 0;
    for (long failAt = 0; failAt < 80; ++failAt) {
        auto a = canvas(), b = canvas();
        PaintBatchHistory batch;
        batch.begin("existing redo");
        paint(batch.touch(0, a));
        paint(batch.touch(1, b));
        batch.commit();
        batch.undo();
        auto beforeA = a->image().rgba, beforeB = b->image().rgba;
        auto redoA = a->redoToken(), redoB = b->redoToken();
        batch.begin("faulted prepare");
        paint(batch.touch(0, a), 4, {0, 1, 0, 1});
        paint(batch.touch(1, b), 4, {0, 1, 0, 1});
        bool failed = false;
        paintAllocationCountdown = failAt;
        try {
            batch.commit();
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        paintAllocationCountdown = -1;
        if (!failed)
            break;
        ++commitFailures;
        check(!batch.active() && !a->strokeActive() && !b->strokeActive() && a->image().rgba == beforeA &&
                  b->image().rgba == beforeB,
              "Every prepare allocation failure rolls back the complete group");
        check(a->redoToken() == redoA && b->redoToken() == redoB && batch.canRedo(),
              "Preparation failures preserve old canvas and group redo branches");
    }
    check(commitFailures >= 8, "Fault injection reaches later canvas prepare allocations");
    size_t undoFailures = 0;
    for (long failAt = 0; failAt < 20; ++failAt) {
        auto a = canvas(), b = canvas();
        PaintBatchHistory batch;
        batch.begin();
        paint(batch.touch(0, a));
        paint(batch.touch(1, b));
        batch.commit();
        auto beforeA = a->image().rgba, beforeB = b->image().rgba;
        bool failed = false;
        paintAllocationCountdown = failAt;
        try {
            batch.undo();
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        paintAllocationCountdown = -1;
        if (!failed)
            break;
        ++undoFailures;
        check(a->image().rgba == beforeA && b->image().rgba == beforeB && batch.canUndo(),
              "Undo stack allocation failure cannot alter any group member");
    }
    check(undoFailures >= 3, "Undo preflight tests coordinator and both canvas allocations");

    size_t touchFailures = 0;
    for (long failAt = 0; failAt < 30; ++failAt) {
        auto a = canvas(), b = canvas();
        auto originalA = a->image().rgba;
        PaintBatchHistory batch;
        batch.begin();
        paint(batch.touch(0, a));
        bool failed = false;
        paintAllocationCountdown = failAt;
        try {
            batch.touch(1, b);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        paintAllocationCountdown = -1;
        if (!failed) {
            batch.cancel();
            break;
        }
        ++touchFailures;
        check(!batch.active() && !a->strokeActive() && !b->strokeActive() && a->image().rgba == originalA,
              "Touch allocation failure cannot leave an orphan active canvas or partial previous paint");
    }
    check(touchFailures >= 2, "Touch fault injection reaches canvas and coordinator allocation");
}
Mesh quad(float left, float right, int material) {
    Mesh mesh;
    mesh.material = material;
    mesh.positions = {{left, -1, 0}, {right, -1, 0}, {right, 1, 0}, {left, 1, 0}};
    mesh.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.uvs = {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    return mesh;
}
void multiMaterialBrush() {
    auto scene = std::make_shared<Scene>();
    scene->materials.resize(3);
    scene->nodes.resize(1);
    scene->order = {0};
    scene->staticLocal = scene->defaultWorld = {Mat::Identity()};
    scene->meshes = {quad(-1, 0, 0), quad(0, 1, 1), quad(5, 6, 2)};
    PaintSurface surface(scene);
    auto hit = surface.raycast(V3(-.02, 0, 3), V3(0, 0, -1));
    check(hit && hit->material == 0, "Synthetic brush centre initially hits left material");
    auto a = canvas(128, 64), b = canvas(64, 128), distant = canvas();
    auto beforeA = a->image().rgba, beforeB = b->image().rgba, beforeDistant = distant->image().rgba;
    PaintBrush brush;
    brush.radius = .25;
    brush.hardness = 1;
    brush.color = {1, 0, 0, 1};
    PaintBatchHistory batch;
    batch.begin("cross material stroke");
    auto left = paintBrush(batch.touch(0, a), surface, *hit, brush, V3(-.02, 0, 3));
    auto right = paintBrush(batch.touch(1, b), surface, *hit, brush, V3(-.02, 0, 3), nullptr, 1);
    check(left.pixels > 10 && right.pixels > 10, "Same world brush centre paints both material canvases");
    auto nextHit = surface.raycast(V3(.1, 0, 3), V3(0, 0, -1));
    check(nextHit && nextHit->material == 1, "Continuous stroke can move onto the second material");
    paintBrush(batch.touch(0, a), surface, *nextHit, brush, V3(.1, 0, 3), nullptr, 0);
    paintBrush(batch.touch(1, b), surface, *nextHit, brush, V3(.1, 0, 3));
    batch.commit();
    auto indexA = (size_t(32) * 128 + 126) * 4, indexB = (size_t(64) * 64 + 1) * 4;
    check(a->image().rgba[indexA + 1] == 0 && b->image().rgba[indexB + 1] == 0,
          "Different-resolution UV islands meet at the painted physical seam");
    check(distant->image().rgba == beforeDistant && !distant->canUndo(), "Outside material is untouched");
    auto afterA = a->image().rgba, afterB = b->image().rgba;
    check(batch.undo() && a->image().rgba == beforeA && b->image().rgba == beforeB,
          "Cross-material brush has one exact undo");
    check(batch.redo() && a->image().rgba == afterA && b->image().rgba == afterB,
          "Cross-material brush has one exact redo");

    auto blocker = quad(0, 1, 2);
    for (auto& point : blocker.positions)
        point[2] = .1f;
    scene->meshes.push_back(blocker);
    PaintSurface occluded(scene);
    auto covered = canvas(64, 64);
    covered->beginStroke();
    auto hidden = paintBrush(*covered, occluded, *hit, brush, V3(-.02, 0, 3), nullptr, 1);
    check(hidden.pixels == 0 && !covered->endStroke(),
          "Painting a second material still respects occlusion by every other material");
}
void sharedCanvasBrushUnion() {
    auto scene = std::make_shared<Scene>();
    scene->materials.resize(3);
    scene->nodes.resize(1);
    scene->order = {0};
    scene->staticLocal = scene->defaultWorld = {Mat::Identity()};
    scene->meshes = {quad(-1, 1, 0), quad(-1, 1, 1), quad(-1, 1, 2)};
    PaintSurface surface(scene);
    auto hit = surface.raycast(V3(0, 0, 3), V3(0, 0, -1));
    check(hit.has_value(), "Shared canvas material aliases have a paintable surface");
    auto single = canvas(64, 64), combined = canvas(64, 64);
    PaintBrush brush;
    brush.radius = .4;
    brush.opacity = .5;
    brush.visibleOnly = false;
    single->beginStroke();
    auto one = paintBrush(*single, surface, *hit, brush, {}, nullptr, 0);
    single->endStroke();
    PaintBatchHistory batch;
    batch.begin();
    batch.touch(0, combined);
    batch.touch(1, combined);
    auto unionStats = paintBrush(*combined, surface, *hit, brush, {}, nullptr, 2, {1, 0, 1});
    batch.commit();
    check(combined->image().rgba == single->image().rgba && unionStats.pixels == one.pixels,
          "Shared UV aliases and duplicate material IDs blend exactly once per stamp");
    check(unionStats.triangles == one.triangles * 2 && unionStats.sharedUVSamples > 0,
          "Union overrides scalar target and excludes unrelated overlapping material");
    check(batch.changedMaterials() == std::vector<int>{0, 1} && batch.undo() && !combined->canUndo(),
          "Shared canvas union commits one undo operation while reporting all aliases");
}
void discardUnchangedCanvases() {
    auto a = canvas(), b = canvas();
    PaintBatchHistory batch;
    fails([&] { batch.discardUnchanged(a); }, "Discard requires an active batch");
    batch.begin();
    paint(batch.touch(0, a));
    batch.commit();
    batch.undo();
    auto oldRedo = a->redoToken();
    batch.begin();
    batch.touch(0, a);
    batch.touch(3, a);
    check(batch.discardUnchanged(a) && !a->strokeActive() && batch.activeMaterials().empty(),
          "Discarding a no-op canvas closes its stroke and removes every alias");
    check(a->redoToken() == oldRedo && !batch.commit() && batch.canRedo(),
          "Discard preserves prior canvas and coordinator redo branches");
    batch.redo();
    auto oldUndo = a->undoToken();
    batch.begin();
    batch.touch(0, a);
    check(batch.discardUnchanged(a) && a->undoToken() == oldUndo,
          "Discard preserves a preexisting canvas undo branch");
    auto temporary = canvas();
    std::weak_ptr<PaintCanvas> weak = temporary;
    batch.touch(7, temporary);
    batch.touch(8, temporary);
    check(batch.discardUnchanged(temporary), "An untouched temporary canvas can be discarded");
    temporary.reset();
    check(weak.expired(), "Discard releases all coordinator references to a temporary canvas");
    paint(batch.touch(1, b));
    auto painted = b->image().rgba;
    check(!batch.discardUnchanged(b) && b->strokeActive() && batch.activeMaterials() == std::vector<int>{1},
          "A changed canvas is retained with every pixel intact");
    batch.touch(9, b);
    auto other = canvas();
    paint(batch.touch(10, other));
    check(batch.commit() && b->image().rgba == painted &&
              batch.changedMaterials() == std::vector<int>{1, 9, 10},
          "A retained prepared canvas accepts aliases and commits with later canvases");
    check(batch.undo() && batch.redo() && b->image().rgba == painted,
          "Discard does not prevent exact undo and redo of remaining members");

    size_t failures = 0;
    for (long failAt = 0; failAt < 20; ++failAt) {
        auto candidate = canvas();
        auto original = candidate->image().rgba;
        PaintBatchHistory operation;
        operation.begin();
        paint(operation.touch(0, candidate));
        operation.touch(2, candidate);
        auto altered = candidate->image().rgba;
        bool failed = false;
        paintAllocationCountdown = failAt;
        try {
            operation.discardUnchanged(candidate);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        paintAllocationCountdown = -1;
        if (!failed) {
            operation.cancel();
            break;
        }
        ++failures;
        check(operation.active() && candidate->strokeActive() && candidate->image().rgba == altered &&
                  operation.activeMaterials() == std::vector<int>{0, 2},
              "Discard preparation failure leaves batch ownership and pixels unchanged");
        operation.cancel();
        check(candidate->image().rgba == original, "Failed discard still allows complete cancellation");
    }
    check(failures > 0, "Discard fault injection reaches preparation allocations");
}
void fullTriangleReference(PaintCanvas& canvas, const PaintSurface& surface, const PaintHit& hit,
                           const PaintBrush& brush, const std::optional<V3>& eye = {}) {
    // Deliberately rasterize each complete triangle at low resolution: an independent reference
    // for clipping. Original barycentric/world/coverage calculations make byte comparisons exact.
    std::unordered_map<size_t, float> coverage;
    for (uint32_t i = 0; i < surface.triangleCount(); ++i) {
        auto face = surface.triangle(i);
        Eigen::Matrix<double, 3, 2> edges;
        edges.col(0) = face.world[1] - face.world[0];
        edges.col(1) = face.world[2] - face.world[0];
        if (std::abs((edges.transpose() * edges).determinant()) < 1e-24)
            continue;
        PaintRasterOptions options;
        options.maxPixels = 16ull * 1024 * 1024;
        rasterizePaintTriangle(
            face, canvas.image().width, canvas.image().height,
            [&](int x, int y, const V3& barycentric) {
                V3 point = face.world[0] * barycentric.x() + face.world[1] * barycentric.y() +
                           face.world[2] * barycentric.z();
                double distance = (point - hit.position).norm() / brush.radius;
                if (distance >= 1)
                    return;
                if (brush.visibleOnly && eye) {
                    V3 direction = point - *eye;
                    double length = direction.norm();
                    if (length < 1e-8)
                        return;
                    if (surface.raycast(*eye, direction, -1, length - std::max(1e-5, length * 2e-6)))
                        return;
                } else {
                    V3 normal = face.normals[0] * barycentric.x() + face.normals[1] * barycentric.y() +
                                face.normals[2] * barycentric.z();
                    if (normal.dot(hit.normal) < 0)
                        return;
                }
                float hardness = std::clamp(brush.hardness, 0.f, 1.f), weight = 1;
                if (distance > hardness && hardness < 1) {
                    float edge = float((1 - distance) / (1 - hardness));
                    weight = edge * edge * (3 - 2 * edge);
                }
                weight *= std::clamp(brush.opacity, 0.f, 1.f);
                size_t pixel = size_t(y) * canvas.image().width + x;
                auto [entry, inserted] = coverage.emplace(pixel, weight);
                if (!inserted)
                    entry->second = std::max(entry->second, weight);
            },
            options);
    }
    for (const auto& [pixel, weight] : coverage)
        canvas.blendPixel(int(pixel % canvas.image().width), int(pixel / canvas.image().width), brush.color,
                          weight, brush.preserveAlpha);
}
void clippedBrushRaster() {
    auto scene = std::make_shared<Scene>();
    scene->materials.resize(2);
    scene->nodes.resize(1);
    scene->order = {0};
    scene->staticLocal = scene->defaultWorld = {Mat::Identity()};
    scene->meshes = {quad(-1, 0, 0), quad(0, 1, 1)};
    // Separate islands, repeated negative UVs, a seam, and skewed world/UV axes.
    for (auto& mesh : scene->meshes)
        for (size_t i = 0; i < mesh.positions.size(); ++i) {
            mesh.positions[i][2] = mesh.positions[i][0] * .17f + mesh.positions[i][1] * .11f;
            mesh.normals[i] = {-.17f, -.11f, 1};
            mesh.uvs[0][i][0] = mesh.uvs[0][i][0] * 1.6f - .3f;
            mesh.uvs[0][i][1] = mesh.uvs[0][i][1] * 1.4f + mesh.uvs[0][i][0] * .23f - .4f;
        }
    PaintSurface surface(scene);
    for (int sample = 0; sample < 30; ++sample) {
        uint32_t width = sample % 2 ? 65 : 96, height = sample % 3 ? 63 : 32;
        PaintImage base(width, height, {37, 59, 173, 91});
        auto expected = PaintCanvas(base), actual = PaintCanvas(base);
        V3 eye((sample % 6 - 2.5) * .31, (sample / 6 - 2) * .41, 3);
        auto hit = surface.raycast(eye, V3(0, 0, -1));
        check(hit.has_value(), "Reference brush ray hits the tilted test surface");
        PaintBrush brush;
        brush.radius = .04 + (sample % 5) * .13;
        brush.hardness = (sample % 4) / 3.f;
        brush.opacity = .57f;
        brush.color = {.9f, .13f, .7f, .61f};
        brush.preserveAlpha = sample % 2 == 0;
        std::optional<V3> visibility = sample % 2 ? std::optional<V3>(eye) : std::nullopt;
        expected.beginStroke();
        actual.beginStroke();
        fullTriangleReference(expected, surface, *hit, brush, visibility);
        paintBrush(actual, surface, *hit, brush, visibility, nullptr, -1, {0, 1});
        expected.endStroke();
        actual.endStroke();
        check(actual.image().rgba == expected.image().rgba,
              "Clipped raster exactly matches full-triangle RGBA across seams, repeats and tilted faces");
    }
    auto sliver = quad(0, 1, 0);
    sliver.positions = {{0, 0, 0}, {100, 100, 0}, {100, 100.001f, 0}};
    sliver.indices = {0, 1, 2};
    sliver.normals.resize(3);
    sliver.uvs = {{{0, 0}, {1, 0}, {0, 1}}};
    scene->meshes = {sliver};
    PaintSurface thin(scene);
    PaintHit hit;
    hit.position = V3(.05, .05, 0);
    hit.normal = V3::UnitZ();
    PaintBrush detailBrush;
    detailBrush.radius = .02;
    detailBrush.visibleOnly = false;
    detailBrush.hardness = 1;
    PaintCanvas highResolution(PaintImage(2048, 2048));
    highResolution.beginStroke();
    auto stats = paintBrush(highResolution, thin, hit, detailBrush);
    check(stats.pixels > 0 && stats.pixels < 32 && highResolution.endStroke(),
          "A tiny 2K brush on a long thin whole-texture triangle avoids the infinite-plane UV budget");
    auto saved = highResolution.image().rgba;
    std::atomic_bool cancel = true;
    highResolution.beginStroke();
    fails([&] { paintBrush(highResolution, thin, hit, detailBrush, {}, &cancel); },
          "Clipped high-resolution brush preserves explicit cancellation");
    highResolution.cancelStroke();
    check(highResolution.image().rgba == saved, "Clipped brush cancellation preserves previous image");
    PaintCanvas smallReference(PaintImage(128, 96)), smallActual(PaintImage(128, 96));
    detailBrush.radius = 2;
    smallReference.beginStroke();
    smallActual.beginStroke();
    fullTriangleReference(smallReference, thin, hit, detailBrush);
    paintBrush(smallActual, thin, hit, detailBrush);
    smallReference.endStroke();
    smallActual.endStroke();
    check(smallReference.image().rgba == smallActual.image().rgba,
          "Finite-triangle clipping also agrees exactly for an ill-conditioned world triangle");
}
} // namespace
int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        ComRuntime runtime;
        batchesAndAliases();
        staleHistoryProtection();
        prepareAndFailureAtomicity();
        multiMaterialBrush();
        sharedCanvasBrushUnion();
        discardUnchangedCanvases();
        clippedBrushRaster();
        std::cout << "Paint batch: " << checks << " checks passed\n";
        return 0;
    } catch (...) {
        paintAllocationCountdown = -1;
        std::cerr << exceptionText() << '\n';
        return 1;
    }
}
