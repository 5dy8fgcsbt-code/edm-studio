#include "paint_decal.h"
#include "paint_conform.h"

namespace edm {
namespace {
struct DecalFootprint {
    std::array<double, 4> uvBounds;
    double depth = 0;
};
void validateSelection(const SurfaceDecalOptions& options) {
    require(options.material >= -1 && options.mesh >= -1, "Invalid surface decal material selection");
    require(std::all_of(options.targetMaterials.begin(), options.targetMaterials.end(),
                        [](int material) { return material >= 0; }),
            "Invalid shared texture selection for surface decal");
    require(std::isfinite(options.opacity) && options.opacity >= 0 && options.opacity <= 1,
            "Invalid surface decal opacity");
    require(options.limits.rasterSamples && options.limits.rayTests &&
                std::isfinite(options.limits.seconds) && options.limits.seconds > 0,
            "Invalid surface decal work budget");
}
void checkWork(Clock::time_point started, const PaintMappingLimits& limits, const std::atomic_bool* cancel) {
    if (cancel && cancel->load(std::memory_order_relaxed))
        throw std::runtime_error("Surface decal cancelled");
    if (seconds(started) > limits.seconds)
        throw std::runtime_error("Surface decal time budget exceeded; reduce decal size");
}
std::optional<DecalFootprint> clippedFootprint(const PaintTriangle& triangle, const SurfaceDecalFrame& frame,
                                               bool frontFacesOnly,
                                               const SurfaceDecalPatchTriangle* chart = nullptr,
                                               const std::optional<V3>& eye = {}) {
    if (!triangle.hasUV)
        return {};
    if (!chart && frontFacesOnly && std::none_of(triangle.normals.begin(), triangle.normals.end(),
                                       [&](const V3& n) { return n.dot(frame.normal) > 0; }))
        return {};
    for (const auto& uv : triangle.uv)
        if (!std::isfinite(uv[0]) || !std::isfinite(uv[1]))
            throw std::runtime_error("Surface decal encountered non-finite UV coordinates");
    const double determinant =
        (double(triangle.uv[1][0]) - triangle.uv[0][0]) * (double(triangle.uv[2][1]) - triangle.uv[0][1]) -
        (double(triangle.uv[2][0]) - triangle.uv[0][0]) * (double(triangle.uv[1][1]) - triangle.uv[0][1]);
    if (std::abs(determinant) < 1e-20)
        return {};
    std::array<V3, 3> local;
    for (size_t i = 0; i < 3; ++i) {
        V3 delta = triangle.world[i] - frame.center;
        local[i] = chart ? V3(chart->coordinates[i].x(), chart->coordinates[i].y(), 0)
                         : V3(delta.dot(frame.right), delta.dot(frame.up), delta.dot(frame.normal));
        if (!local[i].allFinite())
            throw std::runtime_error("Surface decal encountered non-finite world coordinates");
    }
    // Six planes can grow a convex triangle to at most nine vertices. Keep original barycentrics,
    // so UV clipping does not alter the final original-triangle interpolation or seam coverage.
    std::array<V3, 16> polygon{}, output{};
    polygon[0] = V3::UnitX();
    polygon[1] = V3::UnitY();
    polygon[2] = V3::UnitZ();
    size_t count = 3;
    const V3 extent(frame.width * .5, frame.height * .5, frame.depth);
    for (int axis = 0; axis < 3; ++axis)
        for (double sign : {-1., 1.}) {
            if (!count)
                return {};
            size_t size = 0;
            const double slack = 1e-10 * (1 + extent[axis]);
            auto distance = [&](const V3& barycentric) {
                double coordinate = local[0][axis] * barycentric.x() + local[1][axis] * barycentric.y() +
                                    local[2][axis] * barycentric.z();
                return extent[axis] - sign * coordinate + slack;
            };
            auto append = [&](const V3& vertex) {
                if (size && (vertex - output[size - 1]).squaredNorm() == 0)
                    return;
                if (size == output.size())
                    throw std::runtime_error("Surface decal clipping capacity exceeded");
                output[size++] = vertex;
            };
            V3 previous = polygon[count - 1];
            double before = distance(previous);
            for (size_t i = 0; i < count; ++i) {
                V3 current = polygon[i];
                double after = distance(current);
                if ((before >= 0) != (after >= 0))
                    append(previous + (current - previous) * (before / (before - after)));
                if (after >= 0)
                    append(current);
                previous = current;
                before = after;
            }
            if (size > 1 && (output[0] - output[size - 1]).squaredNorm() == 0)
                --size;
            polygon = output;
            count = size;
        }
    if (!count)
        return {};
    DecalFootprint footprint{{1e100, 1e100, -1e100, -1e100},
                             chart && eye ? -((*eye - (triangle.world[0] + triangle.world[1] +
                                                                    triangle.world[2]) / 3).squaredNorm())
                                          : (local[0].z() + local[1].z() + local[2].z()) / 3};
    for (size_t i = 0; i < count; ++i)
        for (int axis = 0; axis < 2; ++axis) {
            double uv = 0;
            for (int vertex = 0; vertex < 3; ++vertex)
                uv += polygon[i][vertex] * double(triangle.uv[vertex][axis]);
            footprint.uvBounds[axis] = std::min(footprint.uvBounds[axis], uv);
            footprint.uvBounds[axis + 2] = std::max(footprint.uvBounds[axis + 2], uv);
        }
    for (int axis = 0; axis < 2; ++axis) {
        const double slack = 1e-10 * std::max({1., std::abs(footprint.uvBounds[axis]),
                                               std::abs(footprint.uvBounds[axis + 2])});
        footprint.uvBounds[axis] -= slack;
        footprint.uvBounds[axis + 2] += slack;
    }
    return footprint;
}
std::vector<uint32_t> candidatePrimitives(const PaintSurface& surface, const SurfaceDecalOptions& options,
                                          const SurfaceDecalFrame& frame) {
    std::vector<uint32_t> result;
    if (options.candidatePrimitives)
        result = *options.candidatePrimitives;
    else if (options.conformPatch) {
        result.reserve(options.conformPatch->triangles.size());
        for (const auto& triangle : options.conformPatch->triangles)
            result.push_back(triangle.primitive);
    } else {
        double radius = std::hypot(frame.width * .5, frame.height * .5, frame.depth);
        require(std::isfinite(radius), "Surface decal dimensions exceed world bounds");
        result = surface.querySphere(frame.center, radius,
                                     options.targetMaterials.empty() ? options.material : -1);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    for (uint32_t primitive : result)
        if (primitive >= surface.triangleCount())
            throw std::runtime_error("Surface decal candidate is outside the current posed surface");
    return result;
}
const SurfaceDecalPatchTriangle* patchTriangle(const SurfaceDecalOptions& options, uint32_t primitive) {
    if (!options.conformPatch)
        return nullptr;
    const auto& triangles = options.conformPatch->triangles;
    const auto found = std::lower_bound(triangles.begin(), triangles.end(), primitive,
                                        [](const auto& triangle, uint32_t id) { return triangle.primitive < id; });
    return found != triangles.end() && found->primitive == primitive ? &*found : nullptr;
}
double rayFront(const PaintSurface& surface, const SurfaceDecalFrame& frame) {
    const auto [lo, hi] = surface.bounds();
    V3 corner;
    for (int axis = 0; axis < 3; ++axis)
        corner[axis] = frame.normal[axis] >= 0 ? hi[axis] : lo[axis];
    double front = std::max(frame.depth, (corner - frame.center).dot(frame.normal));
    return front + std::max(1e-5, std::abs(front) * 1e-5);
}
} // namespace

SurfaceDecalFrame surfaceDecalFrame(const SurfaceDecalOptions& options) {
    require(options.center.allFinite() && options.normal.allFinite() && options.tangent.allFinite(),
            "Surface decal placement contains non-finite vectors");
    require(std::isfinite(options.width) && options.width > 1e-8 && std::isfinite(options.height) &&
                options.height > 1e-8 && std::isfinite(options.depth) && options.depth > 1e-8 &&
                std::isfinite(options.rotationRadians),
            "Surface decal width, height and depth must be positive");
    const double length = options.normal.norm();
    require(std::isfinite(length) && length > 1e-8, "Surface decal requires a nonzero outward normal");
    V3 normal = options.normal / length;
    V3 right = options.tangent - normal * options.tangent.dot(normal);
    double tangentLength = right.norm();
    require(std::isfinite(tangentLength) && tangentLength > 1e-8,
            "Surface decal tangent cannot be parallel to its normal");
    right /= tangentLength;
    V3 up = normal.cross(right);
    double angle = std::remainder(options.rotationRadians, 6.2831853071795864769);
    double cosine = std::cos(angle), sine = std::sin(angle);
    return {options.center,
            V3(right * cosine + up * sine),
            V3(up * cosine - right * sine),
            normal,
            options.width,
            options.height,
            options.depth};
}

PaintMappingCandidates findDecalMaterials(const PaintSurface& surface, const SurfaceDecalOptions& options,
                                          Progress progress, const std::atomic_bool* cancel) {
    auto started = Clock::now();
    validateSelection(options);
    const auto frame = surfaceDecalFrame(options);
    validateSurfaceDecalPatch(surface, options);
    checkWork(started, options.limits, cancel);
    PaintMappingCandidates result;
    if (options.opacity > 0) {
        auto primitives = candidatePrimitives(surface, options, frame);
        for (uint32_t primitive : primitives) {
            if ((result.examinedTriangles & 1023) == 0) {
                checkWork(started, options.limits, cancel);
                if (progress)
                    progress("识别表面贴花覆盖区域 · " + std::to_string(result.examinedTriangles));
            }
            ++result.examinedTriangles;
            auto triangle = surface.triangle(primitive);
            const auto chart = patchTriangle(options, primitive);
            if (options.conformPatch && !chart)
                continue;
            if (!paint_mapping::targetTriangle(triangle, options.material, options.mesh,
                                               options.targetMaterials) ||
                !clippedFootprint(triangle, frame, options.frontFacesOnly, chart, options.projectionEye))
                continue;
            ++result.candidateTriangles;
            ++result.trianglesByMaterial[triangle.material];
            result.primitivesByMaterial[triangle.material].push_back(primitive);
        }
    }
    checkWork(started, options.limits, cancel);
    for (const auto& [material, count] : result.trianglesByMaterial)
        result.materials.push_back(material);
    result.seconds = seconds(started);
    return result;
}

PaintMappingReport applySurfaceDecal(const PaintSurface& surface, PaintCanvas& canvas,
                                     const PaintImage& image, const SurfaceDecalOptions& options,
                                     Progress progress, const std::atomic_bool* cancel) {
    using namespace paint_mapping;
    validateSelection(options);
    // A decal's -1 selection intentionally means every intersecting material on this canvas. The UI
    // normally supplies one canonical texture's targetMaterials union from automatic discovery.
    validate(canvas, image, std::max(0, options.material), options.mesh, options.opacity, options.limits,
             options.externalStroke, options.targetMaterials);
    const auto frame = surfaceDecalFrame(options);
    validateSurfaceDecalPatch(surface, options);
    Operation operation(canvas, "表面贴花", options.limits, std::move(progress), cancel,
                        options.externalStroke);
    if (options.opacity <= 0)
        return operation.finish();
    auto primitives = candidatePrimitives(surface, options, frame);
    struct Candidate {
        uint32_t primitive;
        DecalFootprint footprint;
    };
    std::vector<Candidate> selected;
    for (uint32_t primitive : primitives) {
        operation.check();
        auto triangle = surface.triangle(primitive);
        const auto chart = patchTriangle(options, primitive);
        if (options.conformPatch && !chart)
            continue;
        if (!targetTriangle(triangle, options.material, options.mesh, options.targetMaterials))
            continue;
        if (auto footprint = clippedFootprint(triangle, frame, options.frontFacesOnly, chart,
                                              options.projectionEye))
            selected.push_back({primitive, *footprint});
    }
    // Shared UVs cannot carry different artwork. Process the outward layer first deterministically;
    // exact per-texel parallel rays still reject hidden faces before consuming a shared texel.
    std::sort(selected.begin(), selected.end(), [](const Candidate& a, const Candidate& b) {
        return a.footprint.depth != b.footprint.depth ? a.footprint.depth > b.footprint.depth
                                                      : a.primitive < b.primitive;
    });
    const double front = options.occlusion && !options.conformPatch && !selected.empty()
                             ? rayFront(surface, frame) : 0;
    for (const auto& candidate : selected) {
        operation.check();
        const auto triangle = surface.triangle(candidate.primitive);
        const auto chart = patchTriangle(options, candidate.primitive);
        ++operation.report.triangles;
        PaintRasterOptions raster;
        raster.cancel = cancel;
        raster.maxPixels = options.limits.rasterSamples;
        raster.uvBounds = candidate.footprint.uvBounds;
        rasterizePaintTriangle(
            triangle, canvas.image().width, canvas.image().height,
            [&](int x, int y, const V3& barycentric) {
                operation.sampleVisited();
                const V3 point = position(triangle, barycentric), delta = point - frame.center;
                const Eigen::Vector2d coordinate = chart ?
                    Eigen::Vector2d(chart->coordinates[0] * barycentric.x() +
                                    chart->coordinates[1] * barycentric.y() +
                                    chart->coordinates[2] * barycentric.z()) :
                    Eigen::Vector2d(delta.dot(frame.right), delta.dot(frame.up));
                const double dx = coordinate.x(), dy = coordinate.y(), dz = chart ? 0 : delta.dot(frame.normal);
                if (std::abs(dx) > frame.width * .5 || std::abs(dy) > frame.height * .5 ||
                    std::abs(dz) > frame.depth)
                    return;
                F4 color = sample(image, .5 + dx / frame.width, .5 - dy / frame.height, options.opacity);
                if (color[3] <= 0)
                    return;
                V3 normal = triangle.normals[0] * barycentric.x() + triangle.normals[1] * barycentric.y() +
                            triangle.normals[2] * barycentric.z();
                if (chart)
                    normal *= options.conformPatch->normalSign;
                const V3 towardEye = chart && options.projectionEye ? V3(*options.projectionEye - point)
                                                                   : frame.normal;
                if (options.frontFacesOnly && normal.dot(towardEye) <= 1e-8) {
                    ++operation.report.backfaceSamples;
                    return;
                }
                if (options.occlusion) {
                    const double distance = chart ? towardEye.norm() : front - dz;
                    operation.rayTest();
                    const V3 origin = chart ? *options.projectionEye : V3(point + frame.normal * distance);
                    const V3 direction = chart ? V3(-towardEye / std::max(distance, 1e-20)) : V3(-frame.normal);
                    if (distance <= 1e-12 || surface.raycast(origin, direction, -1,
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
