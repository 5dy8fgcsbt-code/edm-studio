// Explicit smoke mode drives actual ImGui pointer events: point press/release, then button
// press/release. It cannot detach by calling detachModel directly.
bool exerciseDetachmentInteraction() {
    if (detachmentTestStage == 8)
        return true;
    if (attaching || paint.busy() || !texturesReady())
        return false;
    auto& scene = *renderer.model->scene;
    auto pointer = [&](ImVec2 point, bool down) {
        diagnosticMousePosition = point;
        diagnosticMouseDown = down;
    };
    if (detachmentTestStage == 0) {
        detachmentSceneBefore = renderer.model->scene;
        detachmentPaintBefore = paint.snapshot();
        detachmentArgsBefore = args;
        renderer.camera.fit(scene, args);
        detachmentTestStage = 1;
        return false;
    }
    if (detachmentTestStage == 1) {
        int target = findConnector(scene, detachmentTest);
        auto marker = std::find_if(connectorMarkers.begin(), connectorMarkers.end(),
                                   [target](const auto& m) { return m.at("node") == target; });
        require(marker != connectorMarkers.end() && marker->at("occupied").get<bool>(),
                "Detachment diagnostic needs a visible occupied connector");
        pointer({marker->at("x").get<float>(), marker->at("y").get<float>()}, false);
        detachmentTestStage = 2;
        return false;
    }
    if (detachmentTestStage == 2) {
        require(hoveredConnector == findConnector(scene, detachmentTest),
                "Diagnostic cursor did not select the occupied connector");
        pointer(ImGui::GetIO().MousePos, true);
        detachmentTestStage = 3;
        return false;
    }
    if (detachmentTestStage == 3) {
        require(connectorPopupVisible && selectedConnector == findConnector(scene, detachmentTest),
                "Connector click did not open the unload popup");
        require(renderer.model->scene == detachmentSceneBefore && !attaching,
                "Connector click unloaded the object without pressing its button");
        detachmentTestReport["point_click_only_opens_popup"] = true;
        pointer(unloadButtonPosition, false);
        detachmentTestStage = 4;
        return false;
    }
    if (detachmentTestStage == 4) {
        require(connectorPopupVisible && renderer.model->scene == detachmentSceneBefore && !attaching,
                "Releasing the connector click changed the scene");
        if (!capturePath.empty()) {
            auto popup = capturePath.parent_path() / (capturePath.stem().wstring() + L"-popup.png");
            renderer.capture(popup);
            detachmentTestReport["popup_capture"] = pathString(popup);
        }
        pointer(unloadButtonPosition, true);
        detachmentTestStage = 5;
        return false;
    }
    if (detachmentTestStage == 5) {
        require(renderer.model->scene == detachmentSceneBefore && !attaching,
                "The unload button activated before the release");
        pointer(unloadButtonPosition, false);
        detachmentTestStage = 6;
        return false;
    }
    if (detachmentTestStage == 6) {
        require(lastDetachment.is_object() && renderer.model->scene != detachmentSceneBefore,
                "Pressing the unload button did not publish a detached scene");
        require(scene.attachments.size() + lastDetachment.at("removed_count").get<size_t>() ==
                    detachmentSceneBefore->attachments.size(),
                "Wrong attachment removal count");
        const auto materialMap = lastDetachment.at("material_map").get<std::vector<int>>();
        const auto nodeMap = lastDetachment.at("node_map").get<std::vector<int>>();
        auto after = paint.snapshot();
        size_t retained = 0, removed = 0, shifted = 0;
        for (const auto& [old, image] : detachmentPaintBefore) {
            int next = materialMap.at(old);
            if (next < 0) {
                ++removed;
                continue;
            }
            ++retained;
            shifted += next != old;
            require(after.contains(next) && after.at(next)->width == image->width &&
                        after.at(next)->height == image->height && after.at(next)->rgba == image->rgba,
                    "Detachment changed a surviving paint canvas");
            require(renderer.diffuseOverrides.contains(next), "Surviving canvas lost its GPU binding");
        }
        require(after.size() == retained, "A removed canvas remains in the paint snapshot");
        const auto removedArguments = lastDetachment.at("removed_arguments").get<std::set<int>>();
        for (auto [argument, value] : detachmentArgsBefore)
            require(removedArguments.contains(argument) ? !args.contains(argument)
                                                        : args.at(argument) == value,
                    "Detachment did not preserve remaining animation arguments");
        const auto worldBefore = detachmentSceneBefore->evaluate(detachmentArgsBefore);
        const auto worldAfter = scene.evaluate(args);
        double error = 0;
        for (int old = 0; old < int(nodeMap.size()); ++old)
            if (nodeMap[old] >= 0)
                error = std::max(error, (worldBefore[old] - worldAfter[nodeMap[old]]).cwiseAbs().maxCoeff());
        require(error < 1e-8, "Detachment moved a surviving scene node");
        std::set<std::string> keys;
        for (const auto& slots : renderer.textures.material)
            keys.insert(slots.begin(), slots.end());
        for (const auto& [key, texture] : renderer.textures.images)
            require(keys.contains(key), "Removed object retains an unused GPU texture");
        require(renderer.textures.material.size() == scene.materials.size(),
                "GPU material slots not compacted");
        if (removed) {
            require(lastDetachment.at("recovery").is_object(), "Removed paint has no recovery copy");
            auto folder = fs::path(wide(lastDetachment.at("recovery").at("directory").get<std::string>()));
            auto recovered = loadPaintProject(*detachmentSceneBefore, folder / "project.edmpaint.json");
            for (const auto& [material, image] : detachmentPaintBefore)
                require(recovered.contains(material) && recovered.at(material)->rgba == image->rgba,
                        "Recovery changed removed paint pixels");
            detachmentTestReport["removed_paint_recovery_pixels_match"] = true;
        }
        detachmentTestReport.update({{"button_unloaded", true},
                                     {"removed_count", lastDetachment.at("removed_count")},
                                     {"preserved_canvases", retained},
                                     {"shifted_canvases", shifted},
                                     {"removed_canvases", removed},
                                     {"max_survivor_transform_error", error},
                                     {"unused_gpu_textures", 0},
                                     {"recovery", lastDetachment.at("recovery")}});
        detachmentTestStage = 7;
        return false;
    }
    auto state = paint.diagnostic();
    if (paint.active() && state.value("surface_triangles", size_t(0)) == 0)
        return false;
    PaintSurface reference(renderer.model->scene, args);
    if (paint.active())
        require(state.at("surface_triangles").get<size_t>() == reference.triangleCount(),
                "Removed geometry stayed in the paint surface");
    detachmentTestReport["remaining_surface_triangles"] = reference.triangleCount();
    detachmentTestReport["passed"] = true;
    detachmentSceneBefore.reset();
    detachmentPaintBefore.clear();
    diagnosticMousePosition.reset();
    detachmentTestStage = 8;
    return true;
}
