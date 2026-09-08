// Included inside Impl: all canvas publication stays on the UI thread.
int canvasIndex(int index) const {
    return index >= 0 && size_t(index) < canonicalIds.size() ? canonicalIds[index] : index;
}
void ensureAliases() {
    if (!canonicalIds.empty() || !scene)
        return;
    std::map<std::string, int> groups;
    std::vector<int> prepared;
    prepared.reserve(scene->materials.size());
    for (int i = 0; i < int(scene->materials.size()); ++i) {
        const auto& m = scene->materials[i];
        std::string diffuse = m.texture(0) ? lower(m.texture(0)->name) : "";
        if (livery) {
            auto found = livery->textures.find({lower(m.name), 0});
            if (found != livery->textures.end())
                diffuse = lower(found->second.name) + (found->second.common ? "|common" : "|local");
        }
        std::string key = lower(m.name) + "\n" + diffuse;
        if (m.name.empty())
            key += "\n" + std::to_string(i);
        prepared.push_back(groups.emplace(key, i).first->second);
    }
    canonicalIds.swap(prepared);
}
using PreparedCanvases = std::map<int, Entry>;
std::function<std::shared_ptr<PreparedCanvases>(Progress, const std::atomic_bool*)>
canvasLoader(const std::vector<int>& materials) {
    ensureAliases();
    std::set<int> missing;
    for (int actual : materials) {
        int index = canvasIndex(actual);
        if (!entries.contains(index) || (pendingTemplate && pendingTemplateTarget == index))
            missing.insert(index);
    }
    size_t bytes = 0;
    for (const auto& [index, entry] : entries)
        if (!missing.contains(index))
            bytes += entry.canvas->image().rgba.size();
    auto source = scene;
    auto skin = strokePending() ? strokeLivery : livery;
    auto extra = strokePending() ? strokeTextureDirectory : textureDirectory;
    auto templateImage = pendingTemplate;
    auto templateName = pendingTemplateName;
    int templateTarget = pendingTemplateTarget, dimension = paintMaxDimension;
    return [source, skin, extra, missing, bytes, templateImage, templateName, templateTarget,
            dimension](Progress progress, const std::atomic_bool* cancel) mutable {
        auto prepared = std::make_shared<PreparedCanvases>();
        TextureResolver resolver(source->source, extra, skin);
        for (int index : missing) {
            require(!cancel || !*cancel, "Cancelled");
            if (progress)
                progress("自动准备贴图 · " + source->materials.at(index).name);
            PaintImage image;
            std::string name;
            if (templateImage && templateTarget == index) {
                image = *templateImage;
                name = templateName;
            } else if (auto texture = resolver.material(source->materials.at(index))) {
                image = PaintImage::load(*texture, dimension);
                name = texture->key();
            } else {
                image = PaintImage(dimension, dimension);
                name = "自动创建空白贴图";
            }
            require(!cancel || !*cancel, "Cancelled");
            bytes += image.rgba.size();
            require(bytes <= 1024ull * 1024 * 1024,
                    "绘制贴图超过 1 GiB 预算，请缩小覆盖范围或降低新贴图分辨率。");
            prepared->emplace(index, Entry{std::make_shared<PaintCanvas>(std::move(image)), {}, name, {}});
        }
        return prepared;
    };
}
void installCanvases(PreparedCanvases& prepared) {
    for (auto& [index, entry] : prepared) {
        if (strokePending() && !entries.contains(index))
            strokeNewCanvases.insert(index);
        if (strokePending() && pendingTemplate && pendingTemplateTarget == index && entries.contains(index))
            strokeReplacedCanvases.emplace(index, std::move(entries.at(index)));
        entries[index] = std::move(entry);
        if (pendingTemplate && pendingTemplateTarget == index) {
            strokeTemplateIndex = index;
            strokeTemplateInstalled = true;
            pendingTemplate.reset();
            pendingTemplateTarget = -1;
        }
    }
}
bool requestCanvases(const std::vector<int>& materials, bool synchronous) {
    ensureAliases();
    bool missing = false;
    for (int index : materials)
        missing |= !entries.contains(canvasIndex(index)) ||
                   (pendingTemplate && pendingTemplateTarget == canvasIndex(index));
    if (!missing)
        return true;
    auto loader = canvasLoader(materials);
    if (synchronous) {
        auto result = loader({}, nullptr);
        installCanvases(*result);
        return true;
    }
    start(
        [this, loader](Progress progress, const std::atomic_bool* cancel) {
            auto prepared = loader(progress, cancel);
            return [this, prepared] { installCanvases(*prepared); };
        },
        true);
    return false;
}
void queueTemplate(Renderer&, const fs::path& path) {
    require(!controlsBusy(), "请等待当前绘制任务完成");
    auto recoveryImages = saved ? PaintSnapshot{} : snapshot();
    auto source = scene;
    auto base = livery;
    auto extra = textureDirectory;
    auto recovery = recoveryRoot;
    start([this, path, recoveryImages, source, base, extra, recovery](Progress progress,
                                                                      const std::atomic_bool* cancel) {
        if (!recoveryImages.empty())
            savePaintRecovery(*source, recoveryImages, recovery, progress, cancel, base, extra);
        if (progress)
            progress("读取模板 · 下一次落笔自动定位贴图…");
        auto loaded = std::make_shared<PaintImage>(PaintImage::load(ImageSource{path}, 8192));
        require(!*cancel, "Cancelled");
        return [this, path, loaded] {
            history.clear();
            pendingTemplate = loaded;
            pendingTemplateName = pathString(path);
            pendingTemplateTarget = -1;
            tool = 1;
            status = "模板已就绪 · 在模型上落笔，自动用模板替换该位置的贴图";
        };
    });
}
std::vector<int> brushMaterials(const StrokeContext& context, const PaintHit& hit) const {
    std::set<int> ids;
    for (auto triangle : context.surface->querySphere(hit.position, context.brush.radius, -1)) {
        const auto& t = context.surface->triangle(triangle);
        if (t.hasUV && t.material >= 0)
            ids.insert(t.material);
    }
    return {ids.begin(), ids.end()};
}
template <class Options, class Find, class Apply>
void startAutomaticMapping(Options options, Find find, Apply apply, bool verify, bool projection,
                           bool decal = false) {
    require(!controlsBusy(), "请等待当前绘制任务完成");
    require(mappingImage && surface && surfaceArgs == currentArgs, "请载入图片并等待当前姿态的绘制表面就绪");
    ensureAliases();
    auto source = mappingImage;
    auto posed = surface;
    // A single-material budget is too small for a whole-aircraft 2K footprint.
    // Keep a bounded wall-clock request while sharing larger raster/ray budgets across canvases.
    options.limits.rasterSamples = 512ull * 1024 * 1024;
    options.limits.rayTests = 256ull * 1024 * 1024;
    size_t largestDimension = size_t(paintMaxDimension);
    for (const auto& [index, entry] : entries)
        largestDimension = std::max(
            largestDimension, size_t(std::max(entry.canvas->image().width, entry.canvas->image().height)));
    options.limits.seconds = largestDimension >= 2048 ? 300 : 120;
    auto budget = std::make_shared<PaintMappingBudget>(options.limits);
    auto aliases = canonicalIds;
    auto base = livery;
    auto model = scene;
    auto extra = textureDirectory;
    std::map<int, std::shared_ptr<PaintCanvas>> existing;
    size_t existingBytes = 0;
    for (const auto& [index, entry] : entries) {
        existing[index] = entry.canvas;
        existingBytes += entry.canvas->image().rgba.size();
    }
    auto prepared = std::make_shared<PreparedCanvases>();
    struct Outcome {
        bool committed = false, changed = false;
    };
    auto outcome = std::make_shared<Outcome>();
    bool wasSaved = saved;
    auto cleanup = [this, prepared, outcome, wasSaved] {
        // Runs on the UI thread after join, even when close/cancel discards the done callback.
        // Node transfer allocates nothing. A committed operation must survive recovery shutdown.
        if (outcome->committed)
            entries.merge(*prepared);
        saved = outcome->committed && outcome->changed ? false : wasSaved;
    };
    int dimension = paintMaxDimension;
    previousHit.reset();
    projectionDragging = false;
    (projection ? projectionResult : decal ? decalResult : wrapResult) = Json();
    saved = false;
    try {
        start(
            [this, options, find, apply, verify, projection, decal, source, posed, budget, aliases, base,
             model, extra, existing, existingBytes, dimension, prepared,
             outcome](Progress progress, const std::atomic_bool* cancel) mutable {
                options.limits = budget->remaining();
                auto candidates = find(*posed, options, progress, cancel);
                require(!candidates.materials.empty(),
                        "图片覆盖范围内没有可绘制的表面，请调整图片位置或贴花范围。");
                std::map<int, std::vector<int>> groups;
                for (int id : candidates.materials)
                    groups[aliases.at(id)].push_back(id);
                TextureResolver resolver(model->source, extra, base);
                size_t bytes = existingBytes;
                PaintMappingReport total;
                std::map<int, std::vector<uint8_t>> before, after;
                std::map<int, std::shared_ptr<PaintCanvas>> verifiedCanvases;
                struct Pending {
                    int index = -1;
                    std::shared_ptr<PaintCanvas> canvas;
                    Options options;
                    std::future<PaintMappingReport> future;
                };
                std::array<Pending, 4> pending;
                const size_t parallelWorkers = std::min<size_t>(pending.size(), groups.size());
                size_t maxInflight = 0;
                std::atomic_bool abort = false;
                std::mutex taskErrorMutex;
                std::exception_ptr firstTaskError;
                bool receivingResult = false;
                // Weak ownership identities remain unique even when an unchanged canvas is freed
                // and a later texture allocation reuses its address.
                std::set<std::weak_ptr<PaintCanvas>, std::owner_less<std::weak_ptr<PaintCanvas>>>
                    scheduledCanvases;
                auto checkCancelled = [&] {
                    if (cancel && cancel->load(std::memory_order_relaxed)) {
                        abort.store(true, std::memory_order_relaxed);
                        throw std::runtime_error("Cancelled");
                    }
                };
                history.begin(projection ? "自动多贴图相机投影" : decal ? "表面贴花盖印" : "弧面映射");
                try {
                    auto next = groups.begin();
                    while (next != groups.end()) {
                        size_t count = 0;
                        // touch() rolls back the batch if allocation fails. Prepare a complete wave
                        // before launching it, so that rollback can never race another canvas writer.
                        for (; count < parallelWorkers && next != groups.end(); ++count, ++next) {
                            const auto& [index, ids] = *next;
                            checkCancelled();
                            budget->remaining();
                            auto& task = pending[count];
                            task.index = index;
                            if (existing.contains(index))
                                task.canvas = existing.at(index);
                            else {
                                progress("自动载入覆盖位置的贴图 · " + model->materials.at(index).name);
                                PaintImage image;
                                std::string name;
                                if (auto texture = resolver.material(model->materials.at(index))) {
                                    image = PaintImage::load(*texture, dimension);
                                    name = texture->key();
                                } else {
                                    image = PaintImage(dimension, dimension);
                                    name = "自动创建空白贴图";
                                }
                                bytes += image.rgba.size();
                                require(bytes <= 1024ull * 1024 * 1024,
                                        "绘制内容超过 1 GiB，请缩小范围或降低新贴图分辨率；此次操作已回滚。");
                                task.canvas = std::make_shared<PaintCanvas>(std::move(image));
                                prepared->emplace(index, Entry{task.canvas, {}, name, {}});
                            }
                            require(scheduledCanvases.insert(task.canvas).second,
                                    "同一贴图不能被重复安排到多个映射任务，此次操作已回滚。");
                            checkCancelled();
                            if (verify)
                                before[index] = task.canvas->image().rgba;
                            for (int id : ids)
                                history.touch(id, task.canvas);
                            task.options = options;
                            task.options.material = ids.front();
                            task.options.targetMaterials = ids;
                            task.options.candidatePrimitives.emplace();
                            for (int id : ids) {
                                const auto& primitives = candidates.primitivesByMaterial.at(id);
                                task.options.candidatePrimitives->insert(
                                    task.options.candidatePrimitives->end(), primitives.begin(),
                                    primitives.end());
                            }
                            task.options.externalStroke = true;
                        }

                        for (size_t i = 0; i < count; ++i) {
                            checkCancelled();
                            auto& task = pending[i];
                            task.options.limits = budget->remaining();
                            task.future = std::async(
                                std::launch::async, [this, apply, posed, source, canvas = task.canvas,
                                                     local = std::move(task.options), progress, &abort,
                                                     &taskErrorMutex, &firstTaskError]() mutable {
                                    ++activeMappingTasks;
                                    struct ActiveGuard {
                                        std::atomic_int& value;
                                        ~ActiveGuard() {
                                            --value;
                                        }
                                    } activeGuard{activeMappingTasks};
                                    try {
                                        return apply(*posed, *canvas, *source, local, progress, &abort);
                                    } catch (...) {
                                        {
                                            std::lock_guard lock(taskErrorMutex);
                                            if (!firstTaskError)
                                                firstTaskError = std::current_exception();
                                        }
                                        abort.store(true, std::memory_order_relaxed);
                                        throw;
                                    }
                                });
                            maxInflight = std::max(maxInflight, i + 1);
                        }

                        for (size_t i = 0; i < count; ++i) {
                            auto& task = pending[i];
                            while (task.future.wait_for(std::chrono::milliseconds(20)) !=
                                   std::future_status::ready) {
                                checkCancelled();
                                budget->remaining();
                            }
                            checkCancelled();
                            receivingResult = true;
                            auto report = task.future.get();
                            receivingResult = false;
                            // Only the coordinator reads history, aggregates budgets or releases a
                            // canvas, and only after that canvas's sole writer has fully completed.
                            budget->account(report);
                            total.paintedPixels += report.paintedPixels;
                            total.triangles += report.triangles;
                            total.rasterSamples += report.rasterSamples;
                            total.rayTests += report.rayTests;
                            total.occludedSamples += report.occludedSamples;
                            total.backfaceSamples += report.backfaceSamples;
                            total.reusedTexels += report.reusedTexels;
                            if (report.changed)
                                for (const auto& warning : report.warnings)
                                    if (std::find(total.warnings.begin(), total.warnings.end(), warning) ==
                                        total.warnings.end())
                                        total.warnings.push_back(warning);
                            if (!report.changed && history.discardUnchanged(task.canvas)) {
                                if (prepared->erase(task.index))
                                    bytes -= task.canvas->image().rgba.size();
                                before.erase(task.index);
                            } else if (verify)
                                verifiedCanvases[task.index] = task.canvas;
                            task.canvas.reset();
                        }
                    }
                    checkCancelled();
                    outcome->changed = history.commit();
                    outcome->committed = true;
                } catch (...) {
                    auto error = std::current_exception();
                    abort.store(true, std::memory_order_relaxed);
                    // Covers launch/load/history/allocation errors as well as apply failures. Never
                    // roll back or destroy a canvas while a worker can still be writing its tiles.
                    for (auto& task : pending)
                        if (task.future.valid()) {
                            try {
                                task.future.get();
                            } catch (...) {
                                // The first failure is authoritative; cancellation errors are secondary.
                            }
                        }
                    history.cancel();
                    if (receivingResult && firstTaskError)
                        std::rethrow_exception(firstTaskError);
                    std::rethrow_exception(error);
                }
                const auto& changed = history.changedMaterials();
                // Only genuinely changed textures become persistent overrides.
                for (auto it = prepared->begin(); it != prepared->end();) {
                    bool used = std::any_of(changed.begin(), changed.end(),
                                            [&](int id) { return aliases.at(id) == it->first; });
                    if (!used)
                        it = prepared->erase(it);
                    else
                        ++it;
                }
                total.changed = outcome->changed;
                if (!total.changed)
                    total.warnings = {"覆盖范围内没有像素变化，请调整图片位置、透明度或遮挡设置。"};
                if (verify) {
                    require(total.changed, "自动映射没有改变像素");
                    for (const auto& [index, canvas] : verifiedCanvases)
                        after[index] = canvas->image().rgba;
                    require(history.undo(), "多贴图映射无法撤销");
                    for (const auto& [index, canvas] : verifiedCanvases)
                        require(canvas->image().rgba == before.at(index), "多贴图撤销像素不一致");
                    require(history.redo(), "多贴图映射无法重做");
                    for (const auto& [index, canvas] : verifiedCanvases)
                        require(canvas->image().rgba == after.at(index), "多贴图重做像素不一致");
                }
                total.seconds = budget->elapsed();
                auto result = mappingReport(total);
                result["automatic"] = true;
                result["materials"] = history.changedMaterials();
                result["candidate_canvases"] = groups.size();
                result["parallel_workers"] = parallelWorkers;
                result["max_inflight"] = maxInflight;
                result["undo_redo_match"] = verify;
                result["surface_decal"] = decal;
                return [this, result, projection, decal] {
                    (projection ? projectionResult : decal ? decalResult : wrapResult) = result;
                    status = std::string(projection ? "投影完成" : "贴花完成") + " · 自动写入 " +
                             std::to_string(result.at("materials").size()) + " 个材质 · 可一次撤销";
                };
            },
            false, cleanup);
    } catch (...) {
        saved = wasSaved;
        throw;
    }
}
