#include "paint_wrap.h"

namespace edm {
PaintMappingReport applyCylindricalWrap(const PaintSurface& surface, PaintCanvas& canvas,
                                        const PaintImage& image, const CylinderWrapOptions& options,
                                        Progress progress, const std::atomic_bool* cancel) {
    using namespace paint_mapping;
    constexpr double tau = 6.2831853071795864769;
    validate(canvas, image, options.material, options.mesh, options.opacity, options.limits,
             options.externalStroke, options.targetMaterials);
    require(options.center.allFinite() && options.axis.allFinite() && options.radialReference.allFinite(),
            "Invalid cylinder frame");
    require(options.axis.norm() > 1e-8, "Cylinder axis cannot be zero");
    require(std::isfinite(options.height) && options.height > 1e-8 && std::isfinite(options.sweepRadians) &&
                options.sweepRadians > 1e-8 && options.sweepRadians <= tau + 1e-10 &&
                std::isfinite(options.angleRadians),
            "Cylinder height and sweep must be positive; sweep cannot exceed 360 degrees");
    V3 axis = options.axis.normalized();
    V3 reference = options.radialReference - axis * axis.dot(options.radialReference);
    require(reference.norm() > 1e-8, "Cylinder radial reference cannot be parallel to its axis");
    reference.normalize();
    V3 tangent = axis.cross(reference);
    double angleCenter = std::remainder(options.angleRadians, tau);
    Operation operation(canvas, "贴花", options.limits, std::move(progress), cancel,
                        options.externalStroke);

    // Rays start outside the entire posed scene, including other materials that can occlude the target.
    auto [lo, hi] = surface.bounds();
    double outerRadius = 0;
    for (int corner = 0; corner < 8; ++corner) {
        V3 point((corner & 1) ? hi.x() : lo.x(), (corner & 2) ? hi.y() : lo.y(),
                 (corner & 4) ? hi.z() : lo.z());
        V3 delta = point - options.center;
        outerRadius = std::max(outerRadius, (delta - axis * delta.dot(axis)).norm());
    }
    require(std::isfinite(outerRadius), "Invalid posed model bounds");
    outerRadius += std::max(1e-4, outerRadius * .01);

    // A shared UV cannot carry different colours for two surfaces. Stable outer-first order makes
    // this unavoidable ambiguity deterministic, and the report tells the caller when it occurred.
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
            throw std::runtime_error("Wrap candidate primitive is outside the current paint surface");
        auto triangle = surface.triangle(primitive);
        if (!targetTriangle(triangle, options.material, options.mesh, options.targetMaterials))
            continue;
        double radius = 0;
        for (auto& point : triangle.world) {
            V3 delta = point - options.center;
            radius += (delta - axis * delta.dot(axis)).norm();
        }
        triangles.emplace_back(-radius, primitive);
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
                V3 point = position(triangle, barycentric), delta = point - options.center;
                double axial = delta.dot(axis), v = .5 - axial / options.height;
                if (v < 0 || v > 1)
                    return;
                V3 radial = delta - axis * axial;
                double radius = radial.norm();
                if (radius <= 1e-10)
                    return;
                radial /= radius;
                double angle =
                    std::remainder(std::atan2(radial.dot(tangent), radial.dot(reference)) - angleCenter, tau);
                double u = .5 + angle / options.sweepRadians;
                if (u < -1e-10 || u > 1 + 1e-10)
                    return;
                F4 color = sample(image, std::clamp(u, 0., 1.), v, options.opacity);
                if (color[3] <= 0)
                    return;
                V3 normal = triangle.normals[0] * barycentric[0] + triangle.normals[1] * barycentric[1] +
                            triangle.normals[2] * barycentric[2];
                if (options.outwardOnly && normal.dot(radial) <= 1e-8) {
                    ++operation.report.backfaceSamples;
                    return;
                }
                if (options.occlusion) {
                    V3 origin = options.center + axis * axial + radial * outerRadius;
                    double distance = outerRadius - radius;
                    operation.rayTest();
                    if (surface.raycast(origin, -radial, -1, distance - distanceBias(distance))) {
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
