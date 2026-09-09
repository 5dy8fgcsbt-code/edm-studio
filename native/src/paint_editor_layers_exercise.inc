// Included after PaintEditor::Impl. Uses the real posed model, camera picking, texture resolver,
// batch coordinator and D3D11 uploads. It never substitutes a synthetic model or selected material.
bool PaintEditor::exerciseLayers(Renderer& renderer, const Args& args, const fs::path& directory) {
    auto& p = *impl;
    if (p.layersDiagnosticResult.is_object() && p.layersDiagnosticResult.value("passed", false))
        return true;
    if (p.controlsBusy())
        return false;
    Json result{{"passed", false}, {"manual_material_selection", false}, {"checks", 0}};
    auto check = [&](bool condition, const std::string& message) {
        require(condition, message);
        result["checks"] = result.at("checks").get<size_t>() + 1;
    };
    auto sameImage = [](const PaintImage& a, const PaintImage& b) {
        return a.width == b.width && a.height == b.height && a.rgba == b.rgba;
    };
    auto sameInfo = [](const PaintLayerInfo& a, const PaintLayerInfo& b) {
        return a.id == b.id && a.name == b.name && a.visible == b.visible && a.opacity == b.opacity &&
               a.preserveAlpha == b.preserveAlpha;
    };
    auto sameLayout = [&](const std::vector<PaintLayerInfo>& a, const std::vector<PaintLayerInfo>& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), sameInfo);
    };
    auto samePixels = [&](const PaintLayerSnapshot& a, const PaintLayerSnapshot& b) {
        if (a.tiles.size() != b.tiles.size())
            return false;
        std::map<std::pair<uint32_t, uint32_t>, const PaintImage*> tiles;
        for (const auto& tile : a.tiles)
            if (!tiles.emplace(std::pair{tile.x, tile.y}, &tile.image).second)
                return false;
        for (const auto& tile : b.tiles) {
            auto found = tiles.find({tile.x, tile.y});
            if (found == tiles.end() || !sameImage(*found->second, tile.image))
                return false;
        }
        return true;
    };
    auto layer = [](const PaintCanvasLayers& canvas, uint64_t id) -> const PaintLayerSnapshot& {
        auto found = std::find_if(canvas.layers.begin(), canvas.layers.end(),
                                  [&](const auto& candidate) { return candidate.info.id == id; });
        require(found != canvas.layers.end(), "Layer diagnostic lost a layer ID");
        return *found;
    };
    auto sameCanvas = [&](const PaintCanvasLayers& a, const PaintCanvasLayers& b) {
        if (a.activeLayer != b.activeLayer || !sameImage(a.base, b.base) ||
            a.layers.size() != b.layers.size())
            return false;
        for (size_t i = 0; i < a.layers.size(); ++i)
            if (!sameInfo(a.layers[i].info, b.layers[i].info) || !samePixels(a.layers[i], b.layers[i]))
                return false;
        return true;
    };
    auto publish = [&](const char* phase) {
        result["phase"] = phase;
        p.layersDiagnosticResult = result;
        writeJson(directory / "layers-report.json", result);
    };
    auto verifyLayout = [&] {
        check(sameLayout(p.layerControl->layers(), p.layerInfos) &&
                  p.layerControl->activeLayer() == p.activeLayerId,
              "Global layer control differs from the UI layer list");
        for (const auto& [index, entry] : p.entries)
            check(entry.canvas->layered() && sameLayout(entry.canvas->layers(), p.layerInfos) &&
                      entry.canvas->activeLayer() == p.activeLayerId,
                  "A material canvas lost global layer order, settings or selection");
    };
    auto verifyGpu = [&] {
        for (auto& [index, entry] : p.entries) {
            syncCanvas(renderer, *entry.canvas, entry.gpu, true);
            renderer.diffuseOverrides[index] = entry.gpu.view;
            check(gpuMatchesCanvas(renderer, *entry.canvas, entry.gpu),
                  "Layer composite GPU readback differs from CPU pixels");
        }
        for (int id = 0; id < int(p.canonicalIds.size()); ++id)
            if (auto entry = p.entries.find(p.canvasIndex(id)); entry != p.entries.end())
                renderer.diffuseOverrides[id] = entry->second.gpu.view;
        verifyLayout();
    };
    auto compareComposite = [&](const PaintSnapshot& expected) {
        for (const auto& [id, image] : expected) {
            auto found = p.entries.find(p.canvasIndex(id));
            check(found != p.entries.end() && sameImage(found->second.canvas->image(), *image),
                  "Layer operation did not restore the expected material composite");
        }
    };
    auto saveImages = [&](const char* phase) {
        fs::create_directories(directory / phase);
        for (const auto& [index, entry] : p.entries)
            entry.canvas->image().savePNG(directory / phase / ("material_" + std::to_string(index) + ".png"));
    };
    try {
        check(p.error.empty(), p.error);
        check(p.scene && renderer.model && !p.scene->collisionOnly(),
              "Layer diagnostic requires a loaded paintable EDM model");
        check(p.entries.empty() && !p.pendingTemplate && p.layerInfos.size() == 1,
              "Layer diagnostic must start with an unedited document");
        check(!fs::exists(directory / "layers-report.json"), "Use a fresh layer diagnostic directory");
        fs::create_directories(directory);
        result["model"] = pathString(p.scene->source);
        p.paintMaxDimension = 512;
        p.tool = 1;
        p.currentArgs = args;
        p.attachmentsVisible = renderer.options.attachments;
        renderer.options.editedLivery = true;
        p.ensureAliases();
        publish("building_real_surface");
        p.surface = std::make_shared<PaintSurface>(p.scene, args, Progress{}, nullptr, p.attachmentsVisible);
        p.surfaceArgs = args;
        const auto dimensions = renderer.viewportMetrics();
        ImVec2 size{dimensions.at("width").get<float>(), dimensions.at("height").get<float>()};
        auto context = p.frameContext(renderer, {0, 0}, size);
        struct Target {
            int canonical = -1;
            PaintStrokePoint point;
            PaintHit hit;
        };
        std::map<int, std::vector<Target>> visible;
        for (int y = 0; y < 29; ++y)
            for (int x = 0; x < 41; ++x) {
                PaintStrokePoint point{(x + .5) * size.x / 41., (y + .5) * size.y / 29.};
                auto hit = Impl::strokeHit(context, point);
                if (!hit || !p.paintable(hit->material))
                    continue;
                const auto& material = p.scene->materials.at(hit->material);
                if (material.blending >= 1 && material.blending <= 4)
                    continue;
                int canonical = p.canvasIndex(hit->material);
                visible[canonical].push_back({canonical, point, *hit});
            }
        check(visible.size() >= 2,
              "Layer diagnostic requires two visible opaque surfaces with independent textures");
        std::vector<int> ranked;
        for (const auto& [index, points] : visible)
            ranked.push_back(index);
        std::sort(ranked.begin(), ranked.end(), [&](int a, int b) {
            return visible.at(a).size() == visible.at(b).size() ? a < b
                                                                : visible.at(a).size() > visible.at(b).size();
        });
        std::vector<Target> targets;
        result["targets"] = Json::array();
        for (int canonical : {ranked[0], ranked[1]}) {
            const auto& points = visible.at(canonical);
            PaintStrokePoint center{};
            for (const auto& candidate : points) {
                center.x += candidate.point.x / points.size();
                center.y += candidate.point.y / points.size();
            }
            auto distance = [&](const Target& candidate) {
                return std::hypot(candidate.point.x - center.x, candidate.point.y - center.y);
            };
            const auto target =
                *std::min_element(points.begin(), points.end(),
                                  [&](const auto& a, const auto& b) { return distance(a) < distance(b); });
            targets.push_back(target);
            result["targets"].push_back({{"canvas", canonical},
                                         {"material", target.hit.material},
                                         {"name", p.scene->materials.at(canonical).name},
                                         {"screen", {target.point.x, target.point.y}},
                                         {"visible_samples", points.size()}});
        }
        PaintBrush brush;
        brush.radius = std::max(.04, renderer.camera.radius * .025);
        brush.hardness = 1;
        brush.opacity = 1;
        brush.preserveAlpha = true;
        brush.maxPixels = 4ull * 1024 * 1024;
        result["brush_radius_world"] = brush.radius;
        auto paint = [&](const std::vector<Target>& selected, F4 color) {
            p.requireWritableLayer();
            brush.color = color;
            p.history.begin("图层诊断表面画笔");
            try {
                for (const auto& target : selected) {
                    auto canvas = p.entries.at(target.canonical).canvas;
                    std::vector<int> aliases;
                    for (int id = 0; id < int(p.canonicalIds.size()); ++id)
                        if (p.canvasIndex(id) == target.canonical)
                            aliases.push_back(id);
                    auto& touched = p.history.touch(target.hit.material, canvas);
                    auto stats =
                        paintBrush(touched, *p.surface, target.hit, brush, context.eye, nullptr, -1, aliases);
                    check(stats.pixels > 0, "A visible surface brush did not edit any layer texels");
                }
                check(p.history.commit(), "Surface brush did not create an undoable layer edit");
                p.saved = false;
            } catch (...) {
                p.history.cancel();
                throw;
            }
        };
        const auto bottom = p.layerInfos.front().id;
        check(p.requestCanvases({targets[0].hit.material}, true), "First automatic texture did not load");
        verifyLayout();
        paint({targets[0]}, {1, .04f, .05f, 1});
        const auto firstLower = p.layerSnapshot();
        const auto upper = p.nextLayerId;
        auto layout = p.layerInfos;
        layout.push_back({upper, "蓝色标记", true, 1, true});
        p.changeLayers(layout, upper);
        check(p.requestCanvases({targets[1].hit.material}, true), "Second automatic texture did not load");
        check(p.entries.size() == 2, "Diagnostic did not retain two independent automatic canvases");
        verifyLayout();
        check(samePixels(layer(*firstLower.at(targets[0].canonical), bottom),
                         layer(p.entries.at(targets[0].canonical).canvas->layerSnapshot(), bottom)),
              "Creating a global layer changed the existing lower paint");
        result["lazy_canvas_inherits_layers"] = true;
        p.selectLayer(bottom);
        paint({targets[1]}, {1, .04f, .05f, 1});
        const auto lowerPixels = p.layerSnapshot();
        const auto lowerComposite = p.snapshot();
        check(p.undoLayerEdit(), "Cannot undo the lazy canvas's first brush");
        check(p.undoLayerEdit(), "Cannot undo layer creation made before a second canvas existed");
        check(p.layerInfos.size() == 1 && p.layerInfos.front().id == bottom,
              "Undoing layer creation did not reconcile the subsequently loaded canvas");
        verifyLayout();
        check(p.undoLayerEdit(true), "Cannot redo global layer creation across a lazy canvas");
        check(p.undoLayerEdit(true), "Cannot redo a lazy canvas brush after restoring global layers");
        compareComposite(lowerComposite);
        verifyLayout();
        result["lazy_canvas_layer_creation_undo_redo"] = true;
        saveImages("lower-layer");
        p.selectLayer(upper);
        paint(targets, {.03f, .18f, 1, 1});
        auto upperPixels = p.layerSnapshot();
        const auto upperComposite = p.snapshot();
        for (const auto& target : targets) {
            const auto& before = *lowerPixels.at(target.canonical);
            const auto& after = *upperPixels.at(target.canonical);
            check(samePixels(layer(before, bottom), layer(after, bottom)),
                  "Upper brush changed lower-layer pixels");
            check(!samePixels(layer(before, upper), layer(after, upper)),
                  "Upper brush did not create independent layer pixels");
            check(!sameImage(*lowerComposite.at(target.canonical), *upperComposite.at(target.canonical)),
                  "Upper paint did not affect the material composite");
        }
        check(p.undoLayerEdit(), "Cannot undo a brush across two material layers");
        compareComposite(lowerComposite);
        check(p.undoLayerEdit(true), "Cannot redo a brush across two material layers");
        compareComposite(upperComposite);
        verifyGpu();
        saveImages("upper-layer");
        result["independent_layers_and_cross_material_brush_undo"] = true;
        publish("testing_global_layer_operations");

        layout = p.layerInfos;
        layout.back().visible = false;
        p.changeLayers(layout, upper);
        compareComposite(lowerComposite);
        bool blocked = false;
        try {
            p.requireWritableLayer();
        } catch (...) {
            blocked = true;
        }
        check(blocked, "A hidden active layer still accepts viewport painting");
        verifyGpu();
        check(p.undoLayerEdit(), "Cannot undo global visibility");
        compareComposite(upperComposite);
        check(p.undoLayerEdit(true), "Cannot redo global visibility");
        compareComposite(lowerComposite);
        check(p.undoLayerEdit(), "Cannot restore visible layers");
        layout = p.layerInfos;
        layout.back().opacity = 0;
        p.changeLayers(layout, upper);
        compareComposite(lowerComposite);
        layout.back().opacity = .5f;
        p.changeLayers(layout, upper);
        for (const auto& target : targets) {
            const auto& actual = p.entries.at(target.canonical).canvas->image();
            const auto& low = *lowerComposite.at(target.canonical);
            const auto& high = *upperComposite.at(target.canonical);
            check(!sameImage(actual, low) && !sameImage(actual, high),
                  "Half opacity is equivalent to a fully hidden or opaque layer");
            size_t intermediate = 0;
            bool betweenEndpoints = true;
            for (size_t i = 0; i < actual.rgba.size(); ++i) {
                if (i % 4 == 3)
                    continue;
                int lo = std::min(low.rgba[i], high.rgba[i]), hi = std::max(low.rgba[i], high.rgba[i]);
                betweenEndpoints &= actual.rgba[i] >= lo - 1 && actual.rgba[i] <= hi + 1;
                intermediate += actual.rgba[i] > lo && actual.rgba[i] < hi;
            }
            check(betweenEndpoints, "Half opacity produced a color outside its two endpoint composites");
            check(intermediate > 0, "Half opacity did not interpolate any painted colors");
        }
        verifyGpu();
        saveImages("half-opacity");
        layout.back().opacity = 1;
        p.changeLayers(layout, upper);
        compareComposite(upperComposite);
        std::reverse(layout.begin(), layout.end());
        p.changeLayers(layout, upper);
        const auto reordered = p.snapshot();
        for (const auto& target : targets) {
            check(!sameImage(*reordered.at(target.canonical), *upperComposite.at(target.canonical)),
                  "Changing overlapping layer order did not change the composite");
            auto current = p.entries.at(target.canonical).canvas->layerSnapshot();
            check(samePixels(layer(current, bottom), layer(*upperPixels.at(target.canonical), bottom)) &&
                      samePixels(layer(current, upper), layer(*upperPixels.at(target.canonical), upper)),
                  "Reordering layers changed their independent pixels");
        }
        verifyGpu();
        check(p.undoLayerEdit(), "Cannot undo layer order");
        compareComposite(upperComposite);
        check(p.undoLayerEdit(true), "Cannot redo layer order");
        compareComposite(reordered);
        check(p.undoLayerEdit(), "Cannot restore original layer order");
        result["visibility_opacity_order_and_undo"] = true;

        const auto duplicate = p.nextLayerId;
        layout = p.layerInfos;
        layout.push_back({duplicate, "蓝色标记 副本", true, 1, true});
        p.changeLayers(layout, duplicate, upper, duplicate);
        for (const auto& target : targets) {
            auto current = p.entries.at(target.canonical).canvas->layerSnapshot();
            check(samePixels(layer(current, upper), layer(current, duplicate)),
                  "Duplicating a global layer lost source pixels");
        }
        paint(targets, {.06f, 1, .15f, 1});
        const auto duplicatePixels = p.layerSnapshot();
        const auto duplicateComposite = p.snapshot();
        for (const auto& target : targets) {
            const auto& current = *duplicatePixels.at(target.canonical);
            check(samePixels(layer(current, upper), layer(*upperPixels.at(target.canonical), upper)),
                  "Painting the duplicate mutated its source layer");
            check(!samePixels(layer(current, upper), layer(current, duplicate)),
                  "Duplicate paint is not independent from the source");
        }
        layout = p.layerInfos;
        layout.pop_back();
        p.changeLayers(layout, upper);
        compareComposite(upperComposite);
        check(p.undoLayerEdit(), "Cannot undo layer deletion");
        compareComposite(duplicateComposite);
        for (const auto& [id, snapshot] : duplicatePixels)
            check(sameCanvas(*snapshot, p.entries.at(p.canvasIndex(id)).canvas->layerSnapshot()),
                  "Undoing deletion did not restore every layer's pixels and metadata");
        check(p.undoLayerEdit(true), "Cannot redo layer deletion");
        compareComposite(upperComposite);
        check(p.undoLayerEdit(), "Cannot recover duplicate for project persistence test");
        verifyGpu();
        saveImages("duplicate-layer");
        result["duplicate_independence_delete_and_undo"] = true;

        // Retain hidden paint, non-default opacity and a selected duplicate in the saved project.
        // Flattened-only persistence would pass image equality and fail the full stack comparisons.
        layout = p.layerInfos;
        layout.front().visible = false;
        layout[1].opacity = .55f;
        layout.back().opacity = .4f;
        layout.back().preserveAlpha = false;
        p.changeLayers(layout, duplicate);
        verifyGpu();
        const auto expectedImages = p.snapshot();
        const auto expectedLayers = p.layerSnapshot();
        publish("saving_and_reopening_layers");
        const auto saved = savePaintRecovery(*p.scene, expectedImages, directory / "project", {}, nullptr,
                                             p.livery, p.textureDirectory, expectedLayers);
        const auto manifest =
            fs::path(wide(saved.at("directory").get<std::string>())) / "project.edmpaint.json";
        auto reopened = loadPaintDocument(*p.scene, manifest);
        check(sameLayout(reopened.layers, p.layerInfos) && reopened.activeLayer == p.activeLayerId,
              "Reopened project lost global order, layer settings or active layer");
        check(reopened.layerCanvases.size() == expectedLayers.size() &&
                  reopened.images.size() == expectedImages.size(),
              "Reopened project lost a material or alias layer stack");
        for (const auto& [id, expected] : expectedLayers) {
            check(reopened.layerCanvases.contains(id) &&
                      sameCanvas(*expected, *reopened.layerCanvases.at(id)),
                  "Reopened project lost an original base or editable layer pixels");
            check(sameImage(*reopened.images.at(id), *expectedImages.at(id)),
                  "Reopened project changed its flattened composite");
            if (id != p.canvasIndex(id))
                continue;
            PaintCanvas restored(reopened.layerCanvases.at(id)->base);
            restored.restoreLayers(*reopened.layerCanvases.at(id));
            check(sameImage(restored.image(), *expectedImages.at(id)),
                  "Restoring a layered canvas changed the composited pixels");
            EditableGpu restoredGpu;
            syncCanvas(renderer, restored, restoredGpu, true);
            check(gpuMatchesCanvas(renderer, restored, restoredGpu),
                  "Restored project layers failed GPU readback");
        }
        saveImages("saved-composite");
        result["project"] = {{"manifest", pathString(manifest)},
                             {"layers", p.layerInfos.size()},
                             {"materials_including_aliases", expectedLayers.size()},
                             {"full_stack_pixels_match", true},
                             {"restored_gpu_pixels_match", true}};
        result["gpu_pixels_match"] = true;
        result["canvases"] = p.entries.size();
        result["passed"] = true;
        publish("complete");
        p.paintMaxDimension = 2048;
        p.status = "图层绘制、材质同步、撤销重做、GPU 与分层工程保存已验证";
        return true;
    } catch (...) {
        const auto error = exceptionText();
        try {
            result["error"] = error;
            p.layersDiagnosticResult = result;
            if (fs::is_directory(directory))
                writeJson(directory / "layers-report.json", result);
        } catch (...) {
        }
        throw;
    }
}
