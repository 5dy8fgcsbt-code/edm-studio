#include "attachment.h"
#include "paint_batch.h"
#include "paint_decal.h"
#include <iostream>

using namespace edm;
namespace {
int checks = 0;
void check(bool value, const char* message) {
    require(value, std::string("Attachment painting: ") + message);
    ++checks;
}

Mesh quad(float left, float right, float z, int material, int node) {
    Mesh mesh;
    mesh.node = node;
    mesh.material = material;
    mesh.positions = {{left, -1, z}, {right, -1, z}, {right, 1, z}, {left, 1, z}};
    mesh.normals.assign(4, F3{0, 0, 1});
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.uvs = {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    return mesh;
}

// Deliberately use only mesh ownership metadata. A hidden attachment must be excluded even if a
// stale or independently skinned transform still has nonzero geometry in front of the aircraft.
std::shared_ptr<Scene> model(bool adjacent = false, bool skinned = false) {
    auto result = std::make_shared<Scene>();
    result->materials.resize(2);
    result->materials[0].name = "Aircraft paint";
    result->materials[1].name = "External store paint";
    result->nodes.resize(3);
    result->nodes[1].parent = result->nodes[2].parent = 0;
    result->staticLocal.assign(3, Mat::Identity());
    result->defaultWorld = result->staticLocal;
    result->order = {0, 1, 2};
    result->meshes = {quad(-1, adjacent ? 0.f : 1.f, 0, 0, 1),
                      quad(adjacent ? 0.f : -1.f, 1, adjacent ? 0.f : .5f, 1, 2)};
    auto& attachment = result->meshes[1];
    attachment.extras = {{"edm_attachment", 0}};
    if (skinned) {
        attachment.skinNodes = {0};
        attachment.inverseBind = {Mat::Identity()};
        attachment.joints.resize(4);
        attachment.weights.assign(4, std::array<float, 8>{1, 0, 0, 0, 0, 0, 0, 0});
    }
    return result;
}

std::shared_ptr<PaintCanvas> canvas(uint32_t width = 32, uint32_t height = 32) {
    return std::make_shared<PaintCanvas>(PaintImage(width, height, {0, 0, 0, 91}));
}
int channel(const PaintCanvas& canvas, int x, int y, int component = 0) {
    return canvas.image().rgba[(size_t(y) * canvas.image().width + x) * 4 + component];
}
CameraProjectionOptions camera() {
    CameraProjectionOptions result;
    result.material = 0;
    result.eye = V3(0, 0, 2);
    result.size = {.5f, .5f};
    Mat projection = Mat::Zero();
    projection(0, 0) = projection(1, 1) = 1;
    projection(2, 2) = -10. / 9.9;
    projection(2, 3) = -1. / 9.9;
    projection(3, 2) = -1;
    result.viewProjection = projection * translation(V3(0, 0, -2));
    return result;
}
SurfaceDecalOptions decal() {
    SurfaceDecalOptions result;
    result.width = result.height = 2;
    result.depth = .1;
    return result;
}

void pickingAndBounds(bool skinned) {
    const auto source = model(false, skinned);
    PaintSurface visible(source);
    PaintSurface hidden(source, {}, {}, nullptr, false);
    check(visible.triangleCount() == 4 && hidden.triangleCount() == 2,
          "Hiding stores removes both rigid and skinned attachment triangles from the BVH");
    const V3 eye(0, 0, 2), direction(0, 0, -1);
    auto front = visible.raycast(eye, direction), behind = hidden.raycast(eye, direction);
    check(front && front->material == 1 && std::abs(front->position.z() - .5) < 1e-9,
          "A visible external store is the nearest paint target");
    check(behind && behind->material == 0 && std::abs(behind->position.z()) < 1e-9,
          "A hidden external store no longer intercepts clicks on the aircraft behind it");
    check(!hidden.raycast(eye, direction, 1), "An explicit material filter cannot pick a hidden store");
    check(visible.querySphere(V3(0, 0, .5), .1, 1).size() == 2 &&
              hidden.querySphere(V3(0, 0, .5), .1, 1).empty(),
          "Brush candidate queries exclude hidden ownership, including independently skinned stores");
    const auto visibleBounds = visible.bounds(), hiddenBounds = hidden.bounds();
    check(std::abs(visibleBounds.second.z() - .5) < 1e-9 && std::abs(hiddenBounds.second.z()) < 1e-9,
          "Hidden geometry cannot extend projection ray origins or material bounds");
    check(hidden.analyzeUV(1, 16).coveredPixels == 0 && visible.analyzeUV(1, 16).coveredPixels > 0,
          "Hidden store UVs are absent from the paint surface");
    for (uint32_t primitive = 0; primitive < hidden.triangleCount(); ++primitive)
        check(hidden.triangle(primitive).mesh == 0, "Every hidden-surface primitive belongs to the aircraft");
    PaintSurface restored(source, {}, {}, nullptr, true);
    auto again = restored.raycast(eye, direction);
    check(again && again->mesh == front->mesh && again->position == front->position,
          "Rebuilding after showing stores restores the original pick surface without source mutations");
}

void brushIsolation() {
    auto source = model(false, true);
    PaintSurface visible(source), hidden(source, {}, {}, nullptr, false);
    V3 eye(0, 0, 2);
    auto front = visible.raycast(eye, V3(0, 0, -1));
    auto behind = hidden.raycast(eye, V3(0, 0, -1));
    require(front && behind, "Fixture paint targets missing");
    PaintBrush brush;
    brush.radius = 3;
    brush.hardness = 1;
    brush.color = {1, 0, 0, 1};

    auto occludedBase = canvas();
    occludedBase->beginStroke();
    const auto occluded = paintBrush(*occludedBase, visible, *behind, brush, eye, nullptr, 0);
    check(occluded.pixels == 0 && !occludedBase->endStroke(),
          "A visible store correctly blocks a brush trying to reach the aircraft behind it");

    auto exposedBase = canvas();
    exposedBase->beginStroke();
    const auto exposed = paintBrush(*exposedBase, hidden, *behind, brush, eye);
    check(exposed.pixels == 1024 && exposedBase->endStroke() && channel(*exposedBase, 16, 16) == 255,
          "Hiding the same store lets the brush reach all aircraft texels beneath it");
    check(channel(*exposedBase, 16, 16, 3) == 91, "Aircraft alpha masks survive the visibility toggle");

    auto store = canvas();
    store->beginStroke();
    check(paintBrush(*store, visible, *front, brush, eye).pixels == 1024 && store->endStroke(),
          "The normal brush paints a visible external store texture");
    auto before = store->image().rgba;
    brush.color = {0, 1, 0, 1};
    store->beginStroke();
    check(paintBrush(*store, hidden, *front, brush, eye, nullptr, 1).pixels == 0 && !store->endStroke() &&
              store->image().rgba == before,
          "Even a stale hit and an explicit store target cannot paint a hidden store");
}

void projectionIsolation() {
    auto source = model();
    PaintSurface visible(source), hidden(source, {}, {}, nullptr, false);
    PaintImage red(1, 1, {255, 0, 0, 255});
    auto options = camera();
    auto blocked = canvas(), exposed = canvas();
    const auto blockedReport = applyCameraProjection(visible, *blocked, red, options);
    const auto exposedReport = applyCameraProjection(hidden, *exposed, red, options);
    check(!blockedReport.changed && blockedReport.occludedSamples > 0,
          "Visible external stores occlude the aircraft in camera projection");
    check(exposedReport.changed && exposedReport.paintedPixels == 1024 && exposedReport.occludedSamples == 0,
          "Hidden stores neither block nor truncate the camera projection onto the aircraft");

    options.material = -1;
    check(findProjectionMaterials(visible, options).materials == std::vector<int>{0, 1} &&
              findProjectionMaterials(hidden, options).materials == std::vector<int>{0},
          "Automatic camera mapping discovers only currently visible object materials");
    options.material = 1;
    options.occlusion = false;
    auto store = canvas();
    const auto original = store->image().rgba;
    check(!applyCameraProjection(hidden, *store, red, options).changed && store->image().rgba == original &&
              !store->canUndo(),
          "Disabling camera occlusion still cannot paint a hidden external store");
    check(applyCameraProjection(visible, *store, red, options).changed,
          "Camera projection can paint an external store after it is shown");
}

void decalIsolation() {
    auto source = model(false, true);
    PaintSurface visible(source), hidden(source, {}, {}, nullptr, false);
    auto options = decal();
    options.material = 0;
    PaintImage red(1, 1, {255, 0, 0, 255});
    auto blocked = canvas(), exposed = canvas();
    const auto blockedReport = applySurfaceDecal(visible, *blocked, red, options);
    const auto exposedReport = applySurfaceDecal(hidden, *exposed, red, options);
    check(!blockedReport.changed && blockedReport.occludedSamples > 0,
          "A visible store ahead of the decal depth box still correctly blocks its parallel rays");
    check(exposedReport.changed && exposedReport.paintedPixels == 1024 && exposedReport.occludedSamples == 0,
          "A hidden skinned store cannot shadow a surface decal on the aircraft");

    options.material = -1;
    options.depth = 1;
    check(findDecalMaterials(visible, options).materials == std::vector<int>{0, 1} &&
              findDecalMaterials(hidden, options).materials == std::vector<int>{0},
          "Automatic surface-decal discovery removes hidden external store materials");
    options.material = 1;
    options.occlusion = false;
    auto store = canvas();
    const auto original = store->image().rgba;
    check(!applySurfaceDecal(hidden, *store, red, options).changed && store->image().rgba == original &&
              !store->canUndo(),
          "Through-surface decals cannot modify hidden store canvases");
    check(applySurfaceDecal(visible, *store, red, options).changed,
          "Visible stores receive image-brush surface decals");
}

void sharedStampAndHistory() {
    auto source = model(true);
    PaintSurface visible(source), hidden(source, {}, {}, nullptr, false);
    auto options = decal();
    const auto candidates = findDecalMaterials(visible, options);
    check(candidates.materials == std::vector<int>{0, 1},
          "A single image-brush footprint finds adjacent aircraft and external store materials");
    std::array<std::shared_ptr<PaintCanvas>, 2> canvases{canvas(), canvas(64, 32)};
    std::array<std::vector<uint8_t>, 2> original{canvases[0]->image().rgba, canvases[1]->image().rgba};
    PaintImage image(1, 1, {255, 0, 0, 128});
    PaintBatchHistory history;
    history.begin("Aircraft and external store decal");
    options.externalStroke = true;
    for (int material : candidates.materials) {
        options.material = material;
        options.candidatePrimitives = candidates.primitivesByMaterial.at(material);
        auto report = applySurfaceDecal(visible, history.touch(material, canvases[material]), image, options);
        check(report.changed && report.paintedPixels == size_t(canvases[material]->image().width) * 32,
              "One placement fills each object's separate texture at its original resolution");
    }
    check(history.commit() && history.changedMaterials() == std::vector<int>{0, 1},
          "Aircraft and external store changes commit as one user operation");
    std::array<std::vector<uint8_t>, 2> painted{canvases[0]->image().rgba, canvases[1]->image().rgba};
    check(channel(*canvases[0], 31, 16) == 128 && channel(*canvases[1], 0, 16) == 128,
          "The stamp has equal opacity on both sides of the object boundary");
    check(history.undo() && canvases[0]->image().rgba == original[0] &&
              canvases[1]->image().rgba == original[1],
          "One undo restores both the aircraft and the external store");
    check(history.redo() && canvases[0]->image().rgba == painted[0] &&
              canvases[1]->image().rgba == painted[1],
          "One redo restores both objects exactly");

    options = decal();
    auto hiddenCandidates = findDecalMaterials(hidden, options);
    check(hiddenCandidates.materials == std::vector<int>{0},
          "The next hidden-store stamp discovers only the aircraft");
    options.material = 0;
    auto green = PaintImage(1, 1, {0, 255, 0, 255});
    check(applySurfaceDecal(hidden, *canvases[0], green, options).changed &&
              canvases[1]->image().rgba == painted[1],
          "Painting after hiding stores preserves their already-edited textures");
}

void animatedVisibilityAndDefaults() {
    auto source = model();
    Track visibility;
    visibility.node = 1;
    visibility.arg = 77;
    visibility.channel = Channel::Scale;
    visibility.keys = {{0, V4(1, 1, 1, 0)}, {1, V4::Zero()}};
    source->tracks = {visibility};
    source->defaultArgs = {{77, 1}};
    PaintSurface defaultPose(source), allHidden(source, {}, {}, nullptr, false);
    check(defaultPose.triangleCount() == 2 && allHidden.triangleCount() == 0,
          "Stored argument defaults hide the aircraft while attachment visibility stays independent");
    check(!allHidden.raycast(V3(0, 0, 2), V3(0, 0, -1)) && allHidden.querySphere(V3::Zero(), 3).empty(),
          "An empty posed scene has no paintable target or occluder");
    auto cameraOptions = camera();
    cameraOptions.material = -1;
    check(findProjectionMaterials(allHidden, cameraOptions).materials.empty() &&
              findDecalMaterials(allHidden, decal()).materials.empty(),
          "All-hidden geometry yields no automatic image-mapping targets");
    PaintSurface explicitPose(source, {{77, 0}}, {}, nullptr, false);
    auto hit = explicitPose.raycast(V3(0, 0, 2), V3(0, 0, -1));
    check(explicitPose.triangleCount() == 2 && hit && hit->material == 0,
          "An explicit argument overrides the stored default without reviving hidden external stores");
}

void mountedAnimation() {
    auto base = model(), store = model();
    for (auto source : {base, store}) {
        source->meshes.resize(1);
        source->materials.resize(1);
        source->nodes[2].extras = {{"edm_type", "Connector"}};
    }
    base->source = "aircraft.edm";
    base->nodes[2].name = "Pylon1";
    base->nodes[2].t = V3(0, 0, .5);
    Track motion;
    motion.node = 2;
    motion.arg = 9;
    motion.channel = Channel::Position;
    motion.keys = {{0, V4(0, 0, .5, 0)}, {1, V4(2, 0, .5, 0)}};
    base->tracks.push_back(motion);
    store->source = "external-store.edm";
    store->nodes[2].name = "AttachPoint";
    store->nodes[2].t = V3(.2, 0, 0);
    store->meshes[0] = quad(-.3f, .7f, 0, 0, 1);
    for (auto source : {base, store}) {
        for (size_t node = 0; node < source->nodes.size(); ++node)
            source->staticLocal[node] = source->nodes[node].local();
        source->defaultWorld = source->evaluate({});
    }
    auto assembly = attachScene(*base, *store, findConnector(*base, "Pylon1"));
    PaintSurface mounted(assembly), moved(assembly, {{9, 1}}), hidden(assembly, {}, {}, nullptr, false);
    const V3 eye(0, 0, 2), direction(0, 0, -1);
    auto target = mounted.raycast(eye, direction), aircraft = moved.raycast(eye, direction);
    auto movedStore = moved.raycast(V3(2, 0, 2), direction), hiddenBase = hidden.raycast(eye, direction);
    check(target && target->material == 1 && std::abs(target->position.z() - .5) < 1e-9,
          "Actual AttachPoint alignment yields the correct store paint target and global material");
    check(aircraft && aircraft->material == 0 && movedStore && movedStore->material == 1,
          "Paint picking follows the moving aircraft pylon and its mounted store");
    check(hidden.triangleCount() == 2 && hiddenBase && hiddenBase->material == 0,
          "A composed scene hides all attachment geometry while retaining the aircraft");
}
} // namespace

int wmain() {
    try {
        ComRuntime runtime;
        pickingAndBounds(false);
        pickingAndBounds(true);
        brushIsolation();
        projectionIsolation();
        decalIsolation();
        sharedStampAndHistory();
        animatedVisibilityAndDefaults();
        mountedAnimation();
        std::cout << "Attachment painting: " << checks << " checks passed\n";
        return 0;
    } catch (...) {
        std::cerr << exceptionText() << '\n';
        return 1;
    }
}
