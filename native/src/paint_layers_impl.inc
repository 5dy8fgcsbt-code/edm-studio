// Included inside PaintCanvas::Impl. All publication paths below allocate before touching pixels.
static void validateLayers(const std::vector<PaintLayerInfo>& infos, uint64_t selected) {
    require(!infos.empty() && infos.size() <= 64, "涂装必须包含 1 至 64 个图层");
    std::set<uint64_t> ids;
    for (const auto& info : infos) {
        require(info.id && info.id != UINT64_MAX && ids.insert(info.id).second,
                "图层 ID 必须非零、唯一且小于 UINT64_MAX");
        require(!info.name.empty() && info.name.size() <= 256 &&
                    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, info.name.data(),
                                        int(info.name.size()), nullptr, 0) > 0,
                "图层名称必须是有效 UTF-8，长度为 1 至 256 字节");
        std::array<wchar_t, 256> wideName{};
        int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, info.name.data(),
                                         int(info.name.size()), wideName.data(), int(wideName.size()));
        std::array<WORD, 256> characterTypes{};
        require(GetStringTypeW(CT_CTYPE1, wideName.data(), length, characterTypes.data()) != 0,
                "无法验证图层名称");
        bool text = false;
        for (int i = 0; i < length; ++i) {
            require(!(characterTypes[i] & C1_CNTRL), "图层名称不能包含控制字符");
            text |= !(characterTypes[i] & C1_SPACE);
        }
        require(text, "图层名称不能为空白");
        require(std::isfinite(info.opacity) && info.opacity >= 0 && info.opacity <= 1,
                "图层不透明度必须在 0 至 1 之间");
    }
    require(ids.contains(selected), "当前图层不存在");
}
static Layer& findLayer(LayerState& state, uint64_t id) {
    auto found = std::find_if(state.layers.begin(), state.layers.end(),
                              [&](const Layer& layer) { return layer.info.id == id; });
    require(found != state.layers.end(), "图层不存在");
    return *found;
}
static const Layer* maybeLayer(const LayerState& state, uint64_t id) noexcept {
    for (const auto& layer : state.layers)
        if (layer.info.id == id)
            return &layer;
    return nullptr;
}
static bool equalTiles(const std::shared_ptr<LayerTiles>& a, const std::shared_ptr<LayerTiles>& b) {
    if (a == b)
        return true;
    if ((!a || a->empty()) && (!b || b->empty()))
        return true;
    if (!a || !b || a->size() != b->size())
        return false;
    auto other = b->begin();
    for (const auto& [key, tile] : *a) {
        if (other->first != key || (tile != other->second && tile->image.rgba != other->second->image.rgba))
            return false;
        ++other;
    }
    return true;
}
static bool sameLayers(const LayerState& a, const LayerState& b) {
    if (a.active != b.active || a.layers.size() != b.layers.size())
        return false;
    for (size_t i = 0; i < a.layers.size(); ++i)
        if (a.layers[i].info != b.layers[i].info || !equalTiles(a.layers[i].tiles, b.layers[i].tiles))
            return false;
    return true;
}
static size_t layerHistoryBytes(const LayerState& before, const LayerState& after) {
    size_t result = sizeof(LayerState) * 2 + (before.layers.size() + after.layers.size()) * sizeof(Layer);
    for (const auto* state : {&before, &after})
        for (const auto& layer : state->layers) {
            result += layer.info.name.size();
            const auto* other = maybeLayer(state == &before ? after : before, layer.info.id);
            if (!layer.tiles || (other && other->tiles == layer.tiles))
                continue;
            result += layer.tiles->size() * (sizeof(LayerTiles::value_type) + 3 * sizeof(void*));
            for (const auto& [key, tile] : *layer.tiles) {
                auto found = other && other->tiles ? other->tiles->find(key) : LayerTiles::const_iterator{};
                if (!other || !other->tiles || found == other->tiles->end() || found->second != tile)
                    result += tile->image.rgba.size();
            }
        }
    return result;
}
size_t storage() const noexcept {
    size_t result = image.rgba.size();
    if (layerData)
        result += layerData->base->rgba.size() + layerData->memory->bytes;
    result += compositeHistory;
    if (active)
        result += active->compositeBytes();
    return result;
}
void requireStorage(size_t additional) const {
    const size_t used = storage();
    require(used <= layerStorageLimit && additional <= layerStorageLimit - used,
            "单张涂装图层及撤销像素内存超过 2 GiB，请清理图层或降低贴图尺寸");
}
uint32_t layerKey(uint32_t x, uint32_t y) const noexcept {
    return (y / tileSize) * ((image.width + tileSize - 1) / tileSize) + x / tileSize;
}
Tile tileAt(uint32_t key) const {
    const uint32_t across = (image.width + tileSize - 1) / tileSize;
    Tile tile;
    tile.x = key % across * tileSize;
    tile.y = key / across * tileSize;
    tile.width = std::min(tileSize, image.width - tile.x);
    tile.height = std::min(tileSize, image.height - tile.y);
    return tile;
}
static std::array<uint8_t, 4> blendRGBA(const uint8_t* below, const uint8_t* above, float opacity,
                                        bool preserveAlpha) noexcept {
    std::array<uint8_t, 4> result;
    const float alpha = above[3] / 255.f * opacity, oldAlpha = below[3] / 255.f;
    if (alpha == 0) {
        std::copy_n(below, 4, result.begin());
        return result;
    }
    const float newAlpha = alpha + oldAlpha * (1 - alpha);
    for (int channel = 0; channel < 3; ++channel) {
        float source = above[channel] / 255.f, destination = below[channel] / 255.f;
        result[channel] = byte(preserveAlpha ? source * alpha + destination * (1 - alpha)
                                             : (source * alpha + destination * oldAlpha * (1 - alpha)) /
                                                   std::max(newAlpha, 1e-20f));
    }
    result[3] = preserveAlpha ? below[3] : byte(newAlpha);
    return result;
}
std::array<uint8_t, 4> compositePixel(const LayerState& state, uint32_t x, uint32_t y) const noexcept {
    std::array<uint8_t, 4> pixel;
    std::copy_n(layerData->base->rgba.data() + (size_t(y) * image.width + x) * 4, 4, pixel.begin());
    uint32_t key = layerKey(x, y);
    for (const auto& layer : state.layers) {
        if (!layer.info.visible || layer.info.opacity == 0 || !layer.tiles)
            continue;
        auto found = layer.tiles->find(key);
        if (found == layer.tiles->end())
            continue;
        const auto& tile = found->second->image;
        auto* above = tile.rgba.data() + (size_t(y % tileSize) * tile.width + x % tileSize) * 4;
        pixel = blendRGBA(pixel.data(), above, layer.info.opacity, layer.info.preserveAlpha);
    }
    return pixel;
}
std::vector<uint8_t> compositeTile(const LayerState& state, const Tile& tile) const {
    std::vector<uint8_t> result(size_t(tile.width) * tile.height * 4);
    for (uint32_t y = 0; y < tile.height; ++y)
        std::memcpy(result.data() + size_t(y) * tile.width * 4,
                    layerData->base->rgba.data() + (size_t(tile.y + y) * image.width + tile.x) * 4,
                    size_t(tile.width) * 4);
    uint32_t key = layerKey(tile.x, tile.y);
    for (const auto& layer : state.layers) {
        if (!layer.info.visible || layer.info.opacity == 0 || !layer.tiles)
            continue;
        auto found = layer.tiles->find(key);
        if (found == layer.tiles->end())
            continue;
        auto* above = found->second->image.rgba.data();
        for (size_t i = 0; i < result.size(); i += 4) {
            auto pixel =
                blendRGBA(result.data() + i, above + i, layer.info.opacity, layer.info.preserveAlpha);
            std::copy(pixel.begin(), pixel.end(), result.begin() + i);
        }
    }
    return result;
}
std::shared_ptr<LayerState> layout(const std::vector<PaintLayerInfo>& infos, uint64_t selected) const {
    validateLayers(infos, selected);
    auto state = std::make_shared<LayerState>();
    state->active = selected;
    state->layers.reserve(infos.size());
    for (const auto& info : infos) {
        const Layer* existing = layerData ? maybeLayer(*layerData->state, info.id) : nullptr;
        state->layers.push_back({info, existing ? existing->tiles : nullptr});
    }
    return state;
}
void requireLayerEdit() const {
    if (!layerData || !active || prepared)
        throw std::runtime_error("图层编辑需要尚未提交的涂装操作");
}
void publishLayerState(std::shared_ptr<LayerState> next, bool record) {
    // Only supports of layers whose appearance/order changed can change the composite. Renaming,
    // selection, or an empty layer operation never scans a large blank texture.
    std::set<uint32_t> keys;
    const auto& before = *layerData->state;
    auto collect = [&](const LayerState& from, const LayerState& to) {
        for (size_t index = 0; index < from.layers.size(); ++index) {
            const auto& layer = from.layers[index];
            if (!layer.tiles || !layer.info.visible || layer.info.opacity == 0)
                continue;
            const Layer* other = maybeLayer(to, layer.info.id);
            if (other && index < to.layers.size() && to.layers[index].info.id == layer.info.id &&
                other->tiles == layer.tiles && other->info.visible == layer.info.visible &&
                other->info.opacity == layer.info.opacity &&
                other->info.preserveAlpha == layer.info.preserveAlpha)
                continue;
            for (const auto& [key, tile] : *layer.tiles)
                keys.insert(key);
        }
    };
    collect(before, *next);
    collect(*next, before);
    std::vector<Tile> patches;
    patches.reserve(keys.size());
    size_t transientBytes = 0;
    for (uint32_t key : keys) {
        auto tile = tileAt(key);
        size_t count = size_t(tile.width) * tile.height * 4;
        // after patches, history before buffers, and eventual history after buffers are bounded.
        requireStorage(transientBytes + count * (record ? 3 : 1));
        tile.after = compositeTile(*next, tile);
        bool differs = false;
        for (uint32_t y = 0; y < tile.height && !differs; ++y)
            differs = std::memcmp(tile.after.data() + size_t(y) * tile.width * 4,
                                  image.rgba.data() + (size_t(tile.y + y) * image.width + tile.x) * 4,
                                  size_t(tile.width) * 4) != 0;
        if (differs) {
            transientBytes += count * (record ? 3 : 1);
            patches.push_back(std::move(tile));
        }
    }
    if (record)
        for (const auto& patch : patches)
            saveBefore(patch.x, patch.y);
    layerData->state = std::move(next);
    for (const auto& patch : patches)
        restore(patch, patch.after);
    preparedUndo = preparedRedo = 0;
}
bool blendLayerPixel(int x, int y, const F4& color, float coverage) {
    requireLayerEdit();
    auto& layer = findLayer(*layerData->state, layerData->state->active);
    if (!layer.info.visible)
        throw std::runtime_error("当前图层已隐藏，请先显示图层再绘制");
    if (x < 0 || y < 0 || x >= int(image.width) || y >= int(image.height))
        return false;
    for (float value : color)
        if (!std::isfinite(value))
            throw std::runtime_error("Paint color is not finite");
    if (!std::isfinite(coverage))
        throw std::runtime_error("Paint coverage is not finite");
    const float alpha = std::clamp(color[3], 0.f, 1.f) * std::clamp(coverage, 0.f, 1.f);
    if (!alpha)
        return false;
    uint32_t key = layerKey(uint32_t(x), uint32_t(y));
    const LayerTile* oldTile = nullptr;
    if (layer.tiles) {
        auto found = layer.tiles->find(key);
        if (found != layer.tiles->end())
            oldTile = found->second.get();
    }
    const std::array<uint8_t, 4> transparent{};
    const uint32_t width = std::min(tileSize, image.width - uint32_t(x) / tileSize * tileSize);
    size_t offset = (size_t(y % tileSize) * width + x % tileSize) * 4;
    const auto* pixel = oldTile ? oldTile->image.rgba.data() + offset : transparent.data();
    const float oldAlpha = pixel[3] / 255.f, outputAlpha = alpha + oldAlpha * (1 - alpha);
    std::array<uint8_t, 4> output;
    for (int channel = 0; channel < 3; ++channel)
        output[channel] = byte(
            (std::clamp(color[channel], 0.f, 1.f) * alpha + pixel[channel] / 255.f * oldAlpha * (1 - alpha)) /
            std::max(outputAlpha, 1e-20f));
    output[3] = byte(outputAlpha);
    if (std::equal(output.begin(), output.end(), pixel))
        return false;
    // Detach the shared sparse map once per layer/stroke and the pixel tile once per touched tile.
    if (!layer.tiles)
        layer.tiles = std::make_shared<LayerTiles>();
    else if (layer.tiles.use_count() != 1)
        layer.tiles = std::make_shared<LayerTiles>(*layer.tiles);
    auto found = layer.tiles->find(key);
    auto tile = tileAt(key);
    size_t tileBytes = size_t(tile.width) * tile.height * 4;
    bool copyPixels = found == layer.tiles->end() || found->second.use_count() != 1;
    if (copyPixels || !touched.contains(key))
        requireStorage((copyPixels ? tileBytes : 0) + (touched.contains(key) ? 0 : tileBytes * 2));
    std::shared_ptr<LayerTile> writable;
    if (copyPixels) {
        PaintImage pixels = found == layer.tiles->end() ? PaintImage(tile.width, tile.height, {0, 0, 0, 0})
                                                        : found->second->image;
        writable = std::make_shared<LayerTile>(std::move(pixels), layerData->memory);
    } else
        writable = found->second;
    // Register before pixels before mutating state; after successful map insertion nothing allocates.
    saveBefore(uint32_t(x), uint32_t(y));
    if (found == layer.tiles->end())
        layer.tiles->emplace(key, writable);
    else
        found->second = writable;
    std::copy(output.begin(), output.end(), writable->image.rgba.begin() + offset);
    auto composite = compositePixel(*layerData->state, uint32_t(x), uint32_t(y));
    auto* destination = image.rgba.data() + (size_t(y) * image.width + x) * 4;
    if (!std::equal(composite.begin(), composite.end(), destination)) {
        std::copy(composite.begin(), composite.end(), destination);
        mark({x, y, x + 1, y + 1});
        ++revision;
    }
    return true;
}
void clearLayerHistory() noexcept {
    undo.clear();
    redo.clear();
    history = 0;
    compositeHistory = 0;
    touched.clear();
    prepared = false;
    preparedUndo = preparedRedo = 0;
}
