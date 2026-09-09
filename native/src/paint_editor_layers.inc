// Included inside PaintEditor::Impl. One ordered layer list spans every automatic material canvas.
void initializeLayers(PaintCanvas& canvas) const {
    canvas.enableLayers(layerInfos, activeLayerId);
}
void refreshLayerSelection() {
    auto found = std::find_if(layerInfos.begin(), layerInfos.end(),
                              [&](const auto& layer) { return layer.id == activeLayerId; });
    require(found != layerInfos.end(), "当前图层不存在");
    layerNameDraft = found->name;
    layerOpacityDraft = found->opacity;
    preserveAlpha = found->preserveAlpha;
}
void selectLayer(uint64_t id) {
    require(!controlsBusy(), "请等待当前绘制完成后切换图层");
    layerControl->selectLayer(id);
    for (auto& [index, entry] : entries)
        entry.canvas->selectLayer(id);
    activeLayerId = id;
    refreshLayerSelection();
}
void requireWritableLayer() const {
    auto found = std::find_if(layerInfos.begin(), layerInfos.end(),
                              [&](const auto& layer) { return layer.id == activeLayerId; });
    require(found != layerInfos.end() && found->visible, "当前图层已隐藏，请显示它或选择其他图层");
    require(found->opacity > 0, "当前图层不透明度为 0，请先提高不透明度");
}
void prepareLayerCommit() {
    size_t bytes = 0;
    std::set<const PaintCanvas*> counted;
    for (const auto& [index, entry] : entries) {
        if (!counted.insert(entry.canvas.get()).second)
            continue;
        if (entry.canvas->strokeActive())
            entry.canvas->prepareStroke();
        bytes += entry.canvas->storageBytes();
    }
    require(bytes <= 2ull * 1024 * 1024 * 1024, "图层及撤销像素超过 2 GiB 编辑预算，此次操作已回滚");
}
void changeLayers(std::vector<PaintLayerInfo> next, uint64_t active, uint64_t duplicateSource = 0,
                  uint64_t duplicateDestination = 0, bool clearCurrent = false) {
    require(!controlsBusy(), "请先完成当前绘制操作");
    auto selection =
        std::find_if(next.begin(), next.end(), [&](const auto& layer) { return layer.id == active; });
    require(selection != next.end(), "当前图层不存在");
    auto preparedName = selection->name;
    const auto preparedOpacity = selection->opacity;
    const auto preparedAlpha = selection->preserveAlpha;
    auto preparedNext = nextLayerId;
    for (const auto& layer : next)
        preparedNext = std::max(preparedNext, layer.id + 1);
    std::string preparedStatus = clearCurrent ? "已清空当前图层 · 可撤销" : "图层已更新 · 可撤销";
    // Include a small control canvas so layer operations made before the first paint are undoable.
    // New material canvases are reconciled from this authority after undo/redo.
    history.begin("调整涂装图层");
    try {
        history.touchControl(layerControl).setLayers(next, active);
        for (auto& [index, entry] : entries) {
            auto& canvas = history.touch(index, entry.canvas);
            canvas.setLayers(next, active);
            if (duplicateSource)
                canvas.duplicateLayer(duplicateSource, duplicateDestination);
            if (clearCurrent)
                canvas.clearLayer(active);
        }
        prepareLayerCommit();
        const bool changed = history.commit();
        layerInfos.swap(next);
        activeLayerId = active;
        nextLayerId = preparedNext;
        layerNameDraft.swap(preparedName);
        layerOpacityDraft = preparedOpacity;
        preserveAlpha = preparedAlpha;
        if (changed) {
            saved = false;
            status.swap(preparedStatus);
        }
    } catch (...) {
        history.cancel();
        throw;
    }
}
bool undoLayerEdit(bool redo = false) {
    require(!controlsBusy(), "请先完成当前绘制操作");
    if (!(redo ? history.redo() : history.undo()))
        return false;
    saved = false;
    layerInfos = layerControl->layers();
    activeLayerId = layerControl->activeLayer();
    for (auto& [index, entry] : entries)
        entry.canvas->syncLayers(layerInfos, activeLayerId);
    refreshLayerSelection();
    status = redo ? "已重做整次操作" : "已撤销整次操作";
    return true;
}
PaintLayerSnapshotMap layerSnapshot() const {
    size_t bytes = 0;
    std::set<const PaintCanvas*> counted;
    for (const auto& [index, entry] : entries)
        if (counted.insert(entry.canvas.get()).second) {
            const auto amount = entry.canvas->layerSnapshotBytes();
            require(amount <= 2ull * 1024 * 1024 * 1024 - bytes,
                    "完整分层工程展开后超过 2 GiB，请减少重复图层后保存");
            bytes += amount;
        }
    PaintLayerSnapshotMap result;
    std::map<const PaintCanvas*, std::shared_ptr<const PaintCanvasLayers>> shared;
    for (const auto& [index, entry] : entries) {
        auto& snapshot = shared[entry.canvas.get()];
        if (!snapshot)
            snapshot = std::make_shared<PaintCanvasLayers>(entry.canvas->layerSnapshot());
        result[index] = snapshot;
    }
    for (int i = 0; i < int(canonicalIds.size()); ++i)
        if (result.contains(canonicalIds[i]))
            result[i] = result.at(canonicalIds[i]);
    return result;
}
