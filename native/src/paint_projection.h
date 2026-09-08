#pragma once
#include "paint_mapping_common.h"

namespace edm {
struct CameraProjectionOptions {
    int material = -1, mesh = -1;
    // Nonempty overrides material: paint shared texture aliases together with one visited-texel set.
    std::vector<int> targetMaterials;
    // Reuse discovery for this same posed surface and placement. nullopt scans the whole surface;
    // an empty vector deliberately maps nothing. Material/mesh selection is still checked.
    std::optional<std::vector<uint32_t>> candidatePrimitives;
    Mat viewProjection = Mat::Identity();
    V3 eye{0, 0, 10};
    // Image placement in the viewport: top-left (0,0), bottom-right (1,1).
    F2 center{.5f, .5f}, size{1, 1};
    float viewportAspect = 1;
    double rotationRadians = 0;
    float opacity = 1;
    bool preserveAlpha = true, frontFacesOnly = true, occlusion = true;
    // Caller must begin and commit/cancel every touched canvas as one batch when enabled.
    bool externalStroke = false;
    PaintMappingLimits limits;
};

// Accepts the exact Direct3D viewProjection matrix and eye used to render the current pose.
PaintMappingReport applyCameraProjection(const PaintSurface& surface, PaintCanvas& canvas,
                                         const PaintImage& image, const CameraProjectionOptions& options,
                                         Progress progress = {}, const std::atomic_bool* cancel = nullptr);
} // namespace edm
