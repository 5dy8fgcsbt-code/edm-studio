#include "paint_mapping_common.h"

namespace edm::paint_mapping {
F4 sample(const PaintImage& image, double u, double v, float opacity) {
    // Interpolate premultiplied RGB so transparent PNG borders cannot introduce dark fringes.
    double x = std::clamp(u * image.width - .5, 0., double(image.width - 1));
    double y = std::clamp(v * image.height - .5, 0., double(image.height - 1));
    uint32_t x0 = uint32_t(x), y0 = uint32_t(y);
    uint32_t x1 = std::min(x0 + 1, image.width - 1), y1 = std::min(y0 + 1, image.height - 1);
    double tx = x - x0, ty = y - y0;
    std::array<uint32_t, 4> xs{x0, x1, x0, x1}, ys{y0, y0, y1, y1};
    std::array<double, 4> weights{(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
    F4 result{};
    double alpha = 0;
    std::array<double, 3> rgb{};
    for (size_t i = 0; i < 4; ++i) {
        size_t offset = (size_t(ys[i]) * image.width + xs[i]) * 4;
        double a = image.rgba[offset + 3] / 255. * weights[i];
        alpha += a;
        for (int channel = 0; channel < 3; ++channel)
            rgb[channel] += image.rgba[offset + channel] / 255. * a;
    }
    if (alpha > 1e-12)
        for (int channel = 0; channel < 3; ++channel)
            result[channel] = float(rgb[channel] / alpha);
    result[3] = float(alpha) * opacity;
    return result;
}

void validate(const PaintCanvas& canvas, const PaintImage& image, int material, int mesh, float opacity,
              const PaintMappingLimits& limits, bool externalStroke, std::span<const int> targetMaterials) {
    canvas.image().validate();
    image.validate();
    require((targetMaterials.empty() ? material >= 0 : material >= -1) && mesh >= -1,
            "Select a target material before mapping an image");
    require(std::all_of(targetMaterials.begin(), targetMaterials.end(), [](int value) { return value >= 0; }),
            "Invalid shared texture material selection");
    require(std::isfinite(opacity) && opacity >= 0 && opacity <= 1, "Invalid mapping opacity");
    require(limits.rasterSamples && limits.rayTests && std::isfinite(limits.seconds) && limits.seconds > 0,
            "Invalid image mapping budget");
    require(canvas.strokeActive() == externalStroke,
            externalStroke ? "External image mapping requires an active canvas stroke"
                           : "Finish the current brush stroke before mapping an image");
    require(&canvas.image() != &image, "Mapping source must be a separate image from the target canvas");
}

Operation::Operation(PaintCanvas& canvas, const std::string& label, const PaintMappingLimits& limits,
                     Progress progress, const std::atomic_bool* cancel, bool externalStroke)
    : canvas(canvas), limits(limits), progress(std::move(progress)), cancel(cancel),
      ownsStroke(!externalStroke) {
    require(canvas.strokeActive() == externalStroke, "Invalid image mapping stroke ownership");
    check();
    auto& image = canvas.image();
    visited.resize((uint64_t(image.width) * image.height + 63) / 64);
    if (ownsStroke)
        canvas.beginStroke(label);
}

Operation::~Operation() {
    if (ownsStroke && !finished) {
        try {
            canvas.cancelStroke();
        } catch (...) {
            // Destruction must preserve the original cancellation/budget exception.
        }
    }
}

void Operation::check() {
    if (cancel && cancel->load(std::memory_order_relaxed))
        throw std::runtime_error("Image mapping cancelled");
    if (edm::seconds(started) > limits.seconds)
        throw std::runtime_error("图片映射超过时间预算，此次操作已回滚；请缩小覆盖范围或降低新贴图精度。");
    if (progress && report.rasterSamples >= nextProgress) {
        progress("映射像素 " + std::to_string(report.paintedPixels) + "，采样 " +
                 std::to_string(report.rasterSamples));
        nextProgress = report.rasterSamples + 65536;
    }
}

void Operation::sampleVisited() {
    if (++report.rasterSamples > limits.rasterSamples)
        throw std::runtime_error("Image mapping pixel budget exceeded; reduce image size or select a mesh");
    if ((report.rasterSamples & 1023) == 0)
        check();
}

void Operation::rayTest() {
    if (++report.rayTests > limits.rayTests)
        throw std::runtime_error("Image mapping ray budget exceeded; reduce image size or select a mesh");
}

void Operation::blend(int x, int y, const F4& color, bool preserveAlpha) {
    if (color[3] <= 0)
        return;
    size_t index = size_t(y) * canvas.image().width + x;
    uint64_t bit = 1ull << (index & 63);
    if (visited[index / 64] & bit) {
        ++report.reusedTexels;
        return;
    }
    visited[index / 64] |= bit;
    if (canvas.blendPixel(x, y, color, 1, preserveAlpha))
        ++report.paintedPixels;
}

PaintMappingReport Operation::finish() {
    check();
    report.changed = ownsStroke ? canvas.endStroke() : report.paintedPixels != 0;
    finished = true;
    report.seconds = edm::seconds(started);
    if (report.reusedTexels)
        report.warnings.push_back(
            "共享或重复 UV 只写入一次；优先处理靠近外层的面，共用 UV 的部件会同时显示修改。");
    if (!report.paintedPixels)
        report.warnings.push_back("没有像素被修改：检查目标材质、图片范围、表面朝向和遮挡设置。");
    return std::move(report);
}

bool targetTriangle(const PaintTriangle& triangle, int material, int mesh,
                    std::span<const int> targetMaterials) {
    return triangle.hasUV && (mesh < 0 || triangle.mesh == uint32_t(mesh)) &&
           (targetMaterials.empty() ? material < 0 || triangle.material == material
                                    : std::find(targetMaterials.begin(), targetMaterials.end(),
                                                triangle.material) != targetMaterials.end());
}

V3 position(const PaintTriangle& triangle, const V3& barycentric) {
    return triangle.world[0] * barycentric[0] + triangle.world[1] * barycentric[1] +
           triangle.world[2] * barycentric[2];
}

double distanceBias(double distance) {
    return std::max(1e-6, distance * 2e-6);
}
} // namespace edm::paint_mapping
