#include "paint_conform.h"
#include "paint_batch.h"
#include <cmath>
#include <iostream>

using namespace edm;
namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    require(value, std::string("Conforming decal regression: ") + message);
}
template <class F> void fails(F action, const char* message) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, message);
}
Mesh quad(float left = -1, float right = 1, float z = 0, int material = 0) {
    Mesh mesh;
    mesh.material = material;
    mesh.positions = {{left, -1, z}, {right, -1, z}, {right, 1, z}, {left, 1, z}};
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
int channel(const PaintCanvas& canvas, int x, int y, int component = 0) {
    return canvas.image().rgba[(size_t(y) * canvas.image().width + x) * 4 + component];
}
PaintImage ramp() {
    PaintImage image(256, 1);
    for (size_t i = 0; i < 256; ++i) {
        image.rgba[i * 4] = uint8_t(i);
        image.rgba[i * 4 + 1] = 127;
        image.rgba[i * 4 + 2] = uint8_t(255 - i);
        image.rgba[i * 4 + 3] = 255;
    }
    return image;
}
PaintHit hitAt(const PaintSurface& surface, const V3& point = V3::Zero(), int material = -1) {
    auto hit = surface.raycast(point + V3(0, 0, 4), -V3::UnitZ(), material);
    require(hit.has_value(), "Missing analytic surface fixture hit");
    return *hit;
}
SurfaceDecalOptions placement(const PaintHit& hit) {
    SurfaceDecalOptions options;
    options.center = hit.position;
    options.normal = hit.normal;
    options.tangent = V3::UnitX();
    options.width = 2.2;
    options.height = .8;
    options.depth = .02;
    options.occlusion = false;
    options.frontFacesOnly = false;
    return options;
}

// A unit cylinder is developable: horizontal image distance must follow arc length, not sin(theta).
// Every angular segment is a separate mesh. A split fixture also changes material and UV island at 0.
std::shared_ptr<Scene> cylinder(bool splitMaterial = false) {
    std::vector<Mesh> meshes;
    for (int segment = 0; segment < 24; ++segment) {
        const double a = (segment - 12) * .1, b = (segment - 11) * .1;
        Mesh mesh;
        mesh.name = "Cylinder segment " + std::to_string(segment);
        mesh.material = splitMaterial && segment >= 12 ? 1 : 0;
        for (const auto [angle, y] : std::array<std::pair<double, double>, 4>{
                 std::pair{a, -.5}, {b, -.5}, {b, .5}, {a, .5}}) {
            mesh.positions.push_back({float(std::sin(angle)), float(y), float(std::cos(angle) - 1)});
            mesh.normals.push_back({float(std::sin(angle)), 0, float(std::cos(angle))});
        }
        mesh.indices = {0, 1, 2, 0, 2, 3};
        const double start = splitMaterial && segment >= 12 ? 0 : -1.2;
        const double extent = splitMaterial ? 1.2 : 2.4;
        mesh.uvs = {{{float((a - start) / extent), 0}, {float((b - start) / extent), 0},
                     {float((b - start) / extent), 1}, {float((a - start) / extent), 1}}};
        meshes.push_back(std::move(mesh));
    }
    return scene(std::move(meshes));
}

void cylinderArcAndOriginals() {
    auto source = cylinder();
    const auto originals = source->meshes;
    PaintSurface surface(source);
    auto options = placement(hitAt(surface));
    PaintCanvas flat(PaintImage(512, 64, {0, 0, 0, 91}));
    applySurfaceDecal(surface, flat, ramp(), options);
    options.conformPatch = buildSurfaceDecalPatch(surface, hitAt(surface), options);
    check(bool(options.conformPatch), "A connected cylinder creates a curved paint surface");
    PaintCanvas curved(PaintImage(512, 64, {0, 0, 0, 91}));
    const auto report = applySurfaceDecal(surface, curved, ramp(), options);
    check(report.changed && report.paintedPixels > 15000,
          "Curved placement covers a wide cylinder strip despite the very thin planar depth");
    // At theta approximately 0.9, the cylinder lies about 0.38 behind the seed tangent plane.
    const int curvedSide = int((.9 + 1.2) / 2.4 * 512);
    check(channel(flat, curvedSide, 32, 1) == 0 && channel(curved, curvedSide, 32, 1) == 127,
          "The curved decal reaches the fuselage side that fixed-direction depth clipping removes");
    for (int x : {64, 128, 192, 256, 320, 384, 448}) {
        const double angle = (double(x) + .5) / 512 * 2.4 - 1.2;
        // The faceted cylinder's geodesic differs from the analytic arc by less than 0.001 here.
        const double expected = 255 * (.5 + angle / options.width);
        check(std::abs(channel(curved, x, 32) - expected) <= 5,
              "Cylinder image coordinates follow analytic surface distance without planar compression");
        check(channel(curved, x, 32, 3) == 91, "Curve baking preserves the source alpha mask");
    }
    check(source->meshes.size() == originals.size(), "Runtime continuity does not merge source meshes");
    for (size_t i = 0; i < originals.size(); ++i)
        check(source->meshes[i].positions == originals[i].positions &&
                  source->meshes[i].normals == originals[i].normals &&
                  source->meshes[i].indices == originals[i].indices &&
                  source->meshes[i].uvs == originals[i].uvs &&
                  source->meshes[i].material == originals[i].material,
              "Curve building and baking leave export geometry, normals, topology, UVs and materials intact");
}

void materialSeamsAndLayerHistory() {
    PaintSurface surface(cylinder(true));
    auto options = placement(hitAt(surface));
    options.conformPatch = buildSurfaceDecalPatch(surface, hitAt(surface), options);
    const auto found = findDecalMaterials(surface, options);
    check(found.materials == std::vector<int>{0, 1},
          "Connected mesh boundaries discover both original material UV islands");
    auto left = std::make_shared<PaintCanvas>(PaintImage(256, 64, {0, 0, 0, 91}));
    auto right = std::make_shared<PaintCanvas>(PaintImage(128, 128, {0, 0, 0, 91}));
    const std::vector<PaintLayerInfo> layers{{1, "Base artwork"}, {2, "Curved decal"}};
    left->enableLayers(layers, 2);
    right->enableLayers(layers, 2);
    const auto beforeLeft = left->image().rgba, beforeRight = right->image().rgba;
    PaintBatchHistory history;
    history.begin("Curved decal across material seam");
    options.externalStroke = true;
    options.material = 0;
    options.candidatePrimitives = found.primitivesByMaterial.at(0);
    check(applySurfaceDecal(surface, history.touch(0, left), ramp(), options).changed,
          "First original texture receives the curved artwork in the active layer");
    options.material = 1;
    options.candidatePrimitives = found.primitivesByMaterial.at(1);
    check(applySurfaceDecal(surface, history.touch(1, right), ramp(), options).changed,
          "Second resolution and UV island use the same curved placement");
    history.commit();
    check(std::abs(channel(*left, 255, 32) - channel(*right, 0, 64)) <= 3 &&
              std::abs(channel(*left, 255, 32) - 127) <= 3,
          "Artwork remains continuous across a mesh, material and texture-resolution seam");
    const auto afterLeft = left->image().rgba, afterRight = right->image().rgba;
    for (const auto& canvas : {left, right}) {
        auto snapshot = canvas->layerSnapshot();
        check(snapshot.layers[0].tiles.empty() && !snapshot.layers[1].tiles.empty() &&
                  snapshot.base.rgba[0] == 0 && snapshot.base.rgba[3] == 91,
              "Only the selected paint layer gains pixels; the source base and lower layer remain intact");
    }
    check(history.undo() && left->image().rgba == beforeLeft && right->image().rgba == beforeRight,
          "One undo removes the entire curved decal from all original textures");
    check(history.redo() && left->image().rgba == afterLeft && right->image().rgba == afterRight,
          "One redo restores all curved decal pixels exactly");
}

void rotatedCurvedCoordinates() {
    PaintSurface surface(cylinder());
    const auto seed = hitAt(surface);
    auto options = placement(seed);
    options.width = .8;
    options.height = 2.2;
    options.rotationRadians = 3.14159265358979323846 / 2;
    options.conformPatch = buildSurfaceDecalPatch(surface, seed, options);
    PaintCanvas canvas(PaintImage(512, 64, {0, 0, 0, 91}));
    applySurfaceDecal(surface, canvas, ramp(), options);
    for (int y : {16, 32, 48}) {
        const double worldY = (double(y) + .5) / 64 - .5;
        const double expected = 255 * (.5 + worldY / options.width);
        for (int x : {96, 192, 320, 416})
            check(std::abs(channel(canvas, x, y) - expected) <= 5,
                  "Rotating a curved image swaps its surface axes without compressing or reversing artwork");
    }
    options = placement(seed);
    options.width = 2.6;
    options.conformPatch = buildSurfaceDecalPatch(surface, seed, options, 15);
    PaintCanvas gradual(PaintImage(512, 64, {0, 0, 0, 91}));
    applySurfaceDecal(surface, gradual, ramp(), options);
    check(channel(gradual, 8, 32, 1) == 127 && channel(gradual, 503, 32, 1) == 127,
          "Small successive bends can follow a broad cylinder beyond the seed-normal angular limit");
}

void topologyBoundaries() {
    // A nearby parallel shell is well inside the old projection depth but has no surface connection.
    PaintSurface separated(scene({quad(), quad(-1, 1, -.01f, 1)}));
    auto options = placement(hitAt(separated));
    options.width = options.height = 3;
    options.depth = .5;
    options.conformPatch = buildSurfaceDecalPatch(separated, hitAt(separated), options);
    check(findDecalMaterials(separated, options).materials == std::vector<int>{0},
          "A disconnected nearby cockpit or rear shell is excluded even with occlusion disabled");
    options.material = 1;
    options.candidatePrimitives = std::vector<uint32_t>{2, 3};
    PaintCanvas untouched(PaintImage(32, 32, {3, 5, 7, 91}));
    const auto original = untouched.image().rgba;
    check(!applySurfaceDecal(separated, untouched, PaintImage(1, 1), options).changed &&
              untouched.image().rgba == original && !untouched.canUndo(),
          "An explicit target material cannot make a connected decal jump to a separate shell");

    // Two faces meet at 90 degrees: the configured crease guard must stop propagation.
    auto vertical = quad(0, 1, 0, 1);
    for (auto& p : vertical.positions) {
        p[2] = -p[0];
        p[0] = 0;
    }
    vertical.normals.assign(4, F3{1, 0, 0});
    PaintSurface folded(scene({quad(-1, 0), vertical}));
    const auto foldSeed = hitAt(folded, V3(-.25, 0, 0));
    options = placement(foldSeed);
    options.width = options.height = 3;
    options.conformPatch = buildSurfaceDecalPatch(folded, foldSeed, options, 65);
    check(findDecalMaterials(folded, options).materials == std::vector<int>{0},
          "A sharp geometric fold blocks propagation instead of wrapping into an internal wall");

    auto sloped = quad(0, 1, 0, 1);
    const float sine45 = float(std::sqrt(.5));
    for (auto& p : sloped.positions) {
        p[2] = -p[0] * sine45;
        p[0] *= sine45;
    }
    sloped.normals.assign(4, F3{sine45, 0, sine45});
    PaintSurface controlledFold(scene({quad(-1, 0), sloped}));
    const auto controlledSeed = hitAt(controlledFold, V3(-.25, 0, 0));
    options = placement(controlledSeed);
    options.width = options.height = 3;
    options.conformPatch = buildSurfaceDecalPatch(controlledFold, controlledSeed, options, 30);
    check(findDecalMaterials(controlledFold, options).materials == std::vector<int>{0},
          "A lower crease threshold stops a moderately folded neighboring surface");
    options.conformPatch.reset();
    options.conformPatch = buildSurfaceDecalPatch(controlledFold, controlledSeed, options, 65);
    check(findDecalMaterials(controlledFold, options).materials == std::vector<int>{0, 1},
          "Raising the crease threshold deliberately permits the same moderate surface bend");

    auto external = quad(0, 1, 0, 1);
    external.extras = {{"edm_attachment", 0}};
    PaintSurface attachmentBoundary(scene({quad(-1, 0), external}));
    const auto boundarySeed = hitAt(attachmentBoundary, V3(-.25, 0, 0));
    options = placement(boundarySeed);
    options.width = options.height = 3;
    options.conformPatch = buildSurfaceDecalPatch(attachmentBoundary, boundarySeed, options);
    check(findDecalMaterials(attachmentBoundary, options).materials == std::vector<int>{0},
          "Exact contact with an external-store instance does not stitch it into the aircraft skin");

    PaintSurface nonmanifold(scene({quad(-1, 0), quad(0, 1, 0, 1), quad(0, 1, 0, 2)}));
    const auto seamSeed = hitAt(nonmanifold, V3(-.25, 0, 0));
    options = placement(seamSeed);
    options.width = options.height = 3;
    options.conformPatch = buildSurfaceDecalPatch(nonmanifold, seamSeed, options);
    check(findDecalMaterials(nonmanifold, options).materials == std::vector<int>{0},
          "An ambiguous three-face seam is stopped rather than choosing one overlapping shell arbitrarily");

    PaintSurface roundedSeam(scene({quad(-1, 0), quad(.000002f, 1, 0, 1)}));
    const auto roundedSeed = hitAt(roundedSeam, V3(-.25, 0, 0));
    options = placement(roundedSeed);
    options.width = options.height = 3;
    options.conformPatch = buildSurfaceDecalPatch(roundedSeam, roundedSeed, options);
    check(findDecalMaterials(roundedSeam, options).materials == std::vector<int>{0, 1},
          "Microscopic float rounding between matching mesh edges does not tear a painted seam");
    PaintSurface realGap(scene({quad(-1, 0), quad(.002f, 1, 0, 1)}));
    const auto gapSeed = hitAt(realGap, V3(-.25, 0, 0));
    options = placement(gapSeed);
    options.width = options.height = 3;
    options.conformPatch = buildSurfaceDecalPatch(realGap, gapSeed, options);
    check(findDecalMaterials(realGap, options).materials == std::vector<int>{0},
          "A real gap between nearby components is not filled by the seam tolerance");

    auto corner = quad(0, 1, 0, 1);
    for (auto& p : corner.positions)
        p[1] += 2;
    PaintSurface pointContact(scene({quad(-1, 0), corner}));
    const auto cornerSeed = hitAt(pointContact, V3(-.25, 0, 0));
    options = placement(cornerSeed);
    options.width = options.height = 5;
    options.conformPatch = buildSurfaceDecalPatch(pointContact, cornerSeed, options);
    check(findDecalMaterials(pointContact, options).materials == std::vector<int>{0},
          "A lone coincident vertex does not connect otherwise separate surfaces");
}

void aliasesAndReversedUV() {
    auto left = quad(-1, 0), right = quad(0, 1, 0, 1);
    for (auto& uv : left.uvs[0])
        uv[0] *= .5f;
    for (auto& uv : right.uvs[0])
        uv[0] = .5f + uv[0] * .5f;
    PaintSurface surface(scene({left, right}));
    auto options = placement(hitAt(surface));
    options.width = options.height = 2;
    options.conformPatch = buildSurfaceDecalPatch(surface, hitAt(surface), options);
    options.targetMaterials = {1, 0, 1};
    options.candidatePrimitives = std::vector<uint32_t>{0, 1, 2, 3, 0, 2};
    PaintCanvas target(PaintImage(64, 64, {0, 0, 0, 91}));
    const auto report = applySurfaceDecal(surface, target, PaintImage(1, 1, {255, 0, 0, 128}), options);
    check(report.paintedPixels == 4096 && channel(target, 15, 32) == 128 &&
              channel(target, 48, 32) == 128,
          "Shared texture aliases blend a curved placement once per texel despite duplicate candidates");

    right = quad(0, 1, 0, 1);
    for (auto& uv : right.uvs[0])
        uv[0] = 1 - uv[0];
    PaintSurface mirrored(scene({quad(-1, 0), right}));
    options = placement(hitAt(mirrored));
    options.width = options.height = 2;
    options.conformPatch = buildSurfaceDecalPatch(mirrored, hitAt(mirrored), options);
    options.material = 1;
    PaintCanvas flipped(PaintImage(64, 64, {0, 0, 0, 91}));
    applySurfaceDecal(mirrored, flipped, ramp(), options);
    check(channel(flipped, 0, 32) > 250 && std::abs(channel(flipped, 63, 32) - 128) <= 3,
          "Original reversed UV orientation is respected while surface artwork stays oriented globally");
}

void frozenEyeAndHiddenAttachments() {
    auto blocker = quad(-1, 0, .3f, 1);
    blocker.extras = {{"edm_attachment", 0}};
    auto source = scene({quad(), blocker});
    PaintSurface visible(source), hidden(source, {}, {}, nullptr, false);
    check(visible.triangleCount() == 4 && hidden.triangleCount() == 2,
          "Hidden attachment triangles are absent from the surface used for curved painting");
    const auto visibleSeed = hitAt(visible, V3(.25, 0, 0), 0);
    auto options = placement(visibleSeed);
    options.width = options.height = 3;
    options.occlusion = options.frontFacesOnly = true;
    options.projectionEye = V3(0, 0, 3);
    options.conformPatch = buildSurfaceDecalPatch(visible, visibleSeed, options);
    check(findDecalMaterials(visible, options).materials == std::vector<int>{0},
          "An external-store occluder is not automatically made part of the connected decal");
    PaintCanvas blocked(PaintImage(64, 64, {0, 0, 0, 91}));
    auto report = applySurfaceDecal(visible, blocked, PaintImage(1, 1, {255, 0, 0, 255}), options);
    check(report.occludedSamples > 0 && channel(blocked, 8, 32) == 0 &&
              channel(blocked, 48, 32) == 255,
          "Frozen placement-eye rays preserve occlusion by a disconnected visible external store");
    auto movedEye = options;
    movedEye.projectionEye = V3(1, 0, 3);
    fails([&] { findDecalMaterials(visible, movedEye); },
          "A camera eye changed after preview cannot silently change the baked curved footprint");
    fails([&] { findDecalMaterials(hidden, options); },
          "A visibility change invalidates the curved patch and its old occlusion assumptions");
    const auto hiddenSeed = hitAt(hidden, V3(.25, 0, 0));
    options.conformPatch.reset();
    options.conformPatch = buildSurfaceDecalPatch(hidden, hiddenSeed, options);
    PaintCanvas exposed(PaintImage(64, 64, {0, 0, 0, 91}));
    report = applySurfaceDecal(hidden, exposed, PaintImage(1, 1, {255, 0, 0, 255}), options);
    check(report.occludedSamples == 0 && report.paintedPixels == 4096 && channel(exposed, 8, 32) == 255,
          "Hiding the store removes its occlusion, allowing the decal to reach the aircraft beneath it");
    const auto storeSeed = hitAt(visible, V3(-.5, 0, 0), 1);
    options = placement(storeSeed);
    options.width = options.height = 3;
    options.projectionEye = V3(0, 0, 3);
    options.occlusion = options.frontFacesOnly = true;
    options.conformPatch = buildSurfaceDecalPatch(visible, storeSeed, options);
    check(findDecalMaterials(visible, options).materials == std::vector<int>{1},
          "Clicking the store builds its own connected decal patch without reaching the aircraft");
    PaintCanvas store(PaintImage(64, 64, {0, 0, 0, 91}));
    check(applySurfaceDecal(visible, store, PaintImage(1, 1, {255, 0, 0, 255}), options).changed,
          "A visible external store remains directly paintable with conforming decals");
}

void reversedSheetsAndNativeTopology() {
    // Some EDM meshes explicitly duplicate both sides with separate vertex indices and opposite
    // normals. Position-only welding sees four faces at every inner edge and incorrectly stops.
    auto doubled = quad();
    const auto front = doubled;
    doubled.positions.insert(doubled.positions.end(), front.positions.begin(), front.positions.end());
    doubled.normals.insert(doubled.normals.end(), 4, F3{0, 0, -1});
    doubled.uvs[0].insert(doubled.uvs[0].end(), front.uvs[0].begin(), front.uvs[0].end());
    doubled.indices.insert(doubled.indices.end(), {4, 6, 5, 4, 7, 6});
    PaintSurface surface(scene({doubled}));
    const auto facingFront = surface.raycast(V3(0, 0, 3), -V3::UnitZ());
    const auto facingBack = surface.raycast(V3(0, 0, -3), V3::UnitZ());
    check(facingFront && facingFront->primitive < 2 && facingFront->normal.z() > .99 &&
              facingBack && facingBack->primitive >= 2 && facingBack->normal.z() < -.99,
          "Coincident picking chooses the visible front/back twin instead of depending on BVH order");
    check(!surface.raycast(V3(0, 0, 3), -V3::UnitZ(), -1, 3 - 1e-10),
          "Coincident-facing tie resolution never extends the caller's hard maximum ray distance");
    PaintHit seed;
    seed.primitive = seed.triangle = 3;
    seed.position = V3::Zero();
    seed.barycentric = V3(.5, 0, .5);
    seed.normal = -V3::UnitZ();
    auto options = placement(seed);
    options.width = options.height = 2;
    options.normal = V3::UnitZ(); // The pointer chooses the visible side of this authored sheet.
    options.projectionEye = V3(0, 0, 3);
    options.frontFacesOnly = options.occlusion = true;
    options.conformPatch = buildSurfaceDecalPatch(surface, seed, options);
    check(options.conformPatch->normalSign == -1,
          "An inward-authored seed retains the visible-side normal orientation over its entire chart");
    check(options.conformPatch->triangles.size() == 2 &&
              std::all_of(options.conformPatch->triangles.begin(), options.conformPatch->triangles.end(),
                          [](const auto& triangle) { return triangle.primitive >= 2; }),
          "Native indexed edges follow one sheet without crossing its coincident opposite-facing duplicate");
    PaintCanvas canvas(PaintImage(64, 64, {0, 0, 0, 91}));
    auto report = applySurfaceDecal(surface, canvas, PaintImage(1, 1, {255, 0, 0, 255}), options);
    check(report.paintedPixels == 4096 && report.backfaceSamples == 0 && report.occludedSamples == 0,
          "Visible reversed-sheet texels paint normally with front-face and exact occlusion enabled");

    auto overlay = quad(-1, 1, 0, 1);
    overlay.indices.resize(3);
    PaintSurface layered(scene({quad(), overlay}));
    auto nativeSeed = hitAt(layered, V3(-.25, .25, 0), 0);
    options = placement(nativeSeed);
    options.width = options.height = 3;
    options.conformPatch = buildSurfaceDecalPatch(layered, nativeSeed, options);
    check(options.conformPatch->triangles.size() == 2 &&
              findDecalMaterials(layered, options).materials == std::vector<int>{0},
          "A coincident unrelated overlay neither breaks an indexed mesh's inner edge nor joins its chart");
    auto nearerSheet = quad(-1, 1, .00001f);
    nearerSheet.normals.assign(4, F3{0, 0, -1});
    PaintSurface separate(scene({nearerSheet, quad(-1, 1, 0, 1)}));
    const auto nearHit = separate.raycast(V3(0, 0, 3), -V3::UnitZ());
    check(nearHit && nearHit->material == 0 && nearHit->distance < 3,
          "A physically separate nearer surface still wins even when a farther surface faces the camera");
}

void movedPoseInvalidation() {
    auto blocker = quad(-1, 0, .3f, 1);
    blocker.node = 1;
    auto source = scene({quad(), blocker});
    source->nodes.resize(2);
    source->nodes[1].parent = 0;
    source->order = {0, 1};
    source->staticLocal = source->defaultWorld = {Mat::Identity(), Mat::Identity()};
    Track track;
    track.node = 1;
    track.arg = 38;
    track.channel = Channel::Position;
    track.keys = {{0, V4(0, 0, 0, 0)}, {1, V4(3, 0, 0, 0)}};
    source->tracks = {track};
    PaintSurface surface(source, {{38, 0}});
    const auto seed = hitAt(surface, V3(.25, 0, 0), 0);
    const auto seedTriangle = surface.triangle(seed.primitive);
    auto options = placement(seed);
    options.width = options.height = 3;
    options.frontFacesOnly = options.occlusion = true;
    options.projectionEye = V3(0, 0, 3);
    options.conformPatch = buildSurfaceDecalPatch(surface, seed, options);
    check(hitAt(surface, V3(-.5, 0, 0)).material == 1,
          "The initial animated occluder blocks the left aircraft skin");
    // Deliberately retain object address, Scene pointer, primitive count and exact seeded geometry.
    // Only a neighboring animated mesh changes, a case a seed-only stale check cannot detect.
    surface = PaintSurface(source, {{38, 1}});
    const auto after = surface.triangle(seed.primitive);
    check(surface.triangleCount() == 4 && hitAt(surface, V3(-.5, 0, 0)).material == 0 &&
              (after.world[0] - seedTriangle.world[0]).squaredNorm() == 0 &&
              (after.world[1] - seedTriangle.world[1]).squaredNorm() == 0 &&
              (after.world[2] - seedTriangle.world[2]).squaredNorm() == 0,
          "Animation moves only the occluder while the old seed and primitive numbering remain valid");
    fails([&] { findDecalMaterials(surface, options); },
          "Replacing an in-place pose invalidates the old chart even when its seed never moved");
    PaintCanvas target(PaintImage(64, 64, {0, 0, 0, 91}));
    fails([&] { applySurfaceDecal(surface, target, ramp(), options); },
          "Stale occlusion and neighboring connectivity cannot be baked after an in-place pose replacement");
    options.conformPatch.reset();
    options.conformPatch = buildSurfaceDecalPatch(surface, hitAt(surface, V3(.25, 0, 0)), options);
    const auto report = applySurfaceDecal(surface, target, PaintImage(1, 1, {255, 0, 0, 255}), options);
    check(report.paintedPixels == 4096 && report.occludedSamples == 0,
          "Rebuilding the chart after animation reaches the newly exposed aircraft surface");
}

void cancellationAndInvalidInputs() {
    PaintSurface surface(cylinder());
    const auto seed = hitAt(surface);
    auto options = placement(seed);
    std::atomic_bool cancel = true;
    fails([&] { buildSurfaceDecalPatch(surface, seed, options, 65, {}, &cancel); },
          "Cancelled topology construction stops before publishing a usable patch");
    cancel = false;
    auto invalid = seed;
    invalid.primitive = uint32_t(surface.triangleCount());
    fails([&] { buildSurfaceDecalPatch(surface, invalid, options); },
          "A seed primitive outside the current posed surface is rejected");
    invalid = seed;
    invalid.position.x() = std::numeric_limits<double>::quiet_NaN();
    fails([&] { buildSurfaceDecalPatch(surface, invalid, options); },
          "Non-finite seed positions are rejected");
    fails([&] { buildSurfaceDecalPatch(surface, seed, options, -1); },
          "An invalid crease angle is rejected instead of silently changing connectivity");
    auto missingEye = options;
    missingEye.occlusion = true;
    fails([&] { buildSurfaceDecalPatch(surface, seed, missingEye); },
          "An occluded curved placement requires an explicit frozen eye");
    options.conformPatch = buildSurfaceDecalPatch(surface, seed, options);
    PaintCanvas canvas(PaintImage(256, 64, {3, 5, 7, 91}));
    const auto original = canvas.image().rgba;
    cancel = true;
    fails([&] { applySurfaceDecal(surface, canvas, ramp(), options, {}, &cancel); },
          "Pre-cancelled curved rasterization is explicit");
    check(canvas.image().rgba == original && !canvas.canUndo() && !canvas.strokeActive(),
          "Cancelled curved rasterization leaves pixels, layers and undo untouched");
    auto limited = options;
    limited.limits.rasterSamples = 1000;
    fails([&] { applySurfaceDecal(surface, canvas, ramp(), limited); },
          "Curved rasterization enforces the work budget after some texels have been visited");
    check(canvas.image().rgba == original && !canvas.canUndo() && !canvas.strokeActive(),
          "A curved work-budget failure rolls back partial pixels and releases its transaction");
    limited.externalStroke = true;
    canvas.beginStroke("External curved batch failure");
    fails([&] { applySurfaceDecal(surface, canvas, ramp(), limited); },
          "An externally owned curved operation reports the same accumulated raster limit");
    check(canvas.strokeActive() && canvas.image().rgba != original,
          "A failed external operation leaves its partial writes under the batch owner's control");
    canvas.cancelStroke();
    check(canvas.image().rgba == original && !canvas.canUndo() && !canvas.strokeActive(),
          "The batch owner can fully roll back curved partial writes after a failure");
    auto changedPlacement = options;
    changedPlacement.center.x() += .1;
    fails([&] { applySurfaceDecal(surface, canvas, ramp(), changedPlacement); },
          "Moving the decal requires a new curved patch rather than reusing an old preview");
    changedPlacement = options;
    changedPlacement.width *= 2;
    fails([&] { findDecalMaterials(surface, changedPlacement); },
          "Resizing the decal invalidates a patch clipped to the previous footprint");
    options.candidatePrimitives = std::vector<uint32_t>{};
    check(findDecalMaterials(surface, options).materials.empty(),
          "An explicitly empty curved candidate cache maps no materials");
    options.candidatePrimitives = std::vector<uint32_t>{uint32_t(surface.triangleCount())};
    fails([&] { applySurfaceDecal(surface, canvas, ramp(), options); },
          "Out-of-range cached candidates are rejected before writing texels");
    options.candidatePrimitives.reset();
    PaintSurface rebuilt(cylinder());
    fails([&] { applySurfaceDecal(rebuilt, canvas, ramp(), options); },
          "A patch from a previous surface cannot be reused after a pose or visibility rebuild");
    check(canvas.image().rgba == original && !canvas.canUndo(),
          "Malformed or stale curved operations leave the original texture intact");
}
} // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        ComRuntime runtime;
        cylinderArcAndOriginals();
        materialSeamsAndLayerHistory();
        rotatedCurvedCoordinates();
        topologyBoundaries();
        aliasesAndReversedUV();
        frozenEyeAndHiddenAttachments();
        reversedSheetsAndNativeTopology();
        movedPoseInvalidation();
        cancellationAndInvalidInputs();
        std::cout << "Conforming decal: " << checks << " checks passed\n";
        return 0;
    } catch (...) {
        std::cerr << exceptionText() << '\n';
        return 1;
    }
}
