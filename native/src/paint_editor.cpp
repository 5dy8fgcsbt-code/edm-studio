#include "paint_editor.h"
#include "paint_assembly.h"
#include "paint_stroke_path.h"
#include "paint_batch.h"
#include "paint_auto_mapping.h"
#include "paint_wrap.h"
#include "paint_projection.h"
#include "paint_decal.h"
#include <imgui_stdlib.h>
#include <shobjidl.h>
#include <future>

namespace edm {
namespace {
struct PaintDisabledScope {
    bool active = true;
    explicit PaintDisabledScope(bool disabled) {
        ImGui::BeginDisabled(disabled);
    }
    void end() {
        if (active) {
            ImGui::EndDisabled();
            active = false;
        }
    }
    ~PaintDisabledScope() {
        end();
    }
};
struct PaintDrawClipScope {
    ImDrawList* draw;
    PaintDrawClipScope(ImDrawList* draw, ImVec2 minimum, ImVec2 maximum) : draw(draw) {
        draw->PushClipRect(minimum, maximum, true);
    }
    ~PaintDrawClipScope() {
        draw->PopClipRect();
    }
};
std::optional<fs::path> paintDialog(HWND hwnd, const wchar_t* title, int kind) {
    Com<IFileOpenDialog> dialog;
    require(SUCCEEDED(
                CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))),
            "Cannot open paint file dialog");
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (kind == 1 ? FOS_PICKFOLDERS : 0));
    dialog->SetTitle(title);
    if (kind != 1) {
        COMDLG_FILTERSPEC filter =
            kind == 2
                ? COMDLG_FILTERSPEC{L"EDM 绘制工程", L"*.edmpaint.json"}
                : COMDLG_FILTERSPEC{L"平面模板 / 图片", L"*.png;*.dds;*.tga;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff"};
        dialog->SetFileTypes(1, &filter);
    }
    if (dialog->Show(hwnd) != S_OK)
        return {};
    Com<IShellItem> item;
    require(SUCCEEDED(dialog->GetResult(&item)), "Cannot read selected file");
    PWSTR path = nullptr;
    require(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)), "Cannot read selected path");
    fs::path result = path;
    CoTaskMemFree(path);
    return result;
}
void paintHint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, {.49f, .57f, .68f, 1});
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}
struct EditableGpu {
    Com<ID3D11Texture2D> texture;
    std::shared_ptr<GpuTexture> view;
    uint64_t revision = UINT64_MAX;
    bool mipDirty = false;
    Clock::time_point lastMips = Clock::now();
};
void syncCanvas(Renderer& renderer, PaintCanvas& canvas, EditableGpu& gpu, bool finishing) {
    auto& image = canvas.image();
    if (!gpu.texture) {
        EditableGpu prepared;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = image.width;
        desc.Height = image.height;
        desc.MipLevels = 0;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        require(SUCCEEDED(renderer.device->CreateTexture2D(&desc, nullptr, &prepared.texture)),
                "Cannot allocate editable GPU texture");
        prepared.view = std::make_shared<GpuTexture>();
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = UINT(-1);
        require(SUCCEEDED(renderer.device->CreateShaderResourceView(prepared.texture.Get(), &srv,
                                                                    &prepared.view->view)),
                "Cannot create editable texture view");
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        require(SUCCEEDED(renderer.device->CreateShaderResourceView(prepared.texture.Get(), &srv,
                                                                    &prepared.view->linearView)),
                "Cannot create linear texture view");
        prepared.view->width = int(image.width);
        prepared.view->height = int(image.height);
        prepared.view->bytes = image.rgba.size() * 4 / 3;
        renderer.context->UpdateSubresource(prepared.texture.Get(), 0, nullptr, image.rgba.data(),
                                            image.width * 4, 0);
        canvas.takeDirtyRect();
        prepared.mipDirty = true;
        prepared.revision = canvas.revision();

        gpu = std::move(prepared);
    } else if (canvas.revision() != gpu.revision) {
        if (auto dirty = canvas.takeDirtyRect()) {
            D3D11_BOX box{UINT(dirty->x0), UINT(dirty->y0), 0, UINT(dirty->x1), UINT(dirty->y1), 1};
            auto offset = (size_t(dirty->y0) * image.width + dirty->x0) * 4;
            renderer.context->UpdateSubresource(gpu.texture.Get(), 0, &box, image.rgba.data() + offset,
                                                image.width * 4, 0);
            gpu.mipDirty = true;
        }
        gpu.revision = canvas.revision();
    }
    if (gpu.mipDirty && (finishing || !canvas.strokeActive() || seconds(gpu.lastMips) > .033)) {
        renderer.context->GenerateMips(gpu.view->view.Get());
        gpu.mipDirty = false;
        gpu.lastMips = Clock::now();
    }
}
bool gpuMatchesCanvas(Renderer& renderer, const PaintCanvas& canvas, const EditableGpu& gpu) {
    require(bool(gpu.texture), "Editable GPU texture has not been uploaded");
    D3D11_TEXTURE2D_DESC desc{};
    gpu.texture->GetDesc(&desc);
    desc.MipLevels = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Com<ID3D11Texture2D> staging;
    require(SUCCEEDED(renderer.device->CreateTexture2D(&desc, nullptr, &staging)),
            "Cannot allocate paint GPU readback");
    renderer.context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, gpu.texture.Get(), 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    require(SUCCEEDED(renderer.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)),
            "Cannot map paint GPU readback");
    const auto& image = canvas.image();
    bool equal = true;
    for (uint32_t y = 0; y < image.height; ++y)
        equal &= std::memcmp(static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                             image.rgba.data() + size_t(y) * image.width * 4, size_t(image.width) * 4) == 0;
    renderer.context->Unmap(staging.Get(), 0);
    return equal;
}
Json mappingReport(const PaintMappingReport& report) {
    return {{"changed", report.changed},
            {"painted_pixels", report.paintedPixels},
            {"triangles", report.triangles},
            {"samples", report.rasterSamples},
            {"ray_tests", report.rayTests},
            {"occluded_samples", report.occludedSamples},
            {"backface_samples", report.backfaceSamples},
            {"reused_texels", report.reusedTexels},
            {"seconds", report.seconds},
            {"warnings", report.warnings}};
}
} // namespace
struct PaintEditor::Impl {
    struct Entry {
        std::shared_ptr<PaintCanvas> canvas;
        EditableGpu gpu;
        std::string source;
        std::optional<PaintUVInfo> uv;
    };
    struct Work {
        std::thread thread;
        std::atomic_bool cancel = false, finished = false;
        std::mutex mutex;
        std::string progress, error;
        std::function<void()> done;
        std::function<void()> cleanup;
    };
    std::shared_ptr<const Scene> scene;
    std::shared_ptr<Livery> livery;
    fs::path textureDirectory;
    fs::path recoveryRoot;
    std::map<int, Entry> entries;
    std::shared_ptr<PaintSurface> surface;
    Args currentArgs, surfaceArgs;
    bool attachmentsVisible = true;
    std::vector<std::shared_ptr<Work>> work;
    uint64_t generation = 0;
    int material = -1, tool = 0;
    int paintMaxDimension = 2048;
    PaintBatchHistory history;
    std::atomic_int activeMappingTasks{0};
    std::vector<int> canonicalIds;
    std::shared_ptr<const PaintImage> pendingTemplate;
    std::string pendingTemplateName;
    int pendingTemplateTarget = -1;
    bool wrapAligned = false;
    bool surfacePending = false, preserveAlpha = true;
    float radius = .1f, hardness = .65f, opacity = 1;
    F4 color{.88f, .12f, .09f, 1};
    std::string search, status = "选择画笔，直接在模型上绘制；贴图会自动匹配。", error;
    std::string liveryName = "EDM Studio painted livery";
    std::optional<PaintHit> previousHit;
    ImVec2 previousScreen{};
    size_t strokePixels = 0;
    bool saved = true;
    bool canvasOptionsOpen = true;
    int diagnosticStage = 0;
    Json diagnosticResult;
    std::shared_ptr<const PaintImage> mappingImage;
    std::shared_ptr<GpuTexture> mappingPreview;
    std::string mappingPath;
    int wrapAxis = 0, wrapMaterial = -1;
    F3 wrapCenter{0, 0, 0};
    float wrapHeight = 1, wrapAngle = 0, wrapSweep = 360, wrapOpacity = 1;
    bool wrapOutward = true, wrapOcclusion = true, wrapGuides = true;
    double wrapGuideRadius = 1;
    Json wrapResult, wrapDiagnosticResult;
    int wrapDiagnosticStage = 0;
    std::optional<PaintHit> decalHit;
    V3 decalTangent = V3::UnitX();
    Args decalPose;
    float decalWidth = 1, decalHeight = 1, decalDepth = .2f, decalAngle = 0, decalOpacity = 1;
    bool decalAspectLocked = true, decalFront = true, decalOcclusion = true;
    bool decalOverlay = true, decalSuppressed = false;
    ImVec2 decalScreen{};
    ImVec2 decalSuppressedScreen{};
    Json decalResult;
#include "paint_editor_decal_test_state.inc"
    F2 projectionCenter{.5f, .5f}, projectionSize{.65f, .65f};
    float projectionAngle = 0, projectionOpacity = 1, projectionPreviewOpacity = .38f;
    bool projectionAspectLocked = true, projectionFront = true, projectionOcclusion = true;
    bool projectionOverlay = true, projectionDragging = false;
    ImVec2 projectionDragStart{};
    F2 projectionDragCenter{};
    Json projectionResult, projectionDiagnosticResult;
    int projectionDiagnosticStage = 0;
    struct StrokeContext {
        Mat inverse;
        V3 eye;
        double fallbackDistance = 1;
        ImVec2 origin, size;
        std::shared_ptr<PaintSurface> surface;
        PaintBrush brush;
        int material = 0;
    };
    PaintStrokePath strokePath;
    std::map<uint64_t, StrokeContext> strokeContexts;
    std::shared_ptr<PaintCanvas> strokeCanvas;
    bool strokeWasSaved = true;
    std::shared_ptr<Livery> strokeLivery;
    fs::path strokeTextureDirectory;
    std::set<int> strokeNewCanvases;
    std::map<int, Entry> strokeReplacedCanvases;
    int strokeTemplateIndex = -1;
    bool strokeTemplateInstalled = false;
    uint64_t nextStrokeContext = 0, consumedStrokeSamples = 0;
    size_t strokeFrames = 0, peakStrokeContexts = 0;
    Json strokeResult, strokeDiagnosticResult, projectDiagnosticResult;
    int strokeDiagnosticStage = 0, projectDiagnosticStage = 0;
    int autoStage = 0;
    Json autoResult;
    PaintSnapshot autoBefore, autoAfter;
    uint64_t autoExpectedSamples = 0;
    PaintStrokePoint autoEnd;
    uint64_t expectedStrokeSamples = 0;
    PaintStrokePoint expectedStrokeEnd;
    std::vector<uint8_t> strokeBefore;
    std::vector<PaintStrokePoint> strokeTrace;
    PaintSnapshot projectExpected;
    fs::path lastSavedProject;
    std::optional<PaintProjectDocument> pendingAppearance;
    bool lastOpenRestoredAppearance = false;

    bool strokePending() const {
        return strokePath.started();
    }
    bool controlsBusy() const {
        return busy() || strokePending();
    }
    StrokeContext frameContext(const Renderer& renderer, ImVec2 origin, ImVec2 size) const {
        require(size.x > 0 && size.y > 0 && surface, "当前绘制视口尚未就绪");
        StrokeContext context;
        context.inverse = (renderer.camera.projection(size.x / size.y) * renderer.camera.view()).inverse();
        context.eye = renderer.camera.eye();
        context.fallbackDistance = renderer.camera.distance;
        context.origin = origin;
        context.size = size;
        context.surface = surface;
        context.material = material;
        context.brush.radius = radius;
        context.brush.color = color;
        context.brush.hardness = hardness;
        context.brush.opacity = opacity;
        context.brush.preserveAlpha = preserveAlpha;
        return context;
    }
    uint64_t rememberContext(StrokeContext context) {
        if (!strokeContexts.empty()) {
            auto& previous = strokeContexts.rbegin()->second;
            if (previous.inverse == context.inverse && previous.eye == context.eye &&
                previous.fallbackDistance == context.fallbackDistance &&
                previous.origin.x == context.origin.x && previous.origin.y == context.origin.y &&
                previous.size.x == context.size.x && previous.size.y == context.size.y &&
                previous.surface == context.surface && previous.material == context.material &&
                previous.brush.radius == context.brush.radius &&
                previous.brush.color == context.brush.color &&
                previous.brush.hardness == context.brush.hardness &&
                previous.brush.opacity == context.brush.opacity &&
                previous.brush.preserveAlpha == context.brush.preserveAlpha)
                return strokeContexts.rbegin()->first;
        }
        require(strokeContexts.size() < 4096, "画笔视角快照积压过多，请取消此笔后降低移动速度");
        const auto id = ++nextStrokeContext;
        strokeContexts.emplace(id, std::move(context));
        peakStrokeContexts = std::max(peakStrokeContexts, strokeContexts.size());
        return id;
    }
    static std::optional<PaintHit> strokeHit(const StrokeContext& context, PaintStrokePoint point) {
        double x = (point.x - context.origin.x) / context.size.x;
        double y = (point.y - context.origin.y) / context.size.y;
        if (x < 0 || x > 1 || y < 0 || y > 1)
            return {};
        V4 projected = context.inverse * V4(x * 2 - 1, 1 - y * 2, 1, 1);
        if (!projected.allFinite() || std::abs(projected.w()) < 1e-20)
            return {};
        projected /= projected.w();
        return context.surface->raycast(context.eye, projected.head<3>() - context.eye);
    }
    static double strokeSpacing(const StrokeContext& context, const std::optional<PaintHit>& hit) {
        double distance = hit ? (hit->position - context.eye).norm() : context.fallbackDistance;
        double worldPerPixel = std::max(1e-12, distance * 2 * std::tan(.65 / 2) / context.size.y);
        return std::clamp(context.brush.radius * .35 / worldPerPixel, .5, 64.);
    }
    void beginQueuedStroke(StrokeContext context, PaintStrokePoint point, double spacing) {
        require(!controlsBusy(), "请等待当前画笔完成");
        ensureAliases();
        if (auto hit = strokeHit(context, point)) {
            material = hit->material;
            if (pendingTemplate)
                pendingTemplateTarget = canvasIndex(material);
        }
        strokeWasSaved = saved;
        strokeLivery = livery;
        strokeTextureDirectory = textureDirectory;
        strokeNewCanvases.clear();
        strokeTemplateIndex = -1;
        strokeTemplateInstalled = false;
        strokePixels = consumedStrokeSamples = strokeFrames = peakStrokeContexts = 0;
        strokeContexts.clear();
        strokeResult = Json();
        auto id = rememberContext(std::move(context));
        strokePath.begin(point, spacing, id);
        try {
            history.begin("自动多贴图连续画笔");
        } catch (...) {
            strokePath.cancel();
            strokeContexts.clear();
            throw;
        }
        saved = false;
    }
    void cancelQueuedStroke() {
        if (!strokePending())
            return;
        if (busy())
            stop();
        history.cancel();
        for (int index : strokeNewCanvases)
            entries.erase(index);
        for (auto& [index, entry] : strokeReplacedCanvases)
            entries[index] = std::move(entry);
        strokeReplacedCanvases.clear();
        strokeNewCanvases.clear();
        saved = strokeWasSaved;
        strokePath.cancel();
        strokeContexts.clear();
        strokeCanvas.reset();
        previousHit.reset();
    }
    void drainStroke(size_t maximum = 32, double budget = .012) {
        if (!strokePending() || busy())
            return;
        auto started = Clock::now();
        try {
            size_t emitted = 0;
            while (emitted < maximum) {
                auto next = strokePath.peek();
                if (!next)
                    break;
                const auto& context = strokeContexts.at(next->context);
                auto hit = strokeHit(context, next->point);
                if (hit) {
                    auto ids = brushMaterials(context, *hit);
                    if (!requestCanvases(ids, budget == 0))
                        break;
                    std::map<int, std::vector<int>> groups;
                    for (int id : ids)
                        groups[canvasIndex(id)].push_back(id);
                    for (const auto& [index, targets] : groups) {
                        auto canvas = entries.at(index).canvas;
                        for (int id : targets)
                            history.touch(id, canvas);
                        strokePixels += paintBrush(*canvas, *context.surface, *hit, context.brush,
                                                   context.eye, nullptr, targets.front(), targets)
                                            .pixels;
                    }
                    material = hit->material;
                }
                strokePath.consume(1, [&](const PaintStrokeSample& sample) {
                    ++consumedStrokeSamples;
                    if (strokeDiagnosticStage == 1)
                        strokeTrace.push_back(sample.point);
                });
                ++emitted;
                if (budget > 0 && seconds(started) >= budget)
                    break;
            }
            if (emitted)
                ++strokeFrames;
            if (auto last = strokePath.lastConsumed())
                while (!strokeContexts.empty() && strokeContexts.begin()->first < last->context)
                    strokeContexts.erase(strokeContexts.begin());
            if (strokePath.drained()) {
                auto endpoint = strokePath.lastConsumed()->point;
                bool changed = history.commit();
                // Commit is authoritative. Diagnostic/UI allocation failure must never roll it back.
                strokePath.cancel();
                strokeContexts.clear();
                auto created = std::move(strokeNewCanvases);
                strokeNewCanvases.clear();
                strokeReplacedCanvases.clear();
                if (!changed && !strokeTemplateInstalled)
                    saved = strokeWasSaved;
                for (int index : created)
                    if (index != strokeTemplateIndex &&
                        std::none_of(history.changedMaterials().begin(), history.changedMaterials().end(),
                                     [&](int id) { return canvasIndex(id) == index; }))
                        entries.erase(index);
                std::set<int> changedCanvases;
                for (int id : history.changedMaterials())
                    changedCanvases.insert(canvasIndex(id));
                if (strokeTemplateInstalled)
                    changedCanvases.insert(strokeTemplateIndex);
                for (int index : created)
                    if (!changedCanvases.contains(index))
                        entries.erase(index);
                strokeResult = {{"samples", consumedStrokeSamples},
                                {"painted_pixels", strokePixels},
                                {"frames", strokeFrames},
                                {"changed", changed || strokeTemplateInstalled},
                                {"automatic", true},
                                {"materials", history.changedMaterials()},
                                {"changed_canvases", changedCanvases.size()},
                                {"peak_contexts", peakStrokeContexts},
                                {"pending_samples", 0},
                                {"endpoint", {endpoint.x, endpoint.y}},
                                {"discarded_samples", 0}};
                strokePath.cancel();
                strokeContexts.clear();
                strokeCanvas.reset();
                previousHit.reset();
                status =
                    "笔划完成 · 自动写入 " + std::to_string(changedCanvases.size()) + " 张贴图 · 可一次撤销";
            }
        } catch (...) {
            cancelQueuedStroke();
            throw;
        }
    }
    ~Impl() {
        stop();
    }
    void stop() {
        for (auto& w : work)
            w->cancel = true;
        for (auto& w : work)
            if (w->thread.joinable())
                w->thread.join();
        for (auto& w : work)
            if (w->cleanup)
                w->cleanup();
        work.clear();
    }
    bool busy() const {
        return !work.empty();
    }
    template <class F> void start(F function, bool duringStroke = false, std::function<void()> cleanup = {}) {
        require(!strokePending() || duringStroke, "正在补齐本笔轨迹，请完成后再操作");
        auto item = std::make_shared<Work>();
        item->cleanup = std::move(cleanup);
        work.push_back(item);
        try {
            item->thread = std::thread([item, fn = std::move(function)]() mutable {
                try {
                    ComApartment apartment;
                    Progress p = [item](const std::string& text) {
                        std::lock_guard lock(item->mutex);
                        item->progress = text;
                    };
                    auto done = fn(p, &item->cancel);
                    if (!item->cancel)
                        item->done = std::move(done);
                } catch (...) {
                    if (!item->cancel)
                        item->error = exceptionText();
                }
                item->finished = true;
            });
        } catch (...) {
            work.pop_back();
            throw;
        }
    }
    void poll() {
        for (auto it = work.begin(); it != work.end();) {
            auto item = *it;
            if (!item->finished) {
                ++it;
                continue;
            }
            if (item->thread.joinable())
                item->thread.join();
            it = work.erase(it);
            if (item->cleanup)
                item->cleanup();
            if (!item->cancel) {
                if (!item->error.empty()) {
                    error = item->error;
                    surfacePending = false;
                    tool = 0;
                    cancelQueuedStroke();
                } else if (item->done) {
                    try {
                        item->done();
                    } catch (...) {
                        error = exceptionText();
                        surfacePending = false;
                        tool = 0;
                        cancelQueuedStroke();
                    }
                }
            } else
                cancelQueuedStroke();
            return; // A completion may enqueue the next phase and reallocate work.
        }
    }
    void buildSurface() {
        if (!scene || busy() || strokePending() || pendingAppearance ||
            (surface && surfaceArgs == currentArgs))
            return;
        surfacePending = true;
        auto source = scene;
        auto pose = currentArgs;
        auto visible = attachmentsVisible;
        auto gen = generation;
        start([this, source, pose, gen, visible](Progress p, const std::atomic_bool* cancel) {
            auto result = std::make_shared<PaintSurface>(source, pose, p, cancel, visible);
            return [this, result, pose, gen] {
                if (gen != generation)
                    return;
                surface = result;
                surfaceArgs = pose;
                surfacePending = false;
                status = "绘制表面已就绪 · 相机移动不会重建几何";
            };
        });
    }
    bool paintable(int selected) const {
        return scene && std::any_of(scene->meshes.begin(), scene->meshes.end(), [&](const Mesh& mesh) {
                   return mesh.material == selected && mesh.numbers.empty() && !mesh.uvs.empty() &&
                          mesh.positions.size() >= 3 && mesh.indices.size() >= 3;
               });
    }
#include "paint_editor_auto.inc"
    void loadCanvas(std::optional<fs::path> path) {
        if (!scene || busy())
            return;
        require(paintable(material),
                "编号图集由动态编号控制，请选择机体漫反射材质；当前材质没有可绘制表面。");
        auto source = scene;
        auto skin = livery;
        auto extra = textureDirectory;
        ensureAliases();
        int selected = canvasIndex(material);
        uint64_t gen = generation;
        start([this, source, skin, extra, selected, path, gen](Progress p, const std::atomic_bool* cancel) {
            p("读取绘制模板…");
            PaintImage image;
            std::string imageName;
            if (path) {
                image = PaintImage::load(ImageSource{*path}, 8192);
                imageName = pathString(*path);
            } else {
                TextureResolver resolver(source->source, extra, skin);
                auto texture = resolver.material(source->materials.at(selected));
                if (texture) {
                    image = PaintImage::load(*texture, 8192);
                    imageName = texture->key();
                } else {
                    image = PaintImage(2048, 2048);
                    imageName = "空白 2048×2048 模板";
                }
            }
            require(!*cancel, "Cancelled");
            auto canvas = std::make_shared<PaintCanvas>(std::move(image));
            return [this, selected, gen, canvas, imageName] {
                if (gen != generation)
                    return;
                size_t total = canvas->image().rgba.size();
                for (auto& [index, entry] : entries)
                    if (index != selected)
                        total += entry.canvas->image().rgba.size();
                require(total <= 1024ull * 1024 * 1024,
                        "编辑贴图超过 1 GiB 预算，请先保存工程再开始新材质。");
                entries[selected] = Entry{canvas, {}, imageName, {}};
                history.clear();
                canvasOptionsOpen = false;
                saved = false;
                status = "模板已载入 · 直接在模型表面绘制";
                tool = 1;
            };
        });
    }
    void loadMappingImage(Renderer& renderer, const fs::path& path, int imageTool = 2) {
        require(!busy(), "请等待当前绘制任务完成");
        auto gen = generation;
        start([this, &renderer, path, gen, imageTool](Progress p, const std::atomic_bool* cancel) {
            p("读取图片与预览…");
            auto image = std::make_shared<PaintImage>(PaintImage::load(ImageSource{path}, 8192));
            require(!*cancel, "Cancelled");
            auto preview = loadTexture(ImageSource{path}, 512);
            require(!*cancel, "Cancelled");
            return [this, &renderer, path, gen, imageTool, image, preview] {
                if (gen != generation)
                    return;
                auto uploaded = uploadTexture(renderer.device.Get(), *preview);
                mappingImage = image;
                mappingPreview = std::move(uploaded);
                mappingPath = pathString(path);
                if (imageTool == 2) {
                    decalResult = Json();
                    decalSuppressed = false;
                    decalOverlay = true;
                } else
                    projectionResult = Json();
                projectionOverlay = true;
                status = imageTool == 3 ? "图片已就绪 · 在视口中拖动定位，再应用相机投影"
                                        : "图片画笔已就绪 · 移到模型表面预览，左键单击盖印";
                tool = imageTool;
            };
        });
    }
    void alignWrap() {
        require(surface && surfaceArgs == currentArgs, "当前动画姿态的绘制表面尚未就绪");
        auto [lo, hi] = surface->bounds(-1);
        V3 center = (lo + hi) * .5;
        for (int i = 0; i < 3; ++i)
            wrapCenter[i] = float(center[i]);
        wrapHeight = float(std::max(.001, hi[wrapAxis] - lo[wrapAxis]));
        double a = (hi[(wrapAxis + 1) % 3] - lo[(wrapAxis + 1) % 3]) * .5;
        double b = (hi[(wrapAxis + 2) % 3] - lo[(wrapAxis + 2) % 3]) * .5;
        wrapGuideRadius = std::max(.001, std::sqrt(a * a + b * b));
        wrapMaterial = material;
        wrapAligned = true;
    }
    CylinderWrapOptions wrapOptions() const {
        CylinderWrapOptions options;
        options.material = -1;
        options.center = V3(wrapCenter[0], wrapCenter[1], wrapCenter[2]);
        options.axis = V3::Zero();
        options.axis[wrapAxis] = 1;
        options.radialReference = V3::Zero();
        options.radialReference[(wrapAxis + 1) % 3] = 1;
        options.height = wrapHeight;
        options.angleRadians = wrapAngle * 3.14159265358979323846 / 180.;
        options.sweepRadians = wrapSweep * 3.14159265358979323846 / 180.;
        options.opacity = wrapOpacity;
        options.preserveAlpha = preserveAlpha;
        options.outwardOnly = wrapOutward;
        options.occlusion = wrapOcclusion;
        return options;
    }
    void startWrap(bool verifyUndoRedo = false) {
        if (!wrapAligned)
            alignWrap();
        startAutomaticMapping(wrapOptions(), findWrapMaterials, applyCylindricalWrap, verifyUndoRedo, false);
    }
#include "paint_editor_decal.inc"
    CameraProjectionOptions projectionOptions(const Renderer& renderer) const {
        auto dimensions = renderer.viewportMetrics();
        int width = dimensions.at("width").get<int>(), height = dimensions.at("height").get<int>();
        require(width > 0 && height > 0, "模型视口尚未就绪");
        CameraProjectionOptions options;
        options.material = -1;
        options.viewportAspect = float(width) / height;
        options.viewProjection = renderer.camera.projection(options.viewportAspect) * renderer.camera.view();
        options.eye = renderer.camera.eye();
        options.center = projectionCenter;
        options.size = projectionSize;
        if (projectionAspectLocked && mappingImage)
            options.size[1] =
                options.size[0] * options.viewportAspect * mappingImage->height / mappingImage->width;
        options.rotationRadians = projectionAngle * 3.14159265358979323846 / 180.;
        options.opacity = projectionOpacity;
        options.preserveAlpha = preserveAlpha;
        options.frontFacesOnly = projectionFront;
        options.occlusion = projectionOcclusion;
        return options;
    }
    void startProjection(Renderer& renderer, bool verifyUndoRedo = false) {
        startAutomaticMapping(projectionOptions(renderer), findProjectionMaterials, applyCameraProjection,
                              verifyUndoRedo, true);
    }
    void saveProject(const fs::path& directory) {
        require(!controlsBusy(), "请先完成绘制任务");
        auto images = snapshot();
        auto source = scene;
        auto base = livery;
        auto extra = textureDirectory;
        auto name = liveryName;
        auto gen = generation;
        start([this, directory, images, source, base, extra, name, gen](Progress p,
                                                                        const std::atomic_bool* cancel) {
            auto result = savePaintProject(*source, images, directory, name, base, extra, p, cancel);
            return [this, gen, result] {
                if (gen != generation)
                    return;
                lastSavedProject = wide(result.at("directory").get<std::string>());
                lastSavedProject /= "project.edmpaint.json";
                status = "已保存：" + result.at("directory").get<std::string>();
                saved = true;
            };
        });
    }
    void openProject(Renderer& renderer, const fs::path& path) {
        require(!controlsBusy(), "请先完成绘制任务");
        auto source = renderer.model->scene;
        auto device = renderer.device;
        auto gen = generation;
        start([this, &renderer, source, device, path, gen](Progress p, const std::atomic_bool* cancel) {
            p("读取完整绘制工程与基础涂装…");
            auto restoredScene = restorePaintAssembly(source, path, p, cancel);
            auto manifest = fs::is_directory(path) ? path / "project.edmpaint.json" : path;
            auto document =
                std::make_shared<PaintProjectDocument>(loadPaintDocument(*restoredScene, manifest, cancel));
            auto prepared = restoredScene == source
                                ? std::shared_ptr<GpuModel>{}
                                : GpuModel::prepare(device.Get(), restoredScene, cancel, p);
            auto loaded = std::make_shared<std::map<int, Entry>>();
            for (auto& [i, image] : document->images) {
                require(!*cancel, "Cancelled");
                (*loaded)[i] = Entry{std::make_shared<PaintCanvas>(*image), {}, "绘制工程", {}};
            }
            document->images.clear();
            return [this, &renderer, loaded, document, restoredScene, prepared, gen] {
                if (gen != generation)
                    return;
                if (prepared) {
                    renderer.model = prepared;
                    scene = restoredScene;
                    renderer.model->update(renderer.context.Get(), currentArgs, renderer.options.attachments);
                    surface.reset();
                    surfacePending = false;
                    decalHit.reset();
                    renderer.decalPreview = {};
                }
                entries = std::move(*loaded);
                history.clear();
                pendingTemplate.reset();
                canonicalIds.clear();
                lastOpenRestoredAppearance = document->restoreAppearance;
                if (document->restoreAppearance) {
                    livery = document->livery;
                    textureDirectory = document->textureDirectory;
                }
                ensureAliases();
                // Older projects may explicitly contain different images for same-name materials.
                // Preserve those independently; otherwise merge identical alias images once.
                for (auto it = entries.begin(); it != entries.end();) {
                    int canonical = canvasIndex(it->first);
                    auto first = entries.find(canonical);
                    if (canonical != it->first && first != entries.end()) {
                        const auto& a = it->second.canvas->image();
                        const auto& b = first->second.canvas->image();
                        if (a.width == b.width && a.height == b.height && a.rgba == b.rgba) {
                            it = entries.erase(it);
                            continue;
                        }
                        canonicalIds[it->first] = it->first;
                    } else if (canonical != it->first)
                        canonicalIds[it->first] = it->first;
                    ++it;
                }
                pendingAppearance = std::move(*document);
                saved = true;
                status = "工程已打开 · 基础涂装与绘制内容已恢复";
                for (const auto& warning : pendingAppearance->warnings)
                    status += "\n" + warning;
            };
        });
    }
    PaintSnapshot snapshot() const {
        PaintSnapshot images;
        for (const auto& [index, entry] : entries)
            images[index] = std::make_shared<PaintImage>(entry.canvas->image());
        for (int i = 0; i < int(canonicalIds.size()); ++i)
            if (images.contains(canonicalIds[i]))
                images[i] = images.at(canonicalIds[i]);
        return images;
    }
};
PaintEditor::PaintEditor() : impl(std::make_unique<Impl>()) {}
PaintEditor::~PaintEditor() = default;
void PaintEditor::bind(Renderer& renderer, std::shared_ptr<Livery> livery, const fs::path& directory) {
    if (renderer.model && impl->scene != renderer.model->scene) {
        reset(renderer);
        auto& p = *impl;
        p.scene = renderer.model->scene;
        p.radius = float(std::max(.01, renderer.camera.radius * .006));
        p.decalWidth = p.decalHeight = float(std::max(.02, renderer.camera.radius * .08));
        p.decalDepth = p.decalWidth * .2f;
        wchar_t path[32768];
        GetModuleFileNameW(nullptr, path, 32768);
        p.recoveryRoot = fs::path(path).parent_path() / "PaintRecovery";
    }
    auto& p = *impl;
    if (p.entries.empty() && !p.controlsBusy() && (p.livery != livery || p.textureDirectory != directory))
        p.canonicalIds.clear();
    p.livery = std::move(livery);
    p.textureDirectory = directory;
}
void PaintEditor::reset(Renderer& renderer) {
    impl->stop();
    impl->strokePath.release();
    impl->drainStroke(SIZE_MAX, 0);
    impl = std::make_unique<Impl>();
    renderer.diffuseOverrides.clear();
    renderer.decalPreview = {};
}
void PaintEditor::invalidateSurface(Renderer& renderer) {
    require(!busy(), "请等待当前绘制任务完成");
    auto& p = *impl;
    ++p.generation;
    p.surface.reset();
    p.surfacePending = false;
    p.decalHit.reset();
    p.projectionDragging = false;
    p.attachmentsVisible = renderer.options.attachments;
    renderer.decalPreview = {};
}
void PaintEditor::extendScene(Renderer& renderer) {
    require(renderer.model && !busy(), "无法在绘制期间改变挂载模型");
    auto& p = *impl;
    require(!p.scene || (p.scene->source == renderer.model->scene->source &&
                         p.scene->materials.size() <= renderer.model->scene->materials.size() &&
                         p.scene->meshes.size() <= renderer.model->scene->meshes.size()),
            "外挂场景必须保留已有材质和网格索引");
    p.scene = renderer.model->scene;
    // Existing canonical IDs retain their original first material, so canvas/undo keys remain stable.
    auto oldAliases = p.canonicalIds;
    p.canonicalIds.clear();
    p.ensureAliases();
    std::copy(oldAliases.begin(), oldAliases.end(), p.canonicalIds.begin());
    invalidateSurface(renderer);
    p.status = "外挂已连接 · 可直接在机体和外挂表面绘制";
}
void PaintEditor::quiesce() {
    impl->stop();
    impl->strokePath.release();
    impl->drainStroke(SIZE_MAX, 0);
    for (auto& [i, entry] : impl->entries)
        if (entry.canvas->strokeActive())
            entry.canvas->endStroke();
}
void PaintEditor::compactScene(Renderer& renderer, std::shared_ptr<const Scene> scene,
                               const std::vector<int>& materialMap) {
    require(scene && impl->scene && !busy(), "请等待当前绘制任务完成");
    auto& p = *impl;
    require(materialMap.size() == p.scene->materials.size(), "无效的卸载材质映射");
    p.ensureAliases();
    std::vector<int> aliases(scene->materials.size(), -1);
    std::map<int, int> canonical;
    for (int old = 0; old < int(materialMap.size()); ++old) {
        const int next = materialMap[old];
        require(next >= -1 && next < int(aliases.size()), "无效的卸载材质索引");
        if (next < 0)
            continue;
        require(aliases[next] == -1, "重复的卸载材质索引");
        int key = p.canvasIndex(old);
        int preferred = materialMap.at(key);
        aliases[next] = canonical.try_emplace(key, preferred < 0 ? next : preferred).first->second;
    }
    require(std::find(aliases.begin(), aliases.end(), -1) == aliases.end(), "不完整的卸载材质映射");
    std::map<int, Impl::Entry> entries;
    for (auto [old, next] : canonical)
        if (auto it = p.entries.find(old); it != p.entries.end()) {
            entries.emplace(next, it->second);
            entries.at(next).uv.reset();
        }
    std::vector<std::shared_ptr<PaintCanvas>> retained(aliases.size());
    std::map<int, std::shared_ptr<GpuTexture>> overrides;
    for (int i = 0; i < int(aliases.size()); ++i)
        if (auto it = entries.find(aliases[i]); it != entries.end()) {
            retained[i] = it->second.canvas;
            overrides[i] = it->second.gpu.view;
        }
    auto remap = [&](int index) {
        return index >= 0 && index < int(materialMap.size()) ? materialMap[index] : -1;
    };
    std::string status = "外挂已卸载 · 保留物体的绘制内容与撤销记录已保留";
    p.history.remapMaterials(materialMap, retained);
    p.material = remap(p.material);
    p.pendingTemplateTarget = remap(p.pendingTemplateTarget);
    p.wrapMaterial = remap(p.wrapMaterial);
    p.wrapAligned = false;
    p.entries.swap(entries);
    p.canonicalIds.swap(aliases);
    p.scene = std::move(scene);
    p.saved = p.entries.empty();
    renderer.diffuseOverrides.swap(overrides);
    // Publication after history remapping must not allocate: App still owns the old GPU model
    // until this method returns. Invalidate the idle surface without another throwing check.
    ++p.generation;
    p.surface.reset();
    p.surfacePending = false;
    p.decalHit.reset();
    p.projectionDragging = false;
    p.attachmentsVisible = renderer.options.attachments;
    renderer.decalPreview = {};
    p.status.swap(status);
}
void PaintEditor::tick(Renderer& renderer, const Args& args) {
    auto& p = *impl;
    p.poll();
    p.currentArgs = args;
    if (p.attachmentsVisible != renderer.options.attachments && !busy())
        invalidateSurface(renderer);
    if (p.decalHit && p.decalPose != args)
        p.decalHit.reset();
    p.updateDecalPreview(renderer);
    if (p.tool && renderer.options.editedLivery && !p.busy())
        p.buildSurface();
    if (!p.busy()) {
        renderer.diffuseOverrides.clear();
        for (auto& [index, entry] : p.entries) {
            syncCanvas(renderer, *entry.canvas, entry.gpu, false);
            renderer.diffuseOverrides[index] = entry.gpu.view;
        }
        for (int i = 0; i < int(p.canonicalIds.size()); ++i)
            if (p.entries.contains(p.canonicalIds[i]))
                renderer.diffuseOverrides[i] = p.entries.at(p.canonicalIds[i]).gpu.view;
    }
}
bool PaintEditor::active() const {
    return impl->tool != 0;
}
bool PaintEditor::busy() const {
    return impl->controlsBusy();
}
bool PaintEditor::hasEdits() const {
    return !impl->entries.empty() && !impl->saved;
}
PaintSnapshot PaintEditor::snapshot() const {
    require(!busy(), "绘制任务尚未完成，请稍后导出");
    return impl->snapshot();
}
std::optional<PaintProjectDocument> PaintEditor::takeRestoredAppearance() {
    auto result = std::move(impl->pendingAppearance);
    impl->pendingAppearance.reset();
    return result;
}
void PaintEditor::recover(const fs::path& directory) const {
    if (!hasEdits())
        return;
    require(!busy(), "绘制任务尚未结束");
    auto& p = *impl;
    savePaintRecovery(*p.scene, p.snapshot(), directory, {}, nullptr, p.livery, p.textureDirectory);
}
Json PaintEditor::diagnostic() const {
    return {{"editing", active()},
            {"busy", busy()},
            {"material", impl->material},
            {"canvases", impl->entries.size()},
            {"stroke_pixels", impl->strokePixels},
            {"surface_triangles", impl->surface ? impl->surface->triangleCount() : 0},
            {"surface_attachments_visible", impl->attachmentsVisible},
            {"validation", impl->diagnosticResult},
            {"automatic_validation", impl->autoResult},
            {"wrap", impl->wrapResult},
            {"wrap_validation", impl->wrapDiagnosticResult},
            {"decal", impl->decalResult},
            {"decal_validation", impl->decalDiagnosticResult},
            {"projection", impl->projectionResult},
            {"projection_validation", impl->projectionDiagnosticResult},
            {"stroke_path",
             impl->strokeDiagnosticResult.is_object() ? impl->strokeDiagnosticResult : impl->strokeResult},
            {"pending_stroke_samples", impl->strokePath.pendingSamples()},
            {"project_validation", impl->projectDiagnosticResult},
            {"error", impl->error}};
}
bool PaintEditor::exercise(Renderer& renderer, const std::string& material, const fs::path& output) {
    auto& p = *impl;
    require(p.error.empty(), p.error);
    if (p.diagnosticStage == 0) {
        auto found = std::find_if(p.scene->materials.begin(), p.scene->materials.end(),
                                  [&](const auto& m) { return m.name == material; });
        require(found != p.scene->materials.end(), "Paint diagnostic material not found: " + material);
        p.material = int(found - p.scene->materials.begin());
        p.loadCanvas({});
        p.diagnosticStage = 1;
        return false;
    }
    if (p.diagnosticStage == 2)
        return true;
    if (p.busy() || !p.entries.contains(p.canvasIndex(p.material)) || !p.surface ||
        p.surfaceArgs != p.currentArgs)
        return false;
    auto dimensions = renderer.viewportMetrics();
    float aspect = dimensions["width"].get<float>() / dimensions["height"].get<float>();
    V3 eye;
    std::optional<PaintHit> hit;
    auto findVisibleTarget = [&] {
        Mat inverse = (renderer.camera.projection(aspect) * renderer.camera.view()).inverse();
        eye = renderer.camera.eye();
        for (int ring = 0; ring < 25 && !hit; ++ring)
            for (int x = -ring; x <= ring && !hit; ++x)
                for (int y = -ring; y <= ring && !hit; ++y) {
                    if (ring && std::abs(x) != ring && std::abs(y) != ring)
                        continue;
                    V4 point = inverse * V4(x / 30., y / 30., 1, 1);
                    point /= point[3];
                    auto candidate = p.surface->raycast(eye, (point.head<3>() - eye).normalized());
                    if (candidate && candidate->material == p.material)
                        hit = candidate;
                }
    };
    findVisibleTarget();
    if (!hit) {
        // Diagnostics must locate the requested material even when the initial view faces another side.
        auto [lo, hi] = p.surface->bounds(p.material);
        renderer.camera.target = (lo + hi) * .5;
        renderer.camera.distance = std::max(1., (hi - lo).norm() * 1.3);
        for (double pitch : {.4, -.4, 1.2, -1.2}) {
            for (double yaw : {0., 1.57, 3.14, 4.71}) {
                renderer.camera.pitch = pitch;
                renderer.camera.yaw = yaw;
                findVisibleTarget();
                if (hit)
                    break;
            }
            if (hit)
                break;
        }
    }
    require(bool(hit), "Paint diagnostic target is outside the current camera");
    auto& entry = p.entries.at(p.canvasIndex(p.material));
    auto& canvas = *entry.canvas;
    auto before = canvas.image().rgba;
    PaintBrush brush;
    // A store can be much smaller than the aircraft framing the camera. Keep the diagnostic stamp
    // proportional to its target material so the production interactive UV budget remains meaningful.
    auto [materialMin, materialMax] = p.surface->bounds(p.material);
    brush.radius = std::clamp((materialMax - materialMin).norm() * .005, .005, .1);
    brush.color = {.98f, .03f, .05f, 1};
    canvas.beginStroke("诊断画笔");
    auto started = Clock::now();
    auto stats = paintBrush(canvas, *p.surface, *hit, brush, eye);
    canvas.endStroke();
    double elapsed = seconds(started);
    auto painted = canvas.image().rgba;
    require(stats.pixels > 0 && painted != before, "Diagnostic brush did not modify pixels");
    require(canvas.undo() && canvas.image().rgba == before, "Brush undo did not restore original pixels");
    require(canvas.redo() && canvas.image().rgba == painted, "Brush redo did not restore painted pixels");
    syncCanvas(renderer, canvas, entry.gpu, true);
    renderer.diffuseOverrides[p.material] = entry.gpu.view;
    bool equal = gpuMatchesCanvas(renderer, canvas, entry.gpu);
    require(equal, "Editable GPU texture differs from CPU canvas");
    canvas.image().savePNG(output);
    p.strokePixels = stats.pixels;
    p.saved = false;
    p.status = "画笔 / 撤销 / 重做 / GPU 像素回读已验证";
    p.diagnosticResult = {{"painted_pixels", stats.pixels},
                          {"brush_seconds", elapsed},
                          {"gpu_pixels_match", equal},
                          {"undo_redo_match", true},
                          {"png", pathString(output)}};
    p.diagnosticStage = 2;
    return true;
}
bool PaintEditor::exerciseWrap(Renderer& renderer, const fs::path& image, const fs::path& output) {
    auto& p = *impl;
    require(p.error.empty(), p.error);
    require(p.diagnosticStage == 2, "Complete the brush diagnostic before exercising image wrap");
    if (p.wrapDiagnosticStage == 3)
        return true;
    if (p.busy())
        return false;
    if (p.wrapDiagnosticStage == 0) {
        p.tool = 2;
        p.loadMappingImage(renderer, image);
        p.wrapDiagnosticStage = 1;
        return false;
    }
    if (!p.surface || p.surfaceArgs != p.currentArgs)
        return false;
    if (p.wrapDiagnosticStage == 1) {
        require(bool(p.mappingImage), "Diagnostic wrap source image was not loaded");
        p.alignWrap();
        p.startWrap(true);
        p.wrapDiagnosticStage = 2;
        return false;
    }
    require(p.wrapResult.is_object() && p.wrapResult.value("changed", false),
            "Diagnostic image wrap did not complete");
    auto& entry = p.entries.at(p.canvasIndex(p.material));
    syncCanvas(renderer, *entry.canvas, entry.gpu, true);
    renderer.diffuseOverrides[p.material] = entry.gpu.view;
    bool equal = gpuMatchesCanvas(renderer, *entry.canvas, entry.gpu);
    require(equal, "Wrapped GPU texture differs from CPU canvas");
    entry.canvas->image().savePNG(output);
    p.wrapDiagnosticResult = {{"gpu_pixels_match", true},
                              {"undo_redo_match", true},
                              {"png", pathString(output)},
                              {"source_image", pathString(image)}};
    p.status = "贴花 / 撤销 / 重做 / GPU 像素回读已验证";
    p.wrapDiagnosticStage = 3;
    return true;
}
bool PaintEditor::exerciseProjection(Renderer& renderer, const fs::path& image, const fs::path& output) {
    auto& p = *impl;
    require(p.error.empty(), p.error);
    require(p.diagnosticStage == 2, "Complete the brush diagnostic before exercising camera projection");
    if (p.projectionDiagnosticStage == 3)
        return true;
    if (p.busy())
        return false;
    if (p.projectionDiagnosticStage == 0) {
        p.tool = 3;
        p.loadMappingImage(renderer, image, 3);
        p.projectionDiagnosticStage = 1;
        return false;
    }
    if (!p.surface || p.surfaceArgs != p.currentArgs)
        return false;
    if (p.projectionDiagnosticStage == 1) {
        require(bool(p.mappingImage), "Diagnostic projection source image was not loaded");
        // Cover the full live viewport so every target visible to the preceding brush test is tested.
        p.projectionCenter = {.5f, .5f};
        p.projectionSize = {1, 1};
        p.projectionAspectLocked = false;
        p.projectionAngle = 0;
        p.startProjection(renderer, true);
        p.projectionDiagnosticStage = 2;
        return false;
    }
    require(p.projectionResult.is_object() && p.projectionResult.value("changed", false),
            "Diagnostic camera projection did not complete");
    auto& entry = p.entries.at(p.canvasIndex(p.material));
    syncCanvas(renderer, *entry.canvas, entry.gpu, true);
    renderer.diffuseOverrides[p.material] = entry.gpu.view;
    require(gpuMatchesCanvas(renderer, *entry.canvas, entry.gpu),
            "Projected GPU texture differs from CPU canvas");
    entry.canvas->image().savePNG(output);
    p.projectionDiagnosticResult = {{"gpu_pixels_match", true},
                                    {"undo_redo_match", true},
                                    {"png", pathString(output)},
                                    {"source_image", pathString(image)}};
    p.projectionOverlay = false;
    p.status = "相机投影 / 撤销 / 重做 / GPU 像素回读已验证";
    p.projectionDiagnosticStage = 3;
    return true;
}
bool PaintEditor::exerciseStroke(Renderer& renderer) {
    auto& p = *impl;
    require(p.error.empty(), p.error);
    if (p.strokeDiagnosticStage == 2)
        return true;
    require(p.diagnosticStage == 2, "Complete the brush diagnostic before exercising queued strokes");
    if (p.busy())
        return false;
    if (p.strokeDiagnosticStage == 0) {
        if (!p.surface || p.surfaceArgs != p.currentArgs)
            return false;
        auto dimensions = renderer.viewportMetrics();
        ImVec2 size{dimensions.at("width").get<float>(), dimensions.at("height").get<float>()};
        auto context = p.frameContext(renderer, {0, 0}, size);
        context.brush.radius = std::max(.01, renderer.camera.radius * .004);
        context.brush.color = {.03f, .95f, .12f, 1};
        context.brush.hardness = .8f;
        std::optional<PaintStrokePoint> start;
        for (int ring = 0; ring < 25 && !start; ++ring)
            for (int x = -ring; x <= ring && !start; ++x)
                for (int y = -ring; y <= ring && !start; ++y) {
                    PaintStrokePoint point{(.5 + x / 60.) * size.x, (.5 + y / 60.) * size.y};
                    auto hit = Impl::strokeHit(context, point);
                    if (hit && hit->material == p.material)
                        start = point;
                }
        require(start.has_value(), "Queued stroke diagnostic cannot find selected material in viewport");
        p.strokeBefore = p.entries.at(p.canvasIndex(p.material)).canvas->image().rgba;
        p.strokeTrace.clear();
        p.beginQueuedStroke(context, *start, .5);
        auto id = p.strokeContexts.rbegin()->first;
        p.strokePath.append({start->x + 24, start->y}, .5, id);
        p.strokePath.append({start->x + 24, start->y + 24}, .5, id);
        p.expectedStrokeEnd = {start->x + 8, start->y + 24};
        p.strokePath.append(p.expectedStrokeEnd, .5, id);
        p.expectedStrokeSamples = p.strokePath.pendingSamples();
        p.strokePath.release();
        p.tool = 0; // Simulate released input; the normal viewport path must still drain.
        p.strokeDiagnosticStage = 1;
        return false;
    }
    if (p.strokePending())
        return false;
    auto& entry = p.entries.at(p.canvasIndex(p.material));
    auto& canvas = *entry.canvas;
    auto after = canvas.image().rgba;
    require(p.strokeResult.is_object() && p.strokeResult.at("samples") == p.expectedStrokeSamples &&
                p.strokeTrace.size() == p.expectedStrokeSamples && p.expectedStrokeSamples > 32,
            "Queued stroke dropped input samples");
    require(p.strokeResult.at("frames").get<size_t>() > 1,
            "Released stroke must drain over multiple normal viewport frames");
    require(p.strokeTrace.back().x == p.expectedStrokeEnd.x &&
                p.strokeTrace.back().y == p.expectedStrokeEnd.y,
            "Queued stroke missed the released endpoint");
    require(after != p.strokeBefore, "Queued stroke did not paint actual model texels");
    require(p.history.undo() && canvas.image().rgba == p.strokeBefore, "Queued stroke undo mismatch");
    require(p.history.redo() && canvas.image().rgba == after, "Queued stroke redo mismatch");
    syncCanvas(renderer, canvas, entry.gpu, true);
    renderer.diffuseOverrides[p.material] = entry.gpu.view;
    require(gpuMatchesCanvas(renderer, canvas, entry.gpu), "Queued stroke GPU readback mismatch");
    p.strokeDiagnosticResult = p.strokeResult;
    p.strokeDiagnosticResult["expected_samples"] = p.expectedStrokeSamples;
    p.strokeDiagnosticResult["released_before_drain"] = true;
    p.strokeDiagnosticResult["undo_redo_match"] = true;
    p.strokeDiagnosticResult["gpu_pixels_match"] = true;
    p.strokeBefore.clear();
    p.strokeBefore.shrink_to_fit();
    p.strokeTrace.clear();
    p.strokeDiagnosticStage = 2;
    return true;
}
bool PaintEditor::exerciseProject(Renderer& renderer, const fs::path& directory) {
    auto& p = *impl;
    require(p.error.empty(), p.error);
    require(p.diagnosticStage == 2, "Complete the brush diagnostic before saving a paint project");
    if (p.projectDiagnosticStage == 3)
        return true;
    if (p.controlsBusy())
        return false;
    if (p.projectDiagnosticStage == 0) {
        p.projectExpected = p.snapshot();
        p.saveProject(directory);
        p.projectDiagnosticStage = 1;
        return false;
    }
    if (p.projectDiagnosticStage == 1) {
        require(!p.lastSavedProject.empty() && fs::exists(p.lastSavedProject),
                "Full project diagnostic did not publish its manifest");
        if (!p.scene->attachments.empty()) {
            // Exercise opening a saved assembly from only its base EDM, as after restarting the app.
            auto base = Scene::load(p.scene->source);
            renderer.model = GpuModel::prepare(renderer.device.Get(), base);
            renderer.model->update(renderer.context.Get(), p.currentArgs, renderer.options.attachments);
            p.scene = base;
            p.entries.clear();
            p.canonicalIds.clear();
            p.surface.reset();
            p.material = -1;
            renderer.diffuseOverrides.clear();
            p.projectDiagnosticResult["assembly_rebuilt_from_base"] = true;
        }
        p.openProject(renderer, p.lastSavedProject);
        p.projectDiagnosticStage = 2;
        return false;
    }
    require(p.lastOpenRestoredAppearance, "Project reopen did not produce a full appearance event");
    size_t checked = 0;
    for (auto& [material, expected] : p.projectExpected) {
        require(p.entries.contains(p.canvasIndex(material)), "Project reopen lost an edited material");
        auto& entry = p.entries.at(p.canvasIndex(material));
        require(entry.canvas->image().width == expected->width &&
                    entry.canvas->image().height == expected->height &&
                    entry.canvas->image().rgba == expected->rgba,
                "Project reopen changed paint pixels");
        syncCanvas(renderer, *entry.canvas, entry.gpu, true);
        renderer.diffuseOverrides[material] = entry.gpu.view;
        require(gpuMatchesCanvas(renderer, *entry.canvas, entry.gpu),
                "Reopened project GPU readback mismatch");
        ++checked;
    }
    p.projectDiagnosticResult = {{"project", pathString(p.lastSavedProject)},
                                 {"assembly_rebuilt_from_base",
                                  p.projectDiagnosticResult.is_object() &&
                                      p.projectDiagnosticResult.value("assembly_rebuilt_from_base", false)},
                                 {"restored_attachments", p.scene->attachments.size()},
                                 {"materials_checked", checked},
                                 {"pixels_match", true},
                                 {"gpu_pixels_match", true},
                                 {"appearance_event", true},
                                 {"warnings", Json::array()}};
    if (p.pendingAppearance)
        p.projectDiagnosticResult["warnings"] = p.pendingAppearance->warnings;
    p.projectExpected.clear();
    p.projectDiagnosticStage = 3;
    return true;
}

#include "paint_editor_panel.inc"
#include "paint_editor_auto_test.inc"
#include "paint_editor_preview_test.inc"
#include "paint_editor_decal_test.inc"
bool PaintEditor::viewport(Renderer& renderer, ImVec2 origin, ImVec2 size, bool hovered) {
    auto& p = *impl;
    auto& io = ImGui::GetIO();
    bool consume = renderer.options.editedLivery && p.tool != 0 && !io.KeyAlt;
    try {
        if (p.strokePending()) {
            if (!p.strokePath.released()) {
                bool validFrame = renderer.options.editedLivery && p.tool == 1 && p.surface &&
                                  p.surfaceArgs == p.currentArgs && size.x > 1 && size.y > 1 && !io.KeyAlt &&
                                  !io.KeyCtrl && !ImGui::IsMouseDown(ImGuiMouseButton_Right) &&
                                  !ImGui::IsMouseDown(ImGuiMouseButton_Middle);
                if (validFrame) {
                    auto context = p.frameContext(renderer, origin, size);
                    PaintStrokePoint point{io.MousePos.x, io.MousePos.y};
                    auto hit = Impl::strokeHit(context, point);
                    auto spacing = Impl::strokeSpacing(context, hit);
                    auto id = p.rememberContext(std::move(context));
                    p.strokePath.append(point, spacing, id);
                }
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || !validFrame)
                    p.strokePath.release();
            }
            // Always drain released strokes, including when the pointer has left the viewport.
            p.drainStroke();
            if (hovered && renderer.options.editedLivery)
                ImGui::GetWindowDrawList()->AddCircle(io.MousePos, 10, IM_COL32(90, 185, 255, 240), 32, 2);
            return consume;
        }
        if (!renderer.options.editedLivery) {
            p.projectionDragging = false;
            return false;
        }
        if (p.busy())
            return consume;
        if (p.tool != 3 || !ImGui::IsMouseDown(ImGuiMouseButton_Left) || io.KeyAlt)
            p.projectionDragging = false;
        if (p.tool == 3) {
            if (!p.mappingImage || !p.mappingPreview || !p.projectionOverlay || size.x <= 1 || size.y <= 1)
                return consume;
            auto options = p.projectionOptions(renderer);
            double cosine = std::cos(options.rotationRadians), sine = std::sin(options.rotationRadians);
            double mouseX =
                ((io.MousePos.x - origin.x) / size.x - options.center[0]) * options.viewportAspect;
            double mouseY = (io.MousePos.y - origin.y) / size.y - options.center[1];
            double localX = cosine * mouseX + sine * mouseY, localY = -sine * mouseX + cosine * mouseY;
            bool inside = std::abs(localX) <= options.size[0] * options.viewportAspect * .5 &&
                          std::abs(localY) <= options.size[1] * .5;
            if (hovered && inside && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                p.projectionDragging = true;
                p.projectionDragStart = io.MousePos;
                p.projectionDragCenter = p.projectionCenter;
            }
            if (p.projectionDragging) {
                p.projectionCenter = {
                    std::clamp(p.projectionDragCenter[0] + (io.MousePos.x - p.projectionDragStart.x) / size.x,
                               -.5f, 1.5f),
                    std::clamp(p.projectionDragCenter[1] + (io.MousePos.y - p.projectionDragStart.y) / size.y,
                               -.5f, 1.5f)};
                options.center = p.projectionCenter;
            }
            // This is the inverse of the source-coordinate mapping in applyCameraProjection(),
            // including physical viewport aspect and clockwise screen rotation.
            auto corner = [&](double u, double v) {
                double x = (u - .5) * options.size[0] * options.viewportAspect;
                double y = (v - .5) * options.size[1];
                double rotatedX = cosine * x - sine * y, rotatedY = sine * x + cosine * y;
                return ImVec2(origin.x +
                                  float(options.center[0] + rotatedX / options.viewportAspect) * size.x,
                              origin.y + float(options.center[1] + rotatedY) * size.y);
            };
            std::array<ImVec2, 4> corners{corner(0, 0), corner(1, 0), corner(1, 1), corner(0, 1)};
            auto* draw = ImGui::GetWindowDrawList();
            PaintDrawClipScope clip(draw, origin, {origin.x + size.x, origin.y + size.y});
            int alpha = int(255 * std::clamp(p.projectionPreviewOpacity * p.projectionOpacity, 0.f, 1.f));
            draw->AddImageQuad(p.mappingPreview->view.Get(), corners[0], corners[1], corners[2], corners[3],
                               {0, 0}, {1, 0}, {1, 1}, {0, 1}, IM_COL32(255, 255, 255, alpha));
            ImU32 border = p.projectionDragging ? IM_COL32(251, 199, 117, 245) : IM_COL32(114, 202, 253, 235);
            for (int i = 0; i < 4; ++i) {
                draw->AddLine(corners[i], corners[(i + 1) % 4], border, 1.7f);
                draw->AddCircleFilled(corners[i], 3.5f, border);
            }
            ImVec2 center{origin.x + options.center[0] * size.x, origin.y + options.center[1] * size.y};
            draw->AddLine({center.x - 5, center.y}, {center.x + 5, center.y}, border, 1.5f);
            draw->AddLine({center.x, center.y - 5}, {center.x, center.y + 5}, border, 1.5f);
            draw->AddText({origin.x + 16, origin.y + 38}, IM_COL32(178, 217, 240, 240),
                          "图片定位预览 · 左键拖动 · 自动写入覆盖位置的贴图");
            return consume;
        }
        if (p.tool == 2) {
            p.decalViewport(renderer, origin, size, hovered);
            return consume;
        }
        if (p.tool != 1 && p.tool != 4)
            return consume;
        if (!hovered || io.KeyAlt || !p.surface || p.surfaceArgs != p.currentArgs)
            return consume;
        if (size.x <= 1 || size.y <= 1)
            return consume;
        auto context = p.frameContext(renderer, origin, size);
        PaintStrokePoint point{io.MousePos.x, io.MousePos.y};
        auto hit = Impl::strokeHit(context, point);
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddCircle(io.MousePos, 10, hit ? IM_COL32(90, 185, 255, 240) : IM_COL32(245, 146, 82, 230), 32,
                        2);
        if (hit)
            p.material = hit->material;
        if (p.tool != 1 || !hit || !ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
            ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle))
            return consume;
        auto spacing = Impl::strokeSpacing(context, hit);
        p.beginQueuedStroke(std::move(context), point, spacing);
        p.drainStroke();
        return consume;
    } catch (...) {
        p.cancelQueuedStroke();
        if (!p.busy())
            for (auto& [i, entry] : p.entries)
                if (entry.canvas->strokeActive())
                    entry.canvas->cancelStroke();
        p.error = exceptionText();
        return consume;
    }
}
} // namespace edm
