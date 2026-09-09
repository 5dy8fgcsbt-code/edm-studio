#include "paint_conform.h"
#include <cmath>
#include <iostream>

using namespace edm;
namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    require(value, std::string("Conforming contact regression: ") + message);
}
template <class F> void fails(F action, const char* message) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    check(rejected, message);
}

// A ledge ends near the middle of a taller wall, rather than at one of its boundary edges.
// Both the model's original triangle numbering and separate material UV atlases are retained.
Mesh ledge() {
    Mesh mesh;
    mesh.name = "Ledge outer skin";
    mesh.material = 0;
    mesh.positions = {{-.2f, -.12f, 0}, {0, -.12f, 0}, {0, .12f, 0}, {-.2f, .12f, 0}};
    mesh.normals.assign(4, F3{0, 0, 1});
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.uvs = {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    return mesh;
}
Mesh wall(float distance = .001f, int material = 1, unsigned subdivisions = 4) {
    Mesh mesh;
    mesh.name = "Wall spanning above and below ledge";
    mesh.material = material;
    mesh.uvs.resize(1);
    constexpr std::array<float, 4> heights{-.2f, -.08f, .08f, .2f};
    for (float height : heights)
        for (unsigned column = 0; column <= subdivisions; ++column) {
            const float u = float(column) / subdivisions;
            mesh.positions.push_back({distance, -.12f + .24f * u, height});
            mesh.normals.push_back({-1, 0, 0});
            mesh.uvs[0].push_back({u, (height + .2f) / .4f});
        }
    const unsigned stride = subdivisions + 1;
    for (unsigned row = 0; row + 1 < heights.size(); ++row)
        for (unsigned column = 0; column < subdivisions; ++column) {
            const unsigned a = row * stride + column, b = a + stride;
            mesh.indices.insert(mesh.indices.end(), {a, b, b + 1, a, b + 1, a + 1});
        }
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
PaintHit floorHit(const PaintSurface& surface) {
    auto hit = surface.raycast(V3(-.06, -.017, .5), -V3::UnitZ(), 0);
    require(hit.has_value(), "Missing contact fixture ledge hit");
    return *hit;
}
PaintHit wallHit(const PaintSurface& surface) {
    auto hit = surface.raycast(V3(-.5, .013, .05), V3::UnitX(), 1);
    require(hit.has_value(), "Missing contact fixture wall hit");
    return *hit;
}
SurfaceDecalOptions placement(const PaintHit& hit, double gap = .002) {
    SurfaceDecalOptions options;
    options.center = hit.position;
    options.normal = hit.normal;
    options.tangent = hit.material == 0 ? V3::UnitX() : V3::UnitY();
    options.width = options.height = 1;
    options.depth = .01;
    options.gapDistance = gap;
    options.frontFacesOnly = options.occlusion = false;
    options.projectionEye = V3(-.4, -.03, .5);
    return options;
}
std::vector<int> materials(const PaintSurface& surface, SurfaceDecalOptions options, const PaintHit& hit,
                           double bend = 105) {
    options.conformPatch = buildSurfaceDecalPatch(surface, hit, options, bend);
    return findDecalMaterials(surface, options).materials;
}
int channel(const PaintCanvas& canvas, int x, int y, int component = 0) {
    return canvas.image().rgba[(size_t(y) * canvas.image().width + x) * 4 + component];
}
PaintImage ramp() {
    PaintImage image(1024, 1);
    for (unsigned x = 0; x < image.width; ++x) {
        image.rgba[x * 4] = uint8_t(std::lround(double(x) / (image.width - 1) * 255));
        image.rgba[x * 4 + 1] = 127;
        image.rgba[x * 4 + 2] = 31;
    }
    return image;
}

void contactThresholdsAndDirections() {
    for (float distance : {-.0022f, -.001f, 0.f, .001f, .0022f}) {
        PaintSurface surface(scene({ledge(), wall(distance)}));
        const auto hit = floorHit(surface);
        auto options = placement(hit);
        const auto expected = std::abs(distance) < .002f ? std::vector<int>{0, 1} : std::vector<int>{0};
        check(materials(surface, options, hit) == expected,
              "A face-interior contact respects positive separation and shallow penetration distance limits");
        options.gapDistance = 0;
        check(materials(surface, options, hit) == std::vector<int>{0},
              "Disabling gap crossing retains the original disconnected topology even at an exact T contact");
    }
    PaintSurface surface(scene({ledge(), wall()}));
    const auto hit = floorHit(surface);
    auto options = placement(hit);
    check(materials(surface, options, hit, 65) == std::vector<int>{0},
          "A ninety degree face contact still respects the user's lower bend setting");
    options.conformPatch = buildSurfaceDecalPatch(surface, hit, options, 105);
    check(options.conformPatch->contactEdges > 0 && findDecalMaterials(surface, options).materials ==
              std::vector<int>{0, 1}, "Raising the bend limit deliberately permits the T contact");
    const auto reverse = wallHit(surface);
    auto reverseOptions = placement(reverse);
    reverseOptions.conformPatch = buildSurfaceDecalPatch(surface, reverse, reverseOptions, 105);
    check(reverseOptions.conformPatch->contactEdges > 0 &&
              findDecalMaterials(surface, reverseOptions).materials == std::vector<int>{0, 1},
          "The same physical contact can be reached from a wall seed as well as a ledge seed");
}

void clippedArtworkAndOriginalTopology() {
    auto original = scene({ledge(), wall()});
    const auto before = original->meshes;
    PaintSurface surface(original);
    const auto hit = floorHit(surface);
    auto options = placement(hit);
    options.conformPatch = buildSurfaceDecalPatch(surface, hit, options, 105);
    const auto& patch = *options.conformPatch;
    bool sawCrossingFace = false;
    unsigned externalColumns = 0;
    uint32_t previous = UINT32_MAX;
    for (const auto& face : patch.triangles) {
        check(previous == UINT32_MAX || previous < face.primitive,
              "Each original primitive has one sorted chart entry instead of duplicated export triangles");
        previous = face.primitive;
        if (face.triangle.material != 1)
            continue;
        double low = 1, high = -1;
        for (const auto& point : face.triangle.world) {
            low = std::min(low, point.z());
            high = std::max(high, point.z());
        }
        check(high >= -1e-6, "Propagation cannot enter wall triangles entirely below the contact line");
        if (low < -1e-5 && high > 1e-5) {
            sawCrossingFace = true;
            check(face.clipPlane.has_value(),
                  "An original wall triangle crossing the T line retains an explicit visible-half clip");
            for (int k = 0; k < 3; ++k) {
                const auto xy = face.coordinates[k];
                const double clip = face.clipPlane->dot(V3(xy.x(), xy.y(), 1));
                check(clip * face.triangle.world[k].z() >= -1e-7,
                      "The shared preview and bake clip keeps the exterior half and excludes the inner half");
            }
        }
        if (high > .1)
            ++externalColumns;
    }
    check(sawCrossingFace && externalColumns >= 4,
          "One long ledge boundary reaches multiple original wall triangles beyond its initial contact");

    PaintCanvas floorCanvas(PaintImage(256, 256, {0, 0, 0, 91}));
    PaintCanvas wallCanvas(PaintImage(256, 256, {0, 0, 0, 91}));
    wallCanvas.enableLayers({{1, "Contact artwork"}}, 1);
    const auto clean = wallCanvas.image().rgba;
    options.material = 0;
    check(applySurfaceDecal(surface, floorCanvas, ramp(), options).changed,
          "The ledge writes the first portion of the original image");
    options.material = 1;
    check(applySurfaceDecal(surface, wallCanvas, ramp(), options).changed,
          "A separate wall material receives the continued original image");
    for (int x : {8, 64, 127, 192, 247}) {
        for (int y : {8, 48, 96, 120})
            check(channel(wallCanvas, x, y) == 0 && channel(wallCanvas, x, y, 1) == 0,
                  "Hidden inner-wall texels remain untouched even with camera occlusion disabled");
        for (int y : {132, 160, 208, 247}) {
            const double height = -.2 + .4 * (double(y) + .5) / 256;
            const double expected = 255 * (.5 + .06 + .001 + height);
            check(std::abs(channel(wallCanvas, x, y) - expected) <= 3 &&
                      channel(wallCanvas, x, y, 1) == 127 && channel(wallCanvas, x, y, 3) == 91,
                  "Image distance continues up the wall without restarting, stretching, or changing base alpha");
        }
    }
    check(std::abs(channel(floorCanvas, 255, 128) - channel(wallCanvas, 128, 128)) <= 3,
          "The image remains continuous across the last ledge texel and first exterior wall texel");
    const auto painted = wallCanvas.image().rgba;
    check(wallCanvas.undo() && wallCanvas.image().rgba == clean && wallCanvas.redo() &&
              wallCanvas.image().rgba == painted, "Layered contact painting has exact undo and redo");
    check(original->meshes.size() == before.size(), "Contact painting does not append synthetic export meshes");
    for (size_t k = 0; k < before.size(); ++k)
        check(original->meshes[k].positions == before[k].positions &&
                  original->meshes[k].indices == before[k].indices &&
                  original->meshes[k].uvs == before[k].uvs &&
                  original->meshes[k].normals == before[k].normals &&
                  original->meshes[k].material == before[k].material,
              "Source positions, triangle indices, UVs, normals, and materials remain unchanged");
}

void excludedContacts() {
    auto external = wall();
    external.extras = {{"edm_attachment", 4}};
    PaintSurface separate(scene({ledge(), external}));
    const auto hit = floorHit(separate);
    check(materials(separate, placement(hit), hit) == std::vector<int>{0},
          "A nearby face in another attachment instance is not connected to the aircraft");
    PaintSurface hidden(scene({ledge(), external}), {}, {}, nullptr, false);
    const auto hiddenHit = floorHit(hidden);
    check(hidden.triangleCount() == 2 && materials(hidden, placement(hiddenHit), hiddenHit) == std::vector<int>{0},
          "Hidden external objects are absent from contact matching and paint occlusion");
    auto same = ledge();
    same.extras = {{"edm_attachment", 4}};
    PaintSurface sameOwner(scene({same, external}));
    const auto sameHit = floorHit(sameOwner);
    check(materials(sameOwner, placement(sameHit), sameHit) == std::vector<int>{0, 1},
          "One attachment can still paint continuously across its own contact seam");
    PaintSurface ambiguous(scene({ledge(), wall(.0008f, 1), wall(.0012f, 2)}));
    const auto ambiguousHit = floorHit(ambiguous);
    auto options = placement(ambiguousHit);
    options.conformPatch = buildSurfaceDecalPatch(ambiguous, ambiguousHit, options, 105);
    check(options.conformPatch->contactEdges == 0 &&
              findDecalMaterials(ambiguous, options).materials == std::vector<int>{0},
          "Overlapping target shells do not receive an arbitrary nearest-face contact");
    auto lower = ledge();
    lower.material = 1;
    for (auto& point : lower.positions)
        point[2] -= .001f;
    PaintSurface stacked(scene({ledge(), lower}));
    const auto stackedHit = floorHit(stacked);
    check(materials(stacked, placement(stackedHit), stackedHit) == std::vector<int>{0},
          "Adding face-interior contacts does not turn parallel stacked skins into neighboring surfaces");

    // From the receiving wall, two nearly coincident ledge edges are competing continuations.
    // Testing only ambiguity on each ledge's own edge would incorrectly accept both links.
    lower.material = 2;
    PaintSurface receiving(scene({ledge(), lower, wall()}));
    const auto receivingHit = wallHit(receiving);
    auto receivingOptions = placement(receivingHit, .002);
    receivingOptions.conformPatch = buildSurfaceDecalPatch(receiving, receivingHit, receivingOptions, 105);
    check(receivingOptions.conformPatch->contactEdges == 0 &&
              findDecalMaterials(receiving, receivingOptions).materials == std::vector<int>{1},
          "A wall seed rejects competing ledge layers one millimetre apart instead of selecting either or both");
    PaintCanvas excludedLayers(PaintImage(64, 64, {3, 5, 7, 91}));
    const auto unpainted = excludedLayers.image().rgba;
    receivingOptions.targetMaterials = {0, 2};
    check(!applySurfaceDecal(receiving, excludedLayers, ramp(), receivingOptions).changed &&
              excludedLayers.image().rgba == unpainted && !excludedLayers.canUndo(),
          "Neither ambiguous ledge layer receives hidden pixel edits from a reverse wall placement");
}

void cancellationAndPartialRollback() {
    PaintSurface surface(scene({ledge(), wall()}));
    const auto hit = floorHit(surface);
    auto options = placement(hit);
    std::atomic_bool cancel = false;
    bool notified = false;
    fails([&] {
        buildSurfaceDecalPatch(surface, hit, options, 105, [&](const std::string&) {
            notified = true;
            cancel = true;
        }, &cancel);
    }, "Cancellation during chart construction prevents publication of a partial contact patch");
    check(notified, "The cancellation fixture reached chart construction rather than failing input validation");
    auto expired = options;
    expired.limits.seconds = 1e-12;
    fails([&] { buildSurfaceDecalPatch(surface, hit, expired, 105); },
          "Contact discovery respects the existing wall-clock budget");
    options.conformPatch = buildSurfaceDecalPatch(surface, hit, options, 105);
    options.material = 1;
    options.limits.rasterSamples = 4000;
    PaintCanvas canvas(PaintImage(128, 128, {3, 5, 7, 91}));
    const auto clean = canvas.image().rgba;
    canvas.beginStroke("Caller-owned contact transaction");
    options.externalStroke = true;
    fails([&] { applySurfaceDecal(surface, canvas, ramp(), options); },
          "A contact bake enforces its cumulative sample budget across independently bounded triangles");
    check(canvas.image().rgba != clean && canvas.strokeActive(),
          "The budget fixture fails after real partial writes and preserves caller transaction ownership");
    canvas.cancelStroke();
    check(canvas.image().rgba == clean && !canvas.canUndo(),
          "The caller can roll back all partially painted exterior contact pixels exactly");
    options.externalStroke = false;
    fails([&] { applySurfaceDecal(surface, canvas, ramp(), options); },
          "An owned contact bake reports the same cumulative budget failure");
    check(canvas.image().rgba == clean && !canvas.strokeActive() && !canvas.canUndo(),
          "An owned contact bake automatically rolls back partial pixels and undo state");
}

Mesh sharplyFolded(double degrees) {
    auto mesh = ledge();
    const double radians = degrees * 3.14159265358979323846 / 180;
    const V3 normal(std::sin(radians), 0, std::cos(radians));
    const V3 common = (normal + V3::UnitZ()).normalized();
    mesh.normals[1] = mesh.normals[2] = {float(common.x()), 0, float(common.z())};
    for (float y : {-.12f, .12f}) {
        mesh.positions.push_back({float(.2 * std::cos(radians)), y, float(-.2 * std::sin(radians))});
        mesh.normals.push_back({float(normal.x()), 0, float(normal.z())});
        mesh.uvs[0].push_back({2, y < 0 ? 0.f : 1.f});
    }
    mesh.indices.insert(mesh.indices.end(), {1, 4, 5, 1, 5, 2});
    return mesh;
}
void explicitSharpNativeFold() {
    for (double fold : {125., 155.}) {
        PaintSurface surface(scene({sharplyFolded(fold)}));
        auto found = surface.raycast(V3(-.15, 0, .5), -V3::UnitZ(), 0);
        require(found.has_value(), "Missing folded native-surface fixture hit");
        auto options = placement(*found, 0);
        const auto limited = buildSurfaceDecalPatch(surface, *found, options, 65);
        check(limited->triangles.size() == 2 && limited->contactEdges == 0,
              "The original default crease limit stops a sharp indexed fold without invoking contact matching");
        const auto wide = buildSurfaceDecalPatch(surface, *found, options, fold < 150 ? 135 : 150);
        check(wide->triangles.size() == (fold < 150 ? 4 : 2) && wide->contactEdges == 0,
              "A deliberate wider crease setting follows a 125 degree native fold but still rejects 155 degrees");
        fails([&] { buildSurfaceDecalPatch(surface, *found, options, 150.001); },
              "The extended fold control still has a finite enforced upper bound");
    }
}
} // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        ComRuntime runtime;
        contactThresholdsAndDirections();
        clippedArtworkAndOriginalTopology();
        excludedContacts();
        cancellationAndPartialRollback();
        explicitSharpNativeFold();
        std::cout << "Conforming contacts: " << checks << " checks passed\n";
        return 0;
    } catch (...) {
        std::cerr << exceptionText() << '\n';
        return 1;
    }
}
