#pragma once
#include "paint_auto_mapping.h"

namespace edm {
struct SurfaceDecalPatch;
struct SurfaceDecalOptions {
    int material = -1, mesh = -1;
    // Nonempty selects shared texture aliases as one union, with one blend per target texel.
    std::vector<int> targetMaterials;
    // Reuse candidate IDs only with the same PaintSurface pose and decal placement. An empty vector
    // maps nothing; nullopt discovers intersecting triangles without loading any textures.
    std::optional<std::vector<uint32_t>> candidatePrimitives;
    V3 center = V3::Zero(), normal = V3::UnitZ(), tangent = V3::UnitX();
    double width = 1, height = 1, depth = .1;
    // Curved mapping only: optional physical boundary-gap limit in world metres (0 disables it).
    // Connections are virtual; source topology/UVs remain intact and the missing strip keeps its width.
    double gapDistance = 0;
    // Right-handed rotation around the outward normal. The source image's v coordinate points down.
    double rotationRadians = 0;
    float opacity = 1;
    bool preserveAlpha = true, frontFacesOnly = true, occlusion = true;
    bool externalStroke = false;
    // Curved mapping is built once at placement, then shared by preview and every material bake.
    std::shared_ptr<const SurfaceDecalPatch> conformPatch;
    // Frozen placement eye for curved front-face and visibility tests. Planar mapping ignores it.
    std::optional<V3> projectionEye;
    PaintMappingLimits limits;
};

struct SurfaceDecalFrame {
    V3 center, right, up, normal;
    double width = 1, height = 1, depth = .1;
};

// Validates the placement and returns exactly the orthonormal frame used by the CPU projector.
// The volume spans x=[-width/2,width/2], y=[-height/2,height/2], z=[-depth,+depth].
SurfaceDecalFrame surfaceDecalFrame(const SurfaceDecalOptions& options);
PaintMappingCandidates findDecalMaterials(const PaintSurface& surface, const SurfaceDecalOptions& options,
                                          Progress progress = {}, const std::atomic_bool* cancel = nullptr);
// Parallel projection from the outward-normal side. Occlusion tests the complete posed model,
// including materials outside the target union and geometry in front of the decal's depth volume.
PaintMappingReport applySurfaceDecal(const PaintSurface& surface, PaintCanvas& canvas,
                                     const PaintImage& image, const SurfaceDecalOptions& options,
                                     Progress progress = {}, const std::atomic_bool* cancel = nullptr);
} // namespace edm
