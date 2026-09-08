// Included after PaintEditor::Impl, inside namespace edm. All placement and bake
// actions use the same entry points as the visible image-brush controls.
bool PaintEditor::exerciseDecal(Renderer& renderer, const fs::path& image, const fs::path& directory,
                                int maximumDimension) {
    auto& p = *impl;
    auto publish = [&](const char* phase) {
        p.decalDiagnosticResult["phase"] = phase;
        writeJson(directory / "decal-report.json", p.decalDiagnosticResult);
    };
    auto viewportSize = [&] {
        auto dimensions = renderer.viewportMetrics();
        return ImVec2(dimensions.at("width").get<float>(), dimensions.at("height").get<float>());
    };
    auto compareImages = [&](const PaintSnapshot& expected) {
        for (const auto& [id, image] : expected) {
            auto found = p.entries.find(p.canvasIndex(id));
            require(found != p.entries.end(), "Surface decal lost an edited canvas");
            const auto& actual = found->second.canvas->image();
            require(actual.width == image->width && actual.height == image->height &&
                        actual.rgba == image->rgba,
                    "Surface decal texture pixels changed unexpectedly");
        }
    };
    auto verifyGpu = [&] {
        for (auto& [index, entry] : p.entries) {
            syncCanvas(renderer, *entry.canvas, entry.gpu, true);
            renderer.diffuseOverrides[index] = entry.gpu.view;
            require(gpuMatchesCanvas(renderer, *entry.canvas, entry.gpu),
                    "Baked surface decal GPU readback differs from its canvas");
        }
        for (int id = 0; id < int(p.canonicalIds.size()); ++id)
            if (auto found = p.entries.find(p.canvasIndex(id)); found != p.entries.end())
                renderer.diffuseOverrides[id] = found->second.gpu.view;
        return p.entries.size();
    };
    auto saveImages = [&](const PaintSnapshot& images, const char* name) {
        fs::create_directories(directory / name);
        std::set<int> written;
        for (const auto& [id, pixels] : images) {
            const auto index = p.canvasIndex(id);
            if (written.insert(index).second)
                pixels->savePNG(directory / name / ("material_" + std::to_string(index) + ".png"));
        }
    };
    auto changedCanvases = [&](const Json& report) {
        require(report.is_object() && report.at("automatic").get<bool>() &&
                    report.at("surface_decal").get<bool>() && report.at("changed").get<bool>() &&
                    report.at("undo_redo_match").get<bool>(),
                "Surface decal bake or its unified undo/redo did not complete");
        std::set<int> changed;
        for (const auto& id : report.at("materials"))
            changed.insert(p.canvasIndex(id.get<int>()));
        require(!changed.empty() && report.at("painted_pixels").get<uint64_t>() > 0,
                "Surface decal did not change any texture pixels");
        return changed;
    };
    auto frameJson = [](const SurfaceDecalOptions& options) {
        auto frame = surfaceDecalFrame(options);
        auto vector = [](const V3& value) { return Json::array({value.x(), value.y(), value.z()}); };
        return Json{{"center", vector(frame.center)},
                    {"right", vector(frame.right)},
                    {"up", vector(frame.up)},
                    {"normal", vector(frame.normal)},
                    {"width", frame.width},
                    {"height", frame.height},
                    {"depth", frame.depth}};
    };
    auto verifyFixedPlacement = [&] {
        auto expected = surfaceDecalFrame(p.decalDiagnosticPlacement);
        auto actual = surfaceDecalFrame(p.decalOptions());
        require(actual.center.isApprox(expected.center, 1e-12) &&
                    actual.right.isApprox(expected.right, 1e-12) && actual.up.isApprox(expected.up, 1e-12) &&
                    actual.normal.isApprox(expected.normal, 1e-12) && actual.width == expected.width &&
                    actual.height == expected.height && actual.depth == expected.depth,
                "Camera rotation moved the surface decal's model-space placement");
        require(renderer.decalPreview.active &&
                    renderer.decalPreview.center.isApprox(expected.center, 1e-12) &&
                    renderer.decalPreview.right.isApprox(expected.right, 1e-12) &&
                    renderer.decalPreview.up.isApprox(expected.up, 1e-12) &&
                    renderer.decalPreview.normal.isApprox(expected.normal, 1e-12),
                "Surface decal preview frame drifted from the CPU bake frame");
    };
    auto rejectOutside = [&](ImVec2 size) {
        auto before = snapshot();
        const auto count = p.entries.size();
        const auto saved = p.saved;
        const auto canUndo = p.history.canUndo(), canRedo = p.history.canRedo();
        for (const auto point : std::array<PaintStrokePoint, 4>{{{-1, size.y * .5},
                                                                 {size.x + 1., size.y * .5},
                                                                 {size.x * .5, -1},
                                                                 {size.x * .5, size.y + 1.}}})
            require(!p.positionDecal(renderer, {0, 0}, size, point),
                    "A pointer outside the viewport unexpectedly positioned a decal");
        compareImages(before);
        require(p.entries.size() == count && p.saved == saved && !p.busy() &&
                    p.history.canUndo() == canUndo && p.history.canRedo() == canRedo && !p.history.active() &&
                    !renderer.decalPreview.active,
                "Out-of-bounds decal placement changed the document or left a preview active");
    };
    try {
        require(p.error.empty(), p.error);
        if (p.decalDiagnosticStage == 10)
            return true;
        if (p.busy())
            return false;
        if (p.decalDiagnosticStage == 0) {
            require(p.scene && renderer.model, "Surface decal diagnostic requires a loaded model");
            require(p.entries.empty() && !p.strokePending() && !p.history.active(),
                    "Surface decal diagnostic must start without prepared canvases");
            require(fs::is_regular_file(image), "Surface decal source image does not exist");
            require(maximumDimension == 512 || maximumDimension == 1024 || maximumDimension == 2048,
                    "Surface decal diagnostic supports 512, 1024 or 2048 textures");
            require(!fs::exists(directory / "project"), "Use a fresh surface decal diagnostic directory");
            fs::create_directories(directory);
            p.recoveryRoot = directory / "recovery";
            p.paintMaxDimension = maximumDimension;
            p.ensureAliases();
            p.tool = 2;
            p.decalSuppressed = true;
            p.decalDiagnosticCamera = renderer.camera;
            p.decalDiagnosticResult = {{"passed", false},
                                       {"model", pathString(p.scene->source)},
                                       {"source_image", pathString(image)},
                                       {"initial_canvases", p.entries.size()},
                                       {"maximum_new_texture_dimension", maximumDimension},
                                       {"manual_material_selection", false}};
            publish("loading_image_and_surface");
            p.decalDiagnosticStage = 1;
            p.loadMappingImage(renderer, image, 2);
            return false;
        }
        if (p.decalDiagnosticStage == 1) {
            require(bool(p.mappingImage) && bool(p.mappingPreview), "Surface decal image was not loaded");
            if (!p.surface || p.surfaceArgs != p.currentArgs)
                return false;
            const auto size = viewportSize();
            auto context = p.frameContext(renderer, {0, 0}, size);
            std::map<int, std::vector<PaintStrokePoint>> visible;
            for (int y = 0; y < 29; ++y)
                for (int x = 0; x < 41; ++x) {
                    PaintStrokePoint point{(x + .5) * size.x / 41., (y + .5) * size.y / 29.};
                    auto hit = Impl::strokeHit(context, point);
                    if (!hit || !p.paintable(hit->material) || !p.surface->triangle(hit->primitive).hasUV)
                        continue;
                    const auto& material = p.scene->materials.at(hit->material);
                    if (material.blending >= 1 && material.blending <= 4)
                        continue;
                    visible[p.canvasIndex(hit->material)].push_back(point);
                }
            require(visible.size() >= 2,
                    "Surface decal diagnostic needs two visible opaque textured surfaces");
            std::vector<int> ranked;
            Json coverage = Json::array();
            for (const auto& [index, points] : visible) {
                ranked.push_back(index);
                coverage.push_back({{"canvas", index},
                                    {"name", p.scene->materials.at(index).name},
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
            p.decalDiagnosticFirstPoint = representative(ranked[0]);
            p.decalDiagnosticSecondPoint = representative(ranked[1]);
            p.decalDiagnosticResult["viewport_coverage"] = coverage;
            p.decalDiagnosticResult["target_canvases"] = {ranked[0], ranked[1]};
            rejectOutside(size);
            p.decalDiagnosticResult["out_of_bounds_empty_document"] = true;
            renderer.capture(directory / "00-before-preview.png", false);
            p.decalSuppressed = false;
            p.decalOverlay = true;
            p.decalWidth = float(std::max(.02, renderer.camera.radius * .12));
            p.decalHeight = p.decalWidth;
            p.decalDepth = p.decalWidth * .2f;
            p.decalAngle = 0;
            p.decalOpacity = .85f;
            p.decalAspectLocked = true;
            p.decalFront = true;
            p.decalOcclusion = true;
            require(p.positionDecal(renderer, {0, 0}, size, p.decalDiagnosticFirstPoint),
                    "Surface decal could not position on its first visible surface");
            p.decalDiagnosticPlacement = p.decalOptions();
            p.updateDecalPreview(renderer);
            require(snapshot().empty() && p.entries.empty() && !hasEdits(),
                    "Surface decal preview allocated or modified a paint canvas");
            verifyFixedPlacement();
            p.decalDiagnosticResult["placement"] = frameJson(p.decalDiagnosticPlacement);
            publish("displaying_surface_preview");
            p.decalDiagnosticStage = 2;
            return false;
        }
        if (p.decalDiagnosticStage == 2) {
            verifyFixedPlacement();
            renderer.capture(directory / "01-preview.png", false);
            renderer.capture(directory / "01-preview-window.png");
            auto before = PaintImage::load(ImageSource{directory / "00-before-preview.png"}, 8192);
            auto preview = PaintImage::load(ImageSource{directory / "01-preview.png"}, 8192);
            require(before.width == preview.width && before.height == preview.height,
                    "Viewport dimensions changed during the decal preview diagnostic");
            size_t different = 0;
            for (size_t pixel = 0; pixel < before.rgba.size(); pixel += 4)
                different += std::memcmp(before.rgba.data() + pixel, preview.rgba.data() + pixel, 4) != 0;
            require(different > 4, "Surface decal GPU preview did not change the rendered viewport");
            p.decalDiagnosticResult["preview"] = {{"rendered_pixels_changed", different},
                                                  {"canvases_created", p.entries.size()},
                                                  {"snapshot_unchanged", snapshot().empty()}};
            p.decalDiagnosticResult["floating_preview_hidden"] =
                verifyLiveryPreview(renderer, directory / "comparison-floating-preview");
            require(p.entries.empty() && snapshot().empty(), "Surface preview prepared a hidden canvas");
            p.decalDiagnosticDepthUpdates = renderer.decalDepthUpdates;
            p.decalDiagnosticUploads = renderer.model->vertexUploads;
            p.decalDiagnosticEvaluations = renderer.model->sceneEvaluations;
            renderer.camera.yaw += .42;
            renderer.camera.pitch = std::clamp(renderer.camera.pitch + .12, -.8, 1.2);
            p.updateDecalPreview(renderer);
            verifyFixedPlacement();
            publish("orbiting_fixed_surface_preview");
            p.decalDiagnosticStage = 3;
            return false;
        }
        if (p.decalDiagnosticStage == 3) {
            verifyFixedPlacement();
            require(renderer.decalDepthUpdates == p.decalDiagnosticDepthUpdates &&
                        renderer.model->vertexUploads == p.decalDiagnosticUploads &&
                        renderer.model->sceneEvaluations == p.decalDiagnosticEvaluations,
                    "Orbiting a fixed decal preview rebuilt geometry or its projector depth map");
            require(p.entries.empty() && snapshot().empty(), "Orbiting a preview modified paint pixels");
            renderer.capture(directory / "02-preview-orbit.png", false);
            renderer.capture(directory / "02-preview-orbit-window.png");
            p.decalDiagnosticResult["preview"]["camera_orbit_frame_unchanged"] = true;
            p.decalDiagnosticResult["preview"]["cpu_gpu_frame_match"] = true;
            p.decalDiagnosticResult["preview"]["camera_geometry_uploads"] = 0;
            p.decalDiagnosticResult["preview"]["camera_scene_evaluations"] = 0;
            p.decalDiagnosticResult["preview"]["camera_depth_rebuilds"] = 0;
            renderer.camera = p.decalDiagnosticCamera;
            p.updateDecalPreview(renderer);
            p.decalDiagnosticStage = 4;
            return false;
        }
        if (p.decalDiagnosticStage == 4) {
            verifyFixedPlacement();
            publish("baking_first_surface_decal");
            p.decalDiagnosticStage = 5;
            p.startDecal(renderer, true);
            return false;
        }
        if (p.decalDiagnosticStage == 5) {
            const auto changed = changedCanvases(p.decalResult);
            require(changed.contains(p.decalDiagnosticResult.at("target_canvases").at(0).get<int>()),
                    "First decal did not change the texture under its pointer");
            p.decalDiagnosticFirst = snapshot();
            require(p.history.undo(), "First surface decal could not be undone");
            p.decalDiagnosticBefore = snapshot();
            size_t different = 0;
            for (int index : changed)
                different +=
                    p.decalDiagnosticBefore.at(index)->rgba != p.decalDiagnosticFirst.at(index)->rgba;
            require(different > 0, "First decal undo did not restore original pixels");
            require(p.history.redo(), "First surface decal could not be redone");
            compareImages(p.decalDiagnosticFirst);
            p.decalSuppressed = true;
            p.updateDecalPreview(renderer);
            require(!renderer.decalPreview.active, "Decal preview remained active after baking");
            p.decalDiagnosticResult["first_decal"] = p.decalResult;
            p.decalDiagnosticResult["first_decal"]["gpu_canvases_checked"] = verifyGpu();
            p.decalDiagnosticResult["first_decal"]["gpu_pixels_match_without_preview"] = true;
            p.decalDiagnosticResult["first_decal"]["original_canvases_restored"] = different;
            saveImages(p.decalDiagnosticBefore, "original-textures");
            saveImages(p.decalDiagnosticFirst, "after-first-decal");
            const auto size = viewportSize();
            rejectOutside(size);
            p.decalDiagnosticResult["out_of_bounds_existing_canvases"] = true;
            p.decalSuppressed = false;
            p.decalAngle = 17;
            require(p.positionDecal(renderer, {0, 0}, size, p.decalDiagnosticSecondPoint),
                    "Surface decal could not position on its second visible surface");
            compareImages(p.decalDiagnosticFirst);
            p.decalDiagnosticResult["second_placement"] = frameJson(p.decalOptions());
            publish("baking_second_surface_decal");
            p.decalDiagnosticStage = 6;
            p.startDecal(renderer, true);
            return false;
        }
        if (p.decalDiagnosticStage == 6) {
            const auto changed = changedCanvases(p.decalResult);
            require(changed.contains(p.decalDiagnosticResult.at("target_canvases").at(1).get<int>()),
                    "Second decal did not change the texture under its pointer");
            p.decalDiagnosticAfter = snapshot();
            require(p.history.undo(), "Second surface decal could not be undone");
            compareImages(p.decalDiagnosticFirst);
            auto secondBaseline = snapshot();
            size_t different = 0;
            for (int index : changed)
                different += secondBaseline.at(index)->rgba != p.decalDiagnosticAfter.at(index)->rgba;
            require(different > 0, "Second decal undo did not restore its preceding pixels");
            require(p.history.undo(), "Decal history did not retain the first stamp");
            compareImages(p.decalDiagnosticBefore);
            for (const auto& [id, expected] : secondBaseline)
                if (!p.decalDiagnosticBefore.contains(id)) {
                    const auto& actual = p.entries.at(p.canvasIndex(id)).canvas->image();
                    require(actual.width == expected->width && actual.height == expected->height &&
                                actual.rgba == expected->rgba,
                            "Undoing the first decal changed a texture touched only by the second decal");
                }
            require(!p.history.canUndo(), "Two decals unexpectedly created additional history operations");
            require(p.history.redo(), "First surface decal redo order was lost");
            compareImages(secondBaseline);
            require(p.history.redo(), "Second surface decal redo order was lost");
            compareImages(p.decalDiagnosticAfter);
            require(!p.history.canRedo(), "Decal history retained an unexpected redo branch");
            p.decalSuppressed = true;
            p.updateDecalPreview(renderer);
            require(!renderer.decalPreview.active, "Baked decal validation still has a preview overlay");
            p.decalDiagnosticResult["second_decal"] = p.decalResult;
            p.decalDiagnosticResult["second_decal"]["gpu_canvases_checked"] = verifyGpu();
            p.decalDiagnosticResult["second_decal"]["gpu_pixels_match_without_preview"] = true;
            p.decalDiagnosticResult["two_stamp_history_order_match"] = true;
            saveImages(p.decalDiagnosticAfter, "after-two-decals");
            p.decalDiagnosticResult["live_livery_comparison"] =
                verifyLiveryPreview(renderer, directory / "comparison-live");
            publish("saving_full_decal_project");
            p.decalDiagnosticStage = 7;
            p.saveProject(directory / "project");
            return false;
        }
        if (p.decalDiagnosticStage == 7) {
            require(!p.lastSavedProject.empty() && fs::is_regular_file(p.lastSavedProject),
                    "Surface decal project did not publish a complete manifest");
            p.decalDiagnosticResult["project"] = {{"manifest", pathString(p.lastSavedProject)}};
            publish("reopening_full_decal_project");
            p.decalDiagnosticStage = 8;
            p.openProject(p.lastSavedProject);
            return false;
        }
        if (p.decalDiagnosticStage == 8) {
            require(p.lastOpenRestoredAppearance && !hasEdits(),
                    "Reopened surface decal project did not restore its saved appearance");
            compareImages(p.decalDiagnosticAfter);
            p.decalDiagnosticResult["project"]["gpu_canvases_checked"] = verifyGpu();
            p.decalDiagnosticResult["project"]["pixels_match"] = true;
            p.decalDiagnosticResult["project"]["gpu_pixels_match"] = true;
            p.decalDiagnosticResult["project"]["appearance_restored"] = true;
            p.tool = 2;
            p.decalSuppressed = true;
            p.updateDecalPreview(renderer);
            require(!renderer.decalPreview.active, "Reopened project retained a floating decal preview");
            p.decalDiagnosticBefore.clear();
            p.decalDiagnosticFirst.clear();
            p.decalDiagnosticAfter.clear();
            p.paintMaxDimension = 2048;
            publish("rendering_reopened_baked_decals");
            p.decalDiagnosticStage = 9;
            return false;
        }
        require(p.decalDiagnosticStage == 9 && !renderer.decalPreview.active,
                "Surface decal diagnostic ended in an unexpected state");
        p.decalDiagnosticResult["reopened_livery_comparison"] =
            verifyLiveryPreview(renderer, directory / "comparison-reopened");
        auto originalLive = PaintImage::load(ImageSource{directory / "comparison-live/original.png"}, 8192);
        auto originalReopened =
            PaintImage::load(ImageSource{directory / "comparison-reopened/original.png"}, 8192);
        const auto originalDifference = previewPixelDifference(originalLive, originalReopened);
        require(originalDifference.second <= 16,
                "Reopened project did not retain the same original livery appearance");
        p.decalDiagnosticResult["original_livery_survives_reopen"] = true;
        p.decalDiagnosticResult["reopened_original_maximum_channel_error"] = originalDifference.first;
        p.decalDiagnosticResult["reopened_original_outlier_pixels"] = originalDifference.second;
        renderer.capture(directory / "03-baked-reopened.png", false);
        renderer.capture(directory / "03-baked-reopened-window.png");
        p.decalDiagnosticResult["final_canvases"] = p.entries.size();
        p.decalDiagnosticResult["passed"] = true;
        publish("complete");
        p.status = "表面贴花预览、视角固定、连续盖印、撤销重做、GPU 与工程重开已验证";
        p.decalDiagnosticStage = 10;
        return true;
    } catch (...) {
        auto error = exceptionText();
        try {
            p.decalDiagnosticResult["passed"] = false;
            p.decalDiagnosticResult["error"] = error;
            if (fs::is_directory(directory))
                writeJson(directory / "decal-report.json", p.decalDiagnosticResult);
        } catch (...) {
        }
        throw;
    }
}
