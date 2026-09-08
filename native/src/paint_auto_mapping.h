#pragma once
#include "paint_projection.h"
#include "paint_wrap.h"

namespace edm {
struct PaintMappingCandidates {
    std::vector<int> materials; // Sorted global Scene material indices, never sidebar row indices.
    std::map<int, size_t> trianglesByMaterial;
    // Original PaintSurface primitive IDs, reusable while the pose and mapping placement stay fixed.
    std::map<int, std::vector<uint32_t>> primitivesByMaterial;
    size_t examinedTriangles = 0, candidateTriangles = 0;
    double seconds = 0;
};

// Conservative geometric footprint tests, with no texture loading or UV rasterization.
// material == -1 discovers every paintable material; >= 0 restricts to that global material.
// A nonempty targetMaterials union overrides material, as it does for the apply functions.
// Full polygon clipping preserves triangles crossing a near plane or the image's edges even
// when none of their vertices fall inside. Alpha and exact occlusion remain per-texel apply tests:
// a few visibility rays cannot prove a whole triangle is hidden without missing exposed slivers.
PaintMappingCandidates findProjectionMaterials(const PaintSurface& surface,
                                               const CameraProjectionOptions& options, Progress progress = {},
                                               const std::atomic_bool* cancel = nullptr);
PaintMappingCandidates findWrapMaterials(const PaintSurface& surface, const CylinderWrapOptions& options,
                                         Progress progress = {}, const std::atomic_bool* cancel = nullptr);

// One budget for a complete multi-material request, including candidate discovery and texture loading.
// Construct BEFORE discovery/loading. For discovery and EACH apply, copy the same global options,
// set limits=budget.remaining(), and for apply set material plus externalStroke=true. Call account()
// after each successful apply, then commit the external batch only after every canvas succeeds.
// Any exception (including remaining/account) requires cancelling ALL touched canvases.
// Do not recenter/rescale per material: one global placement makes artwork continuous across textures.
class PaintMappingBudget {
    PaintMappingLimits limits;
    Clock::time_point started = Clock::now();
    uint64_t pixels = 0, rays = 0;

  public:
    explicit PaintMappingBudget(PaintMappingLimits limits = {});
    PaintMappingLimits remaining() const;
    void account(const PaintMappingReport& report);
    uint64_t rasterSamples() const {
        return pixels;
    }
    uint64_t rayTests() const {
        return rays;
    }
    double elapsed() const {
        return edm::seconds(started);
    }
};
} // namespace edm
