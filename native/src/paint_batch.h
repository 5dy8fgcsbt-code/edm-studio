#pragma once
#include "paint.h"

namespace edm {
// One user operation across any number of texture canvases. All access, including individual canvas
// edits, must be serialized by the caller. A background image job may own the batch while the UI waits.
class PaintBatchHistory {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    explicit PaintBatchHistory(size_t maxHistorySteps = 256);
    ~PaintBatchHistory();
    PaintBatchHistory(const PaintBatchHistory&) = delete;
    PaintBatchHistory& operator=(const PaintBatchHistory&) = delete;
    void begin(std::string label = "连续表面画笔");
    // Opens this canvas's stroke on its first touch; aliases sharing one canvas are opened only once.
    PaintCanvas& touch(int material, std::shared_ptr<PaintCanvas> canvas);
    // Removes every alias of an unchanged canvas and releases its active stroke. Changed canvases
    // remain prepared for commit; they may gain more aliases but cannot receive further pixels.
    // A preparation exception leaves batch ownership and all history branches unchanged.
    bool discardUnchanged(const std::shared_ptr<PaintCanvas>& canvas);
    // All canvases prepare before any commit. A preparation failure rolls the entire operation back.
    bool commit();
    void cancel() noexcept;
    bool undo();
    bool redo();
    bool canUndo() const noexcept;
    bool canRedo() const noexcept;
    bool active() const noexcept;
    std::vector<int> activeMaterials() const;
    // Materials changed by the last successful commit/undo/redo; empty after begin/cancel/clear.
    const std::vector<int>& changedMaterials() const noexcept;
    // Clears only the batch coordinator's history, leaving independent canvas history intact.
    void clear() noexcept;
};
} // namespace edm
