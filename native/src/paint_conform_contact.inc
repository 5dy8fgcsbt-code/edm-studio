// Virtual contacts are local chart connections, never changes to the original EDM geometry.
// A boundary may meet the middle of another skin: searching only for a second boundary would
// miss such a T junction even when its two visible surfaces overlap by less than a millimetre.
struct ContactLink {
    EdgeRef a;
    uint32_t b = 0;
    V3 origin = V3::Zero(), along = V3::UnitX();
    V3 normalA = V3::UnitZ(), normalB = V3::UnitY();
    double low = 0, high = 0;
};

struct FaceChartMap {
    V3 right, up;
    V2 chartRight, chartUp;
    explicit FaceChartMap(const Face& face) {
        const auto& world = face.patch.triangle.world;
        const V3 edge = world[1] - world[0], third = world[2] - world[0];
        const double length = edge.norm();
        right = edge / length;
        up = edge.cross(third).normalized().cross(right);
        // An orthogonal basis avoids subtracting almost equal squared lengths in the Gram
        // determinant for thin, otherwise valid triangles along the edge of a model panel.
        chartRight = (face.patch.coordinates[1] - face.patch.coordinates[0]) / length;
        chartUp = (face.patch.coordinates[2] - face.patch.coordinates[0] -
                   chartRight * third.dot(right)) / third.dot(up);
    }
    V2 operator()(const V3& vector) const {
        return chartRight * vector.dot(right) + chartUp * vector.dot(up);
    }
};

double clipDistance(const V3& plane, const V2& p) {
    return plane.x() * p.x() + plane.y() * p.y() + plane.z();
}

bool clippedEdgeVisible(const Face& face, int edge, double tolerance) {
    if (!face.patch.clipPlane)
        return true;
    const auto& clip = *face.patch.clipPlane;
    return std::max(clipDistance(clip, face.patch.coordinates[edge]),
                    clipDistance(clip, face.patch.coordinates[(edge + 1) % 3])) > tolerance;
}

bool clippedFaceVisible(const std::array<V2, 3>& xy, const std::optional<V3>& clip, double tolerance) {
    return !clip || std::any_of(xy.begin(), xy.end(), [&](const V2& point) {
        return clipDistance(*clip, point) > tolerance;
    });
}

// The three-dimensional affine map is evaluated in double precision. Points on the real plane
// intersection have identical chart coordinates on both sides. In particular, a penetrating strip
// subtracts distance; it must not be inflated by treating a signed offset as an absolute gap.
std::pair<std::array<V2, 3>, V3> unfoldContact(const Face& from, const Face& to,
                                             const ContactLink& link, bool forward) {
    const auto& world = from.patch.triangle.world;
    const FaceChartMap mapped(from);
    const V3 fromNormal = forward ? link.normalA : link.normalB;
    const V3 toNormal = forward ? link.normalB : link.normalA;
    const Eigen::Quaterniond rotation = Eigen::Quaterniond::FromTwoVectors(toNormal, fromNormal);
    const V2 origin = from.patch.coordinates[0] + mapped(link.origin - world[0]);
    std::array<V2, 3> result;
    for (int k = 0; k < 3; ++k)
        result[k] = origin + mapped(rotation * (to.patch.triangle.world[k] - link.origin));
    const V2 along = mapped(link.along).normalized();
    V2 inward(-along.y(), along.x());
    const V3 exposed = fromNormal - toNormal * fromNormal.dot(toNormal);
    if (inward.dot(mapped(rotation * exposed)) < 0)
        inward = -inward;
    return {result, V3(inward.x(), inward.y(), -inward.dot(origin))};
}

std::vector<ContactLink> findContactLinks(const std::vector<Face>& faces,
                                          const std::unordered_map<uint64_t, Edge>& exactEdges,
                                          const std::vector<GapLink>& gapLinks,
                                          const PaintSurface& surface, const SurfaceDecalOptions& options,
                                          double seamTolerance, double maxBendDegrees,
                                          Clock::time_point started, const std::atomic_bool* cancel,
                                          SurfaceDecalPatch& result) {
    std::unordered_map<uint32_t, uint32_t> faceByPrimitive;
    faceByPrimitive.reserve(faces.size());
    for (uint32_t i = 0; i < faces.size(); ++i)
        faceByPrimitive.emplace(faces[i].patch.primitive, i);
    std::unordered_set<uint64_t> gapBoundaries;
    for (const auto& link : gapLinks) {
        gapBoundaries.insert(uint64_t(link.a.face) * 3 + link.a.edge);
        gapBoundaries.insert(uint64_t(link.b.face) * 3 + link.b.edge);
    }
    std::vector<EdgeRef> boundaries;
    for (const auto& [key, edge] : exactEdges)
        if (edge.count == 1 && !gapBoundaries.contains(uint64_t(edge.first.face) * 3 + edge.first.edge))
            boundaries.push_back(edge.first);
    std::sort(boundaries.begin(), boundaries.end(), [](const EdgeRef& a, const EdgeRef& b) {
        return std::pair{a.face, a.edge} < std::pair{b.face, b.edge};
    });
    struct Candidate {
        ContactLink link;
        double boundaryLow = 0, boundaryHigh = 0;
        bool ambiguous = false;
    };
    std::vector<Candidate> candidates;
    std::vector<std::vector<size_t>> incident(boundaries.size());
    const double epsilon = std::max(1e-8, seamTolerance * 2);
    const double distanceTolerance = std::max(1e-9, options.gapDistance * 1e-5);
    const double bendCosine = std::cos(maxBendDegrees * pi / 180.);
    auto ownerOf = [&](const PaintTriangle& triangle) {
        const auto& mesh = surface.scene().meshes[triangle.mesh];
        return mesh.extras.is_object() ? mesh.extras.value("edm_attachment", -1) : -1;
    };
    auto outward = [&](const PaintTriangle& triangle) -> V3 {
        V3 n = (triangle.world[1] - triangle.world[0]).cross(triangle.world[2] - triangle.world[0]).normalized();
        if (n.dot(triangle.normal() * result.normalSign) < 0)
            n = -n;
        return n;
    };
    uint64_t examined = 0;
    for (size_t boundary = 0; boundary < boundaries.size(); ++boundary) {
        if ((boundary & 127) == 0)
            checkConform(started, options, cancel);
        const auto ref = boundaries[boundary];
        const auto& a = faces[ref.face].patch.triangle;
        const V3 a0 = a.world[ref.edge], a1 = a.world[(ref.edge + 1) % 3];
        const V3 edge = a1 - a0;
        const double length = edge.norm();
        if (length <= epsilon * 4)
            continue;
        const V3 tangent = edge / length, nA = outward(a);
        V3 inside = a.world[(ref.edge + 2) % 3] - a0;
        inside -= tangent * inside.dot(tangent);
        inside.normalize();
        auto nearby = surface.querySphere((a0 + a1) * .5, length * .5 + options.gapDistance + epsilon);
        for (uint32_t primitive : nearby) {
            if ((examined++ & 1023) == 0) {
                checkConform(started, options, cancel);
                require(examined <= topologyLimit * 32,
                        "Surface decal contact matching exceeds its work budget; reduce decal size");
            }
            const auto found = faceByPrimitive.find(primitive);
            if (found == faceByPrimitive.end())
                continue;
            const uint32_t other = found->second;
            const auto& b = faces[other].patch.triangle;
            if (a.mesh == b.mesh || ownerOf(a) != ownerOf(b))
                continue;
            const V3 nB = outward(b);
            const double cosine = nA.dot(nB), sineSquared = std::max(0., 1 - cosine * cosine);
            // Parallel/stacked skins and mild normal steps are not a T junction. Their boundary
            // strips are handled by the original gap matcher, with its stricter sheet safeguards.
            if (sineSquared < .03 || cosine + 1e-12 < bendCosine ||
                a.normal().dot(b.normal()) + 1e-12 < bendCosine)
                continue;
            V3 targetOutward = nB - nA * cosine;
            targetOutward.normalize();
            if (inside.dot(targetOutward) < .8)
                continue; // The other side of the wall or an unrelated crossing shell.
            V3 axis = nA.cross(nB).normalized();
            if (axis.dot(tangent) < 0)
                axis = -axis;
            if (axis.dot(tangent) < .9)
                continue;
            const double signedDistance = (a0 - b.world[0]).dot(nB);
            const V3 origin = a0 - (nB - nA * cosine) * (signedDistance / sineSquared);
            double low = std::min((a0 - origin).dot(axis), (a1 - origin).dot(axis));
            double high = std::max((a0 - origin).dot(axis), (a1 - origin).dot(axis));
            // Clip the infinite intersection line to the receiving original triangle.
            for (int k = 0; k < 3 && low < high; ++k) {
                const V3 start = b.world[k], finish = b.world[(k + 1) % 3];
                V3 inward = nB.cross(finish - start);
                if (inward.dot(b.world[(k + 2) % 3] - start) < 0)
                    inward = -inward;
                const double d = inward.dot(origin - start), slope = inward.dot(axis);
                if (std::abs(slope) < 1e-14) {
                    if (d < -epsilon * inward.norm())
                        high = low;
                } else if (slope > 0)
                    low = std::max(low, -d / slope);
                else
                    high = std::min(high, -d / slope);
            }
            if (high - low <= std::max(epsilon * 4, length * .01))
                continue;
            const V3 exposed = (nA - nB * cosine).normalized();
            double minimumSide = 1e100, maximumSide = -1e100;
            for (const V3& point : b.world) {
                const double side = (point - origin).dot(exposed);
                minimumSide = std::min(minimumSide, side);
                maximumSide = std::max(maximumSide, side);
            }
            if (minimumSide >= -epsilon || maximumSide <= epsilon)
                continue; // An ordinary edge-to-edge seam, not an interior contact.
            bool close = true;
            for (double t : {low, (low + high) * .5, high}) {
                const V3 contact = origin + axis * t;
                const double edgeParameter = std::clamp((contact - a0).dot(tangent), 0., length);
                const double gap = (contact - (a0 + tangent * edgeParameter)).norm();
                if (gap > options.gapDistance + distanceTolerance)
                    close = false;
            }
            if (!close)
                continue;
            if (options.projectionEye && (options.frontFacesOnly || options.occlusion)) {
                const V3 middle = origin + axis * ((low + high) * .5);
                // Both exposed half-surfaces must face the fixed placement camera. This rejects
                // the back skin without using camera-dependent geometry to bend the decal.
                const V3 towardEye = (*options.projectionEye - middle).normalized();
                if (options.frontFacesOnly && (nA.dot(towardEye) <= 0 || nB.dot(towardEye) <= 0))
                    continue;
            }
            require(candidates.size() < topologyLimit * 3,
                    "Surface decal has too many contact candidates; reduce decal size");
            const V3 first = origin + axis * low, last = origin + axis * high;
            const double firstT = (first - a0).dot(tangent), lastT = (last - a0).dot(tangent);
            incident[boundary].push_back(candidates.size());
            candidates.push_back({{ref, other, origin, axis, nA, nB, low, high},
                                   std::min(firstT, lastT), std::max(firstT, lastT)});
        }
    }
    // Adjacent target triangles partition a long boundary. Coincident or overlapping candidates
    // occupy the same interval, so neither is an unambiguous continuation of the visible shell.
    for (const auto& list : incident) {
        checkConform(started, options, cancel);
        if (list.size() > 128) {
            for (size_t id : list)
                candidates[id].ambiguous = true;
            continue;
        }
        for (size_t i = 0; i < list.size(); ++i)
            for (size_t j = i + 1; j < list.size(); ++j) {
                auto& a = candidates[list[i]];
                auto& b = candidates[list[j]];
                if (std::min(a.boundaryHigh, b.boundaryHigh) - std::max(a.boundaryLow, b.boundaryLow) > epsilon * 4) {
                    a.ambiguous = b.ambiguous = true;
                }
            }
    }
    // The same ambiguity check is needed in reverse. Two overlapping ledges can each have one
    // otherwise valid contact with a wall; starting on that wall must not branch onto both skins.
    std::unordered_map<uint32_t, std::vector<size_t>> receiving;
    for (size_t i = 0; i < candidates.size(); ++i)
        receiving[candidates[i].link.b].push_back(i);
    for (const auto& [face, list] : receiving) {
        checkConform(started, options, cancel);
        if (list.size() > 128) {
            for (size_t id : list)
                candidates[id].ambiguous = true;
            continue;
        }
        for (size_t i = 0; i < list.size(); ++i)
            for (size_t j = i + 1; j < list.size(); ++j) {
                auto& a = candidates[list[i]];
                auto& b = candidates[list[j]];
                if (a.link.normalA.dot(b.link.normalA) < .98 ||
                    std::abs(a.link.along.dot(b.link.along)) < .98)
                    continue;
                const V3 b0 = b.link.origin + b.link.along * b.link.low;
                const V3 b1 = b.link.origin + b.link.along * b.link.high;
                const double t0 = (b0 - a.link.origin).dot(a.link.along);
                const double t1 = (b1 - a.link.origin).dot(a.link.along);
                const double low = std::max(a.link.low, std::min(t0, t1));
                const double high = std::min(a.link.high, std::max(t0, t1));
                if (high - low <= epsilon * 4)
                    continue;
                const V3 point = a.link.origin + a.link.along * ((low + high) * .5);
                const V3 delta = point - b.link.origin;
                const double separation = (delta - b.link.along * delta.dot(b.link.along)).norm();
                if (separation <= options.gapDistance + epsilon)
                    a.ambiguous = b.ambiguous = true;
            }
    }
    std::vector<ContactLink> links;
    for (const auto& candidate : candidates)
        if (!candidate.ambiguous)
            links.push_back(candidate.link);
        else
            ++result.blockedEdges;
    return links;
}
