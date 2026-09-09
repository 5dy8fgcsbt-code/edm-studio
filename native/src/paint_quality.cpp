#include "paint_quality.h"

namespace edm {
std::optional<DecalTexelEstimate> estimateDecalTexels(
    const PaintTriangle& triangle, const std::array<Eigen::Vector2d, 3>& coordinates,
    uint32_t canvasWidth, uint32_t canvasHeight, double decalWidth, double decalHeight) noexcept {
    if (!triangle.hasUV || !canvasWidth || !canvasHeight || !std::isfinite(decalWidth) ||
        !std::isfinite(decalHeight) || decalWidth <= 0 || decalHeight <= 0)
        return {};
    for (size_t vertex = 0; vertex < 3; ++vertex)
        if (!coordinates[vertex].allFinite() || !std::isfinite(triangle.uv[vertex][0]) ||
            !std::isfinite(triangle.uv[vertex][1]))
            return {};

    Eigen::Matrix2d xy, uv;
    for (int column = 0; column < 2; ++column) {
        xy.col(column) = coordinates[column + 1] - coordinates[0];
        uv(0, column) = double(triangle.uv[column + 1][0]) - triangle.uv[0][0];
        uv(1, column) = double(triangle.uv[column + 1][1]) - triangle.uv[0][1];
    }
    // Normalize before taking determinants, so microscopic but well-shaped triangles remain valid
    // and very large coordinates cannot overflow an otherwise useful local calculation.
    const double xyScale = xy.cwiseAbs().maxCoeff(), uvScale = uv.cwiseAbs().maxCoeff();
    if (!std::isfinite(xyScale) || !std::isfinite(uvScale) || xyScale <= 0 || uvScale <= 0)
        return {};
    xy /= xyScale;
    uv /= uvScale;
    if (std::abs(xy.determinant()) <= 1e-12 || std::abs(uv.determinant()) <= 1e-12)
        return {};

    Eigen::Matrix2d pixelsPerUnit = uv * xy.inverse();
    pixelsPerUnit.row(0) *= double(canvasWidth);
    pixelsPerUnit.row(1) *= double(canvasHeight);
    pixelsPerUnit *= uvScale / xyScale;
    if (!pixelsPerUnit.allFinite())
        return {};
    DecalTexelEstimate result{pixelsPerUnit.col(0).stableNorm() * decalWidth,
                             pixelsPerUnit.col(1).stableNorm() * decalHeight};
    if (!std::isfinite(result.width) || !std::isfinite(result.height) || result.width <= 0 ||
        result.height <= 0)
        return {};
    return result;
}
} // namespace edm
