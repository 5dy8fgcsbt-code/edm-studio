#include "paint_wrap.h"
#include <iostream>

namespace {
using namespace edm;
int checks = 0;
void check(bool condition, const char* message) {
    require(condition, std::string("Cylinder mapping: ") + message);
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

void cylinder(Scene& scene, float radius = 1, int material = 0, int segments = 32) {
    Mesh mesh;
    mesh.material = material;
    mesh.uvs.resize(1);
    constexpr double pi = 3.14159265358979323846;
    for (int i = 0; i <= segments; ++i) {
        double angle = -pi + 2 * pi * i / segments;
        float y = float(std::cos(angle)), z = float(std::sin(angle));
        mesh.positions.push_back({-1, radius * y, radius * z});
        mesh.positions.push_back({1, radius * y, radius * z});
        mesh.normals.push_back({0, y, z});
        mesh.normals.push_back({0, y, z});
        mesh.uvs[0].push_back({float(i) / segments, 1});
        mesh.uvs[0].push_back({float(i) / segments, 0});
        if (i < segments) {
            uint32_t base = uint32_t(i * 2);
            mesh.indices.insert(mesh.indices.end(), {base, base + 3, base + 1, base, base + 2, base + 3});
        }
    }
    scene.meshes.push_back(std::move(mesh));
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

void runPaintWrapTests() {
    using namespace edm;
    auto model = scene();
    cylinder(*model);
    PaintSurface surface(model);
    CylinderWrapOptions options;
    options.material = 0;
    options.height = 2;
    PaintImage image(2, 2);
    image.rgba = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    PaintCanvas canvas(PaintImage(64, 32, {0, 0, 0, 255}));
    auto report = applyCylindricalWrap(surface, canvas, image, options);
    check(report.changed && report.paintedPixels == 64 * 32, "full cylinder receives one complete image");
    check(channel(canvas, 3, 3, 0) == 255 && channel(canvas, 3, 3, 1) == 0,
          "negative angle starts at image left");
    check(channel(canvas, 60, 3, 1) == 255 && channel(canvas, 60, 3, 0) == 0,
          "positive angle reaches image right");
    check(channel(canvas, 3, 29, 2) == 255 && channel(canvas, 3, 29, 0) == 0,
          "positive axis maps to image top");
    auto mapped = canvas.image().rgba;
    check(canvas.undo() && channel(canvas, 3, 3) == 0, "one undo restores all wrapped pixels");
    check(canvas.redo() && canvas.image().rgba == mapped, "redo reproduces identical wrap");

    PaintImage red(1, 1, {255, 0, 0, 255});
    PaintCanvas limited(PaintImage(64, 32, {0, 0, 0, 255}));
    options.height = 1;
    options.sweepRadians = 3.14159265358979323846;
    report = applyCylindricalWrap(surface, limited, red, options);
    check(report.changed && channel(limited, 32, 16) == 255 && channel(limited, 3, 16) == 0 &&
              channel(limited, 32, 1) == 0,
          "height and angular sweep clip the wrap");
    options.angleRadians = 3.14159265358979323846;
    PaintCanvas rotated(PaintImage(64, 32, {0, 0, 0, 255}));
    applyCylindricalWrap(surface, rotated, red, options);
    check(channel(rotated, 3, 16) == 255 && channel(rotated, 32, 16) == 0,
          "angle rotates the selected wrap sector across its seam");

    options = CylinderWrapOptions{};
    options.material = 0;
    options.height = 2;
    auto repeated = scene();
    cylinder(*repeated);
    for (auto& uv : repeated->meshes[0].uvs[0])
        uv[0] *= 2;
    PaintSurface repeatedSurface(repeated);
    PaintCanvas translucent(PaintImage(32, 16, {0, 0, 0, 73}));
    PaintImage halfRed(1, 1, {255, 0, 0, 128});
    report = applyCylindricalWrap(repeatedSurface, translucent, halfRed, options);
    check(report.reusedTexels > 0 && !report.warnings.empty(), "repeated UV ambiguity is reported");
    check(channel(translucent, 8, 8) >= 127 && channel(translucent, 8, 8) <= 129 &&
              channel(translucent, 8, 8, 3) == 73,
          "repeated samples do not accumulate opacity or destroy existing alpha");

    auto nested = scene();
    cylinder(*nested, 1, 0);
    cylinder(*nested, 2, 1);
    PaintSurface nestedSurface(nested);
    PaintCanvas hidden(PaintImage(32, 16, {0, 0, 0, 255}));
    report = applyCylindricalWrap(nestedSurface, hidden, red, options);
    check(!report.changed && report.occludedSamples > 0, "other materials occlude the inner cylinder");
    options.occlusion = false;
    report = applyCylindricalWrap(nestedSurface, hidden, red, options);
    check(report.changed, "explicit through-surface wrapping can disable occlusion");

    auto moving = scene();
    cylinder(*moving);
    Track move;
    move.node = 0;
    move.arg = 38;
    move.channel = Channel::Position;
    move.keys = {{0, V4(0, 0, 0, 0)}, {1, V4(0, 4, 0, 0)}};
    moving->tracks.push_back(move);
    PaintSurface posed(moving, {{38, .75}});
    PaintCanvas moved(PaintImage(32, 16, {0, 0, 0, 255}));
    options.center = V3(0, 3, 0);
    options.occlusion = true;
    report = applyCylindricalWrap(posed, moved, red, options);
    check(report.paintedPixels == 32 * 16, "wrap uses the current animated world pose");

    PaintImage fringe(2, 1);
    fringe.rgba = {255, 0, 0, 255, 0, 0, 0, 0};
    auto mixed = paint_mapping::sample(fringe, .5, .5, 1);
    check(mixed[0] > .99f && mixed[1] == 0 && std::abs(mixed[3] - .5f) < .001f,
          "transparent image borders use premultiplied filtering");

    options.center.setZero();
    options.axis.setZero();
    auto before = hidden.image().rgba;
    rejected([&] { applyCylindricalWrap(surface, hidden, red, options); },
             "invalid cylinder axis is rejected");
    check(hidden.image().rgba == before && !hidden.strokeActive(),
          "invalid settings leave the canvas intact");
    options.axis = V3::UnitX();
    options.limits.rasterSamples = 10;
    PaintCanvas budget(PaintImage(64, 32, {0, 0, 0, 255}));
    before = budget.image().rgba;
    rejected([&] { applyCylindricalWrap(surface, budget, red, options); }, "pixel budget is enforced");
    check(budget.image().rgba == before && !budget.strokeActive() && !budget.canUndo(),
          "budget failure rolls the operation back");

    options.limits = {};
    options.occlusion = false;
    PaintCanvas cancelled(PaintImage(512, 256, {0, 0, 0, 255}));
    before = cancelled.image().rgba;
    std::atomic_bool cancel = false;
    int progressCalls = 0;
    rejected(
        [&] {
            applyCylindricalWrap(
                surface, cancelled, red, options,
                [&](const std::string&) {
                    if (++progressCalls == 2)
                        cancel = true;
                },
                &cancel);
        },
        "cancellation interrupts an operation in progress");
    check(progressCalls >= 2 && cancelled.image().rgba == before && !cancelled.strokeActive() &&
              !cancelled.canUndo(),
          "cancel restores every pixel after partial mapping");
    std::cout << "Cylinder wrap: " << checks << " checks passed\n";
}

int wmain() {
    try {
        edm::ComRuntime imageRuntime;
        runPaintWrapTests();
        return 0;
    } catch (...) {
        std::cerr << edm::exceptionText() << '\n';
        return 1;
    }
}
