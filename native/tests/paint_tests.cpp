#include "paint.h"
#include <iostream>

using namespace edm;
namespace {
int checks = 0;
void check(bool condition, const char* description) {
    ++checks;
    require(condition, std::string("Paint regression failed: ") + description);
}
template <class F> void fails(F action, const char* description) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, description);
}
std::array<uint8_t, 4> pixel(const PaintImage& image, uint32_t x, uint32_t y) {
    const auto* data = image.rgba.data() + (size_t(y) * image.width + x) * 4;
    return {data[0], data[1], data[2], data[3]};
}
Mesh quad(float left = -1, float right = 1, float z = 0, float u0 = 0, float u1 = 1) {
    Mesh mesh;
    mesh.positions = {{left, -1, z}, {right, -1, z}, {right, 1, z}, {left, 1, z}};
    mesh.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    mesh.uvs = {{{u0, 0}, {u1, 0}, {u1, 1}, {u0, 1}}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}
std::shared_ptr<Scene> scene(std::vector<Mesh> meshes) {
    auto result = std::make_shared<Scene>();
    result->materials.resize(2);
    result->materials[0].name = "base";
    result->materials[1].name = "front";
    result->nodes.resize(2);
    result->staticLocal = {Mat::Identity(), Mat::Identity()};
    result->defaultWorld = result->staticLocal;
    result->order = {0, 1};
    result->meshes = std::move(meshes);
    return result;
}
void workerImageLifetime(const fs::path& output) {
    // The first factory is created on a disposable worker. Later workers and the main thread reuse it.
    // This reproduced the cached WIC factory AV before the process MTA guard was added.
    auto run = [&](auto action) {
        std::exception_ptr failure;
        std::thread worker([&] {
            try {
                action();
            } catch (...) {
                failure = std::current_exception();
            }
        });
        worker.join();
        if (failure)
            std::rethrow_exception(failure);
    };
    run([&] { PaintImage(17, 11, {12, 34, 56, 78}).savePNG(output / "worker-template.png"); });
    for (int index = 0; index < 4; ++index) {
        run([&] {
            auto image = PaintImage::load({output / "worker-template.png"});
            require(pixel(image, 3, 4) == std::array<uint8_t, 4>{12, 34, 56, 78},
                    "Worker PNG pixels changed");
            image.savePNG(output / "worker-template-copy.png");
        });
        check(pixel(PaintImage::load({output / "worker-template-copy.png"}), 3, 4) ==
                  std::array<uint8_t, 4>{12, 34, 56, 78},
              "WIC factory survives worker exit and recreation");
    }
}
void imageAndHistory(const fs::path& output) {
    PaintImage image(65, 67, {30, 70, 120, 91});
    auto original = image.rgba;
    image.savePNG(output / "template.png");
    auto reloaded = PaintImage::load({output / "template.png"});
    check(reloaded.rgba == original && reloaded.width == 65 && reloaded.height == 67,
          "PNG preserves template RGBA pixels");
    image.saveDDS(output / "template.dds");
    auto dds = PaintImage::load({output / "template.dds"});
    check(dds.rgba == original, "DDS export preserves full resolution RGBA");
    auto loadedDDS = loadTexture({output / "template.dds"}, 0);
    check(loadedDDS->pixels.GetMetadata().mipLevels > 1, "DDS export contains mipmaps");
    auto mipPreview = image.texture(true);
    check(mipPreview->pixels.GetMetadata().mipLevels > 1, "Editable preview supports mipmaps");
    fails([&] { PaintImage::load({output / "template.psd"}); }, "PSD layers are explicitly rejected");
    fails([] { PaintImage invalid(65535, 65535); }, "Oversized canvas rejected before allocation");

    PaintCanvas canvas(std::move(image));
    check(canvas.takeDirtyRect().has_value() && !canvas.takeDirtyRect().has_value(),
          "Initial image is dirty once");
    fails([&] { canvas.blendPixel(1, 2, {1, 0, 0, 1}); }, "Painting requires transaction");
    canvas.beginStroke();
    check(canvas.blendPixel(64, 66, {1, 0, 0, .5f}), "Canvas edge pixel can be painted");
    auto painted = pixel(canvas.image(), 64, 66);
    check(painted[0] == 143 && painted[1] == 35 && painted[2] == 60 && painted[3] == 91,
          "Brush alpha blend preserves DCS alpha channel");
    auto dirty = canvas.takeDirtyRect();
    check(dirty && dirty->x0 == 64 && dirty->y0 == 66 && dirty->x1 == 65 && dirty->y1 == 67,
          "Dirty rectangle identifies changed pixel");
    check(canvas.endStroke() && canvas.canUndo(), "Drag becomes one undo operation");
    auto changed = canvas.image().rgba;
    check(canvas.undo() && canvas.image().rgba == original, "Undo restores all four channels exactly");
    check(canvas.redo() && canvas.image().rgba == changed, "Redo replays exact pixel bytes");
    check(PaintImage::load({output / "template.png"}).rgba == original,
          "Editing never changes source template");
    canvas.beginStroke();
    canvas.blendPixel(2, 2, {0, 1, 0, 1});
    canvas.cancelStroke();
    check(canvas.image().rgba == changed && canvas.canUndo(), "Cancel rolls back without destroying history");
    check(canvas.undo(), "Undo after canceled stroke");
    canvas.beginStroke("new branch");
    canvas.blendPixel(1, 1, {0, 0, 1, 1});
    canvas.endStroke();
    check(!canvas.canRedo(), "New stroke invalidates old redo branch");
    canvas.beginStroke();
    check(!canvas.blendPixel(-1, 0, {1, 1, 1, 1}) && !canvas.blendPixel(0, 0, {1, 1, 1, 0}),
          "Out of bounds and transparent strokes do not alter canvas");
    check(!canvas.endStroke(), "Empty stroke does not create history");
    PaintCanvas alpha(PaintImage(1, 1, {0, 0, 255, 0}));
    alpha.beginStroke();
    alpha.blendPixel(0, 0, {1, 0, 0, .5f}, 1, false);
    alpha.endStroke();
    check(pixel(alpha.image(), 0, 0) == std::array<uint8_t, 4>{255, 0, 0, 128},
          "Optional source-over alpha can paint a transparent canvas");
    PaintCanvas bounded(PaintImage(128, 128), 1);
    for (int x : {0, 70}) {
        bounded.beginStroke();
        bounded.blendPixel(x, 0, {0, 0, 0, 1});
        bounded.endStroke();
    }
    check(bounded.undo() && !bounded.canUndo(), "History budget evicts older steps but retains latest undo");
}
void pickingAndPoses() {
    auto basic = scene({quad()});
    PaintSurface surface(basic);
    auto hit = surface.raycast(V3(0, 0, 4), V3(0, 0, -5));
    check(hit && std::abs(hit->distance - 4) < 1e-10 && (hit->position - V3::Zero()).norm() < 1e-10,
          "Nearest ray hit uses normalized world distance");
    check(hit && std::abs(hit->uv[0] - .5) < 1e-7 && std::abs(hit->uv[1] - .5) < 1e-7 &&
              hit->normal.z() > .999,
          "Ray hit interpolates UV and world normal");
    check(!surface.raycast(V3(3, 0, 4), V3(0, 0, -1)), "Ray misses outside model");
    check(!surface.raycast(V3(0, 0, 4), V3(0, 0, -1), -1, 3.99),
          "Ray distance limit excludes remote surfaces");
    check(surface.querySphere(V3(0, 0, 0), .1).size() == 2 && surface.querySphere(V3(10, 0, 0), .1).empty(),
          "BVH sphere query selects nearby faces");
    check(!surface.analyzeUV(0).shared(), "Adjacent triangles are not false UV overlap warnings");
    auto front = quad(-1, 1, 1);
    front.material = 1;
    PaintSurface occlusion(scene({quad(), front}));
    check(occlusion.raycast(V3(0, 0, 4), V3(0, 0, -1))->material == 1,
          "Nearest opaque geometry intercepts ray regardless of material");
    check(occlusion.raycast(V3(0, 0, 4), V3(0, 0, -1), 0)->material == 0,
          "Material-filtered raycast remains available for explicit selection");
    PaintSurface overlapping(scene({quad(), quad()}));
    check(overlapping.analyzeUV(0).sharedPixels > 100, "Mirrored/shared UV islands are reported");

    Track translationTrack;
    translationTrack.node = 0;
    translationTrack.arg = 38;
    translationTrack.channel = Channel::Position;
    translationTrack.keys = {{0, V4(0, 0, 0, 0)}, {1, V4(2, 0, 0, 0)}};
    basic->tracks.push_back(translationTrack);
    PaintSurface moved(basic, {{38, 1}});
    check(!moved.raycast(V3(0, 0, 4), V3(0, 0, -1)) && moved.raycast(V3(2, 0, 4), V3(0, 0, -1)),
          "Picking follows animated rigid geometry");
    auto skin = quad();
    skin.skinNodes = {1};
    skin.inverseBind = {Mat::Identity()};
    skin.joints.resize(4);
    skin.weights.resize(4);
    for (auto& weights : skin.weights)
        weights[0] = 1;
    auto skinnedScene = scene({skin});
    translationTrack.node = 1;
    skinnedScene->tracks = {translationTrack};
    PaintSurface skinned(skinnedScene, {{38, 1}});
    check(skinned.raycast(V3(2, 0, 4), V3(0, 0, -1)) && !skinned.raycast(V3(0, 0, 4), V3(0, 0, -1)),
          "Picking follows animated bone skinning");
    auto mirrored = scene({quad()});
    mirrored->staticLocal[0] = scaling(V3(-1, 1, 1));
    PaintSurface negativeScale(mirrored);
    check(negativeScale.triangle(0).normal().z() > .999,
          "Negative determinant preserves inverse-transpose normal orientation");
    auto hidden = scene({quad()});
    hidden->staticLocal[0] = scaling(V3::Zero());
    PaintSurface invisible(hidden);
    check(invisible.triangleCount() == 0 && !invisible.raycast(V3(0, 0, 4), V3(0, 0, -1)),
          "Animation-hidden geometry is not paintable");
    std::atomic_bool cancel = true;
    fails([&] { PaintSurface canceled(scene({quad()}), {}, {}, &cancel); },
          "BVH construction can be canceled");
}
void brushesAndRaster(const fs::path& output) {
    PaintSurface seams(scene({quad(-1, 0, 0, 0, .4f), quad(0, 1, 0, .6f, 1)}));
    PaintCanvas canvas(PaintImage(128, 128));
    auto hit = seams.raycast(V3(0, 0, 3), V3(0, 0, -1));
    require(hit.has_value(), "Synthetic seam ray missed");
    PaintBrush brush;
    brush.radius = .25;
    brush.hardness = 1;
    brush.color = {1, 0, 0, 1};
    canvas.beginStroke();
    auto statistics = paintBrush(canvas, seams, *hit, brush, V3(0, 0, 3));
    canvas.endStroke();
    check(statistics.pixels > 100, "World-space brush writes surface texels");
    check(pixel(canvas.image(), 50, 64)[1] == 0 && pixel(canvas.image(), 77, 64)[1] == 0,
          "One brush crosses both sides of a UV seam");
    check(pixel(canvas.image(), 64, 64)[1] == 255 && pixel(canvas.image(), 10, 10)[1] == 255,
          "Brush does not spill into unrelated or empty UV islands");
    auto drawn = canvas.image().rgba;
    check(canvas.undo() && pixel(canvas.image(), 50, 64)[1] == 255 && canvas.redo() &&
              canvas.image().rgba == drawn,
          "World brush operation has exact undo and redo");
    canvas.image().savePNG(output / "cross-uv-seam.png");

    auto blocker = quad(-.5f, .5f, .3f);
    blocker.material = 1;
    PaintSurface blocked(scene({quad(), blocker}));
    auto backHit = blocked.raycast(V3(0, 0, 3), V3(0, 0, -1), 0);
    PaintCanvas occluded(PaintImage(64, 64));
    occluded.beginStroke();
    auto blockedStats = paintBrush(occluded, blocked, *backHit, brush, V3(0, 0, 3));
    check(blockedStats.pixels == 0 && !occluded.endStroke(),
          "Visible-only brush cannot paint through front geometry");

    PaintTriangle face = seams.triangle(0);
    face.uv = {F2{-1, 0}, F2{0, 0}, F2{0, 1}};
    size_t count = 0;
    rasterizePaintTriangle(face, 32, 32, [&](int x, int y, const V3& barycentric) {
        check(x >= 0 && x < 32 && y >= 0 && y < 32 && std::abs(barycentric.sum() - 1) < 1e-8,
              "Repeated UV raster coordinates and barycentrics stay valid");
        ++count;
    });
    check(count > 400, "Negative UV coordinates wrap onto target canvas");
    face.uv = {F2{0, 0}, F2{100000, 0}, F2{0, 100000}};
    fails([&] { rasterizePaintTriangle(face, 32, 32, [](int, int, const V3&) {}); },
          "Excessive repeating UV fails before scanning");
    face = seams.triangle(0);
    PaintRasterOptions limited;
    limited.maxPixels = 1;
    fails([&] { rasterizePaintTriangle(face, 1024, 1024, [](int, int, const V3&) {}, limited); },
          "Raster pixel budget prevents unbounded loops");
    PaintBrush huge = brush;
    huge.radius = 10;
    huge.maxPixels = 1;
    canvas.beginStroke();
    fails([&] { paintBrush(canvas, seams, *hit, huge); }, "Interactive brush total pixel work is bounded");
    canvas.cancelStroke();
    check(canvas.image().rgba == drawn, "Rejected brush leaves original pixels intact");
    std::atomic_bool cancel = true;
    canvas.beginStroke();
    fails([&] { paintBrush(canvas, seams, *hit, brush, {}, &cancel); }, "Brush cancellation is explicit");
    canvas.cancelStroke();
    check(canvas.image().rgba == drawn, "Canceled brush preserves prior image");
}
void realModel(const fs::path& path, const fs::path& output) {
    auto loaded = Scene::load(path);
    auto start = Clock::now();
    PaintSurface surface(loaded, {}, [](const std::string& message) { std::cout << message << '\n'; });
    double build = seconds(start);
    auto [minimum, maximum] = surface.bounds();
    V3 center = (minimum + maximum) * .5;
    double radius = std::max(.01, (maximum - minimum).norm() * .5);
    std::optional<PaintHit> hit;
    V3 eye;
    for (V3 direction : {V3(1, .5, 1), V3(0, 1, 0), V3(0, 0, 1), V3(1, 0, 0)}) {
        eye = center + direction.normalized() * radius * 3;
        hit = surface.raycast(eye, center - eye);
        if (hit && surface.triangle(hit->primitive).hasUV)
            break;
    }
    require(hit.has_value(), "Real-model centre rays did not hit geometry");
    PaintCanvas canvas(PaintImage(1024, 1024));
    PaintBrush brush;
    brush.radius = radius * .006;
    brush.color = {1, .05f, .05f, 1};
    start = Clock::now();
    for (int i = 0; i < 1000; ++i)
        surface.raycast(eye, hit->position - eye);
    double rayMs = seconds(start);
    start = Clock::now();
    canvas.beginStroke();
    auto result = paintBrush(canvas, surface, *hit, brush, eye);
    canvas.endStroke();
    double brushMs = seconds(start) * 1000;
    canvas.image().savePNG(output / "real-model-stroke.png");
    Json report{{"source", pathString(path)},
                {"triangles", surface.triangleCount()},
                {"surface_build_seconds", build},
                {"raycast_mean_ms", rayMs},
                {"brush_ms", brushMs},
                {"painted_pixels", result.pixels},
                {"material", hit->material},
                {"mesh", hit->mesh},
                {"brush_triangles", result.triangles}};
    writeJson(output / "real-model-metrics.json", report);
    std::cout << report.dump(2) << '\n';
    require(result.pixels > 0, "Real-model brush did not change any sampled texels");
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        ComRuntime imageRuntime;
        fs::path output = fs::current_path() / "validation" / "paint-core-tests";
        fs::create_directories(output);
        workerImageLifetime(output);
        std::cout << "Paint image/history tests\n" << std::flush;
        imageAndHistory(output);
        std::cout << "Paint picking/pose tests\n" << std::flush;
        pickingAndPoses();
        std::cout << "Paint brush/raster tests\n" << std::flush;
        brushesAndRaster(output);
        if (argc >= 3 && std::wstring_view(argv[1]) == L"--real")
            realModel(argv[2], output);
        std::cout << "Paint core tests passed: " << checks << " checks\n";
        return 0;
    } catch (...) {
        auto error = exceptionText();
        writeJson(fs::current_path() / "validation" / "paint-core-tests" / "failure.json",
                  {{"error", error}});
        std::cerr << error << '\n';
        return 1;
    }
}
