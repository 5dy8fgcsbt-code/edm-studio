#include "paint_projection.h"

namespace edm {
PaintMappingReport applyCameraProjection(const PaintSurface& surface, PaintCanvas& canvas,
                                         const PaintImage& image, const CameraProjectionOptions& options,
                                         Progress progress, const std::atomic_bool* cancel) {
    using namespace paint_mapping;
    validate(canvas, image, options.material, options.mesh, options.opacity, options.limits,
             options.externalStroke, options.targetMaterials);
    require(options.viewProjection.allFinite() && options.eye.allFinite(), "Invalid projection camera");
    require(std::isfinite(options.center[0]) && std::isfinite(options.center[1]) &&
                std::isfinite(options.size[0]) && std::isfinite(options.size[1]) && options.size[0] > 1e-8 &&
                options.size[1] > 1e-8 && std::isfinite(options.viewportAspect) &&
                options.viewportAspect > 1e-8 && std::isfinite(options.rotationRadians),
            "Invalid projected image placement");
    Operation operation(canvas, "相机图片投影", options.limits, std::move(progress), cancel,
                        options.externalStroke);
    double cosine = std::cos(options.rotationRadians), sine = std::sin(options.rotationRadians);
    std::vector<std::pair<double, uint32_t>> triangles;
    size_t count =
        options.candidatePrimitives ? options.candidatePrimitives->size() : surface.triangleCount();
    if (options.candidatePrimitives)
        triangles.reserve(std::min(count, surface.triangleCount()));
    for (size_t i = 0; i < count; ++i) {
        if ((i & 1023) == 0)
            operation.check();
        uint32_t primitive = options.candidatePrimitives ? (*options.candidatePrimitives)[i] : uint32_t(i);
        if (primitive >= surface.triangleCount())
            throw std::runtime_error("Projection candidate primitive is outside the current paint surface");
        auto triangle = surface.triangle(primitive);
        if (!targetTriangle(triangle, options.material, options.mesh, options.targetMaterials))
            continue;
        V3 centre = (triangle.world[0] + triangle.world[1] + triangle.world[2]) / 3.;
        triangles.emplace_back((centre - options.eye).squaredNorm(), primitive);
    }
    std::sort(triangles.begin(), triangles.end());
    PaintRasterOptions raster;
    raster.cancel = cancel;
    raster.maxPixels = options.limits.rasterSamples;
    for (auto [score, primitive] : triangles) {
        operation.check();
        auto triangle = surface.triangle(primitive);
        ++operation.report.triangles;
        rasterizePaintTriangle(
            triangle, canvas.image().width, canvas.image().height,
            [&](int x, int y, const V3& barycentric) {
                operation.sampleVisited();
                V3 point = position(triangle, barycentric);
                V4 clip = options.viewProjection * V4(point.x(), point.y(), point.z(), 1);
                // Direct3D's depth interval is [0,w]. Clipping each actual surface point also handles
                // triangles crossing the near plane without projecting their behind-camera vertices.
                if (!clip.allFinite() || clip.w() <= 1e-10 || clip.z() < 0 || clip.z() > clip.w() ||
                    std::abs(clip.x()) > clip.w() || std::abs(clip.y()) > clip.w())
                    return;
                double screenX = .5 + .5 * clip.x() / clip.w();
                double screenY = .5 - .5 * clip.y() / clip.w();
                double dx = (screenX - options.center[0]) * options.viewportAspect;
                double dy = screenY - options.center[1];
                double u = .5 + (cosine * dx + sine * dy) / (options.size[0] * options.viewportAspect);
                double v = .5 + (-sine * dx + cosine * dy) / options.size[1];
                if (u < 0 || u > 1 || v < 0 || v > 1)
                    return;
                F4 color = sample(image, u, v, options.opacity);
                if (color[3] <= 0)
                    return;
                V3 normal = triangle.normals[0] * barycentric[0] + triangle.normals[1] * barycentric[1] +
                            triangle.normals[2] * barycentric[2];
                V3 towardEye = options.eye - point;
                double distance = towardEye.norm();
                if (distance <= 1e-8)
                    return;
                if (options.frontFacesOnly && normal.dot(towardEye) <= 1e-8) {
                    ++operation.report.backfaceSamples;
                    return;
                }
                if (options.occlusion) {
                    operation.rayTest();
                    if (surface.raycast(options.eye, -towardEye / distance, -1,
                                        distance - distanceBias(distance))) {
                        ++operation.report.occludedSamples;
                        return;
                    }
                }
                operation.blend(x, y, color, options.preserveAlpha);
            },
            raster);
    }
    return operation.finish();
}
} // namespace edm
