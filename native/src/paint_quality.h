#pragma once
#include "paint.h"

namespace edm {

// A first edit should keep the source texture's detail. This is a ceiling, not an instruction to
// upscale smaller textures; PaintImage::load preserves their original dimensions.
inline constexpr int DefaultPaintMaxDimension = 8192;

struct DecalTexelEstimate {
    double width = 0;
    double height = 0;
};

// Local texture-pixel coverage along the decal's X and Y axes, using one original triangle and its
// corresponding decal coordinates in world units. The estimate includes UV rotation, mirroring,
// shear and a non-square canvas. It is not a patch-wide minimum or a count of distinct painted
// texels: shared UVs, occlusion, clipping and varying density elsewhere may reduce useful detail.
// Missing/degenerate UVs or coordinates, nonfinite inputs and nonpositive dimensions return nullopt.
std::optional<DecalTexelEstimate> estimateDecalTexels(
    const PaintTriangle& triangle, const std::array<Eigen::Vector2d, 3>& coordinates,
    uint32_t canvasWidth, uint32_t canvasHeight, double decalWidth, double decalHeight) noexcept;

} // namespace edm
