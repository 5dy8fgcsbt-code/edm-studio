// Included inside PaintEditor::Impl. Placement is shared by viewport input and GUI diagnostics.
bool positionDecal(Renderer& renderer, ImVec2 origin, ImVec2 size, PaintStrokePoint point) {
    if (controlsBusy() || !surface || surfaceArgs != currentArgs || size.x <= 1 || size.y <= 1) {
        decalHit.reset();
        renderer.decalPreview.active = false;
        return false;
    }
    auto context = frameContext(renderer, origin, size);
    auto hit = strokeHit(context, point);
    if (!hit || !surface->triangle(hit->primitive).hasUV || !hit->normal.allFinite() ||
        hit->normal.squaredNorm() < 1e-16) {
        decalHit.reset();
        renderer.decalPreview.active = false;
        return false;
    }
    hit->normal.normalize();
    if (hit->normal.dot(context.eye - hit->position) < 0)
        hit->normal = -hit->normal;
    decalHit = *hit;
    decalPose = currentArgs;
    decalEye = context.eye;
    decalViewProjection = renderer.camera.projection(size.x / size.y) * renderer.camera.view();
    // Keep image-up related to the visible camera while hovering. Once positioned, camera motion
    // alone never changes this frame or the placement captured by an in-flight bake.
    const Mat view = renderer.camera.view();
    decalTangent = view.block<1, 3>(0, 0).transpose();
    decalTangent -= hit->normal * decalTangent.dot(hit->normal);
    if (decalTangent.squaredNorm() < 1e-12) {
        decalTangent = view.block<1, 3>(1, 0).transpose();
        decalTangent -= hit->normal * decalTangent.dot(hit->normal);
    }
    if (decalTangent.squaredNorm() < 1e-12)
        decalTangent = hit->normal.unitOrthogonal();
    decalScreen = {float(point.x), float(point.y)};
    material = hit->material;
    updateDecalPreview(renderer);
    return true;
}
SurfaceDecalOptions decalOptions() const {
    require(decalHit && surface && surfaceArgs == currentArgs && decalPose == currentArgs,
            "请将图片画笔移到当前模型表面，确定盖印位置。");
    SurfaceDecalOptions options;
    options.center = decalHit->position;
    options.normal = decalHit->normal;
    options.tangent = decalTangent;
    options.width = decalWidth;
    options.height = decalAspectLocked && mappingImage
                         ? double(decalWidth) * mappingImage->height / mappingImage->width
                         : decalHeight;
    options.depth = decalDepth;
    options.rotationRadians = decalAngle * 3.14159265358979323846 / 180.;
    options.opacity = decalOpacity;
    options.preserveAlpha = preserveAlpha;
    options.frontFacesOnly = decalFront;
    options.occlusion = decalOcclusion;
    if (decalConforming) {
        options.projectionEye = decalEye;
        const ConformPlacement placement{surface, *decalHit, options, decalMaxBend};
        if (conformReadyPatch && conformReadyPlacement &&
            sameConformPlacement(placement, *conformReadyPlacement))
            options.conformPatch = conformReadyPatch;
    }
    return options;
}
static bool sameConformPlacement(const ConformPlacement& a, const ConformPlacement& b) {
    if (a.surface != b.surface || a.hit.primitive != b.hit.primitive || a.bend != b.bend)
        return false;
    const auto x = surfaceDecalFrame(a.options), y = surfaceDecalFrame(b.options);
    return x.center == y.center && x.right == y.right && x.up == y.up && x.normal == y.normal &&
           x.width == y.width && x.height == y.height && x.depth == y.depth &&
           a.options.projectionEye == b.options.projectionEye;
}
void prepareConformPreview(const SurfaceDecalOptions& options) {
    ConformPlacement wanted{surface, *decalHit, options, decalMaxBend};
    wanted.options.conformPatch.reset();
    if (conformJob && conformJob->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto completed = std::move(conformJob);
        const bool current = sameConformPlacement(wanted, completed->placement);
        try {
            auto patch = completed->future.get();
            if (current && !*completed->cancel) {
                conformReadyPlacement = std::move(completed->placement);
                conformReadyPatch = std::move(patch);
                conformPreviewError.clear();
            }
        } catch (...) {
            if (current && !*completed->cancel) {
                conformReadyPlacement = std::move(completed->placement);
                conformReadyPatch.reset();
                conformPreviewError = exceptionText();
            }
        }
    }
    // Retain successful AND failed placements; retry only when the user changes the placement.
    if (conformReadyPlacement && sameConformPlacement(wanted, *conformReadyPlacement)) {
        if (conformJob && !sameConformPlacement(wanted, conformJob->placement))
            *conformJob->cancel = true;
        return;
    }
    if (conformJob) {
        if (!sameConformPlacement(wanted, conformJob->placement))
            *conformJob->cancel = true;
        return;
    }
    conformPreviewError.clear();
    auto job = std::make_unique<ConformJob>();
    job->placement = wanted;
    auto cancel = job->cancel;
    job->future = std::async(std::launch::async, [wanted = std::move(wanted), cancel] {
        auto bounded = wanted.options;
        bounded.limits.seconds = 8;
        return buildSurfaceDecalPatch(*wanted.surface, wanted.hit, bounded, wanted.bend, {}, cancel.get());
    });
    conformJob = std::move(job);
}
void updateDecalPreview(Renderer& renderer) {
    if (conformJob && *conformJob->cancel &&
        conformJob->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        conformJob.reset();
    if (conformReadyPlacement && conformReadyPlacement->surface != surface) {
        conformReadyPlacement.reset();
        conformReadyPatch.reset();
        conformPreviewError.clear();
    }
    auto& preview = renderer.decalPreview;
    preview.active = false;
    preview.conformPatch.reset();
    if (tool != 2 || controlsBusy() || !renderer.options.editedLivery || !decalOverlay || decalSuppressed || !mappingPreview || !decalHit ||
        !surface || surfaceArgs != currentArgs || decalPose != currentArgs) {
        if (conformJob)
            *conformJob->cancel = true;
        return;
    }
    auto options = decalOptions();
    if (decalConforming) {
        prepareConformPreview(options);
        options = decalOptions();
        if (!options.conformPatch)
            return;
        preview.conformPatch = options.conformPatch;
        preview.projectionVP = decalViewProjection;
    } else if (conformJob)
        *conformJob->cancel = true;
    const auto frame = surfaceDecalFrame(options);
    preview.center = frame.center;
    preview.right = frame.right;
    preview.up = frame.up;
    preview.normal = frame.normal;
    preview.width = frame.width;
    preview.height = frame.height;
    preview.depth = frame.depth;
    preview.opacity = options.opacity;
    preview.frontFacesOnly = options.frontFacesOnly;
    preview.occlusion = options.occlusion;
    preview.preserveAlpha = options.preserveAlpha;
    preview.image = mappingPreview;
    preview.active = true;
}
void startDecal(Renderer& renderer, bool verify = false) {
    const auto options = decalOptions();
    require(bool(mappingImage), "请先加载贴花图片。");
    decalSuppressed = true;
    decalSuppressedScreen = decalScreen;
    renderer.decalPreview.active = false;
    if (conformJob)
        *conformJob->cancel = true;
    // A click while the hover patch is still being prepared captures that exact point. Build it in
    // the existing cancellable paint worker, rather than dropping the click or blocking the UI.
    auto prepare = [posed = surface, hit = *decalHit, bend = decalMaxBend, conform = decalConforming](
                       SurfaceDecalOptions& placement, Progress progress, const std::atomic_bool* cancel) {
        if (conform && !placement.conformPatch)
            placement.conformPatch = buildSurfaceDecalPatch(*posed, hit, placement, bend, progress, cancel);
    };
    startAutomaticMapping(options, findDecalMaterials, applySurfaceDecal, verify, false, true, prepare);
}
void decalViewport(Renderer& renderer, ImVec2 origin, ImVec2 size, bool hovered) {
    auto& io = ImGui::GetIO();
    if (decalDiagnosticStage == 0 && hovered && !io.KeyAlt && !ImGui::IsMouseDown(ImGuiMouseButton_Right) &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Middle) && mappingImage) {
        const float dx = io.MousePos.x - decalSuppressedScreen.x,
                    dy = io.MousePos.y - decalSuppressedScreen.y;
        if (dx * dx + dy * dy > 4)
            decalSuppressed = false;
        if (!io.WantTextInput && !io.KeyCtrl) {
            bool resized = false;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) {
                decalWidth = std::max(.005f, decalWidth / 1.1f);
                resized = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) {
                decalWidth = std::min(1000.f, decalWidth * 1.1f);
                resized = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Q)) {
                decalAngle = std::remainder(decalAngle - 5.f, 360.f);
                decalSuppressed = false;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_E)) {
                decalAngle = std::remainder(decalAngle + 5.f, 360.f);
                decalSuppressed = false;
            }
            if (resized)
                decalSuppressed = false;
        }
        const bool hit = positionDecal(renderer, origin, size, {io.MousePos.x, io.MousePos.y});
        auto* draw = ImGui::GetWindowDrawList();
        const ImU32 color = hit ? IM_COL32(114, 211, 255, 245) : IM_COL32(242, 160, 90, 230);
        draw->AddCircle(io.MousePos, 5, color, 20, 1.5f);
        if (hit && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            startDecal(renderer);
    }
    updateDecalPreview(renderer);
    if (hovered && mappingImage) {
        PaintDrawClipScope clip(ImGui::GetWindowDrawList(), origin, {origin.x + size.x, origin.y + size.y});
        ImGui::GetWindowDrawList()->AddText({origin.x + 16, origin.y + 38}, IM_COL32(178, 217, 240, 240),
                                            decalConforming ? "曲面贴花 · 左键盖印 · [ / ] 大小 · Q / E 旋转"
                                                            : "平面贴花 · 左键盖印 · [ / ] 大小 · Q / E 旋转");
        if (decalConforming && decalHit && !decalSuppressed && !renderer.decalPreview.active &&
            !controlsBusy())
            ImGui::GetWindowDrawList()->AddText(
                {origin.x + 16, origin.y + 60}, IM_COL32(223, 190, 122, 240),
                conformPreviewError.empty() ? "正在计算曲面贴合…" : "此处无法展开，请缩小贴花或调整落点");
    }
}
