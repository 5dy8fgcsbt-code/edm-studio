#include "paint_conform.h"
#include <iostream>

using namespace edm;
namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    require(value, std::string("Conforming gap regression: ") + message);
}
template <class F> void fails(F action, const char* message) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    check(rejected, message);
}
Mesh quad(float left, float right, int material = 0, float bottom = -.12f, float top = .12f, float z = 0) {
    Mesh mesh;
    mesh.material = material;
    mesh.positions = {{left, bottom, z}, {right, bottom, z}, {right, top, z}, {left, top, z}};
    mesh.normals.assign(4, F3{0, 0, 1});
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.uvs = {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    return mesh;
}
std::shared_ptr<Scene> scene(std::vector<Mesh> meshes) {
    auto result = std::make_shared<Scene>();
    result->materials.resize(8);
    result->nodes.resize(1);
    result->order = {0};
    result->staticLocal = result->defaultWorld = {Mat::Identity()};
    result->meshes = std::move(meshes);
    return result;
}
PaintHit hitAt(const PaintSurface& surface, V3 point = V3(-.03, 0, 0), int material = 0) {
    auto hit = surface.raycast(point + V3(0, 0, 3), -V3::UnitZ(), material);
    require(hit.has_value(), "Missing gap fixture hit");
    return *hit;
}
SurfaceDecalOptions placement(const PaintHit& hit, double gap = .003) {
    SurfaceDecalOptions options;
    options.center = hit.position;
    options.normal = hit.normal;
    options.tangent = V3::UnitX();
    options.width = options.height = .6;
    options.occlusion = options.frontFacesOnly = false;
    options.gapDistance = gap;
    return options;
}
std::vector<int> materials(const PaintSurface& surface, SurfaceDecalOptions options, const PaintHit& hit) {
    options.conformPatch = buildSurfaceDecalPatch(surface, hit, options);
    return findDecalMaterials(surface, options).materials;
}
PaintImage ramp() {
    PaintImage image(1024, 1);
    for (size_t i = 0; i < 1024; ++i) {
        image.rgba[i * 4] = uint8_t(std::lround(double(i) / 1023 * 255));
        image.rgba[i * 4 + 1] = 127;
        image.rgba[i * 4 + 2] = 31;
    }
    return image;
}
int channel(const PaintCanvas& canvas, int x, int y, int component = 0) {
    return canvas.image().rgba[(size_t(y) * canvas.image().width + x) * 4 + component];
}

void planarStripAndToggle() {
    auto source = scene({quad(-.2f, 0), quad(.002f, .2f, 1)});
    const auto original = source->meshes;
    PaintSurface surface(source);
    const auto hit = hitAt(surface);
    auto options = placement(hit, 0);
    options.conformPatch = buildSurfaceDecalPatch(surface, hit, options);
    check(options.conformPatch->bridgedEdges == 0 && options.conformPatch->gapDistance == 0 &&
              findDecalMaterials(surface, options).materials == std::vector<int>{0},
          "Disabled gap mode retains the strictly continuous surface and reports no bridge");
    options.gapDistance = .001;
    options.conformPatch.reset();
    check(materials(surface, options, hit) == std::vector<int>{0},
          "A gap larger than the configured millimetre threshold remains disconnected");
    options.gapDistance = .00199;
    check(materials(surface, options, hit) == std::vector<int>{0},
          "The larger coincidence tolerance cannot widen the user's smaller physical gap threshold");
    options.gapDistance = .002;
    check(materials(surface, options, hit) == std::vector<int>{0, 1},
          "The exact physical gap threshold tolerates only numerical roundoff from float vertices");
    options.gapDistance = .003;
    options.conformPatch = buildSurfaceDecalPatch(surface, hit, options);
    check(options.conformPatch->triangles.size() == 4 && options.conformPatch->bridgedEdges == 1 &&
              findDecalMaterials(surface, options).materials == std::vector<int>{0, 1},
          "A compatible two millimetre strip bridges to the original second material");
    for (const auto& triangle : options.conformPatch->triangles)
        for (int k = 0; k < 3; ++k)
            check((triangle.coordinates[k] - Eigen::Vector2d(triangle.triangle.world[k].x() - hit.position.x(),
                                                            triangle.triangle.world[k].y())).norm() < 1e-7,
                  "A missing strip retains physical width instead of collapsing or restarting image coordinates");
    PaintCanvas left(PaintImage(512, 256, {0, 0, 0, 91}));
    PaintCanvas right(PaintImage(256, 128, {0, 0, 0, 91}));
    options.material = 0;
    check(applySurfaceDecal(surface, left, ramp(), options).changed,
          "The original material receives the cross-gap artwork");
    options.material = 1;
    check(applySurfaceDecal(surface, right, ramp(), options).changed,
          "The separate UV island and resolution receive the continued artwork");
    for (int x : {0, 16, 64, 128, 192, 255}) {
        const double worldX = .002 + (double(x) + .5) / 256 * .198;
        const double expected = 255 * (.5 + (worldX - hit.position.x()) / options.width);
        check(std::abs(channel(right, x, 64) - expected) <= 2 && channel(right, x, 64, 3) == 91,
              "Baked image coordinates include the absent strip and preserve original texture alpha");
    }
    const auto painted = right.image().rgba;
    check(right.undo() && channel(right, 128, 64, 1) == 0 && right.redo() && right.image().rgba == painted,
          "Cross-gap painting has the same exact undo and redo semantics");
    check(source->meshes.size() == original.size(), "Gap bridging does not create export meshes");
    for (size_t i = 0; i < original.size(); ++i)
        check(source->meshes[i].positions == original[i].positions && source->meshes[i].indices == original[i].indices &&
                  source->meshes[i].normals == original[i].normals && source->meshes[i].uvs == original[i].uvs &&
                  source->meshes[i].material == original[i].material,
              "Source vertices, normals, indices, UVs and materials remain unchanged");
    options.material = -1;
    options.gapDistance = 0;
    options.conformPatch.reset();
    check(materials(surface, options, hit) == std::vector<int>{0},
          "Disabling gap mode restores the original footprint without mutating topology");
}

void subdivisionsAndCurve() {
    // One long left edge meets two shorter right edges. These disjoint overlaps are compatible,
    // whereas requiring matching endpoints would leave an otherwise ordinary panel seam broken.
    for (float gap : {0.f, .002f}) {
        PaintSurface surface(scene({quad(-.2f, 0), quad(gap, .2f, 1, -.12f, 0),
                                    quad(gap, .2f, 2, 0, .12f)}));
        const auto hit = hitAt(surface);
        auto options = placement(hit);
        options.conformPatch = buildSurfaceDecalPatch(surface, hit, options);
        check(findDecalMaterials(surface, options).materials == std::vector<int>{0, 1, 2} &&
                  options.conformPatch->triangles.size() == 6 && options.conformPatch->bridgedEdges > 0,
              "Projected boundary overlap supports a differently subdivided seam including an exact T junction");
        for (const auto& triangle : options.conformPatch->triangles)
            for (int k = 0; k < 3; ++k)
                check((triangle.coordinates[k] - Eigen::Vector2d(triangle.triangle.world[k].x() - hit.position.x(),
                                                                triangle.triangle.world[k].y())).norm() < 1e-7,
                      "Coarse-to-fine seams use consistent global coordinates at every original vertex");
    }
    // A slightly tapered missing strip must not rotate the artwork to force unlike edges parallel.
    auto angled = quad(.002f, .2f, 1);
    for (auto& p : angled.positions)
        p[0] += p[1] * .005f;
    PaintSurface taper(scene({quad(-.2f, 0), angled}));
    auto taperHit = hitAt(taper);
    auto taperOptions = placement(taperHit);
    taperOptions.conformPatch = buildSurfaceDecalPatch(taper, taperHit, taperOptions);
    check(findDecalMaterials(taper, taperOptions).materials == std::vector<int>{0, 1},
          "Slightly nonparallel facing boundaries can bridge a tapered physical seam");
    for (const auto& triangle : taperOptions.conformPatch->triangles)
        for (int k = 0; k < 3; ++k)
            check((triangle.coordinates[k] - Eigen::Vector2d(triangle.triangle.world[k].x() - taperHit.position.x(),
                                                            triangle.triangle.world[k].y())).norm() < 1e-7,
                  "Tapered planar gaps preserve every world-space image coordinate and triangle orientation");
    auto arc = [](double a, double b, int material) {
        auto mesh = quad(float(a), float(b), material);
        for (size_t k = 0; k < mesh.positions.size(); ++k) {
            const double angle = k == 0 || k == 3 ? a : b;
            mesh.positions[k][0] = float(std::sin(angle));
            mesh.positions[k][2] = float(std::cos(angle) - 1);
            mesh.normals[k] = {float(std::sin(angle)), 0, float(std::cos(angle))};
        }
        return mesh;
    };
    PaintSurface cylinder(scene({arc(-.2, -.002, 0), arc(.002, .2, 1)}));
    const auto hit = hitAt(cylinder);
    auto options = placement(hit, .005);
    options.conformPatch = buildSurfaceDecalPatch(cylinder, hit, options);
    check(options.conformPatch->bridgedEdges == 1 &&
              findDecalMaterials(cylinder, options).materials == std::vector<int>{0, 1},
          "A small missing strip follows a gently curved fuselage across original materials");
    for (const auto& triangle : options.conformPatch->triangles)
        for (int k = 0; k < 3; ++k)
            check(std::abs((triangle.coordinates[k] - triangle.coordinates[(k + 1) % 3]).norm() -
                           (triangle.triangle.world[k] - triangle.triangle.world[(k + 1) % 3]).norm()) < 1e-6,
                  "Virtual strip crossing preserves triangle lengths over the curved surface");
}

void unsafeNeighbors() {
    const auto left = quad(-.2f, 0);
    auto checkExcluded = [&](Mesh neighbor, const char* message) {
        PaintSurface surface(scene({left, std::move(neighbor)}));
        const auto hit = hitAt(surface);
        check(materials(surface, placement(hit, .02), hit) == std::vector<int>{0}, message);
    };
    checkExcluded(quad(-.2f, 0, 1, -.12f, .12f, -.001f),
                  "A nearby parallel stacked skin cannot be reached by a volumetric proximity jump");
    checkExcluded(quad(.002f, .2f, 1, -.12f, .12f, -.001f),
                  "A normal step to a lower skin is excluded even when inside the gap-distance sphere");
    auto underside = quad(.002f, .2f, 1);
    underside.normals.assign(4, F3{0, 0, -1});
    underside.indices = {0, 2, 1, 0, 3, 2};
    checkExcluded(std::move(underside), "Opposite-facing underside boundaries are not bridged");
    auto reversed = quad(.002f, .2f, 1);
    reversed.indices = {0, 2, 1, 0, 3, 2};
    PaintSurface authoredNormals(scene({left, reversed}));
    const auto reversedHit = hitAt(authoredNormals);
    check(materials(authoredNormals, placement(reversedHit), reversedHit) == std::vector<int>{0, 1},
          "Reverse winding with authored outward normals still joins the intended outward-facing sheet");
    auto external = quad(.002f, .2f, 1);
    external.extras = {{"edm_attachment", 3}};
    checkExcluded(std::move(external), "Separate attachment ownership prevents bridging to a pylon or weapon");
    auto folded = quad(.002f, .2f, 1);
    for (auto& p : folded.positions) {
        const float x = p[0] - .002f;
        p[0] = .002f + x * float(std::sqrt(.5));
        p[2] = -x * float(std::sqrt(.5));
    }
    folded.normals.assign(4, F3{float(std::sqrt(.5)), 0, float(std::sqrt(.5))});
    checkExcluded(std::move(folded), "A sharply bent neighboring part is not mistaken for a tiny skin seam");
    auto corner = quad(.002f, .2f, 1, .12f, .3f);
    checkExcluded(std::move(corner), "Corner-only proximity cannot jump between otherwise unrelated panels");

    PaintSurface ambiguous(scene({left, quad(.002f, .2f, 1), quad(.003f, .2f, 2)}));
    auto hit = hitAt(ambiguous);
    auto options = placement(hit, .004);
    options.conformPatch = buildSurfaceDecalPatch(ambiguous, hit, options);
    check(findDecalMaterials(ambiguous, options).materials == std::vector<int>{0} &&
              options.conformPatch->bridgedEdges == 0 && options.conformPatch->blockedEdges > 0,
          "Overlapping candidate strips are all rejected instead of picking an arbitrary nearby sheet");

    PaintSurface stacked(scene({left, quad(-.2f, 0, 1, -.12f, .12f, -.001f), quad(.002f, .2f, 2)}));
    hit = hitAt(stacked);
    check(materials(stacked, placement(hit, .004), hit) == std::vector<int>{0, 2},
          "A valid adjacent strip remains reachable while an overlapping lower sheet stays excluded");
    auto hidden = quad(.002f, .2f, 1);
    hidden.extras = {{"edm_attachment", 0}};
    PaintSurface hiddenSurface(scene({left, hidden}), {}, {}, nullptr, false);
    hit = hitAt(hiddenSurface);
    check(hiddenSurface.triangleCount() == 2 && materials(hiddenSurface, placement(hit), hit) == std::vector<int>{0},
          "Hidden attachments are absent from both gap matching and the underlying paint surface");
}

void cacheCancellationAndLimits() {
    PaintSurface surface(scene({quad(-.2f, 0), quad(.002f, .2f, 1)}));
    const auto hit = hitAt(surface);
    auto options = placement(hit);
    options.conformPatch = buildSurfaceDecalPatch(surface, hit, options);
    auto stale = options;
    stale.gapDistance = 0;
    fails([&] { findDecalMaterials(surface, stale); },
          "Toggling gap mode invalidates a cached chart before material discovery");
    PaintCanvas canvas(PaintImage(128, 128, {1, 2, 3, 91}));
    const auto original = canvas.image().rgba;
    fails([&] { applySurfaceDecal(surface, canvas, ramp(), stale); },
          "Stale gap settings cannot bake a footprint different from the requested preview");
    stale = options;
    stale.gapDistance = .004;
    fails([&] { validateSurfaceDecalPatch(surface, stale); },
          "Changing the physical gap threshold requires rebuilding the preview chart");
    check(canvas.image().rgba == original && !canvas.canUndo(),
          "Rejected stale charts leave original pixels and undo history intact");
    for (double invalid : {-1., .020001, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
        auto bad = options;
        bad.gapDistance = invalid;
        fails([&] { buildSurfaceDecalPatch(surface, hit, bad); },
              "Invalid or excessively large gap distances are rejected explicitly");
    }
    std::atomic_bool cancel = false;
    fails([&] {
        buildSurfaceDecalPatch(surface, hit, options, 65, [&](const std::string& phase) {
            if (phase.find("微小缝隙") != std::string::npos)
                cancel = true;
        }, &cancel);
    }, "Cancellation is observed inside gap construction before publishing its surface chart");
    auto limited = options;
    limited.limits.seconds = 1e-12;
    fails([&] { buildSurfaceDecalPatch(surface, hit, limited); },
          "Gap topology construction honors the same overall wall-clock work budget");
    limited = options;
    limited.material = 1;
    limited.limits.rasterSamples = 64;
    fails([&] { applySurfaceDecal(surface, canvas, ramp(), limited); },
          "Cross-gap baking still enforces the raster work budget");
    check(canvas.image().rgba == original && !canvas.canUndo() && !canvas.strokeActive(),
          "A failed cross-gap bake rolls back partial pixels and leaves no transaction behind");
}
} // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        ComRuntime runtime;
        planarStripAndToggle();
        subdivisionsAndCurve();
        unsafeNeighbors();
        cacheCancellationAndLimits();
        std::cout << "Conforming gaps: " << checks << " checks passed\n";
        return 0;
    } catch (...) {
        std::cerr << exceptionText() << '\n';
        return 1;
    }
}
