// Included in the tool-2 branch of PaintEditor::panel.
if (ImGui::Button(p.mappingImage ? "更换贴花图片…" : "加载贴花图片…", {-1, 33}))
    if (auto path = paintDialog(window, L"选择图片画笔（推荐透明 PNG）", 0))
        p.loadMappingImage(renderer, *path, 2);
{
    PaintDisabledScope stampDisabled(p.controlsBusy() || !posed || !p.mappingImage || !p.decalHit);
    if (primaryButton("在当前落点盖印"))
        p.startDecal(renderer);
}
paintHint(!posed ? "正在准备模型表面…" : "图片跟随鼠标贴在模型表面，左键单击盖印。");
imagePreview();
paintQuality();
decalResolution();
gap();
bool placementChanged = false;
ImGui::SetNextItemWidth(-1);
int conformMode = p.decalConforming ? 0 : 1;
if (ImGui::Combo("##decal-fit-mode", &conformMode, "曲面贴合\0平面投射\0")) {
    p.decalConforming = conformMode == 0;
    placementChanged = true;
}
paintHint(p.decalConforming ? "沿相连表面延伸，自动跨越网格与材质接缝。"
                           : "沿落点法线投射，适合较平坦的区域。");
ImGui::SetNextItemWidth(-1);
placementChanged |=
    ImGui::SliderFloat("##decal-width", &p.decalWidth, .005f, float(std::max(1., renderer.camera.radius * 2)),
                       "宽度 %.3f m", ImGuiSliderFlags_Logarithmic);
if (ImGui::Checkbox("锁定图片长宽比", &p.decalAspectLocked)) {
    if (!p.decalAspectLocked && p.mappingImage)
        p.decalHeight = p.decalWidth * float(p.mappingImage->height) / p.mappingImage->width;
    placementChanged = true;
}
if (!p.decalAspectLocked) {
    ImGui::SetNextItemWidth(-1);
    placementChanged |= ImGui::SliderFloat("##decal-height", &p.decalHeight, .005f,
                                           float(std::max(1., renderer.camera.radius * 2)), "高度 %.3f m",
                                           ImGuiSliderFlags_Logarithmic);
}
ImGui::SetNextItemWidth(-1);
placementChanged |= ImGui::SliderFloat("##decal-angle", &p.decalAngle, -180, 180, "旋转 %.0f°");
ImGui::SetNextItemWidth(-1);
placementChanged |= ImGui::SliderFloat("##decal-opacity", &p.decalOpacity, .01f, 1, "不透明度 %.2f");
placementChanged |= ImGui::Checkbox("显示表面预览", &p.decalOverlay);
paintHint("[ / ] 调整大小，Q / E 旋转；右键旋转模型，滚轮缩放视角。");
if (ImGui::CollapsingHeader("贴合范围与遮挡")) {
    if (p.decalConforming) {
        ImGui::SetNextItemWidth(-1);
        placementChanged |= ImGui::SliderFloat("##decal-max-bend", &p.decalMaxBend, 10, 85,
                                               "允许相邻面折角 %.0f°");
        paintHint("遇到更陡的折角停止延伸；数值较小可避免越过尖锐边缘。");
    } else {
        ImGui::SetNextItemWidth(-1);
        placementChanged |= ImGui::SliderFloat(
            "##decal-depth", &p.decalDepth, .001f, float(std::max(1., renderer.camera.radius)),
            "投射深度 ±%.3f m", ImGuiSliderFlags_Logarithmic);
        paintHint("限制落点前后的投射厚度；弯曲表面漏印时可适当增加。");
    }
    placementChanged |= ImGui::Checkbox("只投射朝外的表面", &p.decalFront);
    placementChanged |= ImGui::Checkbox("跳过被其他部件遮挡处", &p.decalOcclusion);
    if (p.decalConforming)
        paintHint("遮挡按放置时的视角判断；旋转观察不会改变已放置的贴花。");
    if (ImGui::Button("重置大小与旋转", {-1, 29})) {
        p.decalWidth = p.decalHeight = float(std::max(.02, renderer.camera.radius * .08));
        p.decalDepth = p.decalWidth * .2f;
        p.decalAngle = 0;
        p.decalMaxBend = 65;
        placementChanged = true;
    }
    paintHint("一次盖印自动写入覆盖到的贴图，并作为一步撤销。共用 UV 仍会联动。");
    if (p.decalConforming)
        paintHint("强曲率或不连续表面可能限制覆盖范围；可缩小贴花分次盖印。");
}
if (placementChanged) {
    p.decalWidth = std::isfinite(p.decalWidth) ? std::clamp(p.decalWidth, .005f, 1000.f) : 1.f;
    p.decalHeight = std::isfinite(p.decalHeight) ? std::clamp(p.decalHeight, .005f, 1000.f) : 1.f;
    p.decalDepth = std::isfinite(p.decalDepth) ? std::clamp(p.decalDepth, .001f, 1000.f) : .2f;
    p.decalAngle = std::isfinite(p.decalAngle) ? std::remainder(p.decalAngle, 360.f) : 0.f;
    p.decalOpacity = std::isfinite(p.decalOpacity) ? std::clamp(p.decalOpacity, .01f, 1.f) : 1.f;
    p.decalSuppressed = false;
}
p.updateDecalPreview(renderer);
resultSummary(p.decalResult);
