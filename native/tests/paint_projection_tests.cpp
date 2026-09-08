#include "paint_projection.h"
#include <iostream>

namespace {
using namespace edm;
int checks = 0;
void check(bool condition, const char* message) {
    require(condition, std::string("Camera image projection: ") + message);
    ++checks;
}

std::shared_ptr<Scene> scene() {
    auto result = std::make_shared<Scene>();
    result->nodes.resize(1);
    result->staticLocal = {Mat::Identity()};
    result->defaultWorld = {Mat::Identity()};
    result->order = {0};
    result->materials.resize(2);
    return result;
}

void quad(Scene& scene, float x0 = -1, float x1 = 1, float z = 0, int material = 0) {
    Mesh mesh;
    mesh.material = material;
    mesh.positions = {{x0, -1, z}, {x1, -1, z}, {x1, 1, z}, {x0, 1, z}};
    mesh.normals.assign(4, F3{0, 0, 1});
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.uvs = {{{0, 1}, {1, 1}, {1, 0}, {0, 0}}};
    scene.meshes.push_back(std::move(mesh));
}

CameraProjectionOptions camera() {
    CameraProjectionOptions options;
    options.material = 0;
    options.eye = V3(0, 0, 2);
    options.size = {.5f, .5f};
    // Independent perspective matrix: 90 degree FOV, near .1, far 10, camera looking down -Z.
    Mat projection = Mat::Zero();
    projection(0, 0) = projection(1, 1) = 1;
    projection(2, 2) = -10. / 9.9;
    projection(2, 3) = -1. / 9.9;
    projection(3, 2) = -1;
    options.viewProjection = projection * translation(V3(0, 0, -2));
    return options;
}

int channel(const PaintCanvas& canvas, int x, int y, int component = 0) {
    return canvas.image().rgba[(size_t(y) * canvas.image().width + x) * 4 + component];
}

template <class Function> void rejected(Function function, const char* message) {
    bool failed = false;
    try {
        function();
    } catch (const std::exception&) {
        failed = true;
    }
    check(failed, message);
}
} // namespace

void runPaintProjectionTests() {
    using namespace edm;
    auto model = scene();
    quad(*model);
    PaintSurface surface(model);
    auto options = camera();
    PaintImage image(2, 2);
    image.rgba = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    PaintCanvas canvas(PaintImage(32, 32, {0, 0, 0, 255}));
    auto report = applyCameraProjection(surface, canvas, image, options);
    check(report.paintedPixels == 32 * 32 && report.changed, "perspective image fills the expected UV area");
    check(channel(canvas, 3, 3) == 255 && channel(canvas, 3, 3, 1) == 0 && channel(canvas, 28, 3, 1) == 255 &&
              channel(canvas, 3, 28, 2) == 255,
          "camera image orientation matches viewport top-left coordinates");
    auto painted = canvas.image().rgba;
    check(canvas.undo() && channel(canvas, 3, 3) == 0, "projection is a single undo step");
    check(canvas.redo() && canvas.image().rgba == painted, "redo restores projected pixels exactly");

    PaintCanvas turned(PaintImage(32, 32, {0, 0, 0, 255}));
    options.rotationRadians = 3.14159265358979323846 / 2;
    applyCameraProjection(surface, turned, image, options);
    check(channel(turned, 3, 3, 2) == 255 && channel(turned, 3, 3, 0) == 0,
          "rotation uses viewport pixels and rotates the image clockwise");

    PaintCanvas wideViewport(PaintImage(32, 32, {0, 0, 0, 255}));
    options.viewportAspect = 2;
    options.viewProjection.row(0) *= .5;
    options.size[0] = .25f;
    applyCameraProjection(surface, wideViewport, image, options);
    check(wideViewport.image().rgba == turned.image().rgba,
          "rotation preserves its physical placement on a non-square viewport");

    options = camera();
    options.size = {.25f, .25f};
    options.center = {.625f, .5f};
    PaintImage red(1, 1, {255, 0, 0, 255});
    PaintCanvas placed(PaintImage(32, 32, {0, 0, 0, 255}));
    report = applyCameraProjection(surface, placed, red, options);
    check(channel(placed, 24, 16) == 255 && channel(placed, 8, 16) == 0 && channel(placed, 24, 1) == 0,
          "image centre and size limit the projected footprint");

    auto occluded = scene();
    quad(*occluded);
    quad(*occluded, -1.1f, 0, .5f, 1);
    PaintSurface occludedSurface(occluded);
    PaintCanvas masked(PaintImage(32, 32, {0, 0, 0, 255}));
    options = camera();
    report = applyCameraProjection(occludedSurface, masked, red, options);
    check(channel(masked, 5, 16) == 0 && channel(masked, 26, 16) == 255 && report.occludedSamples > 0,
          "another material blocks camera rays without blocking exposed target pixels");
    options.occlusion = false;
    report = applyCameraProjection(occludedSurface, masked, red, options);
    check(channel(masked, 5, 16) == 255, "explicit through-surface option disables occlusion");

    auto reversed = scene();
    quad(*reversed);
    reversed->meshes[0].normals.assign(4, F3{0, 0, -1});
    PaintSurface reversedSurface(reversed);
    PaintCanvas back(PaintImage(32, 32, {0, 0, 0, 255}));
    options = camera();
    report = applyCameraProjection(reversedSurface, back, red, options);
    check(!report.changed && report.backfaceSamples > 0, "back-facing surfaces are excluded");
    options.frontFacesOnly = false;
    report = applyCameraProjection(reversedSurface, back, red, options);
    check(report.changed, "back-face painting can be explicitly enabled");

    auto mirrored = scene();
    quad(*mirrored);
    mirrored->staticLocal[0] = scaling(V3(-1, 1, 1));
    PaintSurface mirroredSurface(mirrored);
    PaintCanvas mirror(PaintImage(32, 32, {0, 0, 0, 255}));
    options = camera();
    report = applyCameraProjection(mirroredSurface, mirror, image, options);
    check(report.changed && channel(mirror, 3, 3, 1) == 255,
          "mirrored transforms use world normals and the actual reflected UV pose");

    auto shifted = scene();
    quad(*shifted);
    Track track;
    track.node = 0;
    track.arg = 9;
    track.channel = Channel::Position;
    track.keys = {{0, V4::Zero()}, {1, V4(1, 0, 0, 0)}};
    shifted->tracks.push_back(track);
    PaintSurface animated(shifted, {{9, .5}});
    PaintCanvas posed(PaintImage(32, 32, {0, 0, 0, 255}));
    report = applyCameraProjection(animated, posed, red, options);
    check(channel(posed, 10, 16) == 255 && channel(posed, 30, 16) == 0,
          "projection follows animated world positions rather than rest-pose geometry");

    auto clipped = scene();
    quad(*clipped, -1, 1, 3);
    PaintSurface clippedSurface(clipped);
    PaintCanvas clippedCanvas(PaintImage(32, 32, {0, 0, 0, 255}));
    options.frontFacesOnly = false;
    report = applyCameraProjection(clippedSurface, clippedCanvas, red, options);
    check(!report.changed, "geometry behind the camera is clipped before projection");
    options.viewProjection(2, 3) = 100;
    report = applyCameraProjection(surface, clippedCanvas, red, options);
    check(!report.changed, "Direct3D far clipping is respected");

    auto duplicated = scene();
    quad(*duplicated);
    quad(*duplicated);
    PaintSurface duplicateSurface(duplicated);
    PaintCanvas alpha(PaintImage(32, 32, {0, 0, 0, 91}));
    PaintImage halfRed(1, 1, {255, 0, 0, 128});
    options = camera();
    report = applyCameraProjection(duplicateSurface, alpha, halfRed, options);
    check(report.reusedTexels > 0 && channel(alpha, 16, 16) >= 127 && channel(alpha, 16, 16) <= 129 &&
              channel(alpha, 16, 16, 3) == 91,
          "overlapping UVs do not compound source opacity or modify the existing alpha mask");
    options.mesh = 1;
    PaintCanvas meshOnly(PaintImage(16, 16, {0, 0, 0, 255}));
    report = applyCameraProjection(duplicateSurface, meshOnly, red, options);
    check(report.triangles == 2 && report.changed, "target mesh selection is honoured");

    options = camera();
    options.size[0] = 0;
    rejected([&] { applyCameraProjection(surface, canvas, red, options); }, "zero image size is rejected");
    check(!canvas.strokeActive() && canvas.image().rgba == painted,
          "invalid image settings preserve prior work");
    options = camera();
    options.limits.rayTests = 10;
    PaintCanvas budget(PaintImage(32, 32, {0, 0, 0, 255}));
    auto before = budget.image().rgba;
    rejected([&] { applyCameraProjection(surface, budget, red, options); }, "ray budget is enforced");
    check(budget.image().rgba == before && !budget.strokeActive() && !budget.canUndo(),
          "ray-budget failure atomically rolls back earlier pixels");

    options = camera();
    options.occlusion = false;
    PaintCanvas cancelled(PaintImage(512, 256, {0, 0, 0, 255}));
    before = cancelled.image().rgba;
    std::atomic_bool cancel = false;
    int callbacks = 0;
    rejected(
        [&] {
            applyCameraProjection(
                surface, cancelled, red, options,
                [&](const std::string&) {
                    if (++callbacks == 2)
                        cancel = true;
                },
                &cancel);
        },
        "projection can be cancelled after partial progress");
    check(callbacks >= 2 && cancelled.image().rgba == before && !cancelled.strokeActive() &&
              !cancelled.canUndo(),
          "cancel rolls back the entire image projection");
    std::cout << "Camera projection: " << checks << " checks passed\n";
}

int wmain() {
    try {
        edm::ComRuntime imageRuntime;
        runPaintProjectionTests();
        return 0;
    } catch (...) {
        std::cerr << edm::exceptionText() << '\n';
        return 1;
    }
}
