// Runs the production asynchronous attach callback after a real brush edit, then toggles the same
// visibility state used by the checkboxes. Invoked only by the explicit --attachment-test smoke mode.
bool exerciseAttachmentInteraction() {
    if (attachmentTestStage == 4)
        return true;
    if (attaching || paint.busy() || !texturesReady())
        return false;
    auto state = paint.diagnostic();
    require(state.value("error", "").empty(), state.value("error", ""));
    auto samePaint = [&] {
        auto now = paint.snapshot();
        for (const auto& [material, image] : attachmentPaintBefore) {
            require(now.contains(material), "Attaching discarded a painted material");
            const auto& current = *now.at(material);
            require(current.width == image->width && current.height == image->height &&
                        current.rgba == image->rgba,
                    "Attaching or hiding changed an existing paint canvas");
        }
    };
    if (attachmentTestStage == 0) {
        require(!paintMaterial.empty(),
                "--attachment-test requires --paint-material for canvas preservation checks");
        attachmentPaintBefore = paint.snapshot();
        require(!attachmentPaintBefore.empty(), "Attachment test requires an actual painted canvas");
        attachmentCountBefore = renderer.model->scene->attachments.size();
        attachModel(findConnector(*renderer.model->scene, attachmentTest->first), attachmentTest->second);
        attachmentTestStage = 1;
        return false;
    }
    const auto triangles = state.value("surface_triangles", size_t(0));
    if (!triangles)
        return false;
    if (attachmentTestStage == 1) {
        require(renderer.model->scene->attachments.size() == attachmentCountBefore + 1,
                "Asynchronous attachment did not publish the combined model");
        samePaint();
        attachmentSurfaceTriangles = triangles;
        attachmentTestReport = {{"preserved_canvases", attachmentPaintBefore.size()},
                                {"visible_surface_triangles", triangles}};
        renderer.options.attachments = false;
        paint.invalidateSurface(renderer);
        renderer.model->update(renderer.context.Get(), args, false);
        attachmentTestStage = 2;
        return false;
    }
    if (attachmentTestStage == 2) {
        require(!state.value("surface_attachments_visible", true), "Paint surface kept hidden attachments");
        require(triangles < attachmentSurfaceTriangles, "Hidden attachments stayed in the paint BVH");
        for (const auto& draw : renderer.model->draws)
            if (renderer.model->scene->meshes[draw.index].extras.contains("edm_attachment"))
                require(!draw.visible, "Hidden attachment still has a render draw");
        samePaint();
        attachmentTestReport["hidden_surface_triangles"] = triangles;
        attachmentTestReport["hidden_gpu"] = renderer.verifyGpu(args);
        renderer.options.attachments = true;
        paint.invalidateSurface(renderer);
        renderer.model->update(renderer.context.Get(), args, true);
        attachmentTestStage = 3;
        return false;
    }
    require(state.value("surface_attachments_visible", false) && triangles == attachmentSurfaceTriangles,
            "Showing attachments did not restore the paint surface");
    samePaint();
    attachmentTestReport["passed"] = true;
    attachmentTestStage = 4;
    attachmentPaintBefore.clear();
    return true;
}
