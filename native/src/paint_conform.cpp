#include "paint_conform.h"
#include <queue>

namespace edm {
namespace {
using V2 = Eigen::Vector2d;
constexpr size_t topologyLimit = 200000;
constexpr double pi = 3.14159265358979323846;
double cross2(const V2& a, const V2& b) { return a.x() * b.y() - a.y() * b.x(); }
void checkConform(Clock::time_point started, const SurfaceDecalOptions& options,
                  const std::atomic_bool* cancel) {
    if (cancel && cancel->load(std::memory_order_relaxed))
        throw std::runtime_error("Surface conforming decal cancelled");
    require(seconds(started) <= options.limits.seconds,
            "Surface conforming decal time budget exceeded; reduce decal size");
}
struct Cell {
    int64_t x, y, z;
    int owner;
    bool operator==(const Cell&) const = default;
};
struct CellHash {
    size_t operator()(const Cell& c) const {
        size_t h = std::hash<int64_t>{}(c.x);
        h ^= std::hash<int64_t>{}(c.y) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int64_t>{}(c.z) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(c.owner) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};
struct Vertex {
    V3 position;
    V2 coordinate = V2::Zero();
    bool mapped = false;
};
struct Face {
    SurfaceDecalPatchTriangle patch;
    std::array<uint32_t, 3> vertices{};
    std::array<int, 3> neighbors{-1, -1, -1};
    std::array<int, 3> neighborEdges{-1, -1, -1};
    bool mapped = false;
};
struct EdgeRef { uint32_t face; int edge; };
struct GapLink {
    EdgeRef a, b;
    // Corresponding edge parameters at the overlap midpoint, plus its physical strip width.
    double aMid = 0, bMid = 0, distance = 0;
};
struct Edge {
    EdgeRef first{}, second{};
    unsigned count = 0;
};
struct NativeEdge {
    uint32_t mesh, a, b;
    bool operator==(const NativeEdge&) const = default;
};
struct NativeEdgeHash {
    size_t operator()(const NativeEdge& edge) const {
        size_t h = edge.mesh;
        h ^= edge.a + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= edge.b + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};
bool intersects(const std::array<V2, 3>& xy, double radius) {
    // Euclidean chart distance is a lower bound for propagation. A triangle crossing the
    // disc must be kept even if none of its vertices is inside (e.g. a large seed triangle).
    double sign = 0;
    bool contains = true;
    for (int k = 0; k < 3; ++k) {
        const V2 a = xy[k], b = xy[(k + 1) % 3], edge = b - a;
        double t = std::clamp(-a.dot(edge) / std::max(1e-30, edge.squaredNorm()), 0., 1.);
        if ((a + edge * t).squaredNorm() <= radius * radius)
            return true;
        const double side = cross2(edge, -a);
        if (sign && side * sign < 0)
            contains = false;
        if (side)
            sign = side;
    }
    return contains;
}
std::array<V2, 3> unfold(const Face& from, int edge, const Face& to, int toEdge) {
    const int a = edge, b = (edge + 1) % 3, c = (edge + 2) % 3;
    int ta = toEdge, tb = (toEdge + 1) % 3, tc = (toEdge + 2) % 3;
    if (from.vertices[a] != to.vertices[ta])
        std::swap(ta, tb);
    const V2 origin = from.patch.coordinates[a];
    const V2 along = from.patch.coordinates[b] - origin;
    const double planarLength = along.norm();
    require(planarLength > 1e-14, "Surface decal chart contains a degenerate edge");
    const double worldLength = (to.patch.triangle.world[tb] - to.patch.triangle.world[ta]).norm();
    const double ac = (to.patch.triangle.world[tc] - to.patch.triangle.world[ta]).norm();
    const double bc = (to.patch.triangle.world[tc] - to.patch.triangle.world[tb]).norm();
    const double x = (ac * ac - bc * bc + worldLength * worldLength) / (2 * worldLength);
    const double y = std::sqrt(std::max(0., ac * ac - x * x));
    const V2 direction = along / planarLength, perpendicular(-direction.y(), direction.x());
    const double side = cross2(along, from.patch.coordinates[c] - origin) >= 0 ? -1. : 1.;
    std::array<V2, 3> result;
    result[ta] = origin;
    result[tb] = from.patch.coordinates[b];
    // An already reconciled edge can have a small distortion. Preserve its endpoint positions;
    // third-vertex distances are checked below before making the chart visible.
    result[tc] = origin + direction * x + perpendicular * (side * y);
    return result;
}

// A gap adds distance rather than welding the two boundaries together. The target face remains
// isometric; only the absent strip is crossed. The same chart subsequently drives preview and bake.
std::array<V2, 3> unfoldGap(const Face& from, int edge, double fromMid, const Face& to,
                           int toEdge, double toMid, double distance) {
    const int a = edge, b = (edge + 1) % 3, c = (edge + 2) % 3;
    const int ta = toEdge, tb = (toEdge + 1) % 3, tc = (toEdge + 2) % 3;
    const V2 along = from.patch.coordinates[b] - from.patch.coordinates[a];
    const V2 direction = along.normalized(), perpendicular(-direction.y(), direction.x());
    const double side = cross2(along, from.patch.coordinates[c] - from.patch.coordinates[a]) >= 0 ? -1. : 1.;
    const V3 worldAlong = (from.patch.triangle.world[b] - from.patch.triangle.world[a]).normalized();
    V3 worldInside = from.patch.triangle.world[c] - from.patch.triangle.world[a];
    worldInside -= worldAlong * worldInside.dot(worldAlong);
    worldInside.normalize();
    const V3 targetEdge = to.patch.triangle.world[tb] - to.patch.triangle.world[ta];
    const double length = targetEdge.norm();
    const V3 third = to.patch.triangle.world[tc] - to.patch.triangle.world[ta];
    V3 targetInside = third - targetEdge * (third.dot(targetEdge) / targetEdge.squaredNorm());
    targetInside.normalize();
    const V3 worldNormal = worldAlong.cross(worldInside).normalized();
    V3 targetNormal = targetEdge.normalized().cross(targetInside).normalized();
    if (targetNormal.dot(worldNormal) < 0)
        targetNormal = -targetNormal;
    const Eigen::Quaterniond rotation = Eigen::Quaterniond::FromTwoVectors(targetNormal, worldNormal);
    auto mappedDirection = [&](const V3& vector) -> V2 {
        const V3 moved = rotation * vector;
        return direction * moved.dot(worldAlong) - perpendicular * (side * moved.dot(worldInside));
    };
    const V2 targetAlong = mappedDirection(targetEdge / length);
    const V2 targetInward = mappedDirection(targetInside);
    const V2 middle = from.patch.coordinates[a] + along * fromMid + perpendicular * (side * distance);
    const double x = third.dot(targetEdge) / length;
    const double y = std::sqrt(std::max(0., third.squaredNorm() - x * x));
    std::array<V2, 3> result;
    result[ta] = middle - targetAlong * (length * toMid);
    result[tb] = result[ta] + targetAlong * length;
    result[tc] = result[ta] + targetAlong * x + targetInward * y;
    return result;
}

std::vector<GapLink> findGapLinks(const std::vector<Face>& faces,
                                 const std::unordered_map<uint64_t, Edge>& exactEdges,
                                 const PaintSurface& surface, const SurfaceDecalOptions& options,
                                 double seamTolerance, double maxBendDegrees, Clock::time_point started,
                                 const std::atomic_bool* cancel, SurfaceDecalPatch& result) {
    struct Boundary {
        EdgeRef ref;
        V3 low, high;
        int owner;
    };
    std::vector<Boundary> boundaries;
    size_t boundaryChecks = 0;
    for (const auto& [key, edge] : exactEdges) {
        if ((boundaryChecks++ & 1023) == 0)
            checkConform(started, options, cancel);
        // Exact seams (including rejected or nonmanifold ones) are never reconsidered as gaps.
        if (edge.count != 1)
            continue;
        const auto& triangle = faces[edge.first.face].patch.triangle;
        const auto& mesh = surface.scene().meshes[triangle.mesh];
        const auto& a = triangle.world[edge.first.edge];
        const auto& b = triangle.world[(edge.first.edge + 1) % 3];
        boundaries.push_back({edge.first, a.cwiseMin(b), a.cwiseMax(b),
                             mesh.extras.is_object() ? mesh.extras.value("edm_attachment", -1) : -1});
    }
    std::sort(boundaries.begin(), boundaries.end(), [](const auto& a, const auto& b) {
        return std::pair{a.ref.face, a.ref.edge} < std::pair{b.ref.face, b.ref.edge};
    });
    if (boundaries.empty())
        return {};
    // Long boundaries are indexed as segments, not sampled into arbitrarily many tiny grid cells.
    struct Node { V3 low, high; uint32_t start = 0, count = 0, left = 0, right = 0; };
    std::vector<uint32_t> order(boundaries.size());
    std::iota(order.begin(), order.end(), 0);
    std::vector<Node> nodes;
    nodes.reserve(boundaries.size() * 2);
    auto build = [&](auto&& self, uint32_t start, uint32_t count) -> uint32_t {
        checkConform(started, options, cancel);
        const uint32_t index = uint32_t(nodes.size());
        Node node{boundaries[order[start]].low, boundaries[order[start]].high, start, count};
        for (uint32_t k = 1; k < count; ++k) {
            node.low = node.low.cwiseMin(boundaries[order[start + k]].low);
            node.high = node.high.cwiseMax(boundaries[order[start + k]].high);
        }
        nodes.push_back(node);
        if (count > 8) {
            Eigen::Index axis;
            (node.high - node.low).maxCoeff(&axis);
            const uint32_t middle = start + count / 2;
            std::nth_element(order.begin() + start, order.begin() + middle, order.begin() + start + count,
                             [&](uint32_t a, uint32_t b) {
                const double x = boundaries[a].low[axis] + boundaries[a].high[axis];
                const double y = boundaries[b].low[axis] + boundaries[b].high[axis];
                return x == y ? a < b : x < y;
            });
            nodes[index].left = self(self, start, middle - start);
            nodes[index].right = self(self, middle, start + count - middle);
            nodes[index].count = 0;
        }
        return index;
    };
    build(build, 0, uint32_t(order.size()));
    struct Candidate {
        GapLink link;
        uint32_t a, b;
        double aLow, aHigh, bLow, bHigh;
        bool ambiguous = false;
    };
    std::vector<Candidate> candidates;
    std::vector<std::vector<uint32_t>> incident(boundaries.size());
    uint64_t examined = 0;
    const double creaseCosine = std::cos(std::min(maxBendDegrees, 30.) * pi / 180.);
    const double parallelCosine = std::cos(2. * pi / 180.);
    const double epsilon = seamTolerance * 4;
    // The existing coincidence tolerance scales with decal size; do not let a large decal's
    // tolerance hide a real normal step or enlarge the user's much smaller physical gap setting.
    const double numericTolerance = std::max(1e-8, std::min(epsilon, options.gapDistance * .001));
    const double distanceTolerance = std::max(1e-9, options.gapDistance * 1e-5);
    auto test = [&](uint32_t ai, uint32_t bi) {
        if ((examined++ & 1023) == 0) {
            checkConform(started, options, cancel);
            require(examined <= topologyLimit * 32,
                    "Surface decal gap matching exceeds its work budget; reduce decal size or gap distance");
        }
        const auto& ba = boundaries[ai];
        const auto& bb = boundaries[bi];
        if (ba.owner != bb.owner || ba.ref.face == bb.ref.face)
            return;
        const auto& a = faces[ba.ref.face].patch.triangle;
        const auto& b = faces[bb.ref.face].patch.triangle;
        const V3 a0 = a.world[ba.ref.edge], a1 = a.world[(ba.ref.edge + 1) % 3];
        const V3 b0 = b.world[bb.ref.edge], b1 = b.world[(bb.ref.edge + 1) % 3];
        const V3 u = (a1 - a0).normalized(), v = (b1 - b0).normalized();
        const double lengthA = (a1 - a0).norm(), lengthB = (b1 - b0).norm();
        const double parallel = u.dot(v);
        if (std::abs(parallel) < parallelCosine || a.normal().dot(b.normal()) < creaseCosine)
            return;
        V3 insideA = a.world[(ba.ref.edge + 2) % 3] - a0;
        V3 insideB = b.world[(bb.ref.edge + 2) % 3] - b0;
        insideA -= u * insideA.dot(u);
        insideB -= v * insideB.dot(v);
        insideA.normalize();
        insideB.normalize();
        if (insideA.dot(insideB) > -creaseCosine)
            return; // Overlapping sheets point into the same half-plane, unlike facing boundaries.
        const double projectedB0 = (b0 - a0).dot(u), projectedB1 = (b1 - a0).dot(u);
        const double low = std::max(0., std::min(projectedB0, projectedB1));
        const double high = std::min(lengthA, std::max(projectedB0, projectedB1));
        if (high - low <= std::max(epsilon, std::min(lengthA, lengthB) * .02))
            return; // A corner touch or tiny incidental overlap is not a seam.
        const V3 normalA = u.cross(insideA).normalized();
        V3 normalB = v.cross(insideB).normalized();
        if (normalA.dot(normalB) < 0)
            normalB = -normalB;
        const V3 averageNormal = (normalA + normalB).normalized();
        auto targetParameter = [&](double x) { return (x - projectedB0) / (projectedB1 - projectedB0); };
        double distance = 0;
        for (double x : {low, (low + high) * .5, high}) {
            const V3 delta = b0 + (b1 - b0) * targetParameter(x) - (a0 + u * x);
            const double gap = delta.norm();
            if (!std::isfinite(gap) || gap > options.gapDistance + distanceTolerance)
                return;
            if (gap > numericTolerance) {
                // A missing strip lies in the tangent plane and leads out of A into B. A normal
                // step to a stacked skin, cockpit surface or wing underside must not become a seam.
                if (delta.dot(insideA) > -gap * .8 || delta.dot(insideB) < gap * .8 ||
                    std::abs(delta.dot(averageNormal)) > std::max(numericTolerance, gap * .05))
                    return;
            }
            if (x == (low + high) * .5)
                distance = gap;
        }
        require(candidates.size() < topologyLimit * 3,
                "Surface decal gap matching has too many candidate seams; reduce its size");
        const uint32_t index = uint32_t(candidates.size());
        const double p0 = targetParameter(low), p1 = targetParameter(high);
        candidates.push_back({{ba.ref, bb.ref, (low + high) * .5 / lengthA,
                               targetParameter((low + high) * .5), distance}, ai, bi,
                              low / lengthA, high / lengthA, std::min(p0, p1), std::max(p0, p1)});
        incident[ai].push_back(index);
        incident[bi].push_back(index);
    };
    for (uint32_t ai = 0; ai < boundaries.size(); ++ai) {
        if ((ai & 255) == 0)
            checkConform(started, options, cancel);
        const V3 low = boundaries[ai].low.array() - (options.gapDistance + seamTolerance);
        const V3 high = boundaries[ai].high.array() + (options.gapDistance + seamTolerance);
        auto query = [&](auto&& self, uint32_t index) -> void {
            const auto& node = nodes[index];
            if ((node.high.array() < low.array()).any() || (node.low.array() > high.array()).any())
                return;
            if (node.count) {
                for (uint32_t k = 0; k < node.count; ++k) {
                    const uint32_t bi = order[node.start + k];
                    if (bi > ai)
                        test(ai, bi);
                }
            } else {
                self(self, node.left);
                self(self, node.right);
            }
        };
        query(query, 0);
    }
    // Different subdivisions may occupy disjoint portions of a long edge. Overlapping choices
    // are ambiguous: stop every conflicting link instead of selecting whichever shell is nearest.
    for (uint32_t edge = 0; edge < incident.size(); ++edge) {
        if ((edge & 1023) == 0)
            checkConform(started, options, cancel);
        auto& list = incident[edge];
        if (list.size() > 64) {
            for (uint32_t id : list)
                candidates[id].ambiguous = true;
            continue;
        }
        auto interval = [&](uint32_t id) {
            const auto& candidate = candidates[id];
            return candidate.a == edge ? std::pair{candidate.aLow, candidate.aHigh} :
                                         std::pair{candidate.bLow, candidate.bHigh};
        };
        const auto& boundary = boundaries[edge];
        const auto& triangle = faces[boundary.ref.face].patch.triangle;
        const double length = (triangle.world[(boundary.ref.edge + 1) % 3] - triangle.world[boundary.ref.edge]).norm();
        for (size_t i = 0; i < list.size(); ++i)
            for (size_t j = i + 1; j < list.size(); ++j) {
                const auto a = interval(list[i]), b = interval(list[j]);
                if ((std::min(a.second, b.second) - std::max(a.first, b.first)) * length > epsilon) {
                    candidates[list[i]].ambiguous = true;
                    candidates[list[j]].ambiguous = true;
                }
            }
    }
    std::vector<GapLink> links;
    for (const auto& candidate : candidates)
        if (!candidate.ambiguous)
            links.push_back(candidate.link);
        else
            ++result.blockedEdges;
    return links;
}
} // namespace

void validateSurfaceDecalPatch(const PaintSurface& surface, const SurfaceDecalOptions& options) {
    if (!options.conformPatch)
        return;
    const auto& patch = *options.conformPatch;
    const auto frame = surfaceDecalFrame(options);
    require(patch.sourceSurface == &surface && patch.sourceGeneration != 0 &&
                patch.sourceGeneration == surface.generation() &&
                patch.sourceScene == &surface.scene() &&
                patch.sourceTriangleCount == surface.triangleCount(),
            "Surface conforming decal belongs to a different posed surface; rebuild its preview");
    const auto seed = std::lower_bound(patch.triangles.begin(), patch.triangles.end(), patch.seedPrimitive,
        [](const auto& triangle, uint32_t id) { return triangle.primitive < id; });
    require(seed != patch.triangles.end() && seed->primitive == patch.seedPrimitive &&
                patch.seedPrimitive < surface.triangleCount(), "Invalid surface conforming decal seed chart");
    const auto actualSeed = surface.triangle(patch.seedPrimitive);
    require(actualSeed.mesh == seed->triangle.mesh && actualSeed.index == seed->triangle.index &&
                actualSeed.material == seed->triangle.material && actualSeed.hasUV == seed->triangle.hasUV,
            "Surface conforming decal source topology changed; rebuild its preview");
    for (int k = 0; k < 3; ++k)
        require(actualSeed.world[k] == seed->triangle.world[k] &&
                    actualSeed.normals[k] == seed->triangle.normals[k] && actualSeed.uv[k] == seed->triangle.uv[k],
                "Surface conforming decal source pose changed; rebuild its preview");
    auto same = [](const V3& a, const V3& b) { return (a - b).squaredNorm() < 1e-24; };
    require(same(frame.center, patch.frame.center) && same(frame.right, patch.frame.right) &&
                same(frame.up, patch.frame.up) && same(frame.normal, patch.frame.normal) &&
                frame.width == patch.frame.width && frame.height == patch.frame.height &&
                frame.depth == patch.frame.depth && options.gapDistance == patch.gapDistance,
            "Surface conforming decal placement changed; rebuild its preview");
    require(options.projectionEye.has_value() == patch.projectionEye.has_value() &&
                (!options.projectionEye || same(*options.projectionEye, *patch.projectionEye)),
            "Surface conforming decal camera changed; rebuild its preview");
    require(!(options.frontFacesOnly || options.occlusion) || patch.projectionEye.has_value(),
            "Surface conforming decal visibility requires its placement camera");
}

std::shared_ptr<const SurfaceDecalPatch> buildSurfaceDecalPatch(
    const PaintSurface& surface, const PaintHit& hit, const SurfaceDecalOptions& options,
    double maxBendDegrees, Progress progress, const std::atomic_bool* cancel) {
    const auto started = Clock::now();
    const auto frame = surfaceDecalFrame(options);
    require(std::isfinite(maxBendDegrees) && maxBendDegrees > 0 && maxBendDegrees < 90,
            "Surface conforming decal bend limit must be between zero and 90 degrees");
    require(std::isfinite(options.gapDistance) && options.gapDistance >= 0 && options.gapDistance <= .02,
            "Surface decal gap distance must be between zero and 20 millimetres");
    require(std::isfinite(options.limits.seconds) && options.limits.seconds > 0 &&
                options.limits.rasterSamples && options.limits.rayTests,
            "Invalid surface conforming decal work budget");
    require(!options.projectionEye || options.projectionEye->allFinite(),
            "Invalid surface conforming decal camera");
    require(!(options.frontFacesOnly || options.occlusion) || options.projectionEye.has_value(),
            "Surface conforming decal visibility requires its placement camera");
    checkConform(started, options, cancel);
    require(hit.primitive < surface.triangleCount() && hit.position.allFinite() && hit.normal.allFinite() &&
                hit.barycentric.allFinite(), "Invalid surface conforming decal seed");
    const auto seed = surface.triangle(hit.primitive);
    require(seed.hasUV && seed.mesh == hit.mesh && seed.index == hit.triangle && seed.material == hit.material,
            "Surface conforming decal seed does not match the current posed surface");
    const double radius = std::hypot(frame.width * .5, frame.height * .5);
    require(std::isfinite(radius), "Surface conforming decal dimensions exceed world bounds");
    const double seamTolerance = std::max(1e-7, std::min(1e-4, radius * 1e-5));
    const V3 hitPoint = seed.world[0] * hit.barycentric.x() + seed.world[1] * hit.barycentric.y() +
                        seed.world[2] * hit.barycentric.z();
    require(std::abs(hit.barycentric.sum() - 1) < 1e-5 && hit.barycentric.minCoeff() >= -1e-5 &&
                (hitPoint - hit.position).norm() <= seamTolerance * 4 &&
                (frame.center - hit.position).norm() <= seamTolerance * 4,
            "Surface conforming decal seed or placement is stale");
    auto result = std::make_shared<SurfaceDecalPatch>();
    result->frame = frame;
    result->projectionEye = options.projectionEye;
    result->sourceSurface = &surface;
    result->sourceScene = &surface.scene();
    result->sourceGeneration = surface.generation();
    result->sourceTriangleCount = surface.triangleCount();
    result->seedPrimitive = hit.primitive;
    result->maxBendDegrees = maxBendDegrees;
    result->seamTolerance = seamTolerance;
    result->gapDistance = options.gapDistance;
    const V3 seedNormal = seed.normals[0] * hit.barycentric.x() + seed.normals[1] * hit.barycentric.y() +
                          seed.normals[2] * hit.barycentric.z();
    result->normalSign = seedNormal.dot(frame.normal) < 0 ? -1. : 1.;
    // Chart distance bounds world distance. A small margin tolerates controlled reconciliation
    // on doubly-curved patches; depth deliberately does not slice a conforming decal off its curve.
    auto candidates = surface.querySphere(frame.center, radius * 1.15 + seamTolerance * 4);
    require(candidates.size() <= topologyLimit,
            "Surface conforming decal covers too many triangles; reduce its size");
    std::sort(candidates.begin(), candidates.end());
    result->examinedTriangles = candidates.size();
    std::vector<Vertex> vertices;
    std::vector<Face> faces;
    faces.reserve(candidates.size());
    std::unordered_map<Cell, std::vector<uint32_t>, CellHash> cells;
    std::unordered_map<NativeEdge, Edge, NativeEdgeHash> nativeEdges;
    std::unordered_map<uint64_t, Edge> edges;
    int seedIndex = -1;
    auto vertexIndex = [&](const V3& point, int owner) {
        require(point.allFinite(), "Non-finite surface conforming decal geometry");
        const V3 scaled = point / seamTolerance;
        require(scaled.cwiseAbs().maxCoeff() < 9e18,
                "Surface conforming decal coordinates exceed spatial index bounds");
        Cell cell{int64_t(std::floor(scaled.x())), int64_t(std::floor(scaled.y())),
                  int64_t(std::floor(scaled.z())), owner};
        uint32_t match = UINT32_MAX;
        for (int x = -1; x <= 1; ++x)
            for (int y = -1; y <= 1; ++y)
                for (int z = -1; z <= 1; ++z) {
                    const auto found = cells.find({cell.x + x, cell.y + y, cell.z + z, owner});
                    if (found != cells.end())
                        for (uint32_t index : found->second)
                            if ((vertices[index].position - point).squaredNorm() <=
                                seamTolerance * seamTolerance)
                                match = std::min(match, index);
                }
        if (match != UINT32_MAX)
            return match;
        uint32_t index = uint32_t(vertices.size());
        vertices.push_back({point});
        cells[cell].push_back(index);
        return index;
    };
    for (uint32_t primitive : candidates) {
        if ((faces.size() & 1023) == 0) {
            checkConform(started, options, cancel);
            if (progress)
                progress("连接曲面贴花的网格接缝 · " + std::to_string(faces.size()));
        }
        Face face;
        face.patch.primitive = primitive;
        face.patch.triangle = surface.triangle(primitive);
        const auto& triangle = face.patch.triangle;
        if (!triangle.hasUV)
            continue;
        const V3 ab = triangle.world[1] - triangle.world[0], ac = triangle.world[2] - triangle.world[0];
        const double area = ab.cross(ac).norm(), scale = std::max(ab.squaredNorm(), ac.squaredNorm());
        if (!std::isfinite(area) || !std::isfinite(scale) || area <= std::max(1e-20, scale * 1e-12)) {
            require(primitive != hit.primitive, "Surface conforming decal seed triangle is degenerate");
            continue;
        }
        const auto& mesh = surface.scene().meshes.at(triangle.mesh);
        const int owner = mesh.extras.is_object() ? mesh.extras.value("edm_attachment", -1) : -1;
        for (int k = 0; k < 3; ++k)
            face.vertices[k] = vertexIndex(triangle.world[k], owner);
        if (face.vertices[0] == face.vertices[1] || face.vertices[1] == face.vertices[2] ||
            face.vertices[2] == face.vertices[0])
            continue;
        uint32_t index = uint32_t(faces.size());
        if (primitive == hit.primitive)
            seedIndex = int(index);
        faces.push_back(std::move(face));
        for (int k = 0; k < 3; ++k) {
            uint32_t nativeA = mesh.indices[size_t(triangle.index) * 3 + k];
            uint32_t nativeB = mesh.indices[size_t(triangle.index) * 3 + (k + 1) % 3];
            if (nativeA > nativeB)
                std::swap(nativeA, nativeB);
            auto& edge = nativeEdges[{triangle.mesh, nativeA, nativeB}];
            if (edge.count == 0)
                edge.first = {index, k};
            else if (edge.count == 1)
                edge.second = {index, k};
            ++edge.count;
        }
    }
    require(seedIndex >= 0, "Surface conforming decal seed is too small or outside the surface chart");
    const double maxCosine = std::cos(maxBendDegrees * pi / 180.);
    size_t edgeIndex = 0;
    auto connect = [&](const Edge& edge) {
        const auto a = edge.first, b = edge.second;
        const auto& ta = faces[a.face].patch.triangle;
        const auto& tb = faces[b.face].patch.triangle;
        V3 along = (ta.world[(a.edge + 1) % 3] - ta.world[a.edge]).normalized();
        V3 sideA = ta.world[(a.edge + 2) % 3] - ta.world[a.edge];
        V3 sideB = tb.world[(b.edge + 2) % 3] - ta.world[a.edge];
        sideA -= along * sideA.dot(along);
        sideB -= along * sideB.dot(along);
        // Opposite interior half-planes distinguish a real shared boundary from overlapping
        // duplicate faces. This also provides a winding-independent local dihedral angle.
        if (sideA.norm() < 1e-12 || sideB.norm() < 1e-12 ||
            sideA.normalized().dot(sideB.normalized()) > -maxCosine ||
            ta.normal().dot(tb.normal()) < maxCosine) {
            ++result->blockedEdges;
            return;
        }
        faces[a.face].neighbors[a.edge] = int(b.face);
        faces[a.face].neighborEdges[a.edge] = b.edge;
        faces[b.face].neighbors[b.edge] = int(a.face);
        faces[b.face].neighborEdges[b.edge] = a.edge;
        if (ta.mesh != tb.mesh)
            ++result->stitchedEdges;
    };
    // Native indexed connectivity identifies the original sheet even when an opposite-facing
    // duplicate lies at exactly the same positions. Only genuine native boundary edges need
    // virtual seam stitching; a global position weld would turn both sheets into nonmanifold soup.
    for (const auto& [key, edge] : nativeEdges) {
        if ((edgeIndex++ & 1023) == 0)
            checkConform(started, options, cancel);
        if (edge.count == 2)
            connect(edge);
        else if (edge.count > 2)
            ++result->blockedEdges;
    }
    for (uint32_t index = 0; index < faces.size(); ++index) {
        if ((index & 1023) == 0)
            checkConform(started, options, cancel);
        const auto& face = faces[index];
        for (int k = 0; k < 3; ++k) {
            // Checking the original key also covers every incident face of a nonmanifold edge,
            // beyond the first two refs kept in the bounded edge record.
            const auto& mesh = surface.scene().meshes[face.patch.triangle.mesh];
            uint32_t na = mesh.indices[size_t(face.patch.triangle.index) * 3 + k];
            uint32_t nb = mesh.indices[size_t(face.patch.triangle.index) * 3 + (k + 1) % 3];
            if (na > nb)
                std::swap(na, nb);
            if (nativeEdges.at({face.patch.triangle.mesh, na, nb}).count != 1)
                continue;
            uint32_t a = face.vertices[k], b = face.vertices[(k + 1) % 3];
            if (a > b)
                std::swap(a, b);
            auto& edge = edges[(uint64_t(a) << 32) | b];
            if (edge.count == 0)
                edge.first = {index, k};
            else if (edge.count == 1)
                edge.second = {index, k};
            ++edge.count;
        }
    }
    for (const auto& [key, edge] : edges) {
        if ((edgeIndex++ & 1023) == 0)
            checkConform(started, options, cancel);
        if (edge.count == 2)
            connect(edge);
        else if (edge.count > 2)
            ++result->blockedEdges;
    }
    std::vector<GapLink> gapLinks;
    std::vector<std::vector<uint32_t>> faceGaps;
    if (options.gapDistance > 0) {
        if (progress)
            progress("查找曲面贴花可跨越的微小缝隙");
        gapLinks = findGapLinks(faces, edges, surface, options, seamTolerance, maxBendDegrees,
                               started, cancel, *result);
        faceGaps.resize(faces.size());
        for (uint32_t k = 0; k < gapLinks.size(); ++k) {
            faceGaps[gapLinks[k].a.face].push_back(k);
            faceGaps[gapLinks[k].b.face].push_back(k);
        }
    }
    auto& first = faces[size_t(seedIndex)];
    V3 faceNormal = (seed.world[1] - seed.world[0]).cross(seed.world[2] - seed.world[0]).normalized();
    if (faceNormal.dot(frame.normal) < 0)
        faceNormal = -faceNormal;
    const Eigen::Quaterniond rotation = Eigen::Quaterniond::FromTwoVectors(frame.normal, faceNormal);
    const V3 right = rotation * frame.right, up = rotation * frame.up;
    for (int k = 0; k < 3; ++k) {
        const V3 delta = seed.world[k] - frame.center;
        first.patch.coordinates[k] = V2(delta.dot(right), delta.dot(up));
        auto& vertex = vertices[first.vertices[k]];
        vertex.mapped = true;
        vertex.coordinate = first.patch.coordinates[k];
    }
    first.mapped = true;
    struct Pending { double distance; uint32_t from; int edge; int gap = -1; };
    auto compare = [](const Pending& a, const Pending& b) {
        if (a.distance != b.distance)
            return a.distance > b.distance;
        if (a.from != b.from)
            return a.from > b.from;
        if (a.edge != b.edge)
            return a.edge > b.edge;
        return a.gap > b.gap;
    };
    std::priority_queue<Pending, std::vector<Pending>, decltype(compare)> pending(compare);
    auto queue = [&](uint32_t from) {
        const auto& face = faces[from];
        if (!intersects(face.patch.coordinates, radius * 1.05 + seamTolerance))
            return;
        for (int k = 0; k < 3; ++k)
            if (face.neighbors[k] >= 0 && !faces[size_t(face.neighbors[k])].mapped) {
                const V2 a = face.patch.coordinates[k], e = face.patch.coordinates[(k + 1) % 3] - a;
                const double t = std::clamp(-a.dot(e) / std::max(e.squaredNorm(), 1e-30), 0., 1.);
                pending.push({(a + t * e).squaredNorm(), from, k});
            }
        if (!faceGaps.empty())
            for (uint32_t id : faceGaps[from]) {
                const auto& gap = gapLinks[id];
                const bool forward = gap.a.face == from;
                const auto own = forward ? gap.a : gap.b, other = forward ? gap.b : gap.a;
                if (faces[other.face].mapped)
                    continue;
                const double t = forward ? gap.aMid : gap.bMid;
                const V2 point = face.patch.coordinates[own.edge] * (1 - t) +
                                 face.patch.coordinates[(own.edge + 1) % 3] * t;
                pending.push({point.squaredNorm(), from, own.edge, int(id)});
            }
    };
    queue(uint32_t(seedIndex));
    size_t steps = 0;
    while (!pending.empty()) {
        if ((steps++ & 1023) == 0) {
            checkConform(started, options, cancel);
            if (progress)
                progress("沿模型曲面展开贴花 · " + std::to_string(steps));
        }
        const auto next = pending.top();
        pending.pop();
        const auto& from = faces[next.from];
        const GapLink* gap = next.gap >= 0 ? &gapLinks[size_t(next.gap)] : nullptr;
        const bool forward = gap && gap->a.face == next.from;
        const uint32_t index = gap ? (forward ? gap->b.face : gap->a.face) : uint32_t(from.neighbors[next.edge]);
        auto& face = faces[index];
        if (face.mapped)
            continue;
        auto coordinates = gap ? unfoldGap(from, next.edge, forward ? gap->aMid : gap->bMid, face,
                                            forward ? gap->b.edge : gap->a.edge,
                                            forward ? gap->bMid : gap->aMid, gap->distance) :
                                 unfold(from, next.edge, face, from.neighborEdges[next.edge]);
        const auto originalCoordinates = coordinates;
        bool conflict = false;
        for (int k = 0; k < 3; ++k) {
            const auto& vertex = vertices[face.vertices[k]];
            if (vertex.mapped) {
                // Reuse one coordinate per stitched vertex. A contradictory wraparound route
                // or strongly doubly-curved chart is cut instead of overlapping/ripping artwork.
                if ((coordinates[k] - vertex.coordinate).norm() > radius * .04 + seamTolerance * 8)
                    conflict = true;
                coordinates[k] = vertex.coordinate;
            }
        }
        const double areaBefore = cross2(originalCoordinates[1] - originalCoordinates[0],
                                         originalCoordinates[2] - originalCoordinates[0]);
        const double areaAfter = cross2(coordinates[1] - coordinates[0], coordinates[2] - coordinates[0]);
        if (areaBefore * areaAfter <= 0 || std::abs(areaAfter) < std::abs(areaBefore) * .5)
            conflict = true;
        for (int k = 0; k < 3; ++k) {
            const double length = (face.patch.triangle.world[k] - face.patch.triangle.world[(k + 1) % 3]).norm();
            const double chart = (coordinates[k] - coordinates[(k + 1) % 3]).norm();
            if (std::abs(chart - length) > length * .12 + seamTolerance * 8)
                conflict = true;
        }
        if (conflict) {
            ++result->conflictTriangles;
            continue;
        }
        if (!intersects(coordinates, radius * 1.05 + seamTolerance))
            continue;
        face.patch.coordinates = coordinates;
        face.mapped = true;
        if (gap)
            ++result->bridgedEdges;
        for (int k = 0; k < 3; ++k) {
            auto& vertex = vertices[face.vertices[k]];
            vertex.mapped = true;
            vertex.coordinate = coordinates[k];
        }
        queue(index);
    }
    for (auto& face : faces)
        if (face.mapped)
            result->triangles.push_back(std::move(face.patch));
    checkConform(started, options, cancel);
    result->seconds = seconds(started);
    return result;
}
} // namespace edm
