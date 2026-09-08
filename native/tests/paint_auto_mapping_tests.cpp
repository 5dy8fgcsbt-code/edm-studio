#include "paint_auto_mapping.h"
#include <iostream>
#include <random>

namespace {
using namespace edm;
constexpr double pi = 3.14159265358979323846;
int checks = 0;
void check(bool condition, const char* message) {
    require(condition, std::string("Automatic image mapping: ") + message);
    ++checks;
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

std::shared_ptr<Scene> scene(size_t materials = 16) {
    auto result = std::make_shared<Scene>();
    result->nodes.resize(1);
    result->staticLocal = {Mat::Identity()};
    result->defaultWorld = {Mat::Identity()};
    result->order = {0};
    result->materials.resize(materials);
    return result;
}
void triangle(Scene& scene, const std::array<F3, 3>& points, int material, F3 normal = {0, 0, 1}) {
    Mesh mesh;
    mesh.material = material;
    mesh.positions.assign(points.begin(), points.end());
    mesh.normals.assign(3, normal);
    mesh.indices = {0, 1, 2};
    mesh.uvs = {{{0, 0}, {1, 0}, {0, 1}}};
    scene.meshes.push_back(std::move(mesh));
}
void quad(Scene& scene, float x0, float x1, float z, int material, bool forward = true) {
    Mesh mesh;
    mesh.material = material;
    mesh.positions = {{x0, -1, z}, {x1, -1, z}, {x1, 1, z}, {x0, 1, z}};
    mesh.normals.assign(4, F3{0, 0, forward ? 1.f : -1.f});
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.uvs = {{{0, 1}, {1, 1}, {1, 0}, {0, 0}}};
    scene.meshes.push_back(std::move(mesh));
}
void cylinderBand(Scene& scene, float x0, float x1, int material, float radius = 1, double start = -pi,
                  double end = pi, bool outward = true) {
    Mesh mesh;
    mesh.material = material;
    mesh.uvs.resize(1);
    constexpr int segments = 32;
    for (int i = 0; i <= segments; ++i) {
        double angle = start + (end - start) * i / segments;
        float y = float(std::cos(angle)), z = float(std::sin(angle));
        mesh.positions.push_back({x0, radius * y, radius * z});
        mesh.positions.push_back({x1, radius * y, radius * z});
        F3 normal{0, outward ? y : -y, outward ? z : -z};
        mesh.normals.push_back(normal);
        mesh.normals.push_back(normal);
        mesh.uvs[0].push_back({float(i) / segments, 1});
        mesh.uvs[0].push_back({float(i) / segments, 0});
        if (i < segments) {
            uint32_t base = uint32_t(i * 2);
            mesh.indices.insert(mesh.indices.end(), {base, base + 3, base + 1, base, base + 2, base + 3});
        }
    }
    scene.meshes.push_back(std::move(mesh));
}
CameraProjectionOptions camera() {
    CameraProjectionOptions options;
    options.eye = V3(0, 0, 2);
    options.size = {.5f, .5f};
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
bool contains(const PaintMappingCandidates& candidates, int material) {
    return std::binary_search(candidates.materials.begin(), candidates.materials.end(), material);
}

void projectionTests() {
    auto model = scene();
    quad(*model, -1, 0, 0, 3);
    quad(*model, 0, 1, 0, 7);
    quad(*model, 20, 21, 0, 9);
    quad(*model, -1, 1, -1, 11, false);
    quad(*model, -1, 1, 3, 12);
    quad(*model, -1, 1, 0, 13);
    model->meshes.back().uvs.clear();
    PaintSurface surface(model);
    auto options = camera();
    auto candidates = findProjectionMaterials(surface, options);
    check(candidates.materials == std::vector<int>({3, 7}),
          "automatic projection finds both global materials, excludes offscreen/backfaces/behind-eye/no-UV");
    check(candidates.candidateTriangles == 4 && candidates.trianglesByMaterial.at(7) == 2,
          "candidate counts identify covered geometry without reading textures");
    options.material = 7;
    check(findProjectionMaterials(surface, options).materials == std::vector<int>({7}),
          "explicit global material filter remains available");
    options.material = -1;
    options.mesh = 0;
    check(findProjectionMaterials(surface, options).materials == std::vector<int>({3}),
          "optional mesh filter does not reinterpret global material ids");
    options = camera();
    PaintImage image(2, 1);
    image.rgba = {255, 0, 0, 255, 0, 255, 0, 255};
    PaintCanvas left(PaintImage(32, 32, {0, 0, 0, 91}));
    PaintCanvas right(PaintImage(64, 64, {0, 0, 0, 91}));
    auto originalLeft = left.image().rgba, originalRight = right.image().rgba;
    left.beginStroke("两种贴图投影");
    right.beginStroke("两种贴图投影");
    options.externalStroke = true;
    PaintMappingBudget budget;
    options.material = 3;
    options.limits = budget.remaining();
    auto a = applyCameraProjection(surface, left, image, options);
    budget.account(a);
    options.material = 7;
    options.limits = budget.remaining();
    auto b = applyCameraProjection(surface, right, image, options);
    budget.account(b);
    check(a.changed && b.changed && left.strokeActive() && right.strokeActive() && !left.canUndo(),
          "external projection writes different resolution canvases without committing either history");
    check(channel(left, 2, 16) == 255 && channel(right, 60, 32, 1) == 255 &&
              std::abs(channel(left, 31, 16) - channel(right, 0, 32)) <= 8 && channel(right, 0, 32, 3) == 91,
          "one global image placement remains continuous at a cross-material seam and preserves alpha");
    check(budget.rasterSamples() == a.rasterSamples + b.rasterSamples &&
              budget.rayTests() == a.rayTests + b.rayTests,
          "one budget accounts every canvas rather than resetting per material");
    check(left.endStroke() && right.endStroke(), "outer owner can commit both projected canvases");
    auto mappedLeft = left.image().rgba, mappedRight = right.image().rgba;
    check(left.undo() && right.undo() && left.image().rgba == originalLeft &&
              right.image().rgba == originalRight,
          "external projection histories restore both original textures exactly");
    check(left.redo() && right.redo() && left.image().rgba == mappedLeft && right.image().rgba == mappedRight,
          "external projection histories redo both image regions exactly");

    auto crossings = scene();
    triangle(*crossings, {F3{-4, -.1f, 0}, F3{4, -.1f, 0}, F3{0, 3, 0}}, 1);
    triangle(*crossings, {F3{-1, -1, 2.1f}, F3{1, -1, 0}, F3{0, 1, 0}}, 2);
    PaintSurface crossingSurface(crossings);
    options = camera();
    options.size = {.1f, .1f};
    options.occlusion = false;
    candidates = findProjectionMaterials(crossingSurface, options);
    check(contains(candidates, 1) && contains(candidates, 2),
          "long triangle edges and camera/near-plane crossings survive homogeneous footprint clipping");
    for (int material : {1, 2}) {
        PaintCanvas target(PaintImage(128, 128, {0, 0, 0, 255}));
        options.material = material;
        check(applyCameraProjection(crossingSurface, target, image, options).changed,
              "retained boundary-crossing triangle really contributes projected texels");
    }

    auto rotated = scene();
    triangle(*rotated, {F3{.23f, .23f, .5f}, F3{.25f, .23f, .5f}, F3{.23f, .25f, .5f}}, 5);
    PaintSurface rotatedSurface(rotated);
    options = CameraProjectionOptions{};
    options.size = {.2f, .2f};
    options.rotationRadians = pi / 4;
    check(findProjectionMaterials(rotatedSurface, options).materials.empty(),
          "rotated image rectangle excludes an area inside its axis-aligned bounding rectangle");

    auto hidden = scene();
    quad(*hidden, -1, 0, 0, 3);
    quad(*hidden, 0, 1, 0, 7);
    quad(*hidden, -2, .01f, .5f, 8);
    PaintSurface hiddenSurface(hidden);
    PaintImage red(1, 1, {255, 0, 0, 255});
    options = camera();
    PaintCanvas behind(PaintImage(32, 32, {0, 0, 0, 255}));
    options.material = 3;
    auto report = applyCameraProjection(hiddenSurface, behind, red, options);
    check(!report.changed && report.occludedSamples > 0,
          "per-texel projection respects occluders belonging to another candidate material");
    PaintCanvas exposed(PaintImage(32, 32, {0, 0, 0, 255}));
    options.material = 7;
    report = applyCameraProjection(hiddenSurface, exposed, red, options);
    check(report.changed && channel(exposed, 25, 16) == 255,
          "a partly hidden material retains its visible region");
}

void wrapTests() {
    auto model = scene();
    cylinderBand(*model, -1, 0, 2);
    cylinderBand(*model, 0, 1, 8);
    cylinderBand(*model, 10, 11, 10);
    cylinderBand(*model, -1, 1, 11, 1, -.5, .5, false);
    PaintSurface surface(model);
    CylinderWrapOptions options;
    options.height = 2;
    auto candidates = findWrapMaterials(surface, options);
    check(candidates.materials == std::vector<int>({2, 8}),
          "automatic wrap spans axial material boundaries and filters outside height or inward normals");
    options.material = 8;
    check(findWrapMaterials(surface, options).materials == std::vector<int>({8}),
          "wrap supports explicit global material restriction");
    // The inward-facing test shell intentionally overlaps the bands with different tessellation.
    // Seam continuity is independent of that shell; exact cross-material occlusion is tested below.
    options.occlusion = false;
    PaintImage image(1, 2);
    image.rgba = {0, 255, 0, 255, 255, 0, 0, 255};
    PaintCanvas bottom(PaintImage(32, 32, {0, 0, 0, 255}));
    PaintCanvas top(PaintImage(64, 64, {0, 0, 0, 255}));
    bottom.beginStroke("跨贴图环绕");
    top.beginStroke("跨贴图环绕");
    options.externalStroke = true;
    options.material = 2;
    auto a = applyCylindricalWrap(surface, bottom, image, options);
    options.material = 8;
    auto b = applyCylindricalWrap(surface, top, image, options);
    check(a.changed && b.changed && bottom.strokeActive() && top.strokeActive(),
          "external wrap leaves both different-resolution canvas transactions active");
    check(channel(bottom, 16, 30) == 255 && channel(top, 32, 2, 1) == 255 &&
              std::abs(channel(bottom, 16, 0) - channel(top, 32, 63)) <= 8,
          "global cylinder frame produces a continuous image across material and resolution boundaries");
    check(bottom.endStroke() && top.endStroke() && bottom.undo() && top.undo() && bottom.redo() && top.redo(),
          "outer owner commits, undoes and redoes cross-material wrap");

    auto ranges = scene();
    cylinderBand(*ranges, -1, 1, 1, 1, -.1, .1);
    cylinderBand(*ranges, -1, 1, 2, 1, 2.9, 3.1);
    cylinderBand(*ranges, -1, 1, 3, 1, 1.45, 1.65);
    triangle(*ranges, {F3{-3, 1, -2}, F3{3, 1, -2}, F3{0, 1, 2}}, 4, {0, 1, 0});
    PaintSurface rangeSurface(ranges);
    options = CylinderWrapOptions{};
    options.height = .5;
    options.sweepRadians = .2;
    options.occlusion = false;
    candidates = findWrapMaterials(rangeSurface, options);
    check(candidates.materials == std::vector<int>({1, 4}),
          "wedge/slab clipping retains edges crossing the wrap footprint when all vertices lie outside");
    PaintCanvas crossing(PaintImage(128, 128, {0, 0, 0, 255}));
    options.material = 4;
    check(applyCylindricalWrap(rangeSurface, crossing, image, options).changed,
          "wrap boundary-crossing triangle really contributes texels");
    options.material = -1;
    options.sweepRadians = 1.5 * pi;
    candidates = findWrapMaterials(rangeSurface, options);
    check(contains(candidates, 1) && contains(candidates, 3) && !contains(candidates, 2),
          "wide non-convex angular sectors use a union and correctly reject their narrow complement");
    options.angleRadians = pi;
    options.sweepRadians = .5;
    candidates = findWrapMaterials(rangeSurface, options);
    check(candidates.materials == std::vector<int>({2}), "wrap footprint crosses the plus/minus pi seam");

    auto hidden = scene();
    cylinderBand(*hidden, -1, 1, 2, 1);
    cylinderBand(*hidden, -1, 1, 8, 2);
    PaintSurface hiddenSurface(hidden);
    options = CylinderWrapOptions{};
    options.height = 2;
    options.material = 2;
    PaintCanvas inner(PaintImage(32, 32, {0, 0, 0, 255}));
    auto report = applyCylindricalWrap(hiddenSurface, inner, image, options);
    check(!report.changed && report.occludedSamples > 0,
          "wrap checks exact occlusion against all materials, including an outer shell");
    options.material = 8;
    PaintCanvas outer(PaintImage(32, 32, {0, 0, 0, 255}));
    check(applyCylindricalWrap(hiddenSurface, outer, image, options).changed,
          "outer material remains paintable while its inner material is occluded");
}

void ownershipAndBudgetTests() {
    auto model = scene();
    quad(*model, -1, 0, 0, 0);
    quad(*model, 0, 1, 0, 1);
    PaintSurface surface(model);
    auto options = camera();
    options.externalStroke = true;
    options.material = 0;
    PaintImage red(1, 1, {255, 0, 0, 255});
    PaintCanvas first(PaintImage(64, 64, {0, 0, 0, 255}));
    PaintCanvas second(PaintImage(512, 512, {0, 0, 0, 255}));
    auto originalFirst = first.image().rgba, originalSecond = second.image().rgba;
    rejected([&] { applyCameraProjection(surface, first, red, options); },
             "external projection rejects a canvas with no active transaction");
    first.beginStroke("统一操作");
    second.beginStroke("统一操作");
    check(applyCameraProjection(surface, first, red, options).changed, "first external material changes");
    options.material = 1;
    std::atomic_bool cancel = false;
    int callbacks = 0;
    rejected(
        [&] {
            applyCameraProjection(
                surface, second, red, options,
                [&](const std::string&) {
                    if (++callbacks == 2)
                        cancel = true;
                },
                &cancel);
        },
        "second external material can be cancelled after partial progress");
    check(first.strokeActive() && second.strokeActive() && first.image().rgba != originalFirst &&
              second.image().rgba != originalSecond,
          "internal cancellation does not prematurely commit or roll back either external transaction");
    first.cancelStroke();
    second.cancelStroke();
    check(first.image().rgba == originalFirst && second.image().rgba == originalSecond && !first.canUndo() &&
              !second.canUndo(),
          "outer cancellation restores every touched material with no partial history");

    auto wrapModel = scene();
    cylinderBand(*wrapModel, -1, 1, 0);
    PaintSurface wrapSurface(wrapModel);
    CylinderWrapOptions wrap;
    wrap.height = 2;
    wrap.material = 0;
    wrap.externalStroke = true;
    rejected([&] { applyCylindricalWrap(wrapSurface, first, red, wrap); },
             "external wrap rejects a canvas with no active transaction");
    first.beginStroke("失败时保留外部事务");
    first.blendPixel(0, 0, {0, 1, 0, 1});
    wrap.height = 1; // Preserve the earlier corner edit outside this wrap's axial footprint.
    wrap.limits.rayTests = 10;
    rejected([&] { applyCylindricalWrap(wrapSurface, first, red, wrap); },
             "external wrap budget failure propagates to the caller");
    check(first.strokeActive() && channel(first, 0, 0, 1) == 255,
          "failed wrap leaves earlier edits in the external stroke until the batch cancels");
    first.cancelStroke();
    check(first.image().rgba == originalFirst, "external wrap rollback restores the caller's earlier edits");

    PaintMappingLimits limits;
    limits.rasterSamples = 100;
    limits.rayTests = 50;
    PaintMappingBudget budget(limits);
    PaintMappingReport report;
    report.rasterSamples = 60;
    report.rayTests = 30;
    budget.account(report);
    auto remaining = budget.remaining();
    check(remaining.rasterSamples == 40 && remaining.rayTests == 20 && remaining.seconds <= limits.seconds,
          "next material receives the remaining shared ray/pixel/time allowance");
    rejected([&] { budget.account(report); },
             "shared limits reject a second individually affordable operation");
    report.rasterSamples = 40;
    report.rayTests = 20;
    budget.account(report);
    rejected([&] { budget.remaining(); },
             "exhausted overall budget cannot silently restart for another material");
    limits.seconds = 1e-15;
    PaintMappingBudget expired(limits);
    rejected([&] { expired.remaining(); }, "wall-clock budget includes work before the next apply begins");

    options = camera();
    cancel = true;
    rejected([&] { findProjectionMaterials(surface, options, {}, &cancel); },
             "projection discovery honours an existing cancellation request");
    rejected([&] { findWrapMaterials(wrapSurface, wrap, {}, &cancel); },
             "wrap discovery honours an existing cancellation request");
    options.opacity = 0;
    check(findProjectionMaterials(surface, options).materials.empty(),
          "zero-opacity image does not trigger unnecessary texture loading");
    options = camera();
    options.size[0] = 0;
    rejected([&] { findProjectionMaterials(surface, options); },
             "candidate search validates image placement");
    wrap.axis = V3::Zero();
    rejected([&] { findWrapMaterials(wrapSurface, wrap); }, "candidate search validates cylinder frame");
}

void sharedTextureTests() {
    auto model = scene();
    quad(*model, -1, 1, 0, 3);
    quad(*model, -1, 1, 0, 7);
    quad(*model, -1, 1, 0, 9);
    PaintSurface surface(model);
    auto options = camera();
    options.material = 9; // A nonempty union overrides the formerly selected material.
    options.targetMaterials = {3, 7};
    options.externalStroke = true;
    check(findProjectionMaterials(surface, options).materials == std::vector<int>({3, 7}),
          "explicit shared-texture material union overrides the single material selection");
    PaintImage translucent(1, 1, {255, 0, 0, 128});
    PaintCanvas canvas(PaintImage(32, 32, {0, 0, 0, 91}));
    canvas.beginStroke("别名贴图一次混合");
    auto report = applyCameraProjection(surface, canvas, translucent, options);
    check(report.triangles == 4 && report.reusedTexels > 0 && report.paintedPixels == 32 * 32 &&
              channel(canvas, 10, 10) >= 127 && channel(canvas, 10, 10) <= 129 &&
              channel(canvas, 10, 10, 3) == 91 && canvas.strokeActive(),
          "one canonical canvas applies source alpha only once across duplicate material UV aliases");
    canvas.cancelStroke();

    auto cylinders = scene();
    cylinderBand(*cylinders, -1, 1, 2);
    cylinderBand(*cylinders, -1, 1, 8);
    PaintSurface cylinderSurface(cylinders);
    CylinderWrapOptions wrap;
    wrap.height = 2;
    wrap.targetMaterials = {2, 8};
    wrap.externalStroke = true;
    canvas.beginStroke("环绕别名贴图一次混合");
    report = applyCylindricalWrap(cylinderSurface, canvas, translucent, wrap);
    check(report.reusedTexels > 0 && report.paintedPixels == 32 * 32 && channel(canvas, 10, 10) >= 127 &&
              channel(canvas, 10, 10) <= 129 && canvas.strokeActive(),
          "wrap union also shares visited texels across multiple material ids");
    canvas.cancelStroke();
    wrap.targetMaterials = {2, -1};
    rejected([&] { findWrapMaterials(cylinderSurface, wrap); },
             "shared texture union rejects invalid global ids");
}

std::vector<uint32_t> primitives(const PaintMappingCandidates& candidates,
                                 std::span<const int> materials = {}) {
    std::vector<uint32_t> result;
    for (const auto& [material, indices] : candidates.primitivesByMaterial)
        if (materials.empty() || std::find(materials.begin(), materials.end(), material) != materials.end())
            result.insert(result.end(), indices.begin(), indices.end());
    return result;
}

void candidateReuseTests() {
    auto model = scene();
    triangle(*model, {F3{-4, -.1f, 0}, F3{4, -.1f, 0}, F3{0, 3, 0}}, 1);
    triangle(*model, {F3{-1, -1, 2.1f}, F3{1, -1, 0}, F3{0, 1, 0}}, 2);
    quad(*model, -.5f, .5f, .5f, 3);
    quad(*model, -.5f, .5f, .5f, 4); // Another material aliases the same target UVs.
    quad(*model, 10, 11, 0, 1);
    quad(*model, -1, 1, -50, 2);
    quad(*model, -1, 1, 0, 3, false);
    PaintSurface surface(model);
    auto options = camera();
    options.size = {.2f, .2f};
    options.rotationRadians = .37;
    options.targetMaterials = {1, 2, 3, 4};
    auto candidates = findProjectionMaterials(surface, options);
    auto selected = primitives(candidates);
    check(selected.size() == candidates.candidateTriangles && selected.size() < surface.triangleCount(),
          "discovery records only covered original primitive ids for reuse");
    bool identified = true;
    for (const auto& [material, indices] : candidates.primitivesByMaterial) {
        identified = identified && indices.size() == candidates.trianglesByMaterial.at(material);
        for (auto primitive : indices)
            identified = identified && primitive < surface.triangleCount() &&
                         surface.triangle(primitive).material == material;
    }
    check(identified, "primitive lists retain global surface identity and material grouping");
    PaintImage image(2, 2);
    image.rgba = {255, 0, 0, 128, 0, 255, 0, 191, 0, 0, 255, 220, 255, 255, 255, 110};
    for (bool occlusion : {false, true}) {
        PaintCanvas full(PaintImage(128, 64, {0, 0, 0, 91}));
        PaintCanvas cached(PaintImage(128, 64, {0, 0, 0, 91}));
        options.occlusion = occlusion;
        options.candidatePrimitives.reset();
        auto a = applyCameraProjection(surface, full, image, options);
        options.candidatePrimitives = selected;
        auto b = applyCameraProjection(surface, cached, image, options);
        check(a.changed && b.changed && full.image().rgba == cached.image().rgba &&
                  a.paintedPixels == b.paintedPixels,
              "cached projection matches every RGBA byte across near clipping, rotated edges, union and "
              "occlusion");
        check(b.triangles < a.triangles && b.rasterSamples < a.rasterSamples,
              "cached projection removes out-of-footprint triangle raster work");
    }
    options.targetMaterials.clear();
    options.material = 3;
    options.mesh = 2;
    options.candidatePrimitives.reset();
    PaintCanvas filteredFull(PaintImage(64, 64, {0, 0, 0, 255}));
    PaintCanvas filteredCached(PaintImage(64, 64, {0, 0, 0, 255}));
    applyCameraProjection(surface, filteredFull, image, options);
    options.candidatePrimitives = selected; // Includes valid primitives from unrelated materials/meshes.
    auto filtered = applyCameraProjection(surface, filteredCached, image, options);
    check(filtered.triangles == 2 && filteredFull.image().rgba == filteredCached.image().rgba,
          "provided candidates still honour target material and mesh selection");
    PaintCanvas blank(PaintImage(64, 64, {0, 0, 0, 255}));
    auto original = blank.image().rgba;
    options.candidatePrimitives = std::vector<uint32_t>{};
    auto empty = applyCameraProjection(surface, blank, image, options);
    check(!empty.changed && !empty.triangles && !empty.rasterSamples && !blank.canUndo() &&
              blank.image().rgba == original,
          "present-but-empty projection candidates skip all geometry instead of scanning the scene");
    options.candidatePrimitives = std::vector<uint32_t>{0, uint32_t(surface.triangleCount())};
    rejected([&] { applyCameraProjection(surface, blank, image, options); },
             "projection rejects an out-of-range primitive even when its material would not match");
    check(!blank.strokeActive() && !blank.canUndo() && blank.image().rgba == original,
          "invalid projection candidates preserve pixels and history");

    auto cylinders = scene();
    cylinderBand(*cylinders, -1, 0, 2);
    cylinderBand(*cylinders, 0, 1, 8);
    cylinderBand(*cylinders, -1, 0, 9); // Shared UV alias, handled in the same operation.
    cylinderBand(*cylinders, 10, 11, 2);
    PaintSurface cylinderSurface(cylinders);
    CylinderWrapOptions wrap;
    wrap.height = .8;
    wrap.angleRadians = 3.02;
    wrap.sweepRadians = 1.3 * pi;
    wrap.targetMaterials = {2, 8, 9};
    auto wrapCandidates = findWrapMaterials(cylinderSurface, wrap);
    for (bool occlusion : {false, true}) {
        PaintCanvas full(PaintImage(128, 64, {0, 0, 0, 91}));
        PaintCanvas cached(PaintImage(128, 64, {0, 0, 0, 91}));
        wrap.occlusion = occlusion;
        wrap.candidatePrimitives.reset();
        auto a = applyCylindricalWrap(cylinderSurface, full, image, wrap);
        wrap.candidatePrimitives = primitives(wrapCandidates, wrap.targetMaterials);
        auto b = applyCylindricalWrap(cylinderSurface, cached, image, wrap);
        check(a.changed && b.changed && a.paintedPixels == b.paintedPixels &&
                  full.image().rgba == cached.image().rgba,
              "cached wrap matches every RGBA byte across axial clipping, a wide seam-crossing sector and "
              "aliases");
        check(b.triangles < a.triangles && b.rasterSamples < a.rasterSamples,
              "cached wrap skips out-of-footprint triangle raster work");
    }
    wrap.candidatePrimitives = std::vector<uint32_t>{};
    empty = applyCylindricalWrap(cylinderSurface, blank, image, wrap);
    check(!empty.changed && !empty.triangles && !empty.rasterSamples && blank.image().rgba == original,
          "present-but-empty wrap candidates skip all geometry");
    wrap.candidatePrimitives = std::vector<uint32_t>{0, UINT32_MAX};
    rejected([&] { applyCylindricalWrap(cylinderSurface, blank, image, wrap); },
             "wrap rejects malicious primitive ids without out-of-bounds access");
    check(!blank.strokeActive() && !blank.canUndo() && blank.image().rgba == original,
          "invalid wrap candidates preserve pixels and history");
    blank.beginStroke("无效候选由外部回滚");
    blank.blendPixel(0, 0, {0, 1, 0, 1});
    wrap.externalStroke = true;
    rejected([&] { applyCylindricalWrap(cylinderSurface, blank, image, wrap); },
             "invalid candidate error propagates through an external batch");
    check(blank.strokeActive() && channel(blank, 0, 0, 1) == 255,
          "invalid candidates leave externally owned rollback to the batch");
    blank.cancelStroke();
}

void conservativeCoverageTests() {
    // Reproducible nontrivial triangles cross the camera plane, frustum, rotated rectangle, cylinder
    // slab and non-convex angular sectors. Compare discovery against actual UV texel application,
    // not an implementation-shaped second clipping routine.
    auto model = scene(96);
    std::mt19937 random(71403);
    std::uniform_real_distribution<float> xy(-3, 3), z(-1, 2.5f), normal(-1, 1);
    for (int material = 0; material < 96; ++material) {
        std::array<F3, 3> points;
        for (auto& point : points)
            point = {xy(random), xy(random), z(random)};
        triangle(*model, points, material);
        for (auto& n : model->meshes.back().normals)
            n = {normal(random), normal(random), normal(random)};
    }
    PaintSurface surface(model);
    PaintImage red(1, 1, {255, 0, 0, 255});
    auto projection = camera();
    projection.size = {.15f, .24f};
    projection.center = {.54f, .48f};
    projection.rotationRadians = .71;
    projection.viewportAspect = 1.7f;
    projection.occlusion = false;
    auto candidates = findProjectionMaterials(surface, projection);
    size_t contributing = 0;
    bool projectionMatches = true;
    for (int material = 0; material < 96; ++material) {
        PaintCanvas canvas(PaintImage(32, 32, {0, 0, 0, 255}));
        projection.material = material;
        projection.candidatePrimitives.reset();
        if (applyCameraProjection(surface, canvas, red, projection).changed) {
            require(contains(candidates, material), "Candidate projection missed a painted random triangle");
            ++contributing;
        }
        PaintCanvas cached(PaintImage(32, 32, {0, 0, 0, 255}));
        projection.candidatePrimitives = candidates.primitivesByMaterial[material];
        applyCameraProjection(surface, cached, red, projection);
        projectionMatches = projectionMatches && cached.image().rgba == canvas.image().rgba;
    }
    check(contributing >= 10,
          "conservative projection discovery retains every contributing random posed triangle");
    check(projectionMatches, "cached primitive projection matches full traversal for every random triangle");
    for (double sweep : {.31, 1.65 * pi}) {
        CylinderWrapOptions wrap;
        wrap.height = .6;
        wrap.center = V3(.1, -.1, .2);
        wrap.axis = V3(1, .2, .1);
        wrap.angleRadians = 3.02;
        wrap.sweepRadians = sweep;
        wrap.occlusion = false;
        candidates = findWrapMaterials(surface, wrap);
        contributing = 0;
        bool wrapMatches = true;
        for (int material = 0; material < 96; ++material) {
            PaintCanvas canvas(PaintImage(32, 32, {0, 0, 0, 255}));
            wrap.material = material;
            wrap.candidatePrimitives.reset();
            if (applyCylindricalWrap(surface, canvas, red, wrap).changed) {
                require(contains(candidates, material), "Candidate wrap missed a painted random triangle");
                ++contributing;
            }
            PaintCanvas cached(PaintImage(32, 32, {0, 0, 0, 255}));
            wrap.candidatePrimitives = candidates.primitivesByMaterial[material];
            applyCylindricalWrap(surface, cached, red, wrap);
            wrapMatches = wrapMatches && cached.image().rgba == canvas.image().rgba;
        }
        check(contributing >= 10,
              "conservative wrap discovery retains every narrow/wide-sector contributing triangle");
        check(wrapMatches, "cached primitive wrap matches full traversal for every random triangle");
    }
}
} // namespace

int wmain() {
    try {
        edm::ComRuntime imageRuntime;
        projectionTests();
        wrapTests();
        ownershipAndBudgetTests();
        sharedTextureTests();
        candidateReuseTests();
        conservativeCoverageTests();
        std::cout << "Automatic image mapping: " << checks << " checks passed\n";
        return 0;
    } catch (...) {
        std::cerr << edm::exceptionText() << '\n';
        return 1;
    }
}
