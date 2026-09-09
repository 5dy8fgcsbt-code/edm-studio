// Included after PaintEditor::Impl. This diagnostic drives the same asynchronous
// entry points as the viewport; it deliberately never prepares or selects a canvas.
bool PaintEditor::exerciseAutomatic(Renderer& renderer, const fs::path& image, const fs::path& directory,
                                    int maximumDimension) {
    auto& p = *impl;
    auto publish = [&](const char* phase) {
        p.autoResult["phase"] = phase;
        writeJson(directory / "auto-paint-report.json", p.autoResult);
    };
    auto canonicalMaterials = [&](const Json& ids) {
        std::set<int> indices;
        for (const auto& id : ids)
            indices.insert(p.canvasIndex(id.get<int>()));
        return indices;
    };
    auto saveImages = [&](const PaintSnapshot& images, const char* name) {
        fs::create_directories(directory / name);
        std::set<int> written;
        for (const auto& [id, pixels] : images) {
            int index = p.canvasIndex(id);
            if (written.insert(index).second)
                pixels->savePNG(directory / name / ("material_" + std::to_string(index) + ".png"));
        }
    };
    auto compareImages = [&](const PaintSnapshot& expected) {
        for (const auto& [id, pixels] : expected) {
            auto entry = p.entries.find(p.canvasIndex(id));
            require(entry != p.entries.end(), "Automatic paint lost an edited canvas");
            const auto& actual = entry->second.canvas->image();
            require(actual.width == pixels->width && actual.height == pixels->height &&
                        actual.rgba == pixels->rgba,
                    "Automatic paint canvas pixels changed unexpectedly");
        }
    };
    auto verifyGpu = [&] {
        for (auto& [index, entry] : p.entries) {
            syncCanvas(renderer, *entry.canvas, entry.gpu, true);
            renderer.diffuseOverrides[index] = entry.gpu.view;
            require(gpuMatchesCanvas(renderer, *entry.canvas, entry.gpu),
                    "Automatic paint GPU readback mismatch");
        }
        for (int id = 0; id < int(p.canonicalIds.size()); ++id)
            if (auto entry = p.entries.find(p.canvasIndex(id)); entry != p.entries.end())
                renderer.diffuseOverrides[id] = entry->second.gpu.view;
        return p.entries.size();
    };
    auto imageProof = [](const PaintImage& pixels) {
        uint64_t hash = 14695981039346656037ull;
        for (uint8_t value : pixels.rgba) {
            hash ^= value;
            hash *= 1099511628211ull;
        }
        return Json{{"width", pixels.width}, {"height", pixels.height}, {"rgba_hash", hash}};
    };
    auto layerProof = [&](const PaintLayerSnapshotMap& canvases) {
        Json proof = Json::object();
        for (const auto& [id, canvas] : canvases) {
            Json layers = Json::array();
            for (const auto& layer : canvas->layers) {
                std::map<std::pair<uint32_t, uint32_t>, Json> ordered;
                for (const auto& tile : layer.tiles)
                    ordered[{tile.x, tile.y}] = imageProof(tile.image);
                Json tiles = Json::array();
                for (const auto& [origin, pixels] : ordered)
                    tiles.push_back({{"x", origin.first}, {"y", origin.second}, {"pixels", pixels}});
                layers.push_back({{"id", layer.info.id},
                                  {"name", layer.info.name},
                                  {"visible", layer.info.visible},
                                  {"opacity", layer.info.opacity},
                                  {"preserve_alpha", layer.info.preserveAlpha},
                                  {"tiles", tiles}});
            }
            proof[std::to_string(id)] = {
                {"base", imageProof(canvas->base)}, {"active", canvas->activeLayer}, {"layers", layers}};
        }
        return proof;
    };
    try {
        require(p.error.empty(), p.error);
        if (p.autoStage == 10)
            return true;
        if (p.autoStage == 12) {
            int active = p.activeMappingTasks.load();
            if (p.busy() && active == 0)
                return false;
            require(p.busy() && active > 0, "Mapping cancellation did not reach a running raster worker");
            p.stop();
            require(!p.busy() && p.activeMappingTasks == 0 && !p.history.active(),
                    "Mapping cancellation left a live worker or active transaction");
            require(p.entries.size() == p.autoResult.at("cancel_baseline_canvases").get<size_t>(),
                    "Canceled mapping published new temporary canvases");
            compareImages(p.autoAfter);
            require(p.history.canUndo(), "Canceled mapping lost the previous brush undo");
            verifyGpu();
            p.autoResult["mapping_cancel"] = {{"active_workers_at_cancel", active},
                                              {"pixels_match", true},
                                              {"gpu_pixels_match", true},
                                              {"temporary_canvases_released", true}};
            p.autoStage = 3;
            publish("mapping_cancel_rolled_back");
            return false;
        }
        if (p.busy())
            return false;
        if (p.autoStage == 0) {
            require(p.scene && renderer.model, "Automatic paint diagnostic needs a loaded model");
            require(p.entries.empty() && !p.strokePending(),
                    "Automatic paint diagnostic must start without prepared canvases");
            require(fs::is_regular_file(image), "Automatic paint source PNG does not exist");
            require(!fs::exists(directory / "project"),
                    "Use a fresh automatic paint output directory; its project already exists");
            fs::create_directories(directory);
            p.recoveryRoot = directory / "recovery";
            require(maximumDimension == 512 || maximumDimension == 1024 || maximumDimension == 2048,
                    "Automatic paint diagnostic supports 512, 1024 or 2048");
            p.paintMaxDimension = maximumDimension;
            p.tool = 1;
            p.radius = float(std::max(.01, renderer.camera.radius * .006));
            p.hardness = .8f;
            p.opacity = 1;
            p.color = {.95f, .06f, .82f, 1};
            p.ensureAliases();
            p.autoResult = {{"passed", false},
                            {"model", pathString(p.scene->source)},
                            {"source_image", pathString(image)},
                            {"initial_canvases", p.entries.size()},
                            {"maximum_new_texture_dimension", maximumDimension},
                            {"brush_radius_world", p.radius},
                            {"manual_material_selection", false}};
            publish("building_surface");
            p.autoStage = 1;
            p.buildSurface();
            return false;
        }
        if (p.autoStage == 1) {
            if (!p.surface || p.surfaceArgs != p.currentArgs)
                return false;
            auto dimensions = renderer.viewportMetrics();
            ImVec2 size{dimensions.at("width").get<float>(), dimensions.at("height").get<float>()};
            auto context = p.frameContext(renderer, {0, 0}, size);
            std::map<int, std::vector<PaintStrokePoint>> visible;
            for (int y = 0; y < 29; ++y)
                for (int x = 0; x < 41; ++x) {
                    PaintStrokePoint point{(x + .5) * size.x / 41., (y + .5) * size.y / 29.};
                    auto hit = Impl::strokeHit(context, point);
                    if (!hit || !p.paintable(hit->material))
                        continue;
                    const auto& material = p.scene->materials.at(hit->material);
                    // Glass and additive effects are poor deterministic paint targets.
                    if (material.blending >= 1 && material.blending <= 4)
                        continue;
                    visible[p.canvasIndex(hit->material)].push_back(point);
                }
            require(visible.size() >= 2,
                    "Automatic paint requires two visible opaque surfaces with different textures");
            std::vector<int> ranked;
            Json coverage = Json::array();
            for (const auto& [index, points] : visible) {
                ranked.push_back(index);
                coverage.push_back({{"canvas", index},
                                    {"material_name", p.scene->materials.at(index).name},
                                    {"viewport_hits", points.size()}});
            }
            std::sort(ranked.begin(), ranked.end(), [&](int a, int b) {
                return visible.at(a).size() != visible.at(b).size()
                           ? visible.at(a).size() > visible.at(b).size()
                           : a < b;
            });
            auto representative = [&](int index) {
                const auto& points = visible.at(index);
                PaintStrokePoint center{};
                for (auto point : points) {
                    center.x += point.x / points.size();
                    center.y += point.y / points.size();
                }
                auto distance = [&](PaintStrokePoint point) {
                    return std::hypot(point.x - center.x, point.y - center.y);
                };
                return *std::min_element(points.begin(), points.end(),
                                         [&](auto a, auto b) { return distance(a) < distance(b); });
            };
            auto first = representative(ranked[0]), second = representative(ranked[1]);
            p.autoResult["viewport_coverage"] = coverage;
            p.autoResult["target_canvases"] = {ranked[0], ranked[1]};
            p.autoResult["stroke_points"] = {
                {first.x, first.y}, {second.x, second.y}, {first.x, first.y}, {second.x, second.y}};
            const bool wasSaved = p.saved;
            p.beginQueuedStroke(context, first, 1.);
            p.drainStroke(1, .002);
            require(p.busy() && p.entries.empty() && p.history.active(),
                    "Lazy-load cancellation diagnostic did not start the asynchronous texture loader");
            p.cancelQueuedStroke();
            require(p.entries.empty() && !p.history.active() && !p.history.canUndo() &&
                        !p.history.canRedo() && !p.busy() && !p.strokePending() && p.saved == wasSaved,
                    "Cancelling first-texture loading did not restore the empty saved document");
            p.autoResult["lazy_load_cancel_rollback"] = true;
            // Release all samples before draining. Texture loading must pause this
            // queue without skipping the initiating sample or its exact endpoint.
            p.beginQueuedStroke(context, first, 1.);
            auto id = p.strokeContexts.rbegin()->first;
            p.strokePath.append(second, 1., id);
            p.strokePath.append(first, 1., id);
            p.strokePath.append(second, 1., id);
            p.autoEnd = second;
            p.autoExpectedSamples = p.strokePath.pendingSamples();
            p.strokePath.release();
            require(p.entries.empty(), "Automatic stroke eagerly prepared a canvas before draining");
            p.autoResult["expected_samples"] = p.autoExpectedSamples;
            publish("released_stroke_loading_textures");
            p.autoStage = 2;
            p.drainStroke(8, .002);
            return false;
        }
        if (p.autoStage == 2) {
            if (p.strokePending()) {
                p.drainStroke(8, .002);
                return false;
            }
            require(p.strokeResult.is_object() &&
                        p.strokeResult.at("samples").get<uint64_t>() == p.autoExpectedSamples &&
                        p.strokeResult.at("pending_samples").get<uint64_t>() == 0 &&
                        p.strokeResult.at("discarded_samples").get<uint64_t>() == 0,
                    "Automatic stroke lost queued input samples during lazy texture loading");
            require(p.autoExpectedSamples > 32 && p.strokeResult.at("frames").get<size_t>() > 1,
                    "Automatic stroke did not exercise multi-frame draining");
            require(p.strokeResult.at("endpoint").at(0).get<double>() == p.autoEnd.x &&
                        p.strokeResult.at("endpoint").at(1).get<double>() == p.autoEnd.y,
                    "Automatic stroke missed its released endpoint");
            auto changed = canonicalMaterials(p.strokeResult.at("materials"));
            require(changed.size() >= 2 && p.strokeResult.at("changed").get<bool>(),
                    "Automatic stroke did not edit two independent canvases");
            p.autoAfter = p.snapshot();
            require(p.history.undo(), "Automatic stroke could not undo its multi-canvas transaction");
            p.autoBefore = p.snapshot();
            size_t different = 0;
            for (int index : changed)
                different += p.autoBefore.at(index)->rgba != p.autoAfter.at(index)->rgba;
            require(different >= 2, "Automatic stroke history did not restore multiple original textures");
            require(p.history.redo(), "Automatic stroke could not redo its multi-canvas transaction");
            compareImages(p.autoAfter);
            auto checked = verifyGpu();
            saveImages(p.autoBefore, "before-brush");
            saveImages(p.autoAfter, "after-brush");
            p.autoBefore.clear();
            p.autoResult["brush"] = p.strokeResult;
            p.autoResult["brush"]["undo_redo_match"] = true;
            p.autoResult["brush"]["gpu_pixels_match"] = true;
            p.autoResult["brush"]["gpu_canvases_checked"] = checked;
            p.autoResult["brush"]["original_canvases_restored"] = different;
            p.autoResult["brush"]["released_before_drain"] = true;
            publish("loading_projection_image");
            p.autoStage = 3;
            p.loadMappingImage(renderer, image, 3);
            return false;
        }
        if (p.autoStage == 3) {
            require(bool(p.mappingImage), "Automatic projection image was not loaded");
            if (!p.surface || p.surfaceArgs != p.currentArgs)
                return false;
            p.projectionCenter = {.5f, .5f};
            // At production 2K precision test the normal UI placement. The 512 pass separately
            // stress-tests a full-viewport footprint across every visible material.
            p.projectionSize = maximumDimension >= 2048 ? F2{.65f, .65f} : F2{1.15f, 1.15f};
            p.projectionAspectLocked = maximumDimension >= 2048;
            p.projectionOpacity = .9f;
            p.projectionFront = true;
            p.projectionOcclusion = true;
            p.projectionOverlay = false;
            p.autoResult["projection_placement"] = {{"size", p.projectionSize},
                                                    {"aspect_locked", p.projectionAspectLocked}};
            bool cancellationProbe = !p.autoResult.contains("mapping_cancel");
            p.autoResult["cancel_baseline_canvases"] = p.entries.size();
            publish(cancellationProbe ? "canceling_active_mapping_workers" : "projecting_visible_surfaces");
            p.autoStage = cancellationProbe ? 12 : 4;
            p.startProjection(renderer, true);
            return false;
        }
        if (p.autoStage == 4) {
            require(p.projectionResult.is_object() && p.projectionResult.at("automatic").get<bool>() &&
                        p.projectionResult.at("changed").get<bool>() &&
                        p.projectionResult.at("undo_redo_match").get<bool>(),
                    "Automatic projection or its multi-canvas undo/redo did not complete");
            auto changed = canonicalMaterials(p.projectionResult.at("materials"));
            require(changed.size() >= 2, "Automatic projection did not cover two independent canvases");
            auto projected = p.snapshot();
            size_t different = 0;
            for (int index : changed)
                different +=
                    !p.autoAfter.contains(index) || p.autoAfter.at(index)->rgba != projected.at(index)->rgba;
            require(different >= 2, "Automatic projection did not change two independent texture images");
            p.autoAfter = std::move(projected);
            p.autoResult["projection"] = p.projectionResult;
            p.autoResult["projection"]["gpu_canvases_checked"] = verifyGpu();
            p.autoResult["projection"]["gpu_pixels_match"] = true;
            p.autoResult["projection"]["changed_canvases"] = different;
            saveImages(p.autoAfter, "after-projection");
            p.alignWrap();
            p.wrapHeight *= .2f;
            p.wrapSweep = 360;
            p.wrapOpacity = .6f;
            p.tool = 2;
            publish("wrapping_model_surfaces");
            p.autoStage = 11;
            p.startWrap(true);
            return false;
        }
        if (p.autoStage == 11) {
            require(p.wrapResult.is_object() && p.wrapResult.at("automatic").get<bool>() &&
                        p.wrapResult.at("changed").get<bool>() &&
                        p.wrapResult.at("undo_redo_match").get<bool>(),
                    "Automatic wrap or its multi-canvas undo/redo did not complete");
            auto changed = canonicalMaterials(p.wrapResult.at("materials"));
            require(changed.size() >= 2, "Automatic wrap did not cover two independent canvases");
            auto wrapped = p.snapshot();
            size_t different = 0;
            for (int index : changed)
                different +=
                    !p.autoAfter.contains(index) || p.autoAfter.at(index)->rgba != wrapped.at(index)->rgba;
            require(different >= 2, "Automatic wrap did not change two independent texture images");
            p.autoAfter = std::move(wrapped);
            p.autoResult["wrap"] = p.wrapResult;
            p.autoResult["wrap"]["gpu_canvases_checked"] = verifyGpu();
            p.autoResult["wrap"]["gpu_pixels_match"] = true;
            p.autoResult["wrap"]["changed_canvases"] = different;
            p.autoResult["wrap"]["aligned_height_fraction"] = .2;
            p.autoResult["wrap"]["sweep_degrees"] = 360;
            p.autoResult["wrap"]["opacity"] = .6;
            saveImages(p.autoAfter, "after-wrap");
            publish("saving_full_project");
            p.autoStage = 5;
            p.saveProject(directory / "project");
            return false;
        }
        if (p.autoStage == 5) {
            require(!p.lastSavedProject.empty() && fs::is_regular_file(p.lastSavedProject),
                    "Automatic paint project did not publish its complete manifest");
            p.autoResult["project"] = {{"manifest", pathString(p.lastSavedProject)}};
            publish("reopening_full_project");
            p.autoStage = 6;
            p.openProject(renderer, p.lastSavedProject);
            return false;
        }
        if (p.autoStage == 6) {
            require(p.lastOpenRestoredAppearance,
                    "Automatic paint project did not restore its complete appearance");
            compareImages(p.autoAfter);
            p.autoResult["project"]["gpu_canvases_checked"] = verifyGpu();
            p.autoResult["project"]["pixels_match"] = true;
            p.autoResult["project"]["gpu_pixels_match"] = true;
            p.autoResult["project"]["appearance_restored"] = true;
            require(!hasEdits(), "Reopened project should be saved before the template diagnostic");
            publish("loading_template_for_zero_opacity_stroke");
            p.autoStage = 7;
            // A uniform image with deliberately different dimensions gives a known expected
            // result after resampling, independent of the resizing implementation.
            const auto templatePath = directory / "layer-template.png";
            PaintImage(37, 23, {25, 180, 227, 255}).savePNG(templatePath);
            p.queueTemplate(renderer, templatePath);
            return false;
        }
        if (p.autoStage == 7) {
            require(bool(p.pendingTemplate), "Automatic template diagnostic did not load its template");
            require(!hasEdits(), "Loading a pending template unexpectedly modified the saved project");
            if (!p.surface || p.surfaceArgs != p.currentArgs)
                return false;
            auto dimensions = renderer.viewportMetrics();
            ImVec2 size{dimensions.at("width").get<float>(), dimensions.at("height").get<float>()};
            auto context = p.frameContext(renderer, {0, 0}, size);
            context.brush.opacity = 0;
            const auto& initialPoint = p.autoResult.at("stroke_points").at(0);
            PaintStrokePoint point{initialPoint.at(0).get<double>(), initialPoint.at(1).get<double>()};
            auto hit = Impl::strokeHit(context, point);
            require(hit.has_value(), "Original visible surface is no longer available for the template test");
            int target = p.canvasIndex(hit->material);
            require(p.entries.contains(target), "Template diagnostic must use an existing canvas");
            auto layout = p.layerInfos;
            const auto templateLayer = p.nextLayerId;
            layout.push_back({templateLayer, "模板诊断图层", true, 1, false});
            p.changeLayers(layout, templateLayer);
            p.autoBefore = p.snapshot();
            const auto beforeLayers = layerProof(p.layerSnapshot());
            p.autoResult["template"] = {{"target_canvas", target},
                                        {"source_image", pathString(directory / "layer-template.png")},
                                        {"layer", templateLayer},
                                        {"brush_opacity", 0},
                                        {"before_layers", beforeLayers}};
            // Exercise cancellation after the template has actually changed the active layer,
            // before the queued stroke commits. The pending template must remain reusable.
            const bool wasSaved = p.saved;
            auto originalCanvas = p.entries.at(target).canvas;
            p.beginQueuedStroke(context, point, 1.);
            require(p.installLayerTemplate(true), "Template cancellation did not install its layer image");
            require(p.entries.at(target).canvas == originalCanvas &&
                        originalCanvas->image().rgba != p.autoBefore.at(target)->rgba,
                    "Layer template did not modify the retained canvas before cancellation");
            p.cancelQueuedStroke();
            require(p.pendingTemplate && !p.history.active() && p.history.canUndo() && p.saved == wasSaved,
                    "Cancelling layer template lost the pending image, prior history or saved state");
            compareImages(p.autoBefore);
            require(layerProof(p.layerSnapshot()) == beforeLayers,
                    "Cancelling template changed the base, earlier paint layers or metadata");
            p.autoResult["template"]["cancel_restores_base_layers_history_and_pending"] = true;
            p.beginQueuedStroke(context, point, 1.);
            p.strokePath.release();
            publish("installing_template_without_brush_pixels");
            p.autoStage = 8;
            p.drainStroke(1, .002);
            return false;
        }
        if (p.autoStage == 8) {
            if (p.strokePending()) {
                p.drainStroke(1, .002);
                return false;
            }
            require(p.strokeResult.is_object() && p.strokeResult.at("painted_pixels").get<uint64_t>() == 0 &&
                        p.strokeResult.at("samples").get<uint64_t>() == 1 &&
                        p.strokeResult.at("changed").get<bool>(),
                    "Zero-opacity template stroke must report a template change and zero painted pixels");
            require(hasEdits() && !p.pendingTemplate,
                    "Installing a template without brush pixels did not mark the document unsaved");
            const int target = p.autoResult.at("template").at("target_canvas").get<int>();
            const auto templateLayer = p.autoResult.at("template").at("layer").get<uint64_t>();
            const auto afterLayers = layerProof(p.layerSnapshot());
            const auto& beforeLayers = p.autoResult.at("template").at("before_layers");
            for (const auto& [key, before] : beforeLayers.items()) {
                const auto& after = afterLayers.at(key);
                require(after.at("base") == before.at("base") && after.at("active") == before.at("active") &&
                            after.at("layers").size() == before.at("layers").size(),
                        "Template replaced the original base or changed global layer settings");
                for (size_t index = 0; index < before.at("layers").size(); ++index) {
                    const auto& oldLayer = before.at("layers").at(index);
                    if (p.canvasIndex(std::stoi(key)) != target || oldLayer.at("id") != templateLayer)
                        require(after.at("layers").at(index) == oldLayer,
                                "Template changed another material or an underlying layer");
                }
            }
            auto templateSnapshot = snapshot();
            for (const auto& [id, expected] : p.autoBefore) {
                require(templateSnapshot.contains(id) && templateSnapshot.at(id)->width == expected->width &&
                            templateSnapshot.at(id)->height == expected->height,
                        "Layer template changed the existing texture resolution");
                if (p.canvasIndex(id) == target) {
                    const PaintImage solid(expected->width, expected->height, {25, 180, 227, 255});
                    require(templateSnapshot.at(id)->rgba == solid.rgba,
                            "Resampled opaque template did not cover its active layer");
                } else
                    require(templateSnapshot.at(id)->rgba == expected->rgba,
                            "Template changed an unrelated material composite");
            }
            require(p.undoLayerEdit(), "Layer template did not create an undoable edit");
            compareImages(p.autoBefore);
            require(layerProof(p.layerSnapshot()) == beforeLayers,
                    "Undoing template did not restore the base and all prior editable layers");
            require(p.undoLayerEdit(true), "Cannot redo layer template installation");
            compareImages(templateSnapshot);
            require(layerProof(p.layerSnapshot()) == afterLayers,
                    "Redoing template lost editable layer content");
            p.autoResult["template"]["undo_redo_preserves_base_and_lower_layers"] = true;
            p.autoResult["template"]["existing_resolution_preserved"] = true;
            p.autoBefore = templateSnapshot;
            auto recoveryRoot = directory / "template-recovery";
            require(!fs::exists(recoveryRoot), "Template recovery diagnostic requires a fresh directory");
            recover(recoveryRoot);
            fs::path recoveryManifest;
            for (const auto& entry : fs::directory_iterator(recoveryRoot)) {
                auto candidate = entry.path() / "project.edmpaint.json";
                if (!entry.is_directory() || !fs::is_regular_file(candidate))
                    continue;
                require(recoveryManifest.empty(), "Template diagnostic produced multiple recovery documents");
                recoveryManifest = std::move(candidate);
            }
            require(!recoveryManifest.empty(), "Template-only edits did not publish a recovery document");
            auto recovered = loadPaintDocument(*p.scene, recoveryManifest);
            for (const auto& [id, expected] : p.autoBefore) {
                require(recovered.images.contains(id) && recovered.images.at(id)->width == expected->width &&
                            recovered.images.at(id)->height == expected->height &&
                            recovered.images.at(id)->rgba == expected->rgba,
                        "Recovery changed or lost the template-only texture pixels");
            }
            require(recovered.restoreAppearance, "Template recovery did not preserve the base appearance");
            require(layerProof(recovered.layerCanvases) == afterLayers,
                    "Template recovery lost the base or editable layer stack");
            p.autoResult["template"]["stroke"] = p.strokeResult;
            p.autoResult["template"]["unsaved_without_painted_pixels"] = true;
            p.autoResult["template"]["snapshot_pixels_match"] = true;
            p.autoResult["template"]["recovery_pixels_match"] = true;
            p.autoResult["template"]["recovery_layers_match"] = true;
            p.autoResult["template"]["recovery_manifest"] = pathString(recoveryManifest);
            p.autoResult["template"]["gpu_canvases_checked"] = verifyGpu();
            p.autoResult["template"]["gpu_pixels_match"] = true;
            p.autoBefore.clear();
            publish("restoring_main_project_after_template_test");
            p.autoStage = 9;
            p.openProject(renderer, p.lastSavedProject);
            return false;
        }
        require(p.autoStage == 9 && p.lastOpenRestoredAppearance && !hasEdits(),
                "Main paint project was not restored after the template diagnostic");
        compareImages(p.autoAfter);
        p.autoResult["template"]["main_project_gpu_canvases_checked"] = verifyGpu();
        p.autoResult["template"]["main_project_restored"] = true;
        p.autoResult["final_canvases"] = p.entries.size();
        p.autoResult["passed"] = true;
        publish("complete");
        p.autoBefore.clear();
        p.autoAfter.clear();
        p.paintMaxDimension = 2048;
        p.status = "自动多贴图画笔、投影、撤销重做、GPU 与工程重开已验证";
        p.autoStage = 10;
        return true;
    } catch (...) {
        auto error = exceptionText();
        try {
            p.autoResult["passed"] = false;
            p.autoResult["error"] = error;
            if (fs::is_directory(directory))
                writeJson(directory / "auto-paint-report.json", p.autoResult);
        } catch (...) {
        }
        throw;
    }
}
