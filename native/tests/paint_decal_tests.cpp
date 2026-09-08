#include "paint_decal.h"
#include "paint_batch.h"
#include <iostream>

using namespace edm;
namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    require(value, std::string("Surface decal regression: ") + message);
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
Mesh quad(float left = -1, float right = 1, float bottom = -1, float top = 1, float z = 0, int material = 0) {
    Mesh mesh;
    mesh.material = material;
    mesh.positions = {{left, bottom, z}, {right, bottom, z}, {right, top, z}, {left, top, z}};
    mesh.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.uvs = {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    return mesh;
}
std::shared_ptr<Scene> scene(std::vector<Mesh> meshes) {
    auto source = std::make_shared<Scene>();
    source->materials.resize(8);
    source->nodes.resize(1);
    source->order = {0};
    source->staticLocal = source->defaultWorld = {Mat::Identity()};
    source->meshes = std::move(meshes);
    return source;
}
int channel(const PaintCanvas& canvas, int x, int y, int component = 0) {
    return canvas.image().rgba[(size_t(y) * canvas.image().width + x) * 4 + component];
}
PaintImage corners() {
    PaintImage image(2, 2);
    image.rgba = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};
    return image;
}
SurfaceDecalOptions options() {
    SurfaceDecalOptions result;
    result.width = result.height = 2;
    result.depth = .1;
    return result;
}
void frameAndPlacement() {
    auto o = options();
    o.normal = V3(0, 0, 4);
    o.tangent = V3(3, 0, 7);
    o.rotationRadians = 3.14159265358979323846 / 2;
    auto frame = surfaceDecalFrame(o);
    check((frame.right - V3::UnitY()).norm() < 1e-12 && (frame.up + V3::UnitX()).norm() < 1e-12,
          "Rotation and projected tangent produce a right-handed orthonormal frame");
    check((frame.right.cross(frame.up) - frame.normal).norm() < 1e-12,
          "CPU frame has the same outward normal as renderer placement");
    auto invalid = o;
    invalid.normal = V3::Zero();
    fails([&] { surfaceDecalFrame(invalid); }, "Zero decal normal is rejected");
    invalid = o;
    invalid.tangent = invalid.normal;
    fails([&] { surfaceDecalFrame(invalid); }, "Parallel tangent is rejected");
    invalid = o;
    invalid.depth = 0;
    fails([&] { surfaceDecalFrame(invalid); }, "Zero depth is rejected explicitly");
    invalid = o;
    invalid.width = std::numeric_limits<double>::infinity();
    fails([&] { surfaceDecalFrame(invalid); }, "Non-finite dimensions are rejected");

    PaintSurface surface(scene({quad()}));
    PaintCanvas target(PaintImage(64, 64, {0, 0, 0, 91}));
    auto source = corners();
    o = options();
    auto report = applySurfaceDecal(surface, target, source, o);
    check(report.changed && report.paintedPixels == 4096, "Default decal covers its finite rectangle");
    check(channel(target, 15, 48) == 255 && channel(target, 15, 48, 1) == 0 &&
              channel(target, 48, 48, 1) == 255 && channel(target, 15, 15, 2) == 255,
          "Image U points right and image V points down relative to the supplied surface frame");
    check(channel(target, 15, 48, 3) == 91, "Decal preserves original alpha mask by default");
    o.width = 1;
    o.height = .5;
    o.rotationRadians = 3.14159265358979323846 / 2;
    PaintCanvas rotated(PaintImage(64, 64, {0, 0, 0, 255}));
    auto red = PaintImage(1, 1, {255, 0, 0, 255});
    auto rotatedReport = applySurfaceDecal(surface, rotated, red, o);
    check(rotatedReport.paintedPixels == 512 && channel(rotated, 31, 17) == 255 &&
              channel(rotated, 17, 31) == 0,
          "Width and height rotate in the tangent plane without perspective distortion");
    o.center = V3(.5, 0, 0);
    o.rotationRadians = 0;
    o.width = .5;
    o.height = 1;
    PaintCanvas moved(PaintImage(64, 64, {0, 0, 0, 255}));
    applySurfaceDecal(surface, moved, red, o);
    check(channel(moved, 47, 31) == 255 && channel(moved, 31, 31) == 0,
          "Surface decal center moves the finite footprint in world units");

    o = options();
    o.center = V3(2, 3, 4);
    o.normal = V3::UnitX();
    o.tangent = V3::UnitY();
    auto side = quad();
    for (size_t i = 0; i < side.positions.size(); ++i) {
        auto p = side.positions[i];
        side.positions[i] = {2, 3 + p[0], 4 + p[1]};
        side.normals[i] = {1, 0, 0};
    }
    PaintSurface sideSurface(scene({side}));
    PaintCanvas sideCanvas(PaintImage(64, 64, {0, 0, 0, 255}));
    applySurfaceDecal(sideSurface, sideCanvas, source, o);
    check(channel(sideCanvas, 15, 48) == 255 && channel(sideCanvas, 15, 15, 2) == 255,
          "An arbitrary model normal defines placement independently of the camera or cylinder axis");
}
void depthAndOcclusion() {
    auto source = PaintImage(1, 1, {255, 0, 0, 255});
    auto o = options();
    PaintSurface layered(scene({quad(), quad(-1, 1, -1, 1, -.05f, 1), quad(-1, 1, -1, 1, -.3f, 2)}));
    auto candidates = findDecalMaterials(layered, o);
    check(candidates.materials == std::vector<int>{0, 1}, "Thin depth volume excludes distant layers");
    PaintCanvas hidden(PaintImage(64, 64, {0, 0, 0, 255}));
    o.material = 1;
    auto report = applySurfaceDecal(layered, hidden, source, o);
    check(!report.changed && report.occludedSamples > 100 && !hidden.canUndo(),
          "Parallel whole-model rays prevent painting an interior layer of another material");
    o.occlusion = false;
    check(applySurfaceDecal(layered, hidden, source, o).changed,
          "Disabling occlusion deliberately allows the depth layer");

    auto rear = quad();
    for (auto& n : rear.normals)
        n = {0, 0, -1};
    PaintSurface back(scene({rear}));
    o = options();
    check(findDecalMaterials(back, o).materials.empty(), "Back-facing surfaces are excluded by default");
    PaintCanvas backCanvas(PaintImage(32, 32, {0, 0, 0, 255}));
    check(!applySurfaceDecal(back, backCanvas, source, o).changed, "Back-facing texels are not painted");
    o.frontFacesOnly = false;
    check(applySurfaceDecal(back, backCanvas, source, o).changed, "Back-face override remains explicit");

    PaintSurface overhang(scene({quad(), quad(-1, 0, -1, 1, .3f, 1)}));
    o = options();
    o.material = 0;
    PaintCanvas partlyHidden(PaintImage(64, 64, {0, 0, 0, 255}));
    report = applySurfaceDecal(overhang, partlyHidden, source, o);
    check(channel(partlyHidden, 15, 31) == 0 && channel(partlyHidden, 48, 31) == 255 &&
              report.occludedSamples > 100,
          "Geometry ahead of the finite depth box still blocks the parallel projection without hiding "
          "exposed slivers");

    auto varying = quad();
    varying.normals[0] = {0, 0, -1};
    varying.normals[1] = {0, 0, -1};
    PaintSurface changingNormals(scene({varying}));
    o = options();
    o.occlusion = false;
    PaintCanvas varied(PaintImage(64, 64, {0, 0, 0, 255}));
    report = applySurfaceDecal(changingNormals, varied, source, o);
    check(!findDecalMaterials(changingNormals, o).materials.empty() && report.backfaceSamples > 0 &&
              report.paintedPixels > 0,
          "Discovery preserves mixed-normal triangles and rejects back-facing portions per texel");
}
void materialsAndHistory() {
    PaintSurface surface(scene({quad(-1, 0, -1, 1, 0, 0), quad(0, 1, -1, 1, 0, 1), quad(4, 5, -1, 1, 0, 2)}));
    auto o = options();
    auto red = PaintImage(1, 1, {255, 0, 0, 128});
    auto candidates = findDecalMaterials(surface, o);
    check(candidates.materials == std::vector<int>{0, 1} && candidates.candidateTriangles == 4,
          "Decal discovery locates both materials across a seam without loading textures");
    auto left = std::make_shared<PaintCanvas>(PaintImage(128, 64, {0, 0, 0, 91}));
    auto right = std::make_shared<PaintCanvas>(PaintImage(64, 128, {0, 0, 0, 91}));
    auto beforeLeft = left->image().rgba, beforeRight = right->image().rgba;
    PaintBatchHistory batch;
    batch.begin("Surface decal across materials");
    o.externalStroke = true;
    o.material = 0;
    o.candidatePrimitives = candidates.primitivesByMaterial.at(0);
    auto first = applySurfaceDecal(surface, batch.touch(0, left), red, o);
    o.material = 1;
    o.candidatePrimitives = candidates.primitivesByMaterial.at(1);
    auto second = applySurfaceDecal(surface, batch.touch(1, right), red, o);
    check(first.changed && second.changed && left->strokeActive() && right->strokeActive(),
          "External decal mapping leaves all material transactions open for the common owner");
    batch.commit();
    auto afterLeft = left->image().rgba, afterRight = right->image().rgba;
    check(channel(*left, 127, 31) == 128 && channel(*right, 0, 63) == 128,
          "One world-space decal crosses different-resolution material boundaries continuously");
    check(batch.undo() && left->image().rgba == beforeLeft && right->image().rgba == beforeRight,
          "One batch undo restores every decal material");
    check(batch.redo() && left->image().rgba == afterLeft && right->image().rgba == afterRight,
          "One batch redo restores the complete decal");

    PaintSurface aliases(scene({quad(), quad(-1, 1, -1, 1, 0, 1)}));
    o = options();
    o.targetMaterials = {1, 0, 1};
    o.candidatePrimitives = std::vector<uint32_t>{0, 1, 2, 3, 0, 2};
    PaintCanvas aliasCanvas(PaintImage(32, 32, {0, 0, 0, 91}));
    auto report = applySurfaceDecal(aliases, aliasCanvas, red, o);
    check(report.paintedPixels == 1024 && channel(aliasCanvas, 10, 10) == 128 && report.reusedTexels > 0,
          "Shared material aliases and duplicate candidate IDs apply alpha only once per texel");
    check(report.triangles == 4, "Duplicate cached candidates do not duplicate raster work");
    o.candidatePrimitives = std::vector<uint32_t>{};
    check(findDecalMaterials(aliases, o).materials.empty() &&
              !applySurfaceDecal(aliases, aliasCanvas, red, o).changed,
          "Explicitly empty candidate cache maps nothing");
    o.candidatePrimitives = std::vector<uint32_t>{999};
    fails([&] { applySurfaceDecal(aliases, aliasCanvas, red, o); },
          "Invalid posed-surface candidate IDs are rejected");
}
void referenceDecal(PaintCanvas& canvas, const PaintSurface& surface, const PaintImage& image,
                    const SurfaceDecalOptions& o) {
    auto frame = surfaceDecalFrame(o);
    paint_mapping::Operation operation(canvas, "Full triangle decal reference", o.limits, {}, nullptr);
    for (uint32_t i = 0; i < surface.triangleCount(); ++i) {
        auto triangle = surface.triangle(i);
        if (!paint_mapping::targetTriangle(triangle, o.material, o.mesh, o.targetMaterials))
            continue;
        PaintRasterOptions raster;
        raster.maxPixels = 16ull * 1024 * 1024;
        rasterizePaintTriangle(
            triangle, canvas.image().width, canvas.image().height,
            [&](int x, int y, const V3& b) {
                V3 p = paint_mapping::position(triangle, b), d = p - frame.center;
                double dx = d.dot(frame.right), dy = d.dot(frame.up), dz = d.dot(frame.normal);
                if (std::abs(dx) > frame.width * .5 || std::abs(dy) > frame.height * .5 ||
                    std::abs(dz) > frame.depth)
                    return;
                V3 n =
                    triangle.normals[0] * b.x() + triangle.normals[1] * b.y() + triangle.normals[2] * b.z();
                if (o.frontFacesOnly && n.dot(frame.normal) <= 1e-8)
                    return;
                operation.blend(
                    x, y,
                    paint_mapping::sample(image, .5 + dx / frame.width, .5 - dy / frame.height, o.opacity),
                    o.preserveAlpha);
            },
            raster);
    }
    operation.finish();
}
void finiteClippingAndReference() {
    auto mesh = quad();
    for (size_t i = 0; i < mesh.positions.size(); ++i) {
        mesh.positions[i][2] = mesh.positions[i][0] * .13f + mesh.positions[i][1] * .07f;
        mesh.normals[i] = {-.13f, -.07f, 1};
        mesh.uvs[0][i][0] = mesh.uvs[0][i][0] * 1.4f - .3f;
        mesh.uvs[0][i][1] = mesh.uvs[0][i][1] * 1.7f + mesh.uvs[0][i][0] * .17f - .4f;
    }
    PaintSurface surface(scene({mesh}));
    auto image = corners();
    for (int i = 0; i < 24; ++i) {
        auto o = options();
        o.occlusion = false;
        o.opacity = .63f;
        o.preserveAlpha = i % 2;
        o.center = V3((i % 6 - 2.5) * .28, (i / 6 - 1.5) * .31, 0);
        o.rotationRadians = i * .237;
        o.width = .1 + (i % 5) * .21;
        o.height = .09 + (i % 4) * .18;
        o.depth = .05 + (i % 3) * .08;
        PaintImage base(i % 2 ? 65 : 96, i % 3 ? 63 : 32, {13, 57, 91, 78});
        PaintCanvas actual(base), reference(base);
        auto found = findDecalMaterials(surface, o);
        std::vector<uint32_t> cached;
        for (const auto& [material, ids] : found.primitivesByMaterial)
            cached.insert(cached.end(), ids.begin(), ids.end());
        o.candidatePrimitives = cached;
        applySurfaceDecal(surface, actual, image, o);
        referenceDecal(reference, surface, image, o);
        check(actual.image().rgba == reference.image().rgba,
              "Finite-box clipping and discovered candidates match full-triangle reference bytes for "
              "rotation, tilted faces, depth and repeating UVs");
    }
    auto longTriangle = quad();
    longTriangle.positions = {{-100, -100, 0}, {100, -100, 0}, {0, 100, 0}};
    longTriangle.indices = {0, 1, 2};
    longTriangle.normals.resize(3);
    longTriangle.uvs = {{{0, 0}, {1, 0}, {.5f, 1}}};
    PaintSurface crossing(scene({longTriangle}));
    auto o = options();
    o.width = o.height = 1;
    o.occlusion = false;
    auto found = findDecalMaterials(crossing, o);
    check(found.candidateTriangles == 1,
          "A huge triangle crossing the decal survives even when all vertices lie outside");
    PaintCanvas big(PaintImage(2048, 2048, {0, 0, 0, 255}));
    o.limits.rasterSamples = 2048;
    auto report = applySurfaceDecal(crossing, big, PaintImage(1, 1, {255, 0, 0, 255}), o);
    check(report.changed && report.rasterSamples < 200 && report.paintedPixels > 0,
          "Local decal on a 2K whole-UV large triangle does not rasterize the whole texture");
    auto sliver = longTriangle;
    sliver.positions = {{0, 0, 0}, {100, 100, 0}, {100, 100.001f, 0}};
    sliver.uvs = {{{0, 0}, {1, 0}, {0, 1}}};
    PaintSurface thin(scene({sliver}));
    o = options();
    o.center = V3(.05, .05, 0);
    o.width = o.height = .04;
    o.occlusion = false;
    o.limits.rasterSamples = 256;
    PaintCanvas thinCanvas(PaintImage(2048, 2048, {0, 0, 0, 255}));
    report = applySurfaceDecal(thin, thinCanvas, PaintImage(1, 1, {255, 0, 0, 255}), o);
    check(report.changed && report.rasterSamples < 32,
          "Finite decal clipping bounds a long thin world triangle with a huge infinite-plane UV projection");
}
void cancellationAndBudgets() {
    PaintSurface surface(scene({quad()}));
    auto red = PaintImage(1, 1, {255, 0, 0, 255});
    auto o = options();
    PaintCanvas canvas(PaintImage(512, 512, {0, 0, 0, 91}));
    auto original = canvas.image().rgba;
    std::atomic_bool cancel = true;
    fails([&] { applySurfaceDecal(surface, canvas, red, o, {}, &cancel); },
          "Pre-cancelled decal is rejected");
    check(canvas.image().rgba == original && !canvas.strokeActive() && !canvas.canUndo(),
          "Pre-cancellation leaves pixels and history unchanged");
    cancel = false;
    size_t progressCalls = 0;
    fails(
        [&] {
            applySurfaceDecal(
                surface, canvas, red, o,
                [&](const std::string&) {
                    if (++progressCalls == 2)
                        cancel = true;
                },
                &cancel);
        },
        "Decal cancellation during raster work is explicit");
    check(progressCalls >= 2 && canvas.image().rgba == original && !canvas.strokeActive() &&
              !canvas.canUndo(),
          "Mid-raster cancellation rolls back already painted texels and creates no undo operation");
    o.limits.rayTests = 32;
    fails([&] { applySurfaceDecal(surface, canvas, red, o); },
          "Ray budget failure terminates decal projection");
    check(canvas.image().rgba == original && !canvas.strokeActive(),
          "Ray budget failure fully restores target pixels");
    o = options();
    o.limits.rasterSamples = 100;
    fails([&] { applySurfaceDecal(surface, canvas, red, o); },
          "Raster work budget is enforced after clipping");
    o = options();
    o.limits.seconds = 1e-300;
    fails([&] { findDecalMaterials(surface, o); }, "Candidate discovery respects the same time budget");
    o = options();
    o.externalStroke = true;
    o.limits.rayTests = 32;
    auto a = std::make_shared<PaintCanvas>(PaintImage(32, 32, {0, 0, 0, 91}));
    auto b = std::make_shared<PaintCanvas>(PaintImage(512, 512, {0, 0, 0, 91}));
    auto beforeA = a->image().rgba, beforeB = b->image().rgba;
    PaintBatchHistory batch;
    batch.begin();
    batch.touch(0, a).blendPixel(1, 1, {0, 1, 0, 1});
    batch.touch(1, b);
    fails([&] { applySurfaceDecal(surface, *b, red, o); },
          "External decal failure remains owned by the group transaction");
    batch.cancel();
    check(a->image().rgba == beforeA && b->image().rgba == beforeB && !a->strokeActive() &&
              !b->strokeActive(),
          "One external cancellation restores prior materials and the partially projected material");
}
} // namespace
int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        ComRuntime runtime;
        frameAndPlacement();
        depthAndOcclusion();
        materialsAndHistory();
        finiteClippingAndReference();
        cancellationAndBudgets();
        std::cout << "Surface decal: " << checks << " checks passed\n";
        return 0;
    } catch (...) {
        std::cerr << exceptionText() << '\n';
        return 1;
    }
}
