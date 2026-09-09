// Real renderer readback: comparison is a display option, never an edit or a reload.
std::pair<int, size_t> previewPixelDifference(const PaintImage& a, const PaintImage& b) {
    require(a.width == b.width && a.height == b.height && a.rgba.size() == b.rgba.size(),
            "Comparison viewport dimensions changed");
    int maximum = 0;
    size_t changed = 0;
    for (size_t pixel = 0; pixel < a.rgba.size(); pixel += 4) {
        int delta = 0;
        for (size_t channel = 0; channel < 4; ++channel)
            delta = std::max(delta, std::abs(int(a.rgba[pixel + channel]) - int(b.rgba[pixel + channel])));
        maximum = std::max(maximum, delta);
        changed += delta > 1;
    }
    return {maximum, changed};
}
// Compare against per-channel ranges measured at this pixel in unchanged reference frames.
// Unchanged FA-18 frames have measured differences at a few edge pixels. Their underlying render
// cause is not established; this envelope uses independent observations, not a larger color tolerance.
std::pair<int, size_t> previewReferenceDifference(const PaintImage& actual,
                                                std::initializer_list<const PaintImage*> references,
                                                std::span<const PaintImage> additional = {}) {
    require(references.size() > 0, "Missing preview reference frames");
    for (const auto* reference : references)
        require(actual.width == reference->width && actual.height == reference->height,
                "Reference frame dimensions changed");
    for (const auto& reference : additional)
        require(actual.width == reference.width && actual.height == reference.height,
                "Additional reference frame dimensions changed");
    int maximum = 0;
    size_t changed = 0;
    for (size_t offset = 0; offset < actual.rgba.size(); offset += 4) {
        int delta = 0;
        for (size_t channel = 0; channel < 4; ++channel) {
            int low = 255, high = 0;
            for (const auto* reference : references) {
                low = std::min(low, int(reference->rgba[offset + channel]));
                high = std::max(high, int(reference->rgba[offset + channel]));
            }
            for (const auto& reference : additional) {
                low = std::min(low, int(reference.rgba[offset + channel]));
                high = std::max(high, int(reference.rgba[offset + channel]));
            }
            delta = std::max(delta, std::max(low - actual.rgba[offset + channel],
                                           actual.rgba[offset + channel] - high));
        }
        maximum = std::max(maximum, delta);
        changed += delta > 1;
    }
    return {maximum, changed};
}
Json PaintEditor::verifyLiveryPreview(Renderer& renderer, const fs::path& directory) {
    auto& p = *impl;
    require(!busy() && renderer.model, "Preview comparison needs an idle loaded model");
    fs::create_directories(directory);
    const auto pixels = snapshot();
    const auto saved = p.saved, undo = p.history.canUndo(), redo = p.history.canRedo();
    const auto overrides = renderer.diffuseOverrides;
    const auto textureImages = renderer.textures.images;
    const auto uploads = renderer.model->vertexUploads, evaluations = renderer.model->sceneEvaluations;
    const auto options = renderer.options;
    const auto preview = renderer.decalPreview;
    const auto dimensions = renderer.viewportMetrics();
    const int width = dimensions.at("width"), height = dimensions.at("height");
    auto capture = [&](const char* name) {
        renderer.render(width, height);
        const auto path = directory / name;
        renderer.capture(path, false);
        return PaintImage::load(ImageSource{path}, 8192);
    };
    auto captureAdditional = [&](const char* prefix) {
        std::vector<PaintImage> frames;
        for (int index = 0; index < 5; ++index)
            frames.push_back(capture((std::string(prefix) + "-extra-" + std::to_string(index) + ".png").c_str()));
        return frames;
    };
    auto restore = [&] {
        renderer.diffuseOverrides = overrides;
        renderer.options = options;
        renderer.decalPreview = preview;
    };
    try {
        renderer.options.editedLivery = true;
        renderer.options.textures = true;
        renderer.decalPreview.active = false;
        const auto edited = capture("edited.png");
        const auto editedRepeat = capture("edited-repeat.png");
        const auto editedFinal = capture("edited-reference-final.png");
        const auto editedAdditional = captureAdditional("edited-reference");
        renderer.options.editedLivery = false;
        renderer.decalPreview = preview; // Even a pending image-brush preview must be hidden.
        const auto original = capture("original.png");
        require(!viewport(renderer, {0, 0}, {float(width), float(height)}, true),
                "Original view consumed a paint gesture");
        renderer.diffuseOverrides.clear();
        renderer.decalPreview.active = false;
        renderer.options.editedLivery = true;
        const auto reference = capture("base-reference.png");
        renderer.capture(directory / "base-same-frame.png", false);
        const auto sameFrame = PaintImage::load(ImageSource{directory / "base-same-frame.png"}, 8192);
        require(reference.width == sameFrame.width && reference.height == sameFrame.height &&
                    reference.rgba == sameFrame.rgba,
                "Repeated readback of one unchanged rendered frame altered its pixels");
        const auto referenceRepeat = capture("base-reference-repeat.png");
        const auto referenceFinal = capture("base-reference-final.png");
        const auto referenceAdditional = captureAdditional("base-reference");
        const auto repeatedDifference = previewPixelDifference(reference, referenceRepeat);
        const auto originalDifference = previewReferenceDifference(original, {&reference, &referenceRepeat, &referenceFinal}, referenceAdditional);
        // Use a fixed eight-frame control set, independent of the frame being tested. Check
        // reference variability itself as well; canvas data always remains an exact comparison.
        const size_t noiseLimit = std::max(size_t(16), size_t(width) * height / 1000);
        auto boundedReferences = [&](const PaintImage& first, std::span<const PaintImage> frames) {
            return std::all_of(frames.begin(), frames.end(), [&](const PaintImage& frame) {
                return previewPixelDifference(first, frame).second <= noiseLimit;
            });
        };
        require(repeatedDifference.second <= noiseLimit &&
                    previewPixelDifference(reference, referenceFinal).second <= noiseLimit &&
                    boundedReferences(reference, referenceAdditional) &&
                    originalDifference.second <= 16,
                "Original view differs from the base livery without any edit or preview bindings");
        renderer.diffuseOverrides = overrides;
        const auto restored = capture("edited-restored.png");
        const auto restoredDifference = previewReferenceDifference(restored, {&edited, &editedRepeat, &editedFinal}, editedAdditional);
        require(previewPixelDifference(edited, editedRepeat).second <= noiseLimit &&
                    previewPixelDifference(edited, editedFinal).second <= noiseLimit &&
                    boundedReferences(edited, editedAdditional) &&
                    restoredDifference.second <= 16,
                "Re-enabling edited livery did not restore its rendered pixels");
        const auto changed = previewPixelDifference(edited, original).second;
        if (!overrides.empty())
            require(changed > 32, "Livery comparison did not reveal the painted surface");
        auto after = snapshot();
        require(after.size() == pixels.size(), "Comparison changed the number of paint snapshots");
        for (const auto& [id, expected] : pixels)
            require(after.at(id)->rgba == expected->rgba && after.at(id)->width == expected->width &&
                        after.at(id)->height == expected->height,
                    "Comparison changed saved/exported paint pixels");
        require(p.saved == saved && p.history.canUndo() == undo && p.history.canRedo() == redo,
                "Comparison changed dirty state or undo history");
        require(renderer.textures.images == textureImages && renderer.diffuseOverrides == overrides &&
                    renderer.model->vertexUploads == uploads &&
                    renderer.model->sceneEvaluations == evaluations,
                "Comparison reloaded textures or rebuilt model geometry");
        restore();
        Json report{{"passed", true},
                    {"changed_viewport_pixels", changed},
                    {"original_matches_base", true},
                    {"maximum_original_channel_error", originalDifference.first},
                    {"maximum_restored_channel_error", restoredDifference.first},
                    {"original_outlier_pixels", originalDifference.second},
                    {"restored_outlier_pixels", restoredDifference.second},
                    {"unchanged_render_outlier_pixels", repeatedDifference.second},
                    {"unchanged_render_maximum_channel_error", repeatedDifference.first},
                    {"maximum_outlier_pixels", 16},
                    {"comparison", "outside unchanged-frame per-pixel color envelope"},
                    {"reference_frames_per_state", 8},
                    {"same_frame_readback_exact", true},
                    {"maximum_unchanged_noise_pixels", noiseLimit},
                    {"render_channel_tolerance", 1},
                    {"snapshot_and_history_unchanged", true},
                    {"texture_reloads", 0},
                    {"geometry_uploads", 0},
                    {"scene_evaluations", 0}};
        writeJson(directory / "report.json", report);
        return report;
    } catch (...) {
        restore();
        throw;
    }
}
