#pragma once
#include "export.h"

namespace edm {
// Shared, in-memory scene payload. FBX and glTF use identical animation/skin data.
struct ExportPayload {
    Json document;
    std::vector<uint8_t> buffer;
    Json report;
};
ExportPayload buildExportPayload(const Scene& scene, const ExportOptions& options, Progress progress = {},
                                 const std::atomic_bool* cancel = nullptr);
Json exportObjScene(const Scene& scene, const fs::path& path, const ExportOptions& options,
                    Progress progress = {}, const std::atomic_bool* cancel = nullptr);
Json exportFbxScene(const Scene& scene, const fs::path& path, const ExportOptions& options,
                    Progress progress = {}, const std::atomic_bool* cancel = nullptr);
} // namespace edm
