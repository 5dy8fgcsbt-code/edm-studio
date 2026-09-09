// Included inside App. The popup's explicit button is the only interactive unload entry point.
void detachModel(int target) {
    require(renderer.model && !loading && !attaching && !paint.busy(), "请等待模型与绘制任务完成");
    const auto source = renderer.model->scene;
    require(target >= 0 && target < int(source->nodes.size()), "无效挂点");
    auto images = paint.snapshot();
    auto selectedLivery = livery;
    auto extra = textureDir;
    auto recovery = settingsPath.parent_path() / "PaintRecovery";
    auto device = renderer.device;
    const auto gen = generation;
    attaching = true;
    playing = false;
    notice = "正在卸载 " + source->nodes[target].name + " 的外挂";
    struct Prepared {
        DetachResult result;
        std::shared_ptr<GpuModel> model;
        Json recovery;
    };
    launch(
        "attachment",
        [source, target, images, selectedLivery, extra, recovery, device](Progress progress,
                                                                          const std::atomic_bool* cancel) {
            progress("清理外挂层级与模型数据");
            Prepared prepared;
            prepared.result = detachScene(*source, target);
            require(!*cancel, "Cancelled");
            const auto& map = prepared.result.materialMap;
            if (std::any_of(images.begin(), images.end(),
                            [&](const auto& image) { return map.at(image.first) < 0; })) {
                progress("保存卸载物体的绘制恢复副本");
                prepared.recovery =
                    savePaintRecovery(*source, images, recovery, progress, cancel, selectedLivery, extra);
            }
            prepared.model = GpuModel::prepare(device.Get(), prepared.result.scene, cancel, progress);
            return prepared;
        },
        [this, gen](Prepared prepared) {
            if (gen != generation)
                return;
            struct Finish {
                bool& busy;
                ~Finish() {
                    busy = false;
                }
            } finish{attaching};
            auto& result = prepared.result;
            TextureBindings textures;
            textures.material.resize(result.scene->materials.size());
            std::set<std::string> used;
            for (int old = 0; old < int(result.materialMap.size()); ++old) {
                int next = result.materialMap[old];
                if (next < 0 || old >= int(renderer.textures.material.size()))
                    continue;
                textures.material[next] = renderer.textures.material[old];
                for (const auto& key : textures.material[next])
                    if (!key.empty())
                        used.insert(key);
            }
            for (const auto& key : used)
                if (auto it = renderer.textures.images.find(key); it != renderer.textures.images.end()) {
                    textures.images.emplace(key, it->second);
                    textures.bytes += it->second->bytes;
                }
            paint.compactScene(renderer, result.scene, result.materialMap);
            cancelKind("textures");
            stream.reset();
            renderer.textures = std::move(textures);
            renderer.model = std::move(prepared.model);
            for (int argument : result.removedArguments) {
                args.erase(argument);
                baseline.erase(argument);
                argNames.erase(argument);
            }
            if (result.removedArguments.contains(selectedArg))
                selectedArg = -1;
            argNames.clear();
            for (const auto& track : result.scene->tracks) {
                const auto& name = result.scene->nodes[track.node].name;
                auto& names = argNames[track.arg];
                if (names.size() < 500 && names.find(name) == std::string::npos)
                    names += name + "\n";
            }
            renderer.model->update(renderer.context.Get(), args, renderer.options.attachments);
            attaching = false;
            hoveredConnector = selectedConnector = -1;
            connectorSelectionScene.reset();
            analysis.reset();
            lastHighlight = -2;
            cancelKind("analysis");
            startAnalysis();
            startTextures(true);
            notice = "已卸载 " + std::to_string(result.removedCount) + " 件外挂 · 其余模型与涂装已保留";
            if (!prepared.recovery.is_null())
                notice += " · 已保存绘制恢复副本";
            record(notice);
            if (smoke)
                lastDetachment = {{"removed_count", result.removedCount},
                                  {"material_map", result.materialMap},
                                  {"node_map", result.nodeMap},
                                  {"removed_arguments", result.removedArguments},
                                  {"recovery", prepared.recovery}};
        });
}
