#include "paint_batch.h"
#include <cstdlib>
#include <iostream>
#include <new>

// Fail one allocation at a time while remapping. Disarm before throwing so assertions and
// exception propagation can allocate normally, as in the existing transaction preflight tests.
thread_local long remapAllocationCountdown = -1;
void* operator new(std::size_t size) {
    if (remapAllocationCountdown >= 0 && remapAllocationCountdown-- == 0) {
        remapAllocationCountdown = -1;
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
void check(bool value, const char* message) {
    require(value, std::string("Paint history material remapping: ") + message);
    ++checks;
}
template <class Function> void fails(Function action, const char* message) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, message);
}
std::shared_ptr<PaintCanvas> canvas() {
    return std::make_shared<PaintCanvas>(PaintImage(8, 8, {0, 0, 0, 91}));
}
void dot(PaintCanvas& canvas, int x = 1, F4 color = {1, 0, 0, 1}) {
    canvas.blendPixel(x, 1, color);
}
void commit(PaintBatchHistory& history, int material, const std::shared_ptr<PaintCanvas>& canvas, int x = 1,
            F4 color = {1, 0, 0, 1}) {
    history.begin();
    dot(history.touch(material, canvas), x, color);
    require(history.commit(), "Fixture operation must change pixels");
}

void mixedGroup() {
    auto aircraft = canvas(), removed = canvas(), weapon = canvas();
    const auto aircraftBefore = aircraft->image().rgba, weaponBefore = weapon->image().rgba;
    PaintBatchHistory history;
    history.begin("Aircraft, rack and weapon stamp");
    dot(history.touch(2, aircraft));
    dot(history.touch(5, removed));
    dot(history.touch(7, weapon));
    history.commit();
    const auto aircraftAfter = aircraft->image().rgba, removedAfter = removed->image().rgba,
               weaponAfter = weapon->image().rgba;
    const auto aircraftToken = aircraft->undoToken(), removedToken = removed->undoToken(),
               weaponToken = weapon->undoToken();
    history.remapMaterials({-1, -1, 0, -1, -1, -1, -1, 1}, {aircraft, weapon});
    check(history.changedMaterials() == std::vector<int>{0, 1},
          "Detachment filters the removed canvas and publishes renumbered material aliases");
    check(aircraft->undoToken() == aircraftToken && removed->undoToken() == removedToken &&
              weapon->undoToken() == weaponToken && aircraft->image().rgba == aircraftAfter &&
              weapon->image().rgba == weaponAfter,
          "Remapping does not alter pixels or any canvas-local operation token");
    check(history.undo() && aircraft->image().rgba == aircraftBefore &&
              weapon->image().rgba == weaponBefore && removed->image().rgba == removedAfter,
          "Undo of a mixed group still restores both surviving canvases without touching the removed rack");
    check(history.changedMaterials() == std::vector<int>{0, 1} && aircraft->redoToken() == aircraftToken &&
              weapon->redoToken() == weaponToken,
          "Undo publishes surviving material indices while preserving their original tokens");
    check(history.redo() && aircraft->image().rgba == aircraftAfter && weapon->image().rgba == weaponAfter &&
              removed->image().rgba == removedAfter,
          "Redo restores the surviving part of a mixed operation exactly");
    check(removed->canUndo() && removed->undoToken() == removedToken,
          "Dropping a coordinator member leaves its independently owned canvas history intact");
}

void removeEmptyGroups() {
    auto keep = canvas(), removed = canvas();
    PaintBatchHistory history;
    commit(history, 0, keep);
    const auto first = keep->image().rgba;
    commit(history, 1, removed);
    commit(history, 0, keep, 3, {0, 1, 0, 1});
    const auto third = keep->image().rgba;
    history.undo(); // Keep the third operation on redo.
    history.undo(); // The removed-only operation is now both redo.back() and lastChanged.
    const auto removedBefore = removed->image().rgba;
    history.remapMaterials({0, -1}, {keep});
    check(history.changedMaterials().empty() && history.canUndo() && history.canRedo(),
          "Removed-only redo and last-changed groups disappear without blocking surviving history");
    check(history.redo() && keep->image().rgba == third && removed->image().rgba == removedBefore,
          "Redo skips a removed-only operation and reaches the next surviving canvas operation");
    check(history.undo() && keep->image().rgba == first && history.undo() && !history.canUndo(),
          "The surviving canvas retains its full older undo chain");
    check(history.redo() && history.redo() && keep->image().rgba == third,
          "The complete retained chain remains redoable after removed groups are pruned");

    PaintBatchHistory top;
    auto earlier = canvas(), detached = canvas();
    commit(top, 0, earlier);
    commit(top, 1, detached);
    top.remapMaterials({0, -1}, {earlier});
    check(
        top.changedMaterials().empty() && top.undo() && !top.canUndo(),
        "An applied removed-only group is pruned from undo and does not hide the older surviving operation");
    top.remapMaterials({-1}, {});
    check(!top.canUndo() && !top.canRedo() && top.changedMaterials().empty(),
          "Removing every material clears all coordinator groups without touching canvas tokens");
}

void aliasesAndRepeatedMapping() {
    auto shared = canvas();
    PaintBatchHistory history;
    // Only the old canonical index was touched. A separate material using the same canvas survives.
    commit(history, 1, shared);
    const auto after = shared->image().rgba;
    const auto token = shared->undoToken();
    history.remapMaterials({-1, -1, 0, 1}, {shared, shared});
    check(history.changedMaterials() == std::vector<int>{0, 1} && history.canUndo(),
          "Removing the touched canonical material retains all surviving aliases of its shared canvas");
    check(history.undo() && history.changedMaterials() == std::vector<int>{0, 1} &&
              shared->redoToken() == token,
          "Shared aliases undo one canvas operation exactly once");
    check(history.redo() && shared->image().rgba == after && shared->undoToken() == token,
          "A retained alias replays the original canonical canvas token without copying history");

    auto other = canvas();
    PaintBatchHistory overlapping;
    commit(overlapping, 5, other);
    // 5 -> 2 and 2 -> 0 catches accidental sequential/double mapping of a shared lastChanged group.
    overlapping.remapMaterials({-1, -1, 0, -1, -1, 2}, {nullptr, nullptr, other});
    check(overlapping.changedMaterials() == std::vector<int>{2} && overlapping.undo() &&
              overlapping.changedMaterials() == std::vector<int>{2},
          "An undo group shared with lastChanged is remapped once, not transitively through its new index");
    overlapping.remapMaterials({-1, -1, 0}, {other});
    check(overlapping.changedMaterials() == std::vector<int>{0} && overlapping.redo() &&
              overlapping.changedMaterials() == std::vector<int>{0},
          "A second detachment remaps both a redo group and its shared lastChanged reference coherently");
}

void invalidMappings() {
    auto a = canvas(), b = canvas();
    PaintBatchHistory history;
    commit(history, 0, a);
    commit(history, 3, b);
    const auto originalA = a->image().rgba, originalB = b->image().rgba;
    const auto tokenA = a->undoToken(), tokenB = b->undoToken();
    const auto unchanged = [&] {
        check(history.changedMaterials() == std::vector<int>{3} && history.canUndo() &&
                  a->undoToken() == tokenA && b->undoToken() == tokenB && a->image().rgba == originalA &&
                  b->image().rgba == originalB,
              "A rejected remapping preserves previous groups, change notifications, pixels and tokens");
    };
    fails([&] { history.remapMaterials({0, -1, -1, -2}, {a, b}); },
          "Indices below the removal sentinel are rejected");
    unchanged();
    fails([&] { history.remapMaterials({0, -1, -1, 2}, {a, b}); },
          "New indices outside the new-scene canvas array are rejected");
    unchanged();
    fails([&] { history.remapMaterials({0}, {a, b}); },
          "A too-short old-to-new map is rejected even after an earlier group could be cloned");
    unchanged();
    history.begin("Active drawing remains usable");
    dot(history.touch(0, a), 4, {0, 0, 1, 1});
    const auto activePixels = a->image().rgba;
    fails([&] { history.remapMaterials({0, -1, -1, 1}, {a, b}); },
          "Material remapping cannot run during an active multi-canvas stroke");
    check(history.active() && a->strokeActive() && history.activeMaterials() == std::vector<int>{0} &&
              a->image().rgba == activePixels && history.commit(),
          "Rejecting an active remap neither cancels nor damages the in-progress stroke");
}

void allocationAtomicity() {
    size_t failures = 0;
    bool completed = false;
    for (long failAt = 0; failAt < 256; ++failAt) {
        auto a = canvas(), removed = canvas(), c = canvas();
        PaintBatchHistory history;
        history.begin();
        dot(history.touch(0, a));
        dot(history.touch(1, removed));
        history.commit();
        commit(history, 2, c);
        commit(history, 0, a, 3, {0, 1, 0, 1});
        history.undo();
        const auto aPixels = a->image().rgba, removedPixels = removed->image().rgba,
                   cPixels = c->image().rgba;
        const auto aUndo = a->undoToken(), aRedo = a->redoToken(), removedUndo = removed->undoToken(),
                   cUndo = c->undoToken();
        const std::vector<int> mapping{1, -1, 0};
        const std::vector<std::shared_ptr<PaintCanvas>> retained{c, a};
        bool failed = false;
        remapAllocationCountdown = failAt;
        try {
            history.remapMaterials(mapping, retained);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        remapAllocationCountdown = -1;
        if (!failed) {
            completed = true;
            check(history.changedMaterials() == std::vector<int>{1} && history.canUndo() && history.canRedo(),
                  "Remapping completes after all allocation sites have been exercised");
            break;
        }
        ++failures;
        check(history.changedMaterials() == std::vector<int>{0} && history.canUndo() && history.canRedo(),
              "Every allocation failure preserves undo, redo and lastChanged in their original namespace");
        check(a->image().rgba == aPixels && removed->image().rgba == removedPixels &&
                  c->image().rgba == cPixels && a->undoToken() == aUndo && a->redoToken() == aRedo &&
                  removed->undoToken() == removedUndo && c->undoToken() == cUndo,
              "Failed preparation cannot alter retained or detached canvas pixels and token chains");
        check(history.undo() && history.changedMaterials() == std::vector<int>{2} && history.undo() &&
                  history.changedMaterials() == std::vector<int>{0, 1},
              "After any allocation failure the old undo chain still includes every original group member");
        check(history.redo() && history.changedMaterials() == std::vector<int>{0, 1} && history.redo() &&
                  history.changedMaterials() == std::vector<int>{2} && history.redo() &&
                  history.changedMaterials() == std::vector<int>{0},
              "After any allocation failure the complete original redo chain remains valid");
    }
    check(completed && failures >= 12,
          "Failure injection covers allocations across both stacks and shared group cloning");
}
} // namespace

int wmain() {
    try {
        mixedGroup();
        removeEmptyGroups();
        aliasesAndRepeatedMapping();
        invalidMappings();
        allocationAtomicity();
        std::cout << "Paint history material remapping: " << checks << " checks passed\n";
        return 0;
    } catch (...) {
        remapAllocationCountdown = -1;
        std::cerr << exceptionText() << '\n';
        return 1;
    }
}
