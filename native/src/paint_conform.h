#pragma once
#include "paint_decal.h"

namespace edm {
struct SurfaceDecalPatchTriangle {
    uint32_t primitive = 0;
    PaintTriangle triangle; // Original mesh/index/material, posed geometry and original texture UVs.
    std::array<Eigen::Vector2d, 3> coordinates; // Decal XY in world units; image V points down.
};

// An immutable local surface chart. No source mesh, UV, hierarchy or animation is modified.
// Coordinates are shared across coincident edges, including splits between mesh/material islands.
// The caller retains the original PaintSurface for the patch lifetime and rebuilds the patch when
// replacing its pose/visibility. The captured raw pointers are identity checks, never owning handles.
struct SurfaceDecalPatch {
    SurfaceDecalFrame frame;
    std::optional<V3> projectionEye;
    const PaintSurface* sourceSurface = nullptr;
    const Scene* sourceScene = nullptr;
    uint64_t sourceGeneration = 0;
    size_t sourceTriangleCount = 0;
    uint32_t seedPrimitive = 0;
    double maxBendDegrees = 65, seamTolerance = 0;
    // The visible side chosen by the seed may reverse a two-sided/authored inward-facing sheet.
    // Keep source normals intact and orient visibility consistently over the entire chart.
    double normalSign = 1;
    size_t examinedTriangles = 0, stitchedEdges = 0, blockedEdges = 0, conflictTriangles = 0;
    double seconds = 0;
    std::vector<SurfaceDecalPatchTriangle> triangles; // Sorted by original primitive ID.
};

// Unfolds adjoining triangles from the hit. maxBendDegrees is the maximum local dihedral angle
// across an edge, not a total angular limit relative to the seed. Nearby disconnected surfaces,
// nonmanifold edges, attachment-instance boundaries and inconsistent charts are not crossed.
// Limits.seconds and cancellation cover construction; topology is capped at 200,000 triangles.
// An eye is required when frontFacesOnly or occlusion is enabled; disable both for a full wrap.
std::shared_ptr<const SurfaceDecalPatch> buildSurfaceDecalPatch(
    const PaintSurface& surface, const PaintHit& hit, const SurfaceDecalOptions& options,
    double maxBendDegrees = 65, Progress progress = {}, const std::atomic_bool* cancel = nullptr);

// Rejects stale surface/placement metadata before discovery, painting, or preview use.
void validateSurfaceDecalPatch(const PaintSurface& surface, const SurfaceDecalOptions& options);
} // namespace edm
