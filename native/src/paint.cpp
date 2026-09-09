#include "paint.h"
#include <deque>
#include <wincodec.h>

namespace edm {
namespace {
constexpr uint32_t tileSize = 64;
std::atomic<uint64_t> nextPaintHistoryToken{1};
uint64_t reservePaintHistoryToken() {
    auto token = nextPaintHistoryToken.load(std::memory_order_relaxed);
    do {
        if (token == UINT64_MAX)
            throw std::overflow_error("Paint history token range exhausted");
    } while (!nextPaintHistoryToken.compare_exchange_weak(token, token + 1, std::memory_order_relaxed));
    return token;
}
V3 vector(const F3& v) {
    return V3(v[0], v[1], v[2]);
}
void cancelled(const std::atomic_bool* cancel) {
    require(!cancel || !cancel->load(), "涂装操作已取消");
}
void imageResult(HRESULT result, const char* operation) {
    require(SUCCEEDED(result), std::string(operation) + " failed: " + std::to_string(uint32_t(result)));
}
int repeat(int value, int size) {
    int result = value % size;
    return result < 0 ? result + size : result;
}
uint8_t byte(float value) {
    return uint8_t(std::lround(std::clamp(value, 0.f, 1.f) * 255));
}
Eigen::Matrix3d normalMatrix(const Mat& matrix) {
    Eigen::Matrix3d linear = matrix.block<3, 3>(0, 0);
    if (std::abs(linear.determinant()) < 1e-20)
        return Eigen::Matrix3d::Zero();
    return linear.inverse().transpose();
}
std::optional<std::array<double, 4>> brushUVBounds(const PaintTriangle& face, const V3& center,
                                                   double radius) {
    // Clip the actual finite triangle, rather than its infinite plane. Thin world triangles can
    // map a tiny brush to enormous infinite-plane UV bounds although almost none of the face is near
    // the brush. Retain original barycentrics: rasterization still evaluates the original triangle.
    std::array<V3, 16> polygon{}, next{};
    polygon[0] = V3::UnitX();
    polygon[1] = V3::UnitY();
    polygon[2] = V3::UnitZ();
    size_t count = 3;
    auto position = [&](const V3& barycentric) {
        return V3(face.world[0] * barycentric.x() + face.world[1] * barycentric.y() +
                  face.world[2] * barycentric.z());
    };
    double scale = radius;
    for (const auto& point : face.world)
        scale = std::max(scale, point.cwiseAbs().maxCoeff());
    const double epsilon = std::max(1e-12, scale * 1e-10);
    for (int axis = 0; axis < 3; ++axis) {
        for (double sign : {-1., 1.}) {
            if (!count)
                return {};
            size_t output = 0;
            auto distance = [&](const V3& barycentric) {
                return radius + epsilon - sign * (position(barycentric)[axis] - center[axis]);
            };
            V3 previous = polygon[count - 1];
            double before = distance(previous);
            for (size_t i = 0; i < count; ++i) {
                const V3 current = polygon[i];
                double after = distance(current);
                if ((before >= 0) != (after >= 0))
                    next[output++] = previous + (current - previous) * (before / (before - after));
                if (after >= 0)
                    next[output++] = current;
                previous = current;
                before = after;
            }
            polygon = next;
            count = output;
        }
    }
    if (!count)
        return {};
    std::array<double, 4> bounds{1e100, 1e100, -1e100, -1e100};
    for (size_t i = 0; i < count; ++i)
        for (int axis = 0; axis < 2; ++axis) {
            double uv = 0;
            for (int vertex = 0; vertex < 3; ++vertex)
                uv += polygon[i][vertex] * double(face.uv[vertex][axis]);
            bounds[axis] = std::min(bounds[axis], uv);
            bounds[axis + 2] = std::max(bounds[axis + 2], uv);
        }
    for (int axis = 0; axis < 2; ++axis) {
        double slack = 1e-10 * std::max({1., std::abs(bounds[axis]), std::abs(bounds[axis + 2])});
        bounds[axis] -= slack;
        bounds[axis + 2] += slack;
    }
    return bounds;
}
} // namespace

PaintImage::PaintImage(uint32_t w, uint32_t h, std::array<uint8_t, 4> color) : width(w), height(h) {
    require(w && h && w <= 16384 && h <= 16384 && uint64_t(w) * h <= 64ull * 1024 * 1024,
            "涂装画布尺寸超限（最多 6400 万像素）");
    rgba.resize(size_t(w) * h * 4);
    for (size_t i = 0; i < rgba.size(); i += 4)
        std::copy(color.begin(), color.end(), rgba.begin() + i);
}
void PaintImage::validate() const {
    require(width && height && width <= 16384 && height <= 16384 &&
                uint64_t(width) * height <= 64ull * 1024 * 1024 && rgba.size() == size_t(width) * height * 4,
            "Invalid RGBA paint image dimensions or byte count");
}
PaintImage PaintImage::load(const ImageSource& source, size_t maxSize) {
    auto path = source.entry.empty() ? source.path : fs::path(wide(source.entry));
    require(lower(pathString(path.extension())) != ".psd",
            "PSD 图层模板暂不支持，请先从图像编辑器导出为 PNG 或 TGA 平面模板。");
    auto loaded = loadTexture(source, maxSize);
    auto* image = loaded->pixels.GetImage(loaded->firstMip, 0, 0);
    require(image != nullptr, "模板没有可用图像");
    DirectX::ScratchImage decoded, converted;
    if (DirectX::IsCompressed(image->format)) {
        imageResult(DirectX::Decompress(*image, DXGI_FORMAT_R8G8B8A8_UNORM, decoded), "Decode paint DDS");
        image = decoded.GetImage(0, 0, 0);
    }
    if (image->format != DXGI_FORMAT_R8G8B8A8_UNORM && image->format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        imageResult(
            DirectX::Convert(*image, DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT, 0, converted),
            "Convert paint image");
        image = converted.GetImage(0, 0, 0);
    }
    PaintImage result(uint32_t(image->width), uint32_t(image->height));
    for (uint32_t y = 0; y < result.height; ++y)
        std::memcpy(result.rgba.data() + size_t(y) * result.width * 4, image->pixels + y * image->rowPitch,
                    size_t(result.width) * 4);
    return result;
}
std::shared_ptr<TextureImage> PaintImage::texture(bool generateMips) const {
    ComApartment apartment;
    validate();
    auto result = std::make_shared<TextureImage>();
    imageResult(result->pixels.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, width, height, 1, 1),
                "Allocate paint texture");
    auto* image = result->pixels.GetImage(0, 0, 0);
    for (uint32_t y = 0; y < height; ++y)
        std::memcpy(image->pixels + y * image->rowPitch, rgba.data() + size_t(y) * width * 4,
                    size_t(width) * 4);
    if (generateMips) {
        DirectX::ScratchImage mips;
        imageResult(DirectX::GenerateMipMaps(*image, DirectX::TEX_FILTER_DEFAULT, 0, mips),
                    "Generate paint preview mipmaps");
        result->pixels = std::move(mips);
    }
    result->srgb = true;
    return result;
}
std::vector<uint8_t> PaintImage::pngBytes() const {
    ComApartment apartment;
    auto pixels = texture();
    DirectX::Blob encoded;
    imageResult(DirectX::SaveToWICMemory(*pixels->pixels.GetImage(0, 0, 0), DirectX::WIC_FLAGS_NONE,
                                         GUID_ContainerFormatPng, encoded),
                "Encode paint PNG");
    const auto* first = static_cast<const uint8_t*>(encoded.GetBufferPointer());
    return {first, first + encoded.GetBufferSize()};
}
void PaintImage::savePNG(const fs::path& path) const {
    auto encoded = pngBytes();
    writeFile(path, encoded);
}
void PaintImage::saveDDS(const fs::path& path) const {
    auto pixels = texture(true);
    DirectX::Blob encoded;
    imageResult(DirectX::SaveToDDSMemory(pixels->pixels.GetImages(), pixels->pixels.GetImageCount(),
                                         pixels->pixels.GetMetadata(), DirectX::DDS_FLAGS_NONE, encoded),
                "Encode paint DDS");
    writeFile(path, {static_cast<const uint8_t*>(encoded.GetBufferPointer()), encoded.GetBufferSize()});
}

struct PaintCanvas::Impl {
    struct Tile {
        uint32_t x = 0, y = 0, width = 0, height = 0;
        std::vector<uint8_t> before, after;
    };
    struct Entry {
        uint64_t token = 0;
        std::string label;
        std::vector<Tile> tiles;
        size_t bytes() const {
            size_t result = 0;
            for (const auto& tile : tiles)
                result += tile.before.size() + tile.after.size();
            return result;
        }
    };
    PaintImage image;
    std::optional<PaintRect> dirty;
    uint64_t revision = 1;
    size_t budget, history = 0;
    std::vector<std::shared_ptr<Entry>> undo, redo;
    std::shared_ptr<Entry> active;
    bool prepared = false;
    uint64_t preparedUndo = 0, preparedRedo = 0;
    std::unordered_map<uint32_t, size_t> touched;
    explicit Impl(PaintImage pixels, size_t memory) : image(std::move(pixels)), budget(memory) {
        image.validate();
        dirty = PaintRect{0, 0, int(image.width), int(image.height)};
    }
    void mark(PaintRect rectangle) noexcept {
        if (!dirty)
            dirty = rectangle;
        else {
            dirty->x0 = std::min(dirty->x0, rectangle.x0);
            dirty->y0 = std::min(dirty->y0, rectangle.y0);
            dirty->x1 = std::max(dirty->x1, rectangle.x1);
            dirty->y1 = std::max(dirty->y1, rectangle.y1);
        }
    }
    std::vector<uint8_t> copy(const Tile& tile) const {
        std::vector<uint8_t> result(size_t(tile.width) * tile.height * 4);
        for (uint32_t y = 0; y < tile.height; ++y)
            std::memcpy(result.data() + size_t(y) * tile.width * 4,
                        image.rgba.data() + (size_t(tile.y + y) * image.width + tile.x) * 4,
                        size_t(tile.width) * 4);
        return result;
    }
    void restore(const Tile& tile, const std::vector<uint8_t>& pixels) noexcept {
        for (uint32_t y = 0; y < tile.height; ++y)
            std::memcpy(image.rgba.data() + (size_t(tile.y + y) * image.width + tile.x) * 4,
                        pixels.data() + size_t(y) * tile.width * 4, size_t(tile.width) * 4);
        mark({int(tile.x), int(tile.y), int(tile.x + tile.width), int(tile.y + tile.height)});
        ++revision;
    }
    void saveBefore(uint32_t x, uint32_t y) {
        uint32_t tilesAcross = (image.width + tileSize - 1) / tileSize;
        uint32_t key = (y / tileSize) * tilesAcross + x / tileSize;
        if (touched.contains(key))
            return;
        Tile tile;
        tile.x = (x / tileSize) * tileSize;
        tile.y = (y / tileSize) * tileSize;
        tile.width = std::min(tileSize, image.width - tile.x);
        tile.height = std::min(tileSize, image.height - tile.y);
        tile.before = copy(tile);
        touched.emplace(key, active->tiles.size());
        active->tiles.push_back(std::move(tile));
    }
};
PaintCanvas::PaintCanvas(PaintImage image, size_t historyBudget)
    : impl(std::make_unique<Impl>(std::move(image), historyBudget)) {}
PaintCanvas::~PaintCanvas() = default;
PaintCanvas::PaintCanvas(PaintCanvas&&) noexcept = default;
PaintCanvas& PaintCanvas::operator=(PaintCanvas&&) noexcept = default;
const PaintImage& PaintCanvas::image() const {
    return impl->image;
}
uint64_t PaintCanvas::revision() const {
    return impl->revision;
}
size_t PaintCanvas::historyBytes() const {
    return impl->history;
}
std::optional<PaintRect> PaintCanvas::takeDirtyRect() {
    auto result = impl->dirty;
    impl->dirty.reset();
    return result;
}
void PaintCanvas::beginStroke(std::string label) {
    require(!impl->active, "A paint operation is already active");
    auto active = std::make_shared<Impl::Entry>();
    active->label = std::move(label);
    impl->active = std::move(active);
    impl->prepared = false;
    impl->preparedUndo = impl->preparedRedo = 0;
    impl->touched.clear();
}
bool PaintCanvas::prepareStroke() {
    require(bool(impl->active), "No active paint operation");
    if (impl->prepared)
        return impl->active->token != 0;
    // Keep before tiles intact until every allocation succeeds, so another canvas failing preparation
    // can still cancel this canvas without changing its existing undo/redo branch.
    auto& entry = *impl->active;
    for (auto& tile : entry.tiles)
        tile.after = impl->copy(tile);
    std::erase_if(entry.tiles, [](const Impl::Tile& tile) { return tile.before == tile.after; });
    if (!entry.tiles.empty()) {
        impl->undo.reserve(impl->undo.size() + 1);
        entry.token = reservePaintHistoryToken();
    }
    impl->prepared = true;
    return entry.token != 0;
}
uint64_t PaintCanvas::preparedStrokeToken() const noexcept {
    return impl->prepared && impl->active ? impl->active->token : 0;
}
uint64_t PaintCanvas::commitPreparedStroke() noexcept {
    if (!impl->prepared || !impl->active)
        return 0;
    auto token = impl->active->token;
    if (token) {
        // prepareStroke has reserved vector capacity; shared_ptr movement cannot allocate or throw.
        for (const auto& discarded : impl->redo)
            impl->history -= discarded->bytes();
        impl->redo.clear();
        impl->history += impl->active->bytes();
        impl->undo.push_back(impl->active);
        while (impl->undo.size() > 1 && impl->history > impl->budget) {
            impl->history -= impl->undo.front()->bytes();
            impl->undo.erase(impl->undo.begin());
        }
    }
    impl->active.reset();
    impl->prepared = false;
    impl->touched.clear();
    return token;
}
bool PaintCanvas::endStroke() {
    prepareStroke();
    return commitPreparedStroke() != 0;
}
void PaintCanvas::cancelStroke() noexcept {
    if (!impl->active)
        return;
    for (const auto& tile : impl->active->tiles)
        impl->restore(tile, tile.before);
    impl->active.reset();
    impl->prepared = false;
    impl->touched.clear();
}
bool PaintCanvas::strokeActive() const {
    return bool(impl->active);
}
bool PaintCanvas::canUndo() const {
    return !impl->active && !impl->undo.empty();
}
bool PaintCanvas::canRedo() const {
    return !impl->active && !impl->redo.empty();
}
uint64_t PaintCanvas::undoToken() const noexcept {
    return !impl->active && !impl->undo.empty() ? impl->undo.back()->token : 0;
}
uint64_t PaintCanvas::redoToken() const noexcept {
    return !impl->active && !impl->redo.empty() ? impl->redo.back()->token : 0;
}
bool PaintCanvas::prepareUndo(uint64_t token) {
    if (!token || undoToken() != token)
        return false;
    impl->redo.reserve(impl->redo.size() + 1);
    impl->preparedUndo = token;
    impl->preparedRedo = 0;
    return true;
}
bool PaintCanvas::prepareRedo(uint64_t token) {
    if (!token || redoToken() != token)
        return false;
    impl->undo.reserve(impl->undo.size() + 1);
    impl->preparedRedo = token;
    impl->preparedUndo = 0;
    return true;
}
bool PaintCanvas::commitPreparedUndo(uint64_t token) noexcept {
    if (!token || impl->preparedUndo != token || undoToken() != token)
        return false;
    auto entry = std::move(impl->undo.back());
    impl->undo.pop_back();
    for (const auto& tile : entry->tiles)
        impl->restore(tile, tile.before);
    impl->redo.push_back(std::move(entry));
    impl->preparedUndo = impl->preparedRedo = 0;
    return true;
}
bool PaintCanvas::commitPreparedRedo(uint64_t token) noexcept {
    if (!token || impl->preparedRedo != token || redoToken() != token)
        return false;
    auto entry = std::move(impl->redo.back());
    impl->redo.pop_back();
    for (const auto& tile : entry->tiles)
        impl->restore(tile, tile.after);
    impl->undo.push_back(std::move(entry));
    impl->preparedUndo = impl->preparedRedo = 0;
    return true;
}
bool PaintCanvas::undo() {
    auto token = undoToken();
    return prepareUndo(token) && commitPreparedUndo(token);
}
bool PaintCanvas::redo() {
    auto token = redoToken();
    return prepareRedo(token) && commitPreparedRedo(token);
}
bool PaintCanvas::blendPixel(int x, int y, const F4& color, float coverage, bool preserveAlpha) {
    if (!impl->active)
        throw std::runtime_error("Painting requires an active undo transaction");
    if (impl->prepared)
        throw std::runtime_error("Cannot paint after preparing a transaction");
    if (x < 0 || y < 0 || x >= int(impl->image.width) || y >= int(impl->image.height))
        return false;
    for (float value : color)
        if (!std::isfinite(value))
            throw std::runtime_error("Paint color is not finite");
    if (!std::isfinite(coverage))
        throw std::runtime_error("Paint coverage is not finite");
    float alpha = std::clamp(color[3], 0.f, 1.f) * std::clamp(coverage, 0.f, 1.f);
    if (alpha == 0)
        return false;
    auto* pixel = impl->image.rgba.data() + (size_t(y) * impl->image.width + x) * 4;
    std::array<uint8_t, 4> output;
    float destinationAlpha = pixel[3] / 255.f;
    float outputAlpha = alpha + destinationAlpha * (1 - alpha);
    for (int channel = 0; channel < 3; ++channel) {
        float source = std::clamp(color[channel], 0.f, 1.f), destination = pixel[channel] / 255.f;
        output[channel] =
            byte(preserveAlpha ? source * alpha + destination * (1 - alpha)
                               : (source * alpha + destination * destinationAlpha * (1 - alpha)) /
                                     std::max(outputAlpha, 1e-20f));
    }
    output[3] = preserveAlpha ? pixel[3] : byte(outputAlpha);
    if (std::equal(output.begin(), output.end(), pixel))
        return false;
    impl->saveBefore(uint32_t(x), uint32_t(y));
    std::copy(output.begin(), output.end(), pixel);
    impl->mark({x, y, x + 1, y + 1});
    ++impl->revision;
    return true;
}

V3 PaintTriangle::normal() const {
    V3 result = normals[0] + normals[1] + normals[2];
    if (result.squaredNorm() < 1e-24)
        result = (world[1] - world[0]).cross(world[2] - world[0]);
    return result.squaredNorm() > 1e-24 ? V3(result.normalized()) : V3::UnitY();
}
struct PaintSurface::Impl {
    struct Cache {
        std::vector<F3> positions, normals;
        std::vector<F2> uv;
        bool hasUV = false;
    };
    struct Primitive {
        uint32_t mesh, triangle;
    };
    struct Box {
        V3 lo = V3::Constant(std::numeric_limits<double>::infinity());
        V3 hi = V3::Constant(-std::numeric_limits<double>::infinity());
        void include(const V3& point) {
            lo = lo.cwiseMin(point);
            hi = hi.cwiseMax(point);
        }
        bool ray(const V3& origin, const V3& direction, double maximum) const {
            double lower = 0, upper = maximum;
            for (int axis = 0; axis < 3; ++axis) {
                if (std::abs(direction[axis]) < 1e-20) {
                    if (origin[axis] < lo[axis] || origin[axis] > hi[axis])
                        return false;
                } else {
                    double a = (lo[axis] - origin[axis]) / direction[axis];
                    double b = (hi[axis] - origin[axis]) / direction[axis];
                    if (a > b)
                        std::swap(a, b);
                    lower = std::max(lower, a);
                    upper = std::min(upper, b);
                    if (lower > upper)
                        return false;
                }
            }
            return true;
        }
        bool sphere(const V3& center, double radius) const {
            return (center.cwiseMax(lo).cwiseMin(hi) - center).squaredNorm() <= radius * radius;
        }
    };
    struct Bvh {
        Box box;
        uint32_t first = 0, count = 0, right = 0;
    };
    std::shared_ptr<const Scene> scene;
    std::vector<Cache> cache;
    std::vector<Primitive> primitives;
    std::vector<F3> centers;
    std::vector<uint32_t> order;
    std::vector<Bvh> nodes;
    Box primitiveBounds(uint32_t index) const {
        auto primitive = primitives[index];
        const auto& mesh = scene->meshes[primitive.mesh];
        const auto& positions = cache[primitive.mesh].positions;
        Box box;
        for (int k = 0; k < 3; ++k)
            box.include(vector(positions[mesh.indices[size_t(primitive.triangle) * 3 + k]]));
        return box;
    }
    uint32_t build(uint32_t first, uint32_t count, uint32_t depth, const std::atomic_bool* cancel) {
        cancelled(cancel);
        uint32_t index = uint32_t(nodes.size());
        nodes.emplace_back();
        Box box, centroid;
        for (uint32_t i = first; i < first + count; ++i) {
            if ((i & 4095) == 0)
                cancelled(cancel);
            auto bounds = primitiveBounds(order[i]);
            box.include(bounds.lo);
            box.include(bounds.hi);
            centroid.include(vector(centers[order[i]]));
        }
        nodes[index].box = box;
        if (count <= 12 || depth >= 48 || (centroid.hi - centroid.lo).maxCoeff() < 1e-10) {
            nodes[index].first = first;
            nodes[index].count = count;
            return index;
        }
        Eigen::Index axis = 0;
        (centroid.hi - centroid.lo).maxCoeff(&axis);
        uint32_t middle = first + count / 2;
        std::nth_element(order.begin() + first, order.begin() + middle, order.begin() + first + count,
                         [&](uint32_t a, uint32_t b) { return centers[a][axis] < centers[b][axis]; });
        build(first, middle - first, depth + 1, cancel);
        nodes[index].right = build(middle, first + count - middle, depth + 1, cancel);
        return index;
    }
};
PaintSurface::PaintSurface(std::shared_ptr<const Scene> scene, const Args& args, Progress progress,
                           const std::atomic_bool* cancel, bool attachmentsVisible)
    : impl(std::make_unique<Impl>()) {
    require(scene != nullptr, "Painting requires a loaded scene");
    impl->scene = std::move(scene);
    auto world = impl->scene->evaluate(args, attachmentsVisible);
    auto effectiveArgs = impl->scene->defaultArgs;
    for (const auto& [argument, value] : args)
        effectiveArgs[argument] = value;
    impl->cache.resize(impl->scene->meshes.size());
    size_t possible = 0;
    for (const auto& mesh : impl->scene->meshes)
        if (attachmentsVisible || !mesh.extras.contains("edm_attachment"))
            possible += mesh.indices.size() / 3;
    require(possible <= std::numeric_limits<uint32_t>::max(), "Paint geometry exceeds index range");
    impl->primitives.reserve(possible);
    impl->centers.reserve(possible);
    if (progress)
        progress("准备当前动画姿态的绘制表面…");
    for (uint32_t mi = 0; mi < impl->scene->meshes.size(); ++mi) {
        cancelled(cancel);
        const auto& mesh = impl->scene->meshes[mi];
        // Visibility is an explicit geometric exclusion, not just a zero-scale transform.
        // In particular, skin bones can otherwise leave pickable geometry outside a hidden root.
        if (!attachmentsVisible && mesh.extras.contains("edm_attachment"))
            continue;
        auto& cache = impl->cache[mi];
        require(mesh.material >= 0 && mesh.material < int(impl->scene->materials.size()),
                "Invalid paint material index");
        if (!mesh.skinned() && world[mesh.node].block<3, 3>(0, 0).cwiseAbs().maxCoeff() < 1e-20)
            continue;
        // Number atlases are separate overlays. They must not intercept a stroke on the diffuse base.
        if (!mesh.numbers.empty())
            continue;
        cache.positions = impl->scene->transformed(mesh, world);
        cache.hasUV = !mesh.uvs.empty();
        cache.uv = textureUV(mesh, impl->scene->materials[mesh.material], 0, effectiveArgs);
        cache.normals.resize(mesh.positions.size());
        std::vector<Eigen::Matrix3d> palette;
        if (mesh.skinned()) {
            for (size_t j = 0; j < mesh.skinNodes.size(); ++j)
                palette.push_back(normalMatrix(world[mesh.skinNodes[j]] * mesh.inverseBind[j]));
        } else
            palette.push_back(normalMatrix(world[mesh.node]));
        for (size_t vertex = 0; vertex < mesh.positions.size(); ++vertex) {
            if ((vertex & 16383) == 0)
                cancelled(cancel);
            V3 normal = vertex < mesh.normals.size() ? vector(mesh.normals[vertex]) : V3::Zero();
            V3 result = V3::Zero();
            if (mesh.skinned()) {
                for (size_t joint = 0; joint < 8; ++joint)
                    if (mesh.weights[vertex][joint])
                        result += palette[mesh.joints[vertex][joint]] * normal * mesh.weights[vertex][joint];
            } else
                result = palette[0] * normal;
            if (result.squaredNorm() > 1e-24)
                result.normalize();
            cache.normals[vertex] = {float(result.x()), float(result.y()), float(result.z())};
        }
        for (uint32_t triangle = 0; triangle < mesh.indices.size() / 3; ++triangle) {
            V3 a = vector(cache.positions[mesh.indices[size_t(triangle) * 3]]);
            V3 b = vector(cache.positions[mesh.indices[size_t(triangle) * 3 + 1]]);
            V3 c = vector(cache.positions[mesh.indices[size_t(triangle) * 3 + 2]]);
            if (!a.allFinite() || !b.allFinite() || !c.allFinite() ||
                (b - a).cross(c - a).squaredNorm() < 1e-24)
                continue;
            V3 center = (a + b + c) / 3;
            impl->primitives.push_back({mi, triangle});
            impl->centers.push_back({float(center.x()), float(center.y()), float(center.z())});
        }
    }
    cancelled(cancel);
    if (progress)
        progress("构建绘制拾取加速结构（" + std::to_string(impl->primitives.size()) + " 个三角面）…");
    impl->order.resize(impl->primitives.size());
    std::iota(impl->order.begin(), impl->order.end(), 0);
    impl->nodes.reserve(impl->primitives.size() / 4 + 1);
    if (!impl->primitives.empty())
        impl->build(0, uint32_t(impl->primitives.size()), 0, cancel);
    impl->centers.clear();
    impl->centers.shrink_to_fit();
}
PaintSurface::~PaintSurface() = default;
PaintSurface::PaintSurface(PaintSurface&&) noexcept = default;
PaintSurface& PaintSurface::operator=(PaintSurface&&) noexcept = default;
const Scene& PaintSurface::scene() const {
    return *impl->scene;
}
size_t PaintSurface::triangleCount() const {
    return impl->primitives.size();
}
PaintTriangle PaintSurface::triangle(uint32_t primitive) const {
    if (primitive >= impl->primitives.size())
        throw std::runtime_error("Paint triangle index is out of range");
    auto source = impl->primitives[primitive];
    const auto& mesh = impl->scene->meshes[source.mesh];
    const auto& cache = impl->cache[source.mesh];
    PaintTriangle result;
    result.mesh = source.mesh;
    result.index = source.triangle;
    result.material = mesh.material;
    result.hasUV = cache.hasUV;
    for (int k = 0; k < 3; ++k) {
        auto index = mesh.indices[size_t(source.triangle) * 3 + k];
        result.world[k] = vector(cache.positions[index]);
        result.normals[k] = vector(cache.normals[index]);
        result.uv[k] = cache.uv[index];
    }
    return result;
}
std::optional<PaintHit> PaintSurface::raycast(const V3& origin, const V3& direction, int material,
                                              double maxDistance) const {
    if (!origin.allFinite() || !direction.allFinite() || direction.norm() <= 1e-15 || std::isnan(maxDistance))
        throw std::runtime_error("Invalid paint ray");
    if (impl->nodes.empty() || maxDistance <= 0)
        return {};
    V3 ray = direction.normalized();
    std::optional<PaintHit> hit;
    // BVH construction is capped at depth 48; a fixed stack removes per-texel heap allocations.
    std::array<uint32_t, 128> pending{};
    size_t pendingCount = 1;
    while (pendingCount) {
        auto index = pending[--pendingCount];
        const auto& node = impl->nodes[index];
        if (!node.box.ray(origin, ray, maxDistance))
            continue;
        if (node.count == 0) {
            if (pendingCount + 2 > pending.size())
                throw std::runtime_error("Paint BVH traversal stack exceeded its depth limit");
            pending[pendingCount++] = node.right;
            pending[pendingCount++] = index + 1;
            continue;
        }
        for (uint32_t i = node.first; i < node.first + node.count; ++i) {
            uint32_t primitive = impl->order[i];
            auto mesh = impl->primitives[primitive].mesh;
            if (material != -1 && impl->scene->meshes[mesh].material != material)
                continue;
            auto face = triangle(primitive);
            V3 ab = face.world[1] - face.world[0], ac = face.world[2] - face.world[0];
            V3 p = ray.cross(ac);
            double determinant = ab.dot(p);
            if (std::abs(determinant) < 1e-15)
                continue;
            double inverse = 1 / determinant;
            V3 t = origin - face.world[0];
            double u = t.dot(p) * inverse;
            if (u < -1e-9 || u > 1 + 1e-9)
                continue;
            V3 q = t.cross(ab);
            double v = ray.dot(q) * inverse;
            if (v < -1e-9 || u + v > 1 + 1e-9)
                continue;
            double distance = ac.dot(q) * inverse;
            if (distance < 1e-8 || distance > maxDistance)
                continue;
            PaintHit candidate;
            candidate.primitive = primitive;
            candidate.mesh = face.mesh;
            candidate.triangle = face.index;
            candidate.material = face.material;
            candidate.distance = distance;
            candidate.position = origin + ray * distance;
            candidate.barycentric = V3(1 - u - v, u, v);
            candidate.normal = face.normals[0] * (1 - u - v) + face.normals[1] * u + face.normals[2] * v;
            if (candidate.normal.squaredNorm() > 1e-24)
                candidate.normal.normalize();
            else
                candidate.normal = face.normal();
            for (int axis = 0; axis < 2; ++axis)
                candidate.uv[axis] =
                    float(face.uv[0][axis] * (1 - u - v) + face.uv[1][axis] * u + face.uv[2][axis] * v);
            hit = candidate;
            maxDistance = distance;
        }
    }
    return hit;
}
std::vector<uint32_t> PaintSurface::querySphere(const V3& center, double radius, int material) const {
    require(center.allFinite() && std::isfinite(radius) && radius > 0, "Invalid world brush bounds");
    std::vector<uint32_t> result;
    if (impl->nodes.empty())
        return result;
    std::vector<uint32_t> pending{0};
    while (!pending.empty()) {
        uint32_t index = pending.back();
        pending.pop_back();
        const auto& node = impl->nodes[index];
        if (!node.box.sphere(center, radius))
            continue;
        if (node.count == 0) {
            pending.push_back(node.right);
            pending.push_back(index + 1);
        } else {
            for (uint32_t i = node.first; i < node.first + node.count; ++i) {
                uint32_t primitive = impl->order[i];
                auto mesh = impl->primitives[primitive].mesh;
                if ((material == -1 || impl->scene->meshes[mesh].material == material) &&
                    impl->primitiveBounds(primitive).sphere(center, radius))
                    result.push_back(primitive);
            }
        }
    }
    return result;
}
std::pair<V3, V3> PaintSurface::bounds(int material) const {
    if (material == -1 && !impl->nodes.empty())
        return {impl->nodes[0].box.lo, impl->nodes[0].box.hi};
    Impl::Box box;
    bool found = false;
    for (uint32_t index = 0; index < impl->primitives.size(); ++index) {
        auto mesh = impl->primitives[index].mesh;
        if (material != -1 && impl->scene->meshes[mesh].material != material)
            continue;
        auto bounds = impl->primitiveBounds(index);
        box.include(bounds.lo);
        box.include(bounds.hi);
        found = true;
    }
    require(found, "此材质没有可绘制表面");
    return {box.lo, box.hi};
}

uint64_t rasterizePaintTriangle(const PaintTriangle& triangle, uint32_t width, uint32_t height,
                                const PaintRasterCallback& callback, const PaintRasterOptions& options) {
    require(width && height && width <= 16384 && height <= 16384, "Invalid paint raster dimensions");
    cancelled(options.cancel);
    if (!triangle.hasUV)
        return 0;
    double u0 = triangle.uv[0][0], v0 = triangle.uv[0][1];
    double du1 = triangle.uv[1][0] - u0, dv1 = triangle.uv[1][1] - v0;
    double du2 = triangle.uv[2][0] - u0, dv2 = triangle.uv[2][1] - v0;
    double determinant = du1 * dv2 - dv1 * du2;
    require(std::isfinite(determinant), "表面包含非有限 UV，无法绘制");
    if (std::abs(determinant) < 1e-20)
        return 0;
    double minU = u0, minV = v0, maxU = u0, maxV = v0;
    for (const auto& uv : triangle.uv) {
        require(std::isfinite(uv[0]) && std::isfinite(uv[1]) && std::abs(uv[0]) <= 1e6 &&
                    std::abs(uv[1]) <= 1e6,
                "UV 坐标超出涂装绘制范围");
        minU = std::min(minU, double(uv[0]));
        minV = std::min(minV, double(uv[1]));
        maxU = std::max(maxU, double(uv[0]));
        maxV = std::max(maxV, double(uv[1]));
    }
    if (options.uvBounds) {
        for (double value : *options.uvBounds)
            require(std::isfinite(value), "Invalid paint UV clipping bounds");
        minU = std::max(minU, (*options.uvBounds)[0]);
        minV = std::max(minV, (*options.uvBounds)[1]);
        maxU = std::min(maxU, (*options.uvBounds)[2]);
        maxV = std::min(maxV, (*options.uvBounds)[3]);
    }
    if (minU > maxU || minV > maxV)
        return 0;
    double tiles =
        std::max(1., std::ceil(maxU) - std::floor(minU)) * std::max(1., std::ceil(maxV) - std::floor(minV));
    require(tiles <= options.maxRepeatTiles, "UV 重复次数过多，请缩小绘制区域");
    int64_t x0 = int64_t(std::ceil(minU * width - .5)), x1 = int64_t(std::floor(maxU * width - .5));
    int64_t y0 = int64_t(std::ceil(minV * height - .5)), y1 = int64_t(std::floor(maxV * height - .5));
    if (x1 < x0 || y1 < y0)
        return 0;
    require(x0 >= INT_MIN && x1 <= INT_MAX && y0 >= INT_MIN && y1 <= INT_MAX,
            "UV raster exceeds integer range");
    require(uint64_t(x1 - x0 + 1) * uint64_t(y1 - y0 + 1) <= options.maxPixels,
            "单个表面的 UV 栅格范围过大，请缩小绘制区域");
    uint64_t samples = 0;
    double inverse = 1 / determinant;
    for (int64_t y = y0; y <= y1; ++y) {
        cancelled(options.cancel);
        double dv = (double(y) + .5) / height - v0;
        for (int64_t x = x0; x <= x1; ++x) {
            double du = (double(x) + .5) / width - u0;
            double b = (du * dv2 - dv * du2) * inverse;
            double c = (du1 * dv - dv1 * du) * inverse;
            if (b < -1e-8 || c < -1e-8 || b + c > 1 + 1e-8)
                continue;
            callback(repeat(int(x), int(width)), repeat(int(y), int(height)), V3(1 - b - c, b, c));
            ++samples;
        }
    }
    return samples;
}
PaintUVInfo PaintSurface::analyzeUV(int material, uint32_t resolution, const std::atomic_bool* cancel) const {
    require(resolution >= 16 && resolution <= 1024, "UV analysis resolution must be between 16 and 1024");
    PaintUVInfo result;
    std::vector<uint32_t> owner(size_t(resolution) * resolution, UINT32_MAX);
    std::vector<uint8_t> shared(owner.size(), 0);
    PaintRasterOptions options;
    options.cancel = cancel;
    options.maxPixels = 16ull * 1024 * 1024;
    for (uint32_t primitive = 0; primitive < triangleCount(); ++primitive) {
        if ((primitive & 4095) == 0)
            cancelled(cancel);
        auto face = triangle(primitive);
        if (face.material != material)
            continue;
        bool repeated = false;
        for (const auto& uv : face.uv)
            repeated |= uv[0] < 0 || uv[0] > 1 || uv[1] < 0 || uv[1] > 1;
        result.repeatedTriangles += repeated;
        double area = double(face.uv[1][0] - face.uv[0][0]) * (face.uv[2][1] - face.uv[0][1]) -
                      double(face.uv[1][1] - face.uv[0][1]) * (face.uv[2][0] - face.uv[0][0]);
        if (!face.hasUV || std::abs(area) < 1e-20)
            ++result.degenerateTriangles;
        rasterizePaintTriangle(
            face, resolution, resolution,
            [&](int x, int y, const V3& barycentric) {
                // Exclude triangle boundaries; adjacent faces are not overlapping islands.
                if (barycentric.minCoeff() < 1e-6)
                    return;
                size_t pixel = size_t(y) * resolution + x;
                if (owner[pixel] == UINT32_MAX) {
                    owner[pixel] = primitive;
                    ++result.coveredPixels;
                } else if (owner[pixel] != primitive && !shared[pixel]) {
                    shared[pixel] = 1;
                    ++result.sharedPixels;
                }
            },
            options);
    }
    return result;
}

PaintStats paintBrush(PaintCanvas& canvas, const PaintSurface& surface, const PaintHit& hit,
                      const PaintBrush& brush, const std::optional<V3>& eye, const std::atomic_bool* cancel,
                      int targetMaterial, const std::vector<int>& targetMaterials) {
    require(canvas.strokeActive(), "Begin a stroke before applying a world brush");
    require(std::isfinite(brush.radius) && brush.radius > 0 && std::isfinite(brush.hardness) &&
                std::isfinite(brush.opacity),
            "Invalid paint brush settings");
    cancelled(cancel);
    PaintStats result;
    const auto& image = canvas.image();
    require(brush.maxPixels > 0 && brush.maxPixels <= 16ull * 1024 * 1024,
            "Interactive brush pixel budget is out of range");
    uint64_t rasterSamples = 0;
    // A sparse stamp map chooses maximum coverage, preventing dark seams when two UV faces share texels.
    std::unordered_map<size_t, float> coverage;
    require(targetMaterial >= -1, "Invalid target paint material");
    auto materials = targetMaterials;
    for (int material : materials)
        require(material >= 0, "Invalid target paint material in union");
    std::sort(materials.begin(), materials.end());
    materials.erase(std::unique(materials.begin(), materials.end()), materials.end());
    auto candidates =
        surface.querySphere(hit.position, brush.radius,
                            materials.empty() ? (targetMaterial == -1 ? hit.material : targetMaterial) : -1);
    coverage.reserve(4096);
    for (uint32_t primitive : candidates) {
        cancelled(cancel);
        auto face = surface.triangle(primitive);
        if (!materials.empty() && !std::binary_search(materials.begin(), materials.end(), face.material))
            continue;
        if (!face.hasUV)
            continue;
        ++result.triangles;
        Eigen::Matrix<double, 3, 2> edges;
        edges.col(0) = face.world[1] - face.world[0];
        edges.col(1) = face.world[2] - face.world[0];
        Eigen::Matrix2d gram = edges.transpose() * edges;
        if (std::abs(gram.determinant()) < 1e-24)
            continue;
        auto uvBounds = brushUVBounds(face, hit.position, brush.radius);
        if (!uvBounds)
            continue;
        PaintRasterOptions options;
        options.cancel = cancel;
        require(rasterSamples < brush.maxPixels, "单次画笔覆盖过大，请缩小笔刷半径");
        options.maxPixels = brush.maxPixels - rasterSamples;
        options.uvBounds = uvBounds;
        rasterSamples += rasterizePaintTriangle(
            face, image.width, image.height,
            [&](int x, int y, const V3& barycentric) {
                V3 point = face.world[0] * barycentric.x() + face.world[1] * barycentric.y() +
                           face.world[2] * barycentric.z();
                double distance = (point - hit.position).norm() / brush.radius;
                if (distance >= 1)
                    return;
                if (brush.visibleOnly && eye) {
                    V3 direction = point - *eye;
                    double length = direction.norm();
                    if (length < 1e-8)
                        return;
                    double epsilon = std::max(1e-5, length * 2e-6);
                    if (surface.raycast(*eye, direction, -1, length - epsilon))
                        return;
                } else {
                    V3 normal = face.normals[0] * barycentric.x() + face.normals[1] * barycentric.y() +
                                face.normals[2] * barycentric.z();
                    if (normal.dot(hit.normal) < 0)
                        return;
                }
                float hardness = std::clamp(brush.hardness, 0.f, 1.f), weight = 1;
                if (distance > hardness && hardness < 1) {
                    float edge = float((1 - distance) / (1 - hardness));
                    weight = edge * edge * (3 - 2 * edge);
                }
                weight *= std::clamp(brush.opacity, 0.f, 1.f);
                size_t pixel = size_t(y) * image.width + x;
                auto [entry, inserted] = coverage.emplace(pixel, weight);
                if (!inserted) {
                    entry->second = std::max(entry->second, weight);
                    ++result.sharedUVSamples;
                }
            },
            options);
    }
    for (const auto& [pixel, weight] : coverage) {
        if ((result.pixels & 4095) == 0)
            cancelled(cancel);
        result.pixels += canvas.blendPixel(int(pixel % image.width), int(pixel / image.width), brush.color,
                                           weight, brush.preserveAlpha);
    }
    return result;
}

} // namespace edm
