#include "paint_batch.h"
#include <climits>

namespace edm {
struct PaintBatchHistory::Impl {
    struct Member {
        std::shared_ptr<PaintCanvas> canvas;
        uint64_t token = 0;
        bool control = false;
    };
    struct Group {
        std::vector<Member> members;
        std::vector<int> materials;
    };
    bool active = false;
    size_t maxSteps;
    std::string label;
    std::map<int, std::shared_ptr<PaintCanvas>> touched;
    std::shared_ptr<PaintCanvas> control;
    std::vector<std::shared_ptr<Group>> undo, redo;
    std::shared_ptr<Group> lastChanged;
    explicit Impl(size_t steps) : maxSteps(steps) {}
    bool matches(const std::shared_ptr<Group>& group, bool isUndo) const noexcept {
        for (const auto& member : group->members)
            if ((isUndo ? member.canvas->undoToken() : member.canvas->redoToken()) != member.token)
                return false;
        return true;
    }
};
PaintBatchHistory::PaintBatchHistory(size_t maxHistorySteps) : impl(std::make_unique<Impl>(maxHistorySteps)) {
    require(maxHistorySteps > 0, "Batch history must retain at least one operation");
}
PaintBatchHistory::~PaintBatchHistory() {
    cancel();
}
void PaintBatchHistory::begin(std::string label) {
    require(!impl->active, "A multi-material paint operation is already active");
    impl->label = std::move(label);
    impl->touched.clear();
    impl->lastChanged.reset();
    impl->active = true;
}
PaintCanvas& PaintBatchHistory::touch(int material, std::shared_ptr<PaintCanvas> canvas) {
    if (!impl->active)
        throw std::runtime_error("Begin a paint batch before touching a material");
    bool opened = false;
    try {
        require(material >= 0 && canvas, "Invalid batch material or canvas");
        require(canvas != impl->control, "A control canvas cannot also be a material canvas");
        if (auto existing = impl->touched.find(material); existing != impl->touched.end()) {
            require(existing->second == canvas, "A material changed canvas during an active paint operation");
            return *canvas;
        }
        bool alias = std::any_of(impl->touched.begin(), impl->touched.end(),
                                 [&](const auto& pair) { return pair.second == canvas; });
        if (!alias) {
            require(!canvas->strokeActive(), "A canvas already belongs to another paint operation");
            canvas->beginStroke(impl->label);
            opened = true;
        }
        impl->touched.emplace(material, canvas);
        return *canvas;
    } catch (...) {
        if (opened)
            canvas->cancelStroke();
        cancel();
        throw;
    }
}
PaintCanvas& PaintBatchHistory::touchControl(std::shared_ptr<PaintCanvas> canvas) {
    if (!impl->active)
        throw std::runtime_error("Begin a paint batch before touching its control canvas");
    bool opened = false;
    try {
        require(bool(canvas), "Invalid batch control canvas");
        if (impl->control) {
            require(impl->control == canvas, "A paint batch can have only one control canvas");
            return *canvas;
        }
        require(std::none_of(impl->touched.begin(), impl->touched.end(),
                             [&](const auto& pair) { return pair.second == canvas; }),
                "A material canvas cannot also be a control canvas");
        require(!canvas->strokeActive(), "A canvas already belongs to another paint operation");
        canvas->beginStroke(impl->label);
        opened = true;
        impl->control = canvas;
        return *canvas;
    } catch (...) {
        if (opened)
            canvas->cancelStroke();
        cancel();
        throw;
    }
}
bool PaintBatchHistory::discardUnchanged(const std::shared_ptr<PaintCanvas>& canvas) {
    if (!impl->active)
        throw std::runtime_error("Begin a paint batch before discarding an unchanged canvas");
    if (!canvas ||
        (canvas != impl->control && std::none_of(impl->touched.begin(), impl->touched.end(),
                                                 [&](const auto& pair) { return pair.second == canvas; })))
        throw std::runtime_error("The canvas does not belong to the active paint batch");
    if (canvas->prepareStroke())
        return false;
    canvas->cancelStroke();
    std::erase_if(impl->touched, [&](const auto& pair) { return pair.second == canvas; });
    if (impl->control == canvas)
        impl->control.reset();
    return true;
}
bool PaintBatchHistory::commit() {
    if (!impl->active)
        throw std::runtime_error("No active multi-material paint operation");
    try {
        auto group = std::make_shared<Impl::Group>();
        group->members.reserve(impl->touched.size() + bool(impl->control));
        group->materials.reserve(impl->touched.size());
        std::vector<std::shared_ptr<PaintCanvas>> canvases;
        canvases.reserve(impl->touched.size() + bool(impl->control));
        if (impl->control)
            canvases.push_back(impl->control);
        for (const auto& [material, canvas] : impl->touched)
            if (std::find(canvases.begin(), canvases.end(), canvas) == canvases.end())
                canvases.push_back(canvas);
        impl->undo.reserve(impl->undo.size() + 1);
        for (const auto& canvas : canvases)
            if (canvas->prepareStroke())
                group->members.push_back({canvas, canvas->preparedStrokeToken(), canvas == impl->control});
        for (const auto& [material, canvas] : impl->touched)
            if (std::any_of(group->members.begin(), group->members.end(),
                            [&](const Impl::Member& member) { return member.canvas == canvas; }))
                group->materials.push_back(material);

        // All allocations/preflight are complete. Everything below is nonthrowing, including each
        // canvas commit; cancellation above never alters either existing undo/redo branch.
        for (const auto& canvas : canvases)
            canvas->commitPreparedStroke();
        bool changed = !group->members.empty();
        if (changed) {
            impl->redo.clear();
            impl->undo.push_back(group);
            if (impl->undo.size() > impl->maxSteps)
                impl->undo.erase(impl->undo.begin());
            impl->lastChanged = std::move(group);
        }
        impl->touched.clear();
        impl->control.reset();
        impl->active = false;
        return changed;
    } catch (...) {
        cancel();
        throw;
    }
}
void PaintBatchHistory::cancel() noexcept {
    for (const auto& [material, canvas] : impl->touched)
        canvas->cancelStroke(); // Idempotent for aliases of the same canvas.
    if (impl->control)
        impl->control->cancelStroke();
    impl->touched.clear();
    impl->control.reset();
    impl->active = false;
    impl->lastChanged.reset();
}
bool PaintBatchHistory::canUndo() const noexcept {
    return !impl->active && !impl->undo.empty() && impl->matches(impl->undo.back(), true);
}
bool PaintBatchHistory::canRedo() const noexcept {
    return !impl->active && !impl->redo.empty() && impl->matches(impl->redo.back(), false);
}
bool PaintBatchHistory::undo() {
    if (!canUndo())
        return false;
    auto group = impl->undo.back();
    impl->redo.reserve(impl->redo.size() + 1);
    for (const auto& member : group->members)
        if (!member.canvas->prepareUndo(member.token))
            return false;
    for (const auto& member : group->members)
        member.canvas->commitPreparedUndo(member.token);
    impl->undo.pop_back();
    impl->redo.push_back(group);
    impl->lastChanged = std::move(group);
    return true;
}
bool PaintBatchHistory::redo() {
    if (!canRedo())
        return false;
    auto group = impl->redo.back();
    impl->undo.reserve(impl->undo.size() + 1);
    for (const auto& member : group->members)
        if (!member.canvas->prepareRedo(member.token))
            return false;
    for (const auto& member : group->members)
        member.canvas->commitPreparedRedo(member.token);
    impl->redo.pop_back();
    impl->undo.push_back(group);
    impl->lastChanged = std::move(group);
    return true;
}
bool PaintBatchHistory::active() const noexcept {
    return impl->active;
}
std::vector<int> PaintBatchHistory::activeMaterials() const {
    std::vector<int> result;
    result.reserve(impl->touched.size());
    for (const auto& [material, canvas] : impl->touched)
        result.push_back(material);
    return result;
}
const std::vector<int>& PaintBatchHistory::changedMaterials() const noexcept {
    static const std::vector<int> empty;
    return impl->lastChanged ? impl->lastChanged->materials : empty;
}
void PaintBatchHistory::remapMaterials(const std::vector<int>& mapping,
                                       const std::vector<std::shared_ptr<PaintCanvas>>& retainedCanvases) {
    require(!impl->active, "Cannot remap material history during an active paint operation");
    require(retainedCanvases.size() <= size_t(INT_MAX), "Remapped paint material count exceeds index range");
    for (int material : mapping)
        require(material >= -1 && (material < 0 || size_t(material) < retainedCanvases.size()),
                "Remapped paint material index is out of range");
    std::unordered_map<const PaintCanvas*, std::vector<int>> aliases;
    for (size_t material = 0; material < retainedCanvases.size(); ++material)
        if (retainedCanvases[material])
            aliases[retainedCanvases[material].get()].push_back(int(material));

    // Undo/redo and lastChanged may share a Group. Clone each old object once, without mutating
    // either its indices or any canvas state until every allocation and validation has succeeded.
    std::unordered_map<const Impl::Group*, std::shared_ptr<Impl::Group>> replacements;
    auto remapGroup = [&](const std::shared_ptr<Impl::Group>& group) -> std::shared_ptr<Impl::Group> {
        if (!group)
            return {};
        if (auto existing = replacements.find(group.get()); existing != replacements.end())
            return existing->second;
        for (int material : group->materials)
            require(material >= 0 && size_t(material) < mapping.size(),
                    "Material history refers to an index outside the supplied remapping");
        auto replacement = std::make_shared<Impl::Group>();
        replacement->members.reserve(group->members.size());
        for (const auto& member : group->members)
            if (member.control || aliases.contains(member.canvas.get()))
                replacement->members.push_back(member);
        for (int oldMaterial : group->materials) {
            const int material = mapping[oldMaterial];
            if (material >= 0 && retainedCanvases[material] &&
                std::any_of(
                    replacement->members.begin(), replacement->members.end(),
                    [&](const Impl::Member& member) { return member.canvas == retainedCanvases[material]; }))
                replacement->materials.push_back(material);
        }
        // A shared canvas may have been touched only through a now-removed canonical material.
        // Its surviving aliases still require GPU updates when this operation is undone or redone.
        for (const auto& member : replacement->members) {
            if (member.control)
                continue;
            const auto& indices = aliases.at(member.canvas.get());
            replacement->materials.insert(replacement->materials.end(), indices.begin(), indices.end());
        }
        std::sort(replacement->materials.begin(), replacement->materials.end());
        replacement->materials.erase(
            std::unique(replacement->materials.begin(), replacement->materials.end()),
            replacement->materials.end());
        if (replacement->members.empty())
            replacement.reset();
        replacements.emplace(group.get(), replacement);
        return replacement;
    };
    auto remapStack = [&](const std::vector<std::shared_ptr<Impl::Group>>& stack) {
        std::vector<std::shared_ptr<Impl::Group>> result;
        result.reserve(stack.size());
        for (const auto& group : stack)
            if (auto replacement = remapGroup(group))
                result.push_back(std::move(replacement));
        return result;
    };
    auto undo = remapStack(impl->undo);
    auto redo = remapStack(impl->redo);
    auto changed = remapGroup(impl->lastChanged);
    // Publication only swaps owners. Token chains and old group contents remain untouched.
    impl->undo.swap(undo);
    impl->redo.swap(redo);
    impl->lastChanged.swap(changed);
}
void PaintBatchHistory::clear() noexcept {
    cancel();
    impl->undo.clear();
    impl->redo.clear();
}
} // namespace edm
