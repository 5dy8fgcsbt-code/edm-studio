#pragma once
#include "paint_document.h"

namespace edm {
// Reconstructs the source EDM assembly before loadPaintDocument performs the complete geometry,
// material and image fingerprint checks. A matching assembly keeps the original shared_ptr.
// The input scene is never modified; callers publish the result only after loading the document.
std::shared_ptr<Scene> restorePaintAssembly(std::shared_ptr<Scene> baseScene, const fs::path& projectPath,
                                            Progress progress = {}, const std::atomic_bool* cancel = nullptr);
} // namespace edm
