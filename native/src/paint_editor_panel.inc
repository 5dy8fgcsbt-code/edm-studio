// Included inside namespace edm, after PaintEditor::Impl and the panel helper definitions.
void PaintEditor::panel(HWND window, Renderer& renderer, const Args& args) {
    auto& p = *impl;
    const ImVec4 blue{.20f, .44f, .76f, 1}, quiet{.12f, .16f, .21f, 1};
    const auto gap = [] { ImGui::Dummy({0, 5}); };
    const auto primaryButton = [&](const char* label, float height = 35) {
        ImGui::PushStyleColor(ImGuiCol_Button, blue);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {.26f, .52f, .86f, 1});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, {.15f, .36f, .65f, 1});
        const bool clicked = ImGui::Button(label, {-1, height});
        ImGui::PopStyleColor(3);
        return clicked;
    };
    const auto imagePreview = [&] {
        if (!p.mappingImage || !p.mappingPreview)
            return;
        const float maxWidth = std::max(1.f, std::min(220.f, ImGui::GetContentRegionAvail().x));
        const float factor = std::min(maxWidth / p.mappingImage->width, 68.f / p.mappingImage->height);
        ImGui::Image(p.mappingPreview->view.Get(),
                     {p.mappingImage->width * factor, p.mappingImage->height * factor});
        const auto name = pathString(fs::path(wide(p.mappingPath)).filename());
        ImGui::TextWrapped("%s · %u × %u", name.c_str(), p.mappingImage->width, p.mappingImage->height);
    };
    const auto resultSummary = [&](const Json& result) {
        if (!result.is_object())
            return;
        ImGui::TextColored({.38f, .78f, .64f, 1}, "已写入 %llu 像素 · %.2f 秒",
                           result.value("painted_pixels", 0ull), result.value("seconds", 0.));
        if (result.value("reused_texels", uint64_t(0)))
            paintHint("共用 UV 的重叠位置已避免重复上色。");
        if (result.contains("warnings") && result["warnings"].is_array() && !result["warnings"].empty() &&
            ImGui::CollapsingHeader("操作详情"))
            for (const auto& warning : result["warnings"])
                if (warning.is_string())
                    paintHint(warning.get_ref<const std::string&>().c_str());
    };

    ImGui::TextUnformatted("涂装绘制");
    paintHint("直接在模型上绘制，自动匹配贴图");
    if (!renderer.options.editedLivery)
        paintHint("正在浏览未编辑涂装。勾选画面下方“浏览已编辑的涂装”后继续绘制。");
    gap();
    try {
        const bool modelReady = p.scene && !p.scene->materials.empty();
        const float width =
            std::max(1.f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * .5f);
        {
            PaintDisabledScope toolsDisabled(!modelReady || p.controlsBusy() ||
                                             !renderer.options.editedLivery);
            const char* toolNames[] = {"浏览", "直接画笔", "贴花", "相机投影"};
            for (int index = 0; index < 4; ++index) {
                if (index & 1)
                    ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, p.tool == index ? blue : quiet);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {.23f, .39f, .57f, 1});
                const bool clicked = ImGui::Button(toolNames[index], {width, 36});
                ImGui::PopStyleColor(2);
                if (clicked) {
                    p.tool = index;
                    p.projectionDragging = false;
                }
            }
        }
        if (!modelReady) {
            gap();
            paintHint("打开 EDM 模型后，选择工具即可开始绘制。");
            return;
        }

        gap();
        PaintDisabledScope operationsDisabled(p.controlsBusy());
        const bool posed = p.surface && p.surfaceArgs == args;

        // Each mode is usable before any editable canvas exists. Loading images and applying them
        // precede placement details, so the primary action stays near the tool switcher.
        PaintDisabledScope originalViewDisabled(!renderer.options.editedLivery);
        if (p.tool == 2) {
#include "paint_editor_decal_panel.inc"
        } else if (p.tool == 3) {
            if (ImGui::Button(p.mappingImage ? "更换投影图片…" : "加载投影图片…", {-1, 33}))
                if (auto path = paintDialog(window, L"选择按当前相机投影的图片", 0))
                    p.loadMappingImage(renderer, *path, 3);
            bool apply = false;
            {
                PaintDisabledScope applyDisabled(p.controlsBusy() || !posed || !p.mappingImage);
                apply = primaryButton("按当前相机应用投影");
            }
            if (apply)
                p.startProjection(renderer);
            if (!posed)
                paintHint("正在准备模型表面，完成后即可应用。");
            else
                paintHint("在视口中拖动图片定位，自动写入画面覆盖到的贴图。");
            imagePreview();
            gap();
            bool placementChanged = false;
            float imageWidth = p.projectionSize[0] * 100;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##projection-width", &imageWidth, 5, 150, "图片宽度 %.0f%%")) {
                p.projectionSize[0] = imageWidth * .01f;
                placementChanged = true;
            }
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##projection-opacity", &p.projectionOpacity, .01f, 1, "图片不透明度 %.2f");
            ImGui::Checkbox("显示图片定位预览", &p.projectionOverlay);
            if (ImGui::CollapsingHeader("投影位置与比例")) {
                auto projection = p.projectionOptions(renderer);
                F2 center{p.projectionCenter[0] * 100, p.projectionCenter[1] * 100};
                ImGui::SetNextItemWidth(-1);
                if (ImGui::DragFloat2("##projection-center", center.data(), .25f, -50, 150, "%.1f%%",
                                      ImGuiSliderFlags_AlwaysClamp)) {
                    p.projectionCenter = {center[0] * .01f, center[1] * .01f};
                    placementChanged = true;
                }
                paintHint("中心位置：水平 / 垂直，也可在视口中左键拖动。");
                if (ImGui::Checkbox("锁定图片长宽比", &p.projectionAspectLocked)) {
                    p.projectionSize = projection.size;
                    placementChanged = true;
                }
                {
                    PaintDisabledScope heightDisabled(p.projectionAspectLocked);
                    float height = projection.size[1] * 100;
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::SliderFloat("##projection-height", &height, 5, 150, "高度 %.0f%%")) {
                        p.projectionSize[1] = height * .01f;
                        placementChanged = true;
                    }
                }
                ImGui::SetNextItemWidth(-1);
                placementChanged |=
                    ImGui::SliderFloat("##projection-angle", &p.projectionAngle, -180, 180, "旋转 %.0f°");
                if (ImGui::Button("居中", {width, 29})) {
                    p.projectionCenter = {.5f, .5f};
                    placementChanged = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("适应视口", {width, 29})) {
                    p.projectionCenter = {.5f, .5f};
                    p.projectionSize = {.9f, .9f};
                    if (p.projectionAspectLocked && p.mappingImage)
                        p.projectionSize[0] =
                            std::min(.9f, .9f * p.mappingImage->width / p.mappingImage->height /
                                              projection.viewportAspect);
                    placementChanged = true;
                }
            }
            if (placementChanged)
                p.projectionOverlay = true;
            if (ImGui::CollapsingHeader("投影表面与预览")) {
                ImGui::Checkbox("仅投影朝向相机的表面", &p.projectionFront);
                ImGui::Checkbox("跳过被其他部件遮挡处", &p.projectionOcclusion);
                ImGui::Checkbox("保留原始透明遮罩", &p.preserveAlpha);
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat("##projection-preview-opacity", &p.projectionPreviewOpacity, .05f, .8f,
                                   "定位预览透明度 %.2f");
                paintHint("定位预览透明度不影响实际写入，相机位置在应用时固定。");
            }
            resultSummary(p.projectionResult);
        } else {
            // Brush controls remain visible in browse/brush modes without loading any canvas first.
            ImGui::ColorEdit4("画笔颜色", p.color.data(),
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreviewHalf);
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##paint-radius", &p.radius, .001f,
                               float(std::max(.01, renderer.camera.radius * .15)), "半径 %.3f m",
                               ImGuiSliderFlags_Logarithmic);
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##paint-opacity", &p.opacity, .01f, 1, "不透明度 %.2f");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##paint-hardness", &p.hardness, 0, 1, "硬度 %.2f");
            ImGui::Checkbox("保留原始透明遮罩", &p.preserveAlpha);
            paintHint(p.tool == 1 ? "左键直接绘制，跨过部件时自动切换对应贴图。"
                                  : "选择直接画笔，然后在模型表面落笔。");
        }

        gap();
        // Batch history is owned by a worker during image application. Do not query it while busy.
        {
            PaintDisabledScope undoDisabled(p.controlsBusy() || !p.history.canUndo());
            if (ImGui::Button("撤销整次操作", {width, 31}) && p.history.undo()) {
                p.saved = false;
                p.status = "已撤销 · 恢复 " + std::to_string(p.history.changedMaterials().size()) + " 张贴图";
            }
        }
        ImGui::SameLine();
        {
            PaintDisabledScope redoDisabled(p.controlsBusy() || !p.history.canRedo());
            if (ImGui::Button("重做整次操作", {width, 31}) && p.history.redo()) {
                p.saved = false;
                p.status = "已重做 · 更新 " + std::to_string(p.history.changedMaterials().size()) + " 张贴图";
            }
        }
        gap();
        ImGui::Separator();
        gap();
        if (p.material >= 0 && p.material < int(p.scene->materials.size())) {
            ImGui::PushStyleColor(ImGuiCol_Text, {.49f, .57f, .68f, 1});
            ImGui::TextWrapped("当前表面：%s", p.scene->materials[p.material].name.c_str());
            ImGui::PopStyleColor();
        } else
            paintHint("当前表面：将鼠标移到模型上自动识别");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("这里只显示识别结果，绘制范围由落笔位置或图片覆盖范围决定。");
        const char* qualityNames[] = {"2048 · 流畅", "4096 · 精细", "8192 · 高精度"};
        int quality = p.paintMaxDimension >= 8192 ? 2 : p.paintMaxDimension >= 4096 ? 1 : 0;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##paint-resolution", &quality, qualityNames, 3))
            p.paintMaxDimension = std::array<int, 3>{2048, 4096, 8192}[quality];
        paintHint("首次触及自动载入当前外观，已编辑画布不重采样。");
        if (ImGui::Button("加载平面涂装模板…", {-1, 32}))
            if (auto path = paintDialog(window, L"载入供下一次落笔使用的平面模板", 0))
                p.queueTemplate(renderer, *path);
        if (p.pendingTemplate)
            ImGui::TextColored({.38f, .78f, .64f, 1}, "模板已就绪 · 下次落笔自动应用");
        else
            paintHint("模板自动用于下一次落笔部位，无需选择材质。");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("支持 PNG / DDS / TGA / JPG；PSD 图层请先导出平面 PNG。");

        originalViewDisabled.end();
        if (ImGui::CollapsingHeader("已编辑贴图")) {
            if (p.controlsBusy())
                paintHint("操作完成后可查看贴图预览。");
            else if (p.entries.empty())
                paintHint("落笔或应用图片后，修改过的贴图将显示在这里。");
            else {
                paintHint("这里只预览已编辑内容，不限制绘制范围。");
                for (const auto& [material, entry] : p.entries) {
                    ImGui::PushID(material);
                    if (material >= 0 && material < int(p.scene->materials.size()))
                        ImGui::TextWrapped("%s", p.scene->materials[material].name.c_str());
                    const auto& image = entry.canvas->image();
                    ImGui::TextDisabled("%u × %u · %.1f MiB", image.width, image.height,
                                        image.rgba.size() / 1048576.);
                    if (entry.gpu.view) {
                        const float maxWidth =
                            std::max(1.f, std::min(140.f, ImGui::GetContentRegionAvail().x));
                        const float factor = std::min(maxWidth / image.width, 84.f / image.height);
                        ImGui::Image(entry.gpu.view->view.Get(),
                                     {image.width * factor, image.height * factor});
                    }
                    const bool checkUV = p.surface && ImGui::Button("检查共用 UV", {-1, 28});
                    if (entry.uv && entry.uv->shared())
                        paintHint("此贴图含共用 UV，相关表面会同时变化。");
                    ImGui::Separator();
                    ImGui::PopID();
                    if (checkUV) {
                        auto surface = p.surface;
                        auto generation = p.generation;
                        p.start([&p, surface, material, generation](Progress progress,
                                                                    const std::atomic_bool* cancel) {
                            progress("检查重叠和重复 UV…");
                            auto info = surface->analyzeUV(material, 256, cancel);
                            return [&p, material, generation, info] {
                                if (generation != p.generation || !p.entries.contains(material))
                                    return;
                                p.entries[material].uv = info;
                                p.status = info.shared() ? "检测到共用 / 重叠 UV，相关表面会同时变化。"
                                                         : "采样检查未发现重叠 UV。";
                            };
                        });
                    }
                }
            }
        }

        gap();
        ImGui::Separator();
        gap();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##paint-name", "涂装名称", &p.liveryName);
        {
            PaintDisabledScope fileActionsDisabled(p.controlsBusy());
            {
                PaintDisabledScope saveDisabled(p.controlsBusy() || p.entries.empty());
                if (primaryButton("保存工程与 DCS 涂装…"))
                    if (auto directory = paintDialog(window, L"选择保存位置（将在其中新建文件夹）", 1))
                        p.saveProject(*directory);
            }
            // A save started above may now own the canvas state. Re-check before another file action.
            PaintDisabledScope openDisabled(p.controlsBusy());
            if (ImGui::Button("打开绘制工程…", {-1, 31}))
                if (auto path = paintDialog(window, L"打开绘制工程", 2)) {
                    if (hasEdits())
                        recover(p.recoveryRoot);
                    p.openProject(renderer, *path);
                }
        }
        operationsDisabled.end();

        // Cancellation remains outside the disabled operation region, including multi-frame strokes.
        if (p.strokePending()) {
            gap();
            ImGui::TextWrapped("%s · 待处理 %llu 个采样点",
                               p.strokePath.released() ? "正在补齐笔迹" : "正在绘制",
                               static_cast<unsigned long long>(p.strokePath.pendingSamples()));
            if (ImGui::Button("取消当前整笔", {-1, 31})) {
                p.cancelQueuedStroke();
                p.status = "当前笔划已取消并完整回滚";
            }
        }
        if (p.busy()) {
            gap();
            for (const auto& item : p.work)
                if (!item->finished) {
                    std::lock_guard lock(item->mutex);
                    paintHint(item->progress.c_str());
                }
            if (ImGui::Button("取消绘制任务", {-1, 31})) {
                for (auto& item : p.work)
                    item->cancel = true;
                p.tool = 0;
                p.surfacePending = false;
            }
        }
        gap();
        paintHint(p.status.c_str());
        paintHint("右键旋转 · 中键平移。共用 UV 的表面会联动，GLB 导出包含绘制结果。");
    } catch (...) {
        p.error = exceptionText();
    }
    if (!p.error.empty()) {
        ImGui::TextWrapped("%s", p.error.c_str());
        if (ImGui::Button("清除提示"))
            p.error.clear();
    }
}
