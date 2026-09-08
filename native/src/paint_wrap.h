#pragma once
#include "paint_mapping_common.h"

namespace edm {
struct CylinderWrapOptions {
    int material = -1, mesh = -1;
    // Nonempty overrides material: paint shared texture aliases together with one visited-texel set.
    std::vector<int> targetMaterials;
    // Reuse discovery for this same posed surface and placement. nullopt scans the whole surface;
    // an empty vector deliberately maps nothing. Material/mesh selection is still checked.
    std::optional<std::vector<uint32_t>> candidatePrimitives;
    V3 center = V3::Zero(), axis = V3::UnitX(), radialReference = V3::UnitY();
    double height = 10, angleRadians = 0, sweepRadians = 6.2831853071795864769;
    float opacity = 1;
    bool preserveAlpha = true, outwardOnly = true, occlusion = true;
    // Caller must begin and commit/cancel every touched canvas as one batch when enabled.
    bool externalStroke = false;
    PaintMappingLimits limits;
};

// Image U follows angle around axis; image V runs from +height/2 to -height/2.
// angleRadians is the centre of the image, measured from radialReference towards axis cross reference.
PaintMappingReport applyCylindricalWrap(const PaintSurface& surface, PaintCanvas& canvas,
                                        const PaintImage& image, const CylinderWrapOptions& options,
                                        Progress progress = {}, const std::atomic_bool* cancel = nullptr);
} // namespace edm
