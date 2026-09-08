#include "paint_auto_mapping.h"

namespace edm {
namespace {
constexpr double pi = 3.14159265358979323846;

void validateSelection(int material, int mesh, float opacity, const PaintMappingLimits& limits,
                       std::span<const int> targetMaterials = {}) {
    require(material >= -1 && mesh >= -1, "Invalid automatic mapping selection");
    require(std::isfinite(opacity) && opacity >= 0 && opacity <= 1, "Invalid mapping opacity");
    require(limits.rasterSamples && limits.rayTests && std::isfinite(limits.seconds) && limits.seconds > 0,
            "Invalid image mapping budget");
    require(std::all_of(targetMaterials.begin(), targetMaterials.end(), [](int value) { return value >= 0; }),
            "Invalid shared texture material selection");
}

// At most 3+11 vertices survive our eleven convex clipping planes. Fixed storage avoids a heap
// allocation for each of the millions of triangles in an aircraft. Coplanar boundaries are kept.
template <class Point> struct Polygon {
    std::array<Point, 20> vertices;
    size_t size = 0;
    template <class Distance> void clip(Distance distance) {
        if (!size)
            return;
        Polygon result;
        auto append = [&](const Point& value) {
            if (result.size && (value - result.vertices[result.size - 1]).squaredNorm() == 0)
                return;
            if (result.size == result.vertices.size())
                throw std::runtime_error("Image footprint clipping capacity exceeded");
            result.vertices[result.size++] = value;
        };
        Point previous = vertices[size - 1];
        double previousDistance = distance(previous);
        for (size_t i = 0; i < size; ++i) {
            const Point& current = vertices[i];
            double currentDistance = distance(current);
            bool previousInside = previousDistance >= 0, currentInside = currentDistance >= 0;
            if (previousInside != currentInside) {
                double t = previousDistance / (previousDistance - currentDistance);
                append(previous + (current - previous) * t);
            }
            if (currentInside)
                append(current);
            previous = current;
            previousDistance = currentDistance;
        }
        if (result.size > 1 && (result.vertices[0] - result.vertices[result.size - 1]).squaredNorm() == 0)
            --result.size;
        *this = std::move(result);
    }
};

bool paintable(const PaintTriangle& triangle, const Scene& scene, int material, int mesh,
               std::span<const int> targetMaterials) {
    if (triangle.material < 0 || size_t(triangle.material) >= scene.materials.size() ||
        !paint_mapping::targetTriangle(triangle, material, mesh, targetMaterials))
        return false;
    for (auto& uv : triangle.uv)
        if (!std::isfinite(uv[0]) || !std::isfinite(uv[1]))
            return false;
    double area =
        (double(triangle.uv[1][0]) - triangle.uv[0][0]) * (double(triangle.uv[2][1]) - triangle.uv[0][1]) -
        (double(triangle.uv[1][1]) - triangle.uv[0][1]) * (double(triangle.uv[2][0]) - triangle.uv[0][0]);
    return std::abs(area) >= 1e-20;
}

// Dot(interpolated normal, interpolated direction) is quadratic in barycentrics.
// Checking only the three matching vertex pairs can discard an interior front-facing region.
// All nine coefficients non-positive is a safe bound for the complete triangle.
bool possiblyFacing(const PaintTriangle& triangle, const std::array<V3, 3>& directions) {
    for (const auto& normal : triangle.normals)
        for (const auto& direction : directions)
            if (normal.dot(direction) > 0)
                return true;
    return false;
}

template <class Coverage>
PaintMappingCandidates discover(const PaintSurface& surface, int material, int mesh, float opacity,
                                std::span<const int> targetMaterials, const PaintMappingLimits& limits,
                                Coverage coverage, Progress progress, const std::atomic_bool* cancel) {
    auto started = Clock::now();
    PaintMappingCandidates result;
    auto check = [&] {
        if (cancel && cancel->load(std::memory_order_relaxed))
            throw std::runtime_error("Automatic image mapping cancelled");
        if (edm::seconds(started) > limits.seconds)
            throw std::runtime_error("Automatic image mapping time budget exceeded");
    };
    check();
    if (opacity > 0)
        for (size_t i = 0; i < surface.triangleCount(); ++i) {
            if ((i & 1023) == 0) {
                check();
                if (progress && (i & 65535) == 0)
                    progress("自动识别贴图 " + std::to_string(i) + "/" +
                             std::to_string(surface.triangleCount()));
            }
            ++result.examinedTriangles;
            auto triangle = surface.triangle(uint32_t(i));
            if (paintable(triangle, surface.scene(), material, mesh, targetMaterials) && coverage(triangle)) {
                ++result.candidateTriangles;
                ++result.trianglesByMaterial[triangle.material];
                result.primitivesByMaterial[triangle.material].push_back(uint32_t(i));
            }
        }
    check();
    result.materials.reserve(result.trianglesByMaterial.size());
    for (auto [index, count] : result.trianglesByMaterial)
        result.materials.push_back(index);
    result.seconds = edm::seconds(started);
    return result;
}
} // namespace

PaintMappingCandidates findProjectionMaterials(const PaintSurface& surface,
                                               const CameraProjectionOptions& options, Progress progress,
                                               const std::atomic_bool* cancel) {
    validateSelection(options.material, options.mesh, options.opacity, options.limits,
                      options.targetMaterials);
    require(options.viewProjection.allFinite() && options.eye.allFinite(), "Invalid projection camera");
    require(std::isfinite(options.center[0]) && std::isfinite(options.center[1]) &&
                std::isfinite(options.size[0]) && std::isfinite(options.size[1]) && options.size[0] > 1e-8 &&
                options.size[1] > 1e-8 && std::isfinite(options.viewportAspect) &&
                options.viewportAspect > 1e-8 && std::isfinite(options.rotationRadians),
            "Invalid projected image placement");
    double cosine = std::cos(options.rotationRadians), sine = std::sin(options.rotationRadians);
    V4 horizontal(.5 * options.viewportAspect, 0, 0, (.5 - options.center[0]) * options.viewportAspect);
    V4 vertical(0, -.5, 0, .5 - options.center[1]);
    V4 rotatedX = cosine * horizontal + sine * vertical;
    V4 rotatedY = -sine * horizontal + cosine * vertical;
    V4 halfWidth(0, 0, 0, .5 * double(options.size[0]) * options.viewportAspect);
    V4 halfHeight(0, 0, 0, .5 * double(options.size[1]));
    std::array<V4, 10> planes{V4(1, 0, 0, 1),       V4(-1, 0, 0, 1),      V4(0, 1, 0, 1),
                              V4(0, -1, 0, 1),      V4(0, 0, 1, 0),       V4(0, 0, -1, 1),
                              halfWidth + rotatedX, halfWidth - rotatedX, halfHeight + rotatedY,
                              halfHeight - rotatedY};
    return discover(
        surface, options.material, options.mesh, options.opacity, options.targetMaterials, options.limits,
        [&](const PaintTriangle& triangle) {
            if (options.frontFacesOnly &&
                !possiblyFacing(triangle, {options.eye - triangle.world[0], options.eye - triangle.world[1],
                                           options.eye - triangle.world[2]}))
                return false;
            Polygon<V4> polygon;
            for (const auto& point : triangle.world) {
                V4 clip = options.viewProjection * V4(point.x(), point.y(), point.z(), 1);
                if (!clip.allFinite())
                    throw std::runtime_error("Non-finite projected model coordinates");
                polygon.vertices[polygon.size++] = clip;
            }
            // A tolerance keeps boundary samples conservative under floating-point round-off. Unlike
            // screen-space division, homogeneous clipping also works when an edge crosses the eye.
            polygon.clip([](const V4& p) { return p.w() - 1e-10; });
            for (const auto& plane : planes) {
                polygon.clip(
                    [&](const V4& p) { return plane.dot(p) + 1e-10 * (1 + p.cwiseAbs().maxCoeff()); });
                if (!polygon.size)
                    return false;
            }
            return polygon.size != 0;
        },
        std::move(progress), cancel);
}

PaintMappingCandidates findWrapMaterials(const PaintSurface& surface, const CylinderWrapOptions& options,
                                         Progress progress, const std::atomic_bool* cancel) {
    validateSelection(options.material, options.mesh, options.opacity, options.limits,
                      options.targetMaterials);
    require(options.center.allFinite() && options.axis.allFinite() && options.radialReference.allFinite(),
            "Invalid cylinder frame");
    require(options.axis.norm() > 1e-8, "Cylinder axis cannot be zero");
    require(std::isfinite(options.height) && options.height > 1e-8 && std::isfinite(options.sweepRadians) &&
                options.sweepRadians > 1e-8 && options.sweepRadians <= 2 * pi + 1e-10 &&
                std::isfinite(options.angleRadians),
            "Cylinder height and sweep must be positive; sweep cannot exceed 360 degrees");
    V3 axis = options.axis.normalized();
    V3 reference = options.radialReference - axis * axis.dot(options.radialReference);
    require(reference.norm() > 1e-8, "Cylinder radial reference cannot be parallel to its axis");
    reference.normalize();
    V3 tangent = axis.cross(reference);
    double angle = std::remainder(options.angleRadians, 2 * pi);
    // Sectors larger than 180 degrees are non-convex. Split into two convex wedges instead of
    // intersecting half-planes (which would silently keep the complementary narrow sector).
    bool split = options.sweepRadians > pi;
    double halfSweep = options.sweepRadians / (split ? 4 : 2);
    std::array<double, 2> centers{angle - (split ? halfSweep : 0), angle + halfSweep};
    std::array<std::array<V3, 2>, 2> wedgePlanes;
    for (int i = 0; i < (split ? 2 : 1); ++i) {
        V3 x(0, std::cos(centers[i]), std::sin(centers[i]));
        V3 y(0, -std::sin(centers[i]), std::cos(centers[i]));
        wedgePlanes[i] = {std::sin(halfSweep) * x - std::cos(halfSweep) * y,
                          std::sin(halfSweep) * x + std::cos(halfSweep) * y};
    }
    return discover(
        surface, options.material, options.mesh, options.opacity, options.targetMaterials, options.limits,
        [&](const PaintTriangle& triangle) {
            Polygon<V3> polygon;
            std::array<V3, 3> radial;
            for (size_t i = 0; i < 3; ++i) {
                V3 delta = triangle.world[i] - options.center;
                double height = delta.dot(axis);
                radial[i] = delta - axis * height;
                V3 point(height, delta.dot(reference), delta.dot(tangent));
                if (!point.allFinite())
                    throw std::runtime_error("Non-finite cylinder model coordinates");
                polygon.vertices[polygon.size++] = point;
            }
            if (options.outwardOnly && !possiblyFacing(triangle, radial))
                return false;
            double tolerance = 1e-10 * (1 + options.height);
            polygon.clip([&](const V3& p) { return options.height * .5 + p.x() + tolerance; });
            polygon.clip([&](const V3& p) { return options.height * .5 - p.x() + tolerance; });
            if (!polygon.size || options.sweepRadians >= 2 * pi - 1e-10)
                return polygon.size != 0;
            for (int wedge = 0; wedge < (split ? 2 : 1); ++wedge) {
                auto sector = polygon;
                for (const auto& plane : wedgePlanes[wedge])
                    sector.clip(
                        [&](const V3& p) { return plane.dot(p) + 1e-9 * (1 + p.cwiseAbs().maxCoeff()); });
                if (sector.size)
                    return true;
            }
            return false;
        },
        std::move(progress), cancel);
}

PaintMappingBudget::PaintMappingBudget(PaintMappingLimits limits) : limits(limits) {
    validateSelection(-1, -1, 1, limits);
}

PaintMappingLimits PaintMappingBudget::remaining() const {
    double timeLeft = limits.seconds - elapsed();
    require(timeLeft > 0 && pixels < limits.rasterSamples && rays < limits.rayTests,
            "Automatic image mapping shared budget exhausted; reduce image size or footprint");
    return {limits.rasterSamples - pixels, limits.rayTests - rays, timeLeft};
}

void PaintMappingBudget::account(const PaintMappingReport& report) {
    require(report.rasterSamples <= limits.rasterSamples - pixels &&
                report.rayTests <= limits.rayTests - rays && elapsed() <= limits.seconds,
            "Automatic image mapping shared budget exceeded; cancel all touched canvases");
    pixels += report.rasterSamples;
    rays += report.rayTests;
}
} // namespace edm
