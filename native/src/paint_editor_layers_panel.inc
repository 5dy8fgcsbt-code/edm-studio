// The layer list is global: clicking a model surface still chooses its texture automatically.
if (ImGui::CollapsingHeader("涂装图层", ImGuiTreeNodeFlags_DefaultOpen)) {
    PaintDisabledScope layersDisabled(p.controlsBusy() || !renderer.options.editedLivery);
    auto next = p.layerInfos;
    uint64_t selected = p.activeLayerId;
    bool changed = false;
    ImGui::TextDisabled("%zu / 64 层 · 上方覆盖下方", next.size());
    {
        PaintDisabledScope addDisabled(next.size() >= 64);
        if (ImGui::Button("+ 新建图层", {-1, 29})) {
            auto position = std::find_if(next.begin(), next.end(),
                                         [&](const auto& layer) { return layer.id == selected; });
            selected = p.nextLayerId;
            next.insert(position + 1, {selected, "图层 " + std::to_string(selected), true, 1, true});
            changed = true;
        }
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, {.055f, .072f, .094f, 1});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4, 3});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {6, 4});
    const float listHeight = float(std::min<size_t>(4, next.size())) *
                                 (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y) +
                             ImGui::GetStyle().WindowPadding.y * 2;
    ImGui::BeginChild("##paint-layer-list", {-1, listHeight}, ImGuiChildFlags_Borders);
    for (auto it = next.rbegin(); it != next.rend(); ++it) {
        ImGui::PushID(int(it->id));
        changed |= ImGui::Checkbox("##layer-visible", &it->visible);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("显示 / 隐藏此图层，隐藏内容仍随工程保存。");
        ImGui::SameLine();
        if (ImGui::Selectable(it->name.c_str(), selected == it->id, 0, {0, 21}))
            selected = it->id;
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    if (changed)
        p.changeLayers(std::move(next), selected);
    else if (selected != p.activeLayerId)
        p.selectLayer(selected);

    ImGui::TextDisabled("底图 · 保留原始外观");
    auto current = std::find_if(p.layerInfos.begin(), p.layerInfos.end(),
                                [&](const auto& layer) { return layer.id == p.activeLayerId; });
    const auto position = size_t(current - p.layerInfos.begin());
    ImGui::SetNextItemWidth(-1);
    bool rename = ImGui::InputText("##layer-name", &p.layerNameDraft, ImGuiInputTextFlags_EnterReturnsTrue);
    rename |= ImGui::IsItemDeactivatedAfterEdit();
    if (rename && p.layerNameDraft != current->name) {
        auto edited = p.layerInfos;
        edited[position].name = p.layerNameDraft;
        p.changeLayers(std::move(edited), p.activeLayerId);
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##layer-opacity", &p.layerOpacityDraft, 0, 1, "图层不透明度 %.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        auto edited = p.layerInfos;
        edited[position].opacity = p.layerOpacityDraft;
        p.changeLayers(std::move(edited), p.activeLayerId);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("松开后更新模型；每次调整作为一步撤销。");
    bool keepAlpha = p.preserveAlpha;
    if (ImGui::Checkbox("保持底图透明遮罩", &keepAlpha)) {
        auto edited = p.layerInfos;
        edited[position].preserveAlpha = keepAlpha;
        p.changeLayers(std::move(edited), p.activeLayerId);
    }
    {
        PaintDisabledScope topDisabled(position + 1 >= p.layerInfos.size());
        if (ImGui::Button("上移", {width, 26})) {
            auto edited = p.layerInfos;
            std::swap(edited[position], edited[position + 1]);
            p.changeLayers(std::move(edited), p.activeLayerId);
        }
    }
    ImGui::SameLine();
    {
        PaintDisabledScope bottomDisabled(position == 0);
        if (ImGui::Button("下移", {width, 26})) {
            auto edited = p.layerInfos;
            std::swap(edited[position], edited[position - 1]);
            p.changeLayers(std::move(edited), p.activeLayerId);
        }
    }
    {
        PaintDisabledScope duplicateDisabled(p.layerInfos.size() >= 64);
        if (ImGui::Button("复制图层", {width, 26})) {
            auto edited = p.layerInfos;
            auto duplicate = edited[position];
            const auto original = duplicate.id;
            duplicate.id = p.nextLayerId;
            duplicate.name = "副本 " + std::to_string(duplicate.id);
            edited.insert(edited.begin() + position + 1, duplicate);
            p.changeLayers(std::move(edited), duplicate.id, original, duplicate.id);
        }
    }
    ImGui::SameLine();
    {
        PaintDisabledScope deleteDisabled(p.layerInfos.size() <= 1);
        if (ImGui::Button("删除图层", {width, 26})) {
            auto edited = p.layerInfos;
            edited.erase(edited.begin() + position);
            const auto active = edited[std::min(position, edited.size() - 1)].id;
            p.changeLayers(std::move(edited), active);
        }
    }
    if (ImGui::Button("清空当前图层", {-1, 26}))
        p.changeLayers(p.layerInfos, p.activeLayerId, 0, 0, true);
    paintHint("自动匹配模型表面的贴图。");
    gap();
}
