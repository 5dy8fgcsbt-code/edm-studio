// Layer API definitions. Kept apart from surface geometry and flat-canvas painting.
void PaintCanvas::enableLayers(const std::vector<PaintLayerInfo>& infos, uint64_t selected) {
    require(!impl->active && !impl->layerData, "Only an inactive flat canvas can enable layers");
    auto state = impl->layout(infos, selected);
    require(impl->image.rgba.size() <= Impl::layerStorageLimit / 2, "涂装图层基础画布超过内存限制");
    auto data = std::make_unique<Impl::LayerData>();
    data->base = std::make_shared<const PaintImage>(impl->image);
    data->state = std::move(state);
    impl->layerData = std::move(data);
    impl->clearLayerHistory();
}
bool PaintCanvas::layered() const {
    return bool(impl->layerData);
}
std::vector<PaintLayerInfo> PaintCanvas::layers() const {
    std::vector<PaintLayerInfo> result;
    if (impl->layerData) {
        result.reserve(impl->layerData->state->layers.size());
        for (const auto& layer : impl->layerData->state->layers)
            result.push_back(layer.info);
    }
    return result;
}
uint64_t PaintCanvas::activeLayer() const {
    return impl->layerData ? impl->layerData->state->active : 0;
}
void PaintCanvas::selectLayer(uint64_t selected) {
    require(impl->layerData && !impl->prepared, "Cannot select a layer on this canvas");
    Impl::findLayer(*impl->layerData->state, selected);
    if (selected == impl->layerData->state->active)
        return;
    auto state = std::make_shared<Impl::LayerState>(*impl->layerData->state);
    state->active = selected;
    impl->layerData->state = std::move(state);
    impl->preparedUndo = impl->preparedRedo = 0;
}
void PaintCanvas::setLayers(const std::vector<PaintLayerInfo>& infos, uint64_t selected) {
    impl->requireLayerEdit();
    impl->publishLayerState(impl->layout(infos, selected), true);
}
void PaintCanvas::syncLayers(const std::vector<PaintLayerInfo>& infos, uint64_t selected) {
    require(impl->layerData && !impl->active, "Layer synchronization requires an inactive layered canvas");
    impl->publishLayerState(impl->layout(infos, selected), false);
}
void PaintCanvas::duplicateLayer(uint64_t source, uint64_t destination) {
    impl->requireLayerEdit();
    require(source != destination, "复制图层的目标不能是原图层");
    auto state = std::make_shared<Impl::LayerState>(*impl->layerData->state);
    auto& target = Impl::findLayer(*state, destination);
    target.tiles = Impl::findLayer(*state, source).tiles;
    impl->publishLayerState(std::move(state), true);
}
void PaintCanvas::clearLayer(uint64_t id) {
    impl->requireLayerEdit();
    auto state = std::make_shared<Impl::LayerState>(*impl->layerData->state);
    Impl::findLayer(*state, id).tiles.reset();
    impl->publishLayerState(std::move(state), true);
}
void PaintCanvas::replaceLayerImage(uint64_t id, const PaintImage& image) {
    impl->requireLayerEdit();
    image.validate();
    require(image.width == impl->image.width && image.height == impl->image.height,
            "图层模板尺寸必须与画布相同");
    auto state = std::make_shared<Impl::LayerState>(*impl->layerData->state);
    auto& layer = Impl::findLayer(*state, id);
    require(layer.info.visible, "当前图层已隐藏，请先显示图层再导入模板");
    auto tiles = std::make_shared<Impl::LayerTiles>();
    uint32_t across = (image.width + tileSize - 1) / tileSize;
    uint32_t down = (image.height + tileSize - 1) / tileSize;
    for (uint32_t key = 0; key < across * down; ++key) {
        auto tile = impl->tileAt(key);
        bool opaque = false;
        for (uint32_t y = 0; y < tile.height && !opaque; ++y)
            for (uint32_t x = 0; x < tile.width && !opaque; ++x)
                opaque = image.rgba[(size_t(tile.y + y) * image.width + tile.x + x) * 4 + 3] != 0;
        if (!opaque)
            continue;
        impl->requireStorage(size_t(tile.width) * tile.height * 4);
        PaintImage pixels(tile.width, tile.height);
        for (uint32_t y = 0; y < tile.height; ++y)
            std::memcpy(pixels.rgba.data() + size_t(y) * tile.width * 4,
                        image.rgba.data() + (size_t(tile.y + y) * image.width + tile.x) * 4,
                        size_t(tile.width) * 4);
        tiles->emplace(key, std::make_shared<Impl::LayerTile>(std::move(pixels), impl->layerData->memory));
    }
    layer.tiles = tiles->empty() ? nullptr : std::move(tiles);
    impl->publishLayerState(std::move(state), true);
}
size_t PaintCanvas::layerSnapshotBytes() const {
    require(bool(impl->layerData), "Canvas does not have paint layers");
    // Archive tiles are independent values, so a COW duplicate becomes a logical pixel copy here.
    // Reject an oversized snapshot before allocating any copies, including room for its composite.
    size_t bytes = impl->layerData->base->rgba.size() + impl->image.rgba.size();
    for (const auto& layer : impl->layerData->state->layers)
        if (layer.tiles)
            for (const auto& [key, tile] : *layer.tiles) {
                require(bytes <= Impl::layerStorageLimit &&
                            tile->image.rgba.size() <= Impl::layerStorageLimit - bytes,
                        "图层工程的展开像素超过 2 GiB，保存前请减少完整贴图的重复图层");
                bytes += tile->image.rgba.size();
            }
    return bytes;
}
PaintCanvasLayers PaintCanvas::layerSnapshot() const {
    layerSnapshotBytes();
    PaintCanvasLayers snapshot;
    snapshot.base = *impl->layerData->base;
    snapshot.activeLayer = impl->layerData->state->active;
    snapshot.layers.reserve(impl->layerData->state->layers.size());
    for (const auto& layer : impl->layerData->state->layers) {
        PaintLayerSnapshot copy;
        copy.info = layer.info;
        if (layer.tiles) {
            copy.tiles.reserve(layer.tiles->size());
            for (const auto& [key, tile] : *layer.tiles) {
                auto position = impl->tileAt(key);
                copy.tiles.push_back({position.x, position.y, tile->image});
            }
        }
        snapshot.layers.push_back(std::move(copy));
    }
    return snapshot;
}
void PaintCanvas::restoreLayers(PaintCanvasLayers snapshot) {
    require(!impl->active, "Cannot restore layers during an active operation");
    snapshot.base.validate();
    std::vector<PaintLayerInfo> infos;
    infos.reserve(snapshot.layers.size());
    for (const auto& layer : snapshot.layers)
        infos.push_back(layer.info);
    Impl::validateLayers(infos, snapshot.activeLayer);
    // Build off to the side, so malformed archives or allocation failures cannot alter this canvas.
    auto prepared = std::make_unique<Impl>(snapshot.base, impl->budget);
    auto data = std::make_unique<Impl::LayerData>();
    data->base = std::make_shared<const PaintImage>(std::move(snapshot.base));
    data->state = std::make_shared<Impl::LayerState>();
    data->state->active = snapshot.activeLayer;
    data->state->layers.reserve(snapshot.layers.size());
    prepared->layerData = std::move(data);
    std::set<uint32_t> keys;
    for (auto& layer : snapshot.layers) {
        auto tiles = std::make_shared<Impl::LayerTiles>();
        for (auto& tile : layer.tiles) {
            tile.image.validate();
            require(tile.x < prepared->image.width && tile.y < prepared->image.height &&
                        tile.x % tileSize == 0 && tile.y % tileSize == 0 &&
                        tile.image.width == std::min(tileSize, prepared->image.width - tile.x) &&
                        tile.image.height == std::min(tileSize, prepared->image.height - tile.y),
                    "图层分块位置或尺寸无效");
            uint32_t key = prepared->layerKey(tile.x, tile.y);
            require(!tiles->contains(key), "图层分块重复或重叠");
            prepared->requireStorage(tile.image.rgba.size());
            tiles->emplace(
                key, std::make_shared<Impl::LayerTile>(std::move(tile.image), prepared->layerData->memory));
            keys.insert(key);
        }
        prepared->layerData->state->layers.push_back(
            {std::move(layer.info), tiles->empty() ? nullptr : std::move(tiles)});
    }
    for (uint32_t key : keys) {
        auto tile = prepared->tileAt(key);
        auto composite = prepared->compositeTile(*prepared->layerData->state, tile);
        prepared->restore(tile, composite);
    }
    prepared->revision = impl->revision + 1;
    impl.swap(prepared);
}
size_t PaintCanvas::storageBytes() const {
    return impl->storage();
}
