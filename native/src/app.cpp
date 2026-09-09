#include "renderer.h"
#include "export.h"
#include "animation_analysis.h"
#include "paint_editor.h"
#include "attachment.h"
#include "resource.h"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <imgui_stdlib.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <deque>
#include <condition_variable>
#include <sstream>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace edm {
namespace {
ImVec4 accent{.29f, .58f, 1, 1}, muted{.49f, .57f, .68f, 1}, green{.36f, .81f, .66f, 1};
void mutedText(const std::string& s) {
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    ImGui::TextWrapped("%s", s.c_str());
    ImGui::PopStyleColor();
}
void space(float pixels = 8) {
    ImGui::Dummy({0, pixels});
}
bool primary(const char* text, ImVec2 size = {0, 0}) {
    ImGui::PushStyleColor(ImGuiCol_Button, {.19f, .41f, .81f, 1});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {.25f, .51f, .96f, 1});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, {.15f, .35f, .72f, 1});
    bool clicked = ImGui::Button(text, size);
    ImGui::PopStyleColor(3);
    return clicked;
}
void title(const char* text, const char* subtitle = nullptr) {
    ImGui::PushFont(nullptr, 21);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    if (subtitle)
        mutedText(subtitle);
    space(8);
}
std::string compactNumber(size_t n) {
    std::ostringstream s;
    if (n >= 1000000) {
        s.precision(2);
        s << std::fixed << n / 1000000. << " M";
    } else if (n >= 1000) {
        s.precision(1);
        s << std::fixed << n / 1000. << " K";
    } else
        s << n;
    return s.str();
}
double previewBudgetGB(double value) {
    return std::isfinite(value) ? std::clamp(value, .25, 100.) : 2.;
}
constexpr const wchar_t* exportExtensions[]{L"glb", L"gltf", L"obj", L"fbx"};
std::optional<fs::path> dialog(HWND hwnd, bool save, bool folder, const wchar_t* title,
                               const wchar_t* extension = L"edm", const wchar_t* initial = nullptr) {
    Com<IFileDialog> d;
    HRESULT hr =
        save ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))
             : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d));
    require(SUCCEEDED(hr), "Cannot open file dialog");
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
    d->SetTitle(title);
    if (!folder) {
        COMDLG_FILTERSPEC filters[4];
        UINT count = 1;
        if (std::wstring_view(extension) == L"edm")
            filters[0] = {L"DCS 模型 (*.edm)", L"*.edm"};
        else if (save) {
            filters[0] = {L"GLB 单文件模型 (*.glb)", L"*.glb"};
            filters[1] = {L"glTF 嵌入式模型 (*.gltf)", L"*.gltf"};
            filters[2] = {L"OBJ 当前姿态 + MTL 贴图 (*.obj)", L"*.obj"};
            filters[3] = {L"FBX 模型与动画 (*.fbx)", L"*.fbx"};
            count = 4;
        } else
            filters[0] = {L"DCS 涂装 (description.lua; *.zip)", L"*.lua;*.zip"};
        d->SetFileTypes(count, filters);
        if (save && count == 4)
            for (UINT i = 0; i < 4; ++i)
                if (std::wstring_view(extension) == exportExtensions[i])
                    d->SetFileTypeIndex(i + 1);
        d->SetDefaultExtension(extension);
        if (initial)
            d->SetFileName(initial);
    }
    if (d->Show(hwnd) != S_OK)
        return {};
    Com<IShellItem> item;
    d->GetResult(&item);
    PWSTR path = nullptr;
    item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    fs::path result = path;
    CoTaskMemFree(path);
    return result;
}
struct Job {
    std::atomic_bool cancel = false, finished = false;
    std::thread thread;
    std::string kind;
    std::mutex mutex;
    std::string progress;
    ~Job() {
        cancel = true;
        if (thread.joinable())
            thread.join();
    }
};
struct TexturePending {
    std::string key;
    std::shared_ptr<TextureImage> image;
};
struct TextureStream {
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<TexturePending> queue;
    std::vector<std::array<std::string, 3>> bindings;
    std::vector<std::string> warnings, missing;
    Json resolved;
    bool finished = false;
    size_t total = 0;
    double seconds = 0;
};
struct Loaded {
    std::shared_ptr<GpuModel> model;
    Camera camera;
    double seconds;
};
class App {
  public:
    HWND hwnd = nullptr;
    Renderer renderer;
    PaintEditor paint;
    std::vector<std::shared_ptr<Job>> jobs;
    std::mutex completionsMutex;
    std::deque<std::function<void()>> completions;
    uint64_t generation = 0, textureGeneration = 0;
    std::shared_ptr<TextureStream> stream;
    std::shared_ptr<Catalog> catalog;
    std::shared_ptr<Livery> livery;
    std::shared_ptr<AnimationAnalysis> analysis;
    bool analyzing = false, highlightMotion = false;
    int lastHighlight = -2;
    int initialHighlight = -1;
    Args args, baseline, initialArgs;
    std::vector<fs::path> extraRoots;
    fs::path textureDir, settingsPath;
    Json settings = Json::object(), luaContext = Json::object();
    std::string contextText = "{}", error, notice = "拖入 EDM 文件，开始查看模型", argSearch, liverySearch,
                bortText, log;
    int selectedArg = -1, exportMode = 0, exportFormat = 0, rightTab = 0, textureQuality = 2;
    double textureBudgetGB = 2, textureBudgetInputGB = 2;
    std::optional<double> initialTextureBudget;
    fs::path diagnosticSettings;
    float duration = 3, leftWidth = 262, rightWidth = 300;
    bool playing = false, exportTextures = true, showPanels = true, poseDirty = false, shutdown = false,
         loading = false, exporting = false, scanning = false, openContext = false, openRoots = false;
    double playTime = 0, loadSeconds = 0;
    fs::path currentPath;
    std::vector<int> filteredArgs;
    std::map<int, std::string> argNames;
    bool cameraMoving = false;
    bool attaching = false;
    int hoveredConnector = -1;
    int selectedConnector = -1;
    ImVec2 connectorPopupPosition{}, unloadButtonPosition{};
    bool connectorPopupVisible = false;
    std::shared_ptr<const Scene> connectorSelectionScene;
    std::string detachmentTest;
    int detachmentTestStage = 0;
    PaintSnapshot detachmentPaintBefore;
    std::shared_ptr<const Scene> detachmentSceneBefore;
    Args detachmentArgsBefore;
    Json detachmentTestReport, lastDetachment;
    std::optional<ImVec2> diagnosticMousePosition;
    std::optional<bool> diagnosticMouseDown;
    std::vector<std::pair<std::string, fs::path>> initialAttachments;
    Json connectorMarkers = Json::array();
    std::optional<std::pair<std::string, fs::path>> attachmentTest;
    int attachmentTestStage = 0;
    PaintSnapshot attachmentPaintBefore;
    size_t attachmentCountBefore = 0, attachmentSurfaceTriangles = 0;
    Json attachmentTestReport;
    std::vector<std::string> importEntries;
    fs::path importPath;
    bool openImport = false;
    fs::path smokePath, capturePath, metricsPath, initialLivery;
    std::string initialBort;
    std::string paintMaterial;
    fs::path paintResult;
    fs::path wrapImage, wrapResult;
    fs::path projectionImage, projectionResult;
    fs::path paintProjectDirectory;
    fs::path layersTestDirectory;
    fs::path autoPaintDirectory, autoPaintImage;
    int autoPaintDimension = 512;
    fs::path decalTestDirectory, decalTestImage;
    int decalTestDimension = 2048;
    bool decalTestConforming = false;
    bool strokeTest = false;
    bool smoke = false, noAutoScan = false, noTextures = false, verifyGPU = false;
    Json gpuReport = Json::array();
    int benchmarkFrames = 0;
    std::vector<double> frames;
    uint64_t benchUploads = 0, benchEvals = 0;
    bool benchStarted = false;
    double readyTime = 0;
    double textureReadyTime = 0;
    Clock::time_point launched = Clock::now();
    ~App() {
        shutdown = true;
        for (auto& job : jobs)
            job->cancel = true;
        if (stream)
            stream->condition.notify_all();
        for (auto& job : jobs)
            if (job->thread.joinable())
                job->thread.join();
        jobs.clear();
        saveSettings();
    }
    void record(const std::string& text) {
        log += text + "\n";
        if (log.size() > 50000)
            log.erase(0, log.size() - 40000);
    }
    template <class F, class D> void launch(const std::string& kind, F fn, D done) {
        auto job = std::make_shared<Job>();
        job->kind = kind;
        jobs.push_back(job);
        job->thread = std::thread([this, job, kind, fn = std::move(fn), done = std::move(done)]() mutable {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            Progress progress = [job](const std::string& s) {
                std::lock_guard lock(job->mutex);
                job->progress = s;
            };
            try {
                auto result = fn(progress, &job->cancel);
                if (!job->cancel) {
                    std::lock_guard lock(completionsMutex);
                    completions.push_back(
                        [job, done = std::move(done), result = std::move(result)]() mutable {
                            if (!job->cancel)
                                done(std::move(result));
                        });
                }
            } catch (...) {
                std::string message = exceptionText();
                if (!job->cancel) {
                    std::lock_guard lock(completionsMutex);
                    completions.push_back([this, job, message, kind] {
                        if (job->cancel)
                            return;
                        error = message;
                        record(kind + ": " + message);
                        if (kind == "model")
                            loading = false;
                        if (kind == "attachment")
                            attaching = false;
                        if (kind == "export")
                            exporting = false;
                        if (kind == "catalog")
                            scanning = false;
                        if (kind == "analysis")
                            analyzing = false;
                    });
                }
            }
            job->finished = true;
            CoUninitialize();
        });
    }
    void cancelKind(const std::string& kind) {
        for (auto& job : jobs)
            if (job->kind == kind)
                job->cancel = true;
    }
    void poll() {
        std::deque<std::function<void()>> pending;
        {
            std::lock_guard lock(completionsMutex);
            pending.swap(completions);
        }
        for (auto& fn : pending)
            fn();
        for (auto it = jobs.begin(); it != jobs.end();)
            if ((*it)->finished) {
                if ((*it)->thread.joinable())
                    (*it)->thread.join();
                if ((*it)->kind == "export" && (*it)->cancel) {
                    exporting = false;
                    notice = "导出任务已停止";
                }
                it = jobs.erase(it);
            } else
                ++it;
    }
    void restore() {
        wchar_t exe[32768];
        GetModuleFileNameW(nullptr, exe, 32768);
        settingsPath = fs::path(exe).parent_path() / "edm-studio.settings.json";
        if (smoke && diagnosticSettings.empty())
            return;
        if (smoke)
            settingsPath = diagnosticSettings;
        try {
            if (fs::exists(settingsPath))
                settings = Json::parse(readFile(settingsPath, 4000000));
            if (settings.contains("extra_roots"))
                for (auto& p : settings["extra_roots"])
                    extraRoots.emplace_back(wide(p.get<std::string>()));
            textureDir = wide(settings.value("texture_directory", ""));
            textureQuality = std::clamp(settings.value("texture_quality", 2), 0, 3);
            exportFormat = std::clamp(settings.value("export_format", 0), 0, 3);
            if (auto budget = settings.find("texture_budget_gb");
                budget != settings.end() && budget->is_number())
                textureBudgetGB = previewBudgetGB(budget->get<double>());
            textureBudgetInputGB = textureBudgetGB;
            leftWidth = settings.value("left_width", 262.f);
            rightWidth = settings.value("right_width", 300.f);
        } catch (...) {
            record("设置读取失败：" + exceptionText());
        }
    }
    void saveSettings() {
        if ((smoke && diagnosticSettings.empty()) || settingsPath.empty())
            return;
        try {
            settings["extra_roots"] = Json::array();
            for (auto& p : extraRoots)
                settings["extra_roots"].push_back(pathString(p));
            settings["texture_directory"] = pathString(textureDir);
            settings["texture_quality"] = textureQuality;
            settings["export_format"] = exportFormat;
            settings["texture_budget_gb"] = textureBudgetGB;
            settings["left_width"] = leftWidth;
            settings["right_width"] = rightWidth;
            writeJson(settingsPath, settings);
        } catch (...) {
        }
    }
    void openModel(const fs::path& path) {
        paint.quiesce();
        if (paint.hasEdits())
            paint.recover(settingsPath.parent_path() / "PaintRecovery");
        paint.reset(renderer);
        cancelKind("model");
        cancelKind("attachment");
        attaching = false;
        hoveredConnector = -1;
        selectedConnector = -1;
        connectorSelectionScene.reset();
        cancelKind("catalog");
        cancelKind("textures");
        cancelKind("livery");
        cancelKind("analysis");
        analysis.reset();
        analyzing = false;
        lastHighlight = -2;
        generation++;
        textureGeneration++;
        stream.reset();
        catalog.reset();
        livery.reset();
        renderer.textures = {};
        renderer.model.reset();
        args.clear();
        baseline.clear();
        selectedArg = -1;
        argNames.clear();
        playing = false;
        loading = true;
        scanning = false;
        currentPath = path;
        error.clear();
        notice = "正在读取 " + pathString(path.filename());
        uint64_t gen = generation;
        auto device = renderer.device;
        auto attachments = initialAttachments;
        launch(
            "model",
            [path, device, attachments](Progress p, const std::atomic_bool* cancel) {
                auto start = Clock::now();
                auto scene = Scene::load(path, p, cancel);
                for (const auto& [connector, childPath] : attachments) {
                    require(!*cancel, "Cancelled");
                    int target = connectorIndex(*scene, connector);
                    auto child = Scene::load(childPath, p, cancel);
                    scene = attachScene(*scene, *child, target);
                }
                auto gpu = GpuModel::prepare(device.Get(), scene, cancel, p);
                Camera camera;
                camera.fit(*scene);
                return Loaded{gpu, camera, seconds(start)};
            },
            [this, gen](Loaded loaded) {
                if (gen != generation)
                    return;
                loading = false;
                renderer.model = std::move(loaded.model);
                renderer.camera = loaded.camera;
                baseline = renderer.model->scene->defaultArgs;
                args = baseline;
                renderer.model->update(renderer.context.Get(), args, renderer.options.attachments);
                loadSeconds = loaded.seconds;
                auto& scene = *renderer.model->scene;
                if (!scene.limits.empty())
                    selectedArg = scene.limits.begin()->first;
                if (scene.limits.contains(initialHighlight)) {
                    selectedArg = initialHighlight;
                    highlightMotion = true;
                }
                for (auto& track : scene.tracks) {
                    auto name = scene.nodes[track.node].name;
                    auto& names = argNames[track.arg];
                    if (names.size() < 500 && names.find(name) == std::string::npos)
                        names += name + "\n";
                }
                notice = (scene.collisionOnly() ? "碰撞模型已载入 · " : "模型已载入 · ") +
                         std::to_string(scene.meshes.size()) + " 个网格";
                record(scene.summary().dump(2));
                applyInitialArguments();
                if (!paintMaterial.empty() || !autoPaintDirectory.empty() || !decalTestDirectory.empty() ||
                    !layersTestDirectory.empty())
                    rightTab = 3;
                else if (scene.collisionOnly())
                    rightTab = 2;
                startAnalysis();
                startTextures();
                if (!noAutoScan && !scene.collisionOnly())
                    scan();
                if (!initialLivery.empty())
                    selectLivery(initialLivery);
            });
    }
    static int connectorIndex(const Scene& scene, const std::string& name) {
        return findConnector(scene, name);
    }
    void attachModel(int target, const fs::path& path) {
        require(renderer.model && !loading && !attaching && !paint.busy(), "请等待模型与绘制任务完成");
        auto source = renderer.model->scene;
        require(target >= 0 && target < int(source->nodes.size()), "无效挂点");
        attaching = true;
        playing = false;
        const uint64_t gen = generation;
        auto device = renderer.device;
        notice = "正在连接 " + pathString(path.filename()) + " → " + source->nodes[target].name;
        launch(
            "attachment",
            [source, target, path, device](Progress progress, const std::atomic_bool* cancel) {
                auto child = Scene::load(path, progress, cancel);
                require(!*cancel, "Cancelled");
                auto combined = attachScene(*source, *child, target);
                return GpuModel::prepare(device.Get(), combined, cancel, progress);
            },
            [this, gen](std::shared_ptr<GpuModel> model) {
                if (gen != generation)
                    return;
                renderer.model = std::move(model);
                for (auto [argument, value] : renderer.model->scene->defaultArgs) {
                    baseline.try_emplace(argument, value);
                    args.try_emplace(argument, value);
                }
                renderer.options.attachments = true;
                renderer.model->update(renderer.context.Get(), args, true);
                paint.extendScene(renderer);
                attaching = false;
                lastHighlight = -2;
                analysis.reset();
                cancelKind("analysis");
                startAnalysis();
                for (const auto& track : renderer.model->scene->tracks) {
                    auto name = renderer.model->scene->nodes[track.node].name;
                    auto& names = argNames[track.arg];
                    if (names.size() < 500 && names.find(name) == std::string::npos)
                        names += name + "\n";
                }
                startTextures(true);
                const auto& a = renderer.model->scene->attachments.back();
                notice = "已连接 " + pathString(a.source.filename()) + " → " +
                         renderer.model->scene->nodes[a.targetNode].name + " · 可直接绘制外挂涂装";
                if (a.attachNode < 0)
                    notice += "（未找到 AttachPoint，已按模型原点连接）";
                record(notice);
                for (const auto& warning : renderer.model->scene->warnings)
                    record(warning);
            });
    }
#include "detachment_app.inc"
    bool connectorOverlay(ImVec2 origin, ImVec2 size, bool hovered) {
        hoveredConnector = -1;
        connectorMarkers = Json::array();
        connectorPopupVisible = false;
        if (!renderer.model || !renderer.options.connectors || size.x <= 0 || size.y <= 0)
            return false;
        const auto& scene = *renderer.model->scene;
        const auto& world = renderer.model->world;
        const Mat clip = renderer.camera.projection(size.x / size.y) * renderer.camera.view();
        struct Marker {
            int node;
            ImVec2 position;
            bool mount, occupied;
        };
        std::vector<Marker> markers;
        float nearest = 11 * 11;
        for (int i = 0; i < int(scene.nodes.size()); ++i) {
            const auto& node = scene.nodes[i];
            if (node.extras.value("edm_type", "") != "Connector" || size_t(i) >= world.size() ||
                (!renderer.options.attachments && node.extras.contains("edm_attachment")) ||
                world[i].block<3, 3>(0, 0).cwiseAbs().maxCoeff() < 1e-20)
                continue;
            V4 point = clip * world[i].col(3);
            if (point.w() <= 1e-7)
                continue;
            point /= point.w();
            if (std::abs(point.x()) > 1 || std::abs(point.y()) > 1 || point.z() < 0 || point.z() > 1)
                continue;
            const ImVec2 screen{origin.x + float(point.x() * .5 + .5) * size.x,
                                origin.y + float(.5 - point.y() * .5) * size.y};
            const auto name = lower(node.name);
            bool mount = name.starts_with("pylon") || name.starts_with("point");
            bool occupied = std::any_of(scene.attachments.begin(), scene.attachments.end(),
                                        [i](const auto& item) { return item.targetNode == i; });
            markers.push_back({i, screen, mount, occupied});
            connectorMarkers.push_back(
                {{"node", i}, {"name", node.name}, {"x", screen.x}, {"y", screen.y}, {"occupied", occupied}});
            float dx = ImGui::GetIO().MousePos.x - screen.x, dy = ImGui::GetIO().MousePos.y - screen.y;
            float distance = dx * dx + dy * dy + (occupied ? 0 : mount ? .01f : .02f);
            if (hovered && distance < nearest) {
                nearest = distance;
                hoveredConnector = i;
            }
        }
        auto* draw = ImGui::GetWindowDrawList();
        std::stable_sort(markers.begin(), markers.end(), [](const auto& a, const auto& b) {
            return int(a.occupied) * 2 + int(a.mount) < int(b.occupied) * 2 + int(b.mount);
        });
        draw->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
        for (const auto& marker : markers) {
            const bool hot = marker.node == hoveredConnector;
            const ImU32 color = marker.occupied ? IM_COL32(255, 190, 96, 235)
                                : marker.mount  ? IM_COL32(98, 193, 255, 245)
                                                : IM_COL32(134, 156, 182, 190);
            draw->AddCircleFilled(marker.position, hot ? 8.f : 5.f, IM_COL32(9, 17, 27, 230));
            draw->AddCircle(marker.position, hot ? 8.f : 5.f, color, 20, hot ? 2.5f : 1.5f);
            draw->AddCircleFilled(marker.position, 1.7f, color);
        }
        draw->PopClipRect();
        bool captured = ImGui::IsPopupOpen("连接点操作");
        if (hoveredConnector >= 0) {
            bool occupied = std::any_of(scene.attachments.begin(), scene.attachments.end(),
                                        [&](const auto& a) { return a.targetNode == hoveredConnector; });
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(scene.nodes[hoveredConnector].name.c_str());
            ImGui::TextDisabled(occupied ? "点击显示卸载按钮" : "点击加载 EDM · 自动对齐 AttachPoint");
            ImGui::EndTooltip();
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !attaching && !loading && !paint.busy()) {
                int target = hoveredConnector;
                if (occupied) {
                    selectedConnector = target;
                    connectorSelectionScene = renderer.model->scene;
                    const auto marker = std::find_if(markers.begin(), markers.end(),
                                                     [target](const auto& m) { return m.node == target; });
                    connectorPopupPosition = {marker->position.x + 14, marker->position.y + 8};
                    ImGui::OpenPopup("连接点操作");
                } else if (auto path = dialog(hwnd, false, false, L"选择要连接到挂点的 EDM"))
                    attachModel(target, *path);
            }
            captured = true;
        }
        ImGui::SetNextWindowPos(connectorPopupPosition, ImGuiCond_Appearing);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14, 12});
        if (ImGui::BeginPopup("连接点操作")) {
            captured = connectorPopupVisible = true;
            bool valid = connectorSelectionScene == renderer.model->scene && selectedConnector >= 0 &&
                         selectedConnector < int(scene.nodes.size()) &&
                         (renderer.options.attachments ||
                          !scene.nodes[selectedConnector].extras.contains("edm_attachment"));
            if (!valid) {
                ImGui::CloseCurrentPopup();
            } else {
                ImGui::TextUnformatted(scene.nodes[selectedConnector].name.c_str());
                ImGui::TextDisabled("卸载此处外挂及其下级物体");
                ImGui::Spacing();
                ImGui::BeginDisabled(attaching || loading || paint.busy());
                ImGui::PushStyleColor(ImGuiCol_Button, {.44f, .25f, .10f, 1});
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {.62f, .35f, .12f, 1});
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, {.72f, .41f, .14f, 1});
                bool unload = ImGui::Button("卸载", {172, 32});
                auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                unloadButtonPosition = {(min.x + max.x) * .5f, (min.y + max.y) * .5f};
                ImGui::PopStyleColor(3);
                ImGui::EndDisabled();
                if (unload) {
                    detachModel(selectedConnector);
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        if (!ImGui::IsPopupOpen("连接点操作")) {
            selectedConnector = -1;
            connectorSelectionScene.reset();
        }
        return captured;
    }
    void startTextures(bool reuseExisting = false) {
        textureReadyTime = 0;
        cancelKind("textures");
        textureGeneration++;
        auto cached =
            reuseExisting ? renderer.textures.images : std::map<std::string, std::shared_ptr<GpuTexture>>{};
        if (!reuseExisting)
            renderer.textures = {};
        if (noTextures || !renderer.model || renderer.model->scene->collisionOnly()) {
            if (renderer.model && renderer.model->scene->collisionOnly())
                renderer.textures = {};
            stream.reset();
            return;
        }
        auto source = renderer.model->scene;
        auto selected = livery;
        auto extra = textureDir;
        auto next = std::make_shared<TextureStream>();
        next->bindings.resize(source->materials.size());
        if (reuseExisting)
            std::copy_n(renderer.textures.material.begin(),
                        std::min(renderer.textures.material.size(), next->bindings.size()),
                        next->bindings.begin());
        stream = next;
        int maxSize = std::array<int, 4>{1024, 2048, 4096, 8192}[textureQuality];
        const size_t budget = size_t(previewBudgetGB(textureBudgetGB) * (1024ull * 1024 * 1024));
        launch(
            "textures",
            [next, source, selected, extra, maxSize, budget, cached](Progress progress,
                                                                     const std::atomic_bool* cancel) {
                auto start = Clock::now();
                TextureResolver resolver(source->source, extra, selected);
                std::set<std::string> seen;
                std::vector<ImageSource> requests;
                for (size_t i = 0; i < source->materials.size(); i++) {
                    if (*cancel)
                        break;
                    progress("匹配贴图 " + std::to_string(i + 1) + " / " +
                             std::to_string(source->materials.size()));
                    for (int slot = 0; slot < 3; slot++) {
                        auto found =
                            resolver.material(source->materials[i], std::array<int, 3>{0, 13, 3}[slot]);
                        if (!found)
                            continue;
                        auto key = found->key();
                        {
                            std::lock_guard lock(next->mutex);
                            next->bindings[i][slot] = key;
                        }
                        if (seen.insert(key).second)
                            requests.push_back(*found);
                    }
                }
                size_t used = 0;
                for (size_t i = 0; i < requests.size(); i++) {
                    if (*cancel)
                        break;
                    auto& found = requests[i];
                    auto key = found.key();
                    progress("加载贴图 " + std::to_string(i + 1) + " / " + std::to_string(requests.size()));
                    try {
                        size_t allowance = (budget - used) / std::max(size_t(1), requests.size() - i);
                        if (auto old = cached.find(key);
                            old != cached.end() && old->second->bytes <= allowance) {
                            used += old->second->bytes;
                            std::lock_guard lock(next->mutex);
                            ++next->total;
                            continue;
                        }
                        auto image = loadTexture(found, maxSize);
                        while (image->bytes() > allowance &&
                               image->firstMip + 1 < image->pixels.GetMetadata().mipLevels)
                            image->firstMip++;
                        // DDS files without mipmaps need a smaller decoded preview. Every texture
                        // receives a share of the remaining budget; later materials never vanish.
                        int smaller = maxSize;
                        while (image->bytes() > allowance && smaller > 64) {
                            smaller /= 2;
                            image = loadTexture(found, smaller);
                            while (image->bytes() > allowance &&
                                   image->firstMip + 1 < image->pixels.GetMetadata().mipLevels)
                                image->firstMip++;
                        }
                        require(used + image->bytes() <= budget, "Texture exceeds preview budget");
                        used += image->bytes();
                        std::unique_lock lock(next->mutex);
                        while (next->queue.size() >= 4 && !*cancel)
                            next->condition.wait_for(lock, std::chrono::milliseconds(30));
                        if (*cancel)
                            break;
                        next->queue.push_back({key, image});
                        next->total++;
                    } catch (...) {
                        resolver.warnings.push_back(key + ": " + exceptionText());
                    }
                }
                std::lock_guard lock(next->mutex);
                next->warnings = resolver.warnings;
                next->missing = resolver.missing;
                next->resolved = resolver.resolved;
                next->seconds = seconds(start);
                next->finished = true;
                return true;
            },
            [](bool) {});
    }
    void applyTextureBudget() {
        const auto value = previewBudgetGB(textureBudgetInputGB);
        const bool changed = value != textureBudgetGB;
        textureBudgetInputGB = textureBudgetGB = value;
        saveSettings();
        if (changed)
            startTextures();
    }
    void pumpTextures() {
        if (!stream)
            return;
        auto start = Clock::now();
        std::unique_lock lock(stream->mutex);
        renderer.textures.material = stream->bindings;
        renderer.textures.warnings = stream->warnings;
        renderer.textures.missing = stream->missing;
        renderer.textures.resolved = stream->resolved;
        if (cameraMoving)
            return;
        while (!stream->queue.empty() && seconds(start) < .002) {
            auto pending = std::move(stream->queue.front());
            stream->queue.pop_front();
            stream->condition.notify_one();
            lock.unlock();
            try {
                auto texture = uploadTexture(renderer.device.Get(), *pending.image);
                if (auto old = renderer.textures.images.find(pending.key);
                    old != renderer.textures.images.end())
                    renderer.textures.bytes -= old->second->bytes;
                renderer.textures.bytes += texture->bytes;
                renderer.textures.images[pending.key] = texture;
            } catch (...) {
                record("贴图上传失败：" + pending.key + ": " + exceptionText());
            }
            lock.lock();
        }
    }
    bool texturesReady() {
        if (!stream)
            return true;
        std::lock_guard lock(stream->mutex);
        return stream->finished && stream->queue.empty();
    }
    void scan() {
        if (!renderer.model)
            return;
        cancelKind("catalog");
        scanning = true;
        auto path = currentPath;
        auto roots = extraRoots;
        uint64_t gen = generation;
        launch(
            "catalog",
            [path, roots](Progress progress, const std::atomic_bool* cancel) {
                return std::make_shared<Catalog>(discoverLiveries(path, roots, progress, cancel));
            },
            [this, gen](std::shared_ptr<Catalog> found) {
                if (gen != generation)
                    return;
                catalog = found;
                scanning = false;
                record("发现 " + std::to_string(found->liveries.size()) + " 个涂装，扫描 " +
                       std::to_string(found->roots.size()) + " 个目录。");
                for (auto& warning : found->warnings)
                    record(warning);
                auto key = lower(pathString(currentPath));
                if (!livery && initialLivery.empty() && settings.contains("liveries") &&
                    settings["liveries"].contains(key)) {
                    auto id = settings["liveries"][key].get<std::string>();
                    auto it = std::find_if(found->liveries.begin(), found->liveries.end(),
                                           [&](auto& l) { return l.identifier() == id; });
                    if (it != found->liveries.end())
                        applyLivery(std::make_shared<Livery>(*it));
                }
            });
    }
    void applyLivery(std::shared_ptr<Livery> selected) {
        livery = selected;
        baseline = renderer.model ? renderer.model->scene->defaultArgs : Args{};
        if (livery)
            for (auto [argument, value] : livery->args)
                baseline[argument] = value;
        args = baseline;
        poseDirty = true;
        playing = false;
        bortText.clear();
        applyInitialArguments();
        startAnalysis();
        notice = livery ? "已应用涂装：" + livery->name : "已恢复模型默认贴图";
        settings["liveries"][lower(pathString(currentPath))] = livery ? livery->identifier() : "";
        saveSettings();
        startTextures();
    }
    void startAnalysis() {
        if (!renderer.model)
            return;
        cancelKind("analysis");
        auto scene = renderer.model->scene;
        auto defaults = baseline;
        uint64_t gen = generation;
        analyzing = true;
        launch(
            "analysis",
            [scene, defaults](Progress progress, const std::atomic_bool* cancel) {
                return std::make_shared<AnimationAnalysis>(
                    analyzeAnimations(*scene, defaults, {}, progress, cancel));
            },
            [this, gen](std::shared_ptr<AnimationAnalysis> result) {
                if (gen != generation)
                    return;
                analysis = std::move(result);
                analyzing = false;
                lastHighlight = -2;
                record("动画部件分析完成：" + std::to_string(analysis->arguments.size()) + " 个参数，" +
                       std::to_string(analysis->seconds) + " 秒。");
            });
    }
    const AnimationFinding* finding(int argument) const {
        if (!analysis)
            return nullptr;
        auto found = analysis->arguments.find(argument);
        return found == analysis->arguments.end() ? nullptr : &found->second;
    }
    void findingTooltip(const AnimationFinding& f) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(460);
        ImGui::Text("%s · 匹配 %.0f 分", f.label.c_str(), f.confidence * 100);
        ImGui::TextWrapped("%s / %s", f.location.c_str(), f.motion.c_str());
        ImGui::Text("受影响网格 %zu · 最大位移 %.3f m", f.meshes.size(), f.maximumDisplacement);
        for (auto& evidence : f.evidence)
            ImGui::TextWrapped("• %s", evidence.c_str());
        for (auto& candidate : f.candidates)
            ImGui::Text("候选 %s：%.0f 分", candidate.label.c_str(), candidate.score * 100);
        ImGui::TextDisabled("启发式评分不代表准确率，名称不是 DCS 官方标注。");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    void applyInitialArguments() {
        if (!renderer.model)
            return;
        for (auto [argument, value] : initialArgs)
            args[argument] = value;
        if (!initialBort.empty()) {
            for (auto [argument, value] : bortArguments(*renderer.model->scene, initialBort))
                args[argument] = value;
            bortText = initialBort;
        }
        poseDirty = true;
    }
    void selectLivery(const fs::path& path, const std::string& entry = "") {
        if (!renderer.model)
            return;
        cancelKind("livery");
        auto context = luaContext;
        uint64_t gen = generation;
        launch(
            "livery",
            [path, entry, context](Progress, const std::atomic_bool*) {
                return std::make_shared<Livery>(readLivery(path, entry, "手动选择", "", context));
            },
            [this, gen](std::shared_ptr<Livery> selected) {
                if (gen == generation)
                    applyLivery(selected);
            });
    }
    void importLiveryImpl(const fs::path& path) {
        if (lower(pathString(path.extension())) == ".zip") {
            auto entries = zipEntries(path);
            importEntries.clear();
            for (auto& e : entries)
                if (lower(pathString(fs::path(wide(e)).filename())) == "description.lua")
                    importEntries.push_back(e);
            if (importEntries.size() > 1) {
                importPath = path;
                openImport = true;
                return;
            }
        }
        selectLivery(path);
    }
    void openDialog() {
        try {
            openDialogImpl();
        } catch (...) {
            error = exceptionText();
        }
    }
    void exportDialog() {
        try {
            exportDialogImpl();
        } catch (...) {
            error = exceptionText();
        }
    }
    void importLivery(const fs::path& path) {
        try {
            importLiveryImpl(path);
        } catch (...) {
            error = exceptionText();
        }
    }
    void openDialogImpl() {
        if (auto path = dialog(hwnd, false, false, L"打开 DCS EDM 模型"))
            openModel(*path);
    }
    void exportDialogImpl() {
        if (!renderer.model || exporting)
            return;
        require(!paint.busy(), "绘制任务尚未完成，请稍后导出");
        auto extension = exportExtensions[exportFormat];
        auto filename = currentPath.stem().wstring() + L"." + extension;
        auto path = dialog(hwnd, true, false, L"导出模型与动画", extension, filename.c_str());
        if (!path)
            return;
        for (int i = 0; i < 4; ++i)
            if (lower(pathString(path->extension())) == "." + utf8(exportExtensions[i]))
                exportFormat = i;
        ExportOptions options;
        options.duration = duration;
        options.textures = exportTextures && !renderer.model->scene->collisionOnly();
        options.textureDirectory = textureDir;
        options.livery = livery;
        options.baseline = args;
        const bool staticObj = lower(pathString(path->extension())) == ".obj";
        if (staticObj || exportMode == 2)
            options.arguments = std::vector<int>{};
        else if (exportMode == 1) {
            require(selectedArg >= 0, "请先在左侧选择要导出的动画参数");
            options.arguments = std::vector<int>{selectedArg};
        }
        auto scene = renderer.model->scene;
        auto painted = paint.snapshot();
        exporting = true;
        launch(
            "export",
            [scene, path, options, painted](Progress progress, const std::atomic_bool* cancel) mutable {
                for (auto& [material, image] : painted) {
                    if (!options.textures)
                        break;
                    require(!*cancel, "Cancelled");
                    progress("编码绘制材质 " + std::to_string(material));
                    options.diffuseOverrides[material] = image->pngBytes();
                }
                return exportScene(*scene, *path, options, progress, cancel);
            },
            [this, path](Json report) {
                exporting = false;
                notice = "导出完成：" + pathString(*path);
                if (report.contains("description_lua"))
                    notice += " · 涂装配置：" + report["description_lua"].get<std::string>();
                record(notice);
                record(report.dump(2));
            });
    }
    void fit() {
        if (renderer.model)
            renderer.camera.fit(*renderer.model->scene, args);
    }
    void theme() {
        auto& style = ImGui::GetStyle();
        ImGui::StyleColorsDark();
        style.WindowPadding = {18, 16};
        style.FramePadding = {12, 8};
        style.ItemSpacing = {9, 10};
        style.ItemInnerSpacing = {8, 6};
        style.WindowRounding = 12;
        style.ChildRounding = 12;
        style.FrameRounding = 7;
        style.PopupRounding = 10;
        style.ScrollbarRounding = 8;
        style.GrabRounding = 5;
        style.TabRounding = 7;
        style.WindowBorderSize = 0;
        style.ChildBorderSize = 1;
        style.FrameBorderSize = 0;
        style.ScrollbarSize = 11;
        style.GrabMinSize = 12;
        auto* c = style.Colors;
        c[ImGuiCol_Text] = {.88f, .92f, .98f, 1};
        c[ImGuiCol_TextDisabled] = muted;
        c[ImGuiCol_WindowBg] = {.045f, .059f, .081f, 1};
        c[ImGuiCol_ChildBg] = {.061f, .078f, .106f, 1};
        c[ImGuiCol_PopupBg] = {.08f, .105f, .145f, 1};
        c[ImGuiCol_Border] = {.15f, .19f, .25f, .75f};
        c[ImGuiCol_FrameBg] = {.095f, .124f, .167f, 1};
        c[ImGuiCol_FrameBgHovered] = {.13f, .19f, .27f, 1};
        c[ImGuiCol_FrameBgActive] = {.15f, .23f, .34f, 1};
        c[ImGuiCol_Button] = {.115f, .16f, .23f, 1};
        c[ImGuiCol_ButtonHovered] = {.17f, .25f, .35f, 1};
        c[ImGuiCol_ButtonActive] = {.2f, .32f, .49f, 1};
        c[ImGuiCol_Header] = {.13f, .23f, .37f, 1};
        c[ImGuiCol_HeaderHovered] = {.14f, .25f, .4f, 1};
        c[ImGuiCol_HeaderActive] = {.17f, .32f, .52f, 1};
        c[ImGuiCol_CheckMark] = accent;
        c[ImGuiCol_SliderGrab] = accent;
        c[ImGuiCol_SliderGrabActive] = {.53f, .75f, 1, 1};
        c[ImGuiCol_Separator] = {.14f, .18f, .24f, 1};
        c[ImGuiCol_Tab] = {.09f, .13f, .19f, 1};
        c[ImGuiCol_TabSelected] = {.16f, .26f, .41f, 1};
        c[ImGuiCol_TabHovered] = {.19f, .32f, .49f, 1};
    }
    void leftPanel(float height) {
        ImGui::BeginChild("Animation", {leftWidth, height}, ImGuiChildFlags_Borders);
        title("动画控制", "按 DCS 参数独立预览");
        if (!renderer.model) {
            mutedText("载入模型后，动画参数将在这里显示。");
            ImGui::EndChild();
            return;
        }
        auto& scene = *renderer.model->scene;
        ImGui::TextColored(accent, "%zu", scene.limits.size());
        ImGui::SameLine();
        mutedText("个可用参数");
        space();
        if (selectedArg >= 0) {
            ImGui::Text("参数 %03d", selectedArg);
            auto [lo, hi] = scene.limits.at(selectedArg);
            double value = argValue(args, selectedArg);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderScalar("##argument", ImGuiDataType_Double, &value, &lo, &hi, "%.3f")) {
                args[selectedArg] = value;
                poseDirty = true;
                playing = false;
            }
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputDouble("##precise", &value, 0, 0, "%.5f", ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (std::isfinite(value)) {
                    args[selectedArg] = std::clamp(value, lo, hi);
                    poseDirty = true;
                    playing = false;
                }
            }
            float half = (ImGui::GetContentRegionAvail().x - 9) / 2;
            if (primary(playing ? "暂停" : "播放", {half, 36})) {
                playing = !playing;
                playTime = (hi > lo) ? (argValue(args, selectedArg) - lo) / (hi - lo) * duration : 0;
            }
            ImGui::SameLine();
            if (ImGui::Button("复位", {half, 36})) {
                args[selectedArg] = argValue(baseline, selectedArg);
                playing = false;
                poseDirty = true;
            }
            space(4);
            ImGui::TextDisabled("范围  %.3f — %.3f", lo, hi);
            if (ImGui::IsItemHovered() && argNames.contains(selectedArg))
                ImGui::SetTooltip("%s", argNames[selectedArg].c_str());
            if (auto* f = finding(selectedArg)) {
                ImGui::TextColored(accent, "%s · %.0f 分", f->label.c_str(), f->confidence * 100);
                if (ImGui::IsItemHovered())
                    findingTooltip(*f);
                if (ImGui::Checkbox("高亮运动网格", &highlightMotion))
                    lastHighlight = -2;
                ImGui::SameLine();
                ImGui::BeginDisabled(f->meshes.empty());
                if (ImGui::SmallButton("定位")) {
                    renderer.camera.target = f->bounds.center();
                    renderer.camera.distance = std::max(.2, f->bounds.extent().norm() * 1.7);
                }
                ImGui::EndDisabled();
            } else if (analyzing)
                mutedText("正在分析运动区域…");
        }
        space();
        ImGui::Separator();
        space();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##argsearch", "搜索部件、参数或节点", &argSearch);
        filteredArgs.clear();
        auto search = lower(argSearch);
        for (auto [a, r] : scene.limits)
            if (search.empty() || std::to_string(a).find(search) != std::string::npos ||
                lower(argNames[a]).find(search) != std::string::npos ||
                (finding(a) &&
                 lower(finding(a)->label + " " + finding(a)->location).find(search) != std::string::npos))
                filteredArgs.push_back(a);
        ImGui::BeginChild("Argument list", {0, -42});
        ImGuiListClipper clipper;
        clipper.Begin(int(filteredArgs.size()), 40);
        while (clipper.Step())
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                int a = filteredArgs[row];
                ImGui::PushID(a);
                char number[16];
                std::snprintf(number, sizeof(number), "%03d", a);
                auto* f = finding(a);
                std::string label = std::string(number) + "   " + (f ? f->label : "待分析");
                if (ImGui::Selectable(label.c_str(), a == selectedArg, 0, {0, 30})) {
                    selectedArg = a;
                    playing = false;
                }
                if (ImGui::IsItemHovered()) {
                    if (f)
                        findingTooltip(*f);
                    else if (argNames.contains(a))
                        ImGui::SetTooltip("%s", argNames[a].c_str());
                }
                ImGui::PopID();
            }
        ImGui::EndChild();
        if (ImGui::Button("全部恢复涂装默认值", {-1, 34})) {
            args = baseline;
            poseDirty = true;
            playing = false;
        }
        ImGui::EndChild();
    }
    void liveryPanel() {
        if (renderer.model && renderer.model->scene->collisionOnly()) {
            title("碰撞模型", "壳体几何与参数动画");
            mutedText("颜色用于区分碰撞壳体。\n该模型不含可绘制的涂装 UV，\n无需加载涂装或贴图。");
            space();
            mutedText("可在左侧调节动画参数，\n勾选下方“线框”查看结构，\n或导出为通用模型文件。");
            return;
        }
        title("外观与涂装", "自动发现本体与保存的游戏目录");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##livery", livery ? livery->name.c_str() : "模型默认贴图")) {
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##liverysearch", "搜索涂装", &liverySearch);
            if (ImGui::Selectable("模型默认贴图", !livery))
                applyLivery(nullptr);
            if (catalog)
                for (size_t i = 0; i < catalog->liveries.size(); i++) {
                    auto& item = catalog->liveries[i];
                    if (!liverySearch.empty() &&
                        lower(item.name).find(lower(liverySearch)) == std::string::npos)
                        continue;
                    ImGui::PushID(int(i));
                    if (ImGui::Selectable(item.name.c_str(),
                                          livery && livery->identifier() == item.identifier())) {
                        if (luaContext.empty())
                            applyLivery(std::make_shared<Livery>(item));
                        else
                            selectLivery(item.path, item.entry);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s\n%s", item.origin.c_str(), item.identifier().c_str());
                    ImGui::PopID();
                }
            ImGui::EndCombo();
        }
        space(3);
        if (scanning)
            mutedText("正在扫描涂装目录…");
        else if (catalog)
            mutedText("发现 " + std::to_string(catalog->liveries.size()) + " 个涂装 · " +
                      std::to_string(catalog->installs.size()) + " 处 DCS 安装");
        if (livery) {
            space();
            ImGui::TextWrapped("%s", livery->name.c_str());
            mutedText(livery->origin);
        }
        space();
        float half = (ImGui::GetContentRegionAvail().x - 9) / 2;
        if (ImGui::Button("导入涂装", {half, 36})) {
            if (auto p = dialog(hwnd, false, false, L"导入涂装 description.lua 或 ZIP", L"lua"))
                importLivery(*p);
        }
        ImGui::SameLine();
        if (ImGui::Button("重新扫描", {half, 36}))
            scan();
        if (ImGui::Button("管理扫描目录", {-1, 36}))
            openRoots = true;
        space(12);
        ImGui::Separator();
        space(12);
        title("机身编号");
        auto mapping = renderer.model ? bortMapping(*renderer.model->scene) : std::vector<int>{};
        ImGui::BeginDisabled(mapping.empty());
        ImGui::SetNextItemWidth(std::max(80.f, ImGui::GetContentRegionAvail().x - 70));
        ImGui::InputTextWithHint("##bort", "例如 408", &bortText, ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
        if (primary("应用", {60, 0})) {
            try {
                for (auto [a, v] : bortArguments(*renderer.model->scene, bortText))
                    args[a] = v;
                poseDirty = true;
                playing = false;
            } catch (...) {
                error = exceptionText();
            }
        }
        ImGui::EndDisabled();
        mutedText(mapping.empty() ? "此模型没有识别到快捷编号布局，可在参数列表中调整。"
                                  : "编号与涂装一起预览，并保留在导出文件中。");
        space(14);
        ImGui::Separator();
        space(12);
        title("动态 Lua 配置");
        mutedText("支持变量、条件、函数、循环及涂装目录中的 Lua 引用。");
        if (ImGui::Button("配置变量 / 重新求值", {-1, 36})) {
            contextText = luaContext.dump(2);
            openContext = true;
        }
        if (livery && livery->evaluation.contains("includes"))
            mutedText("Lua 5.4 · " + std::to_string(livery->evaluation["includes"].size()) + " 个引用文件");
        space(12);
        ImGui::Separator();
        space(12);
        title("贴图质量");
        const char* quality[]{"节省显存 · 1024", "均衡 · 2048", "清晰 · 4096", "原画细节 · 8192"};
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##quality", &textureQuality, quality, 4)) {
            saveSettings();
            startTextures();
        }
        space(4);
        mutedText("预览贴图预算（GB）");
        ImGui::SetNextItemWidth(-1);
        const bool submitBudget = ImGui::InputDouble("##texture-budget", &textureBudgetInputGB, .25, 1,
                                                     "%.2f", ImGuiInputTextFlags_EnterReturnsTrue);
        const bool applyBudget = ImGui::Button("应用贴图预算", {-1, 32});
        if (submitBudget || applyBudget)
            applyTextureBudget();
        ImGui::TextDisabled("当前 %.2f GB · 默认 2 GB", textureBudgetGB);
        mutedText("可输入 0.25–100 GB，回车或点击应用。\n预算是加载上限，按实际贴图占用；导出不受此限制。");
    }
    void exportPanel() {
        title("通用模型导出", "GLB · glTF · OBJ · FBX");
        const char* formats[]{"GLB · 单文件", "glTF · 嵌入式", "OBJ · 静态模型", "FBX · 模型与动画"};
        mutedText("文件格式");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##exportformat", &exportFormat, formats, 4))
            saveSettings();
        const bool staticObj = exportFormat == 2;
        const char* modes[]{"全部参数动画", "仅当前参数动画", "当前姿态（静态）"};
        ImGui::BeginDisabled(staticObj);
        ImGui::SetNextItemWidth(-1);
        int displayedMode = staticObj ? 2 : exportMode;
        if (ImGui::Combo("##exportmode", &displayedMode, modes, 3))
            exportMode = displayedMode;
        space();
        mutedText("每个动画片段的时长");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputFloat("##duration", &duration, .5f, 1.f, "%.1f 秒"))
            duration = std::clamp(duration, .1f, 120.f);
        ImGui::EndDisabled();
        const bool collision = renderer.model && renderer.model->scene->collisionOnly();
        if (collision)
            mutedText("碰撞壳体按几何与参数动画导出，\n不附加涂装贴图。");
        else
            ImGui::Checkbox("导出贴图与当前涂装", &exportTextures);
        space(12);
        ImGui::BeginDisabled(!renderer.model || exporting);
        if (primary(exporting ? "正在导出…" : staticObj ? "导出当前姿态" : "导出模型与动画", {-1, 44}))
            exportDialog();
        ImGui::EndDisabled();
        if (exporting && ImGui::Button("取消导出", {-1, 34})) {
            cancelKind("export");
            notice = "正在停止导出…";
        }
        space(8);
        if (!collision && ImGui::Button("指定额外贴图目录", {-1, 36})) {
            if (auto p = dialog(hwnd, false, true, L"选择贴图目录")) {
                textureDir = *p;
                saveSettings();
                startTextures();
            }
        }
        if (!collision)
            mutedText(textureDir.empty() ? "使用模型、模块和涂装目录自动匹配。" : pathString(textureDir));
        space(12);
        ImGui::Separator();
        space(12);
        if (staticObj) {
            mutedText(
                "保存当前姿态与编号。\nOBJ 不包含动画或骨骼。\n移动文件时，请同时携带 MTL\n和贴图文件夹。");
        } else {
            mutedText("按原始范围导出参数动画。\n每段动画将其他参数固定在\n导出时的当前姿态。");
            space();
            mutedText("保留蒙皮与可见性。\n动态编号转为离散动画。\n各软件对材质的显示可能不同。");
        }
        if (exportFormat == 3 && !collision) {
            space();
            mutedText("FBX 嵌入颜色与透明贴图。\nRoughMet 作为资源保留，\n导入后可能需要手动连接。");
        }
    }
    void infoPanel() {
        title("模型信息");
        if (renderer.model) {
            auto& s = *renderer.model->scene;
            auto stats = s.summary();
            ImGui::TextWrapped("%s", pathString(currentPath.filename()).c_str());
            if (s.collisionOnly()) {
                space();
                ImGui::TextColored(accent, "碰撞模型");
                mutedText("保留壳体结构与父节点动画。\n颜色仅用于区分壳体，无涂装 UV。");
            }
            space();
            for (auto [label, key] :
                 std::vector<std::pair<const char*, const char*>>{{"网格", "export_meshes"},
                                                                  {"三角形", "triangles"},
                                                                  {"材质", "materials"},
                                                                  {"蒙皮网格", "skinned_meshes"},
                                                                  {"连接点", "connectors"}}) {
                mutedText(label);
                ImGui::SameLine(130);
                ImGui::TextUnformatted(compactNumber(stats[key].get<size_t>()).c_str());
            }
            if (s.collisionCount) {
                mutedText("碰撞壳体");
                ImGui::SameLine(130);
                ImGui::Text("%d", s.collisionCount);
            }
            space();
            ImGui::Text("模型准备  %.2f 秒", loadSeconds);
            ImGui::Text("贴图显存  %.1f MB", renderer.textures.bytes / 1048576.);
            ImGui::Text("贴图数量  %zu", renderer.textures.images.size());
            mutedText(renderer.adapterName);
            space();
            for (auto& warning : s.warnings)
                mutedText(warning);
            for (auto& warning : renderer.textures.warnings)
                mutedText(warning);
            if (!renderer.textures.missing.empty())
                mutedText(std::to_string(renderer.textures.missing.size()) + " 项贴图未找到");
        }
        space();
        if (ImGui::CollapsingHeader("读取与导出日志")) {
            ImGui::InputTextMultiline("##logs", &log, {-1, 260}, ImGuiInputTextFlags_ReadOnly);
        }
    }
    void rightPanel(float height) {
        ImGui::BeginChild("Properties", {rightWidth, height}, ImGuiChildFlags_Borders);
        if (ImGui::BeginTabBar("##properties")) {
            if (ImGui::BeginTabItem("涂装", nullptr, rightTab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
                liveryPanel();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("绘制", nullptr, rightTab == 3 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::BeginDisabled(attaching);
                paint.panel(hwnd, renderer, args);
                ImGui::EndDisabled();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("导出", nullptr, rightTab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                exportPanel();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("信息", nullptr, rightTab == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
                infoPanel();
                ImGui::EndTabItem();
            }
            rightTab = -1;
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
    }
    void splitter(const char* id, float height, float& width, bool left) {
        ImGui::InvisibleButton(id, {7, height});
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive())
            width = std::clamp(width + ImGui::GetIO().MouseDelta.x * (left ? 1 : -1), 220.f, 460.f);
    }
    void modals() {
        if (openContext) {
            ImGui::OpenPopup("Lua 配置变量");
            openContext = false;
        }
        ImGui::SetNextWindowSize({640, 510}, ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Lua 配置变量", nullptr, ImGuiWindowFlags_NoResize)) {
            mutedText("输入 JSON 对象，例如 {\"variant\": \"navy\", \"number\": "
                      "408}。变量可直接访问，也可通过 EDM_STUDIO 访问。");
            space();
            ImGui::InputTextMultiline("##context", &contextText, {-1, 300},
                                      ImGuiInputTextFlags_AllowTabInput);
            space();
            if (primary("应用并重新求值", {180, 38})) {
                try {
                    auto context = Json::parse(contextText);
                    require(context.is_object(), "Lua 变量必须为 JSON 对象");
                    luaContext = context;
                    if (livery)
                        selectLivery(livery->path, livery->entry);
                    ImGui::CloseCurrentPopup();
                } catch (...) {
                    error = exceptionText();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("取消", {90, 38}))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (openRoots) {
            ImGui::OpenPopup("扫描目录");
            openRoots = false;
        }
        ImGui::SetNextWindowSize({700, 500}, ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("扫描目录", nullptr, ImGuiWindowFlags_NoResize)) {
            mutedText(
                "自动检测 Windows“保存的游戏”、DCS 配置、Steam 库及 DCS 安装目录。这里可以补充自定义位置。");
            space();
            if (ImGui::Button("添加目录…", {140, 36})) {
                if (auto p = dialog(hwnd, false, true, L"添加涂装 / DCS / 保存的游戏目录")) {
                    extraRoots.push_back(*p);
                    extraRoots = uniquePaths(extraRoots);
                    saveSettings();
                    scan();
                }
            }
            ImGui::BeginChild("Roots", {0, 310});
            for (size_t i = 0; i < extraRoots.size(); i++) {
                ImGui::PushID(int(i));
                if (ImGui::SmallButton("移除")) {
                    extraRoots.erase(extraRoots.begin() + i);
                    saveSettings();
                    scan();
                    ImGui::PopID();
                    break;
                }
                ImGui::SameLine();
                ImGui::TextWrapped("%s", pathString(extraRoots[i]).c_str());
                ImGui::PopID();
            }
            space();
            mutedText("自动检测的涂装目录");
            if (catalog)
                for (auto& root : catalog->roots)
                    ImGui::TextWrapped("%s", pathString(root).c_str());
            ImGui::EndChild();
            if (primary("完成", {110, 36}))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (openImport) {
            ImGui::OpenPopup("选择 ZIP 内的涂装");
            openImport = false;
        }
        if (ImGui::BeginPopupModal("选择 ZIP 内的涂装", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            for (auto& entry : importEntries)
                if (ImGui::Selectable(entry.c_str())) {
                    selectLivery(importPath, entry);
                    ImGui::CloseCurrentPopup();
                }
            if (ImGui::Button("取消"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    void ui() {
        auto& io = ImGui::GetIO();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {20, 16});
        ImGui::Begin("EDM Studio", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleVar();
        auto at = ImGui::GetCursorScreenPos();
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled({at.x, at.y + 3}, {at.x + 34, at.y + 37}, IM_COL32(44, 102, 202, 255), 9);
        draw->AddTriangle({at.x + 8, at.y + 25}, {at.x + 17, at.y + 10}, {at.x + 26, at.y + 25},
                          IM_COL32(224, 238, 255, 255), 2);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 46);
        ImGui::PushFont(nullptr, 27);
        ImGui::TextUnformatted("EDM Studio");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 7);
        ImGui::TextColored(muted, "NATIVE  /  DCS 模型与动画");
        ImGui::SameLine(io.DisplaySize.x - 363);
        if (ImGui::Button("打开 EDM", {130, 42}))
            openDialog();
        ImGui::SameLine();
        ImGui::BeginDisabled(!renderer.model || exporting);
        if (primary("导出模型与动画", {182, 42}))
            exportDialog();
        ImGui::EndDisabled();
        space(6);
        mutedText(currentPath.empty() ? "模型工作台  /  支持 EDM 8 与 EDM 10" : pathString(currentPath));
        space(7);
        float height = std::max(200.f, ImGui::GetContentRegionAvail().y - 37);
        float side = showPanels ? leftWidth + rightWidth + 32 : 0;
        float center = ImGui::GetContentRegionAvail().x - side;
        if (center < 320 && showPanels) {
            leftWidth = std::max(220.f, leftWidth - (320 - center) * .5f);
            rightWidth = std::max(220.f, rightWidth - (320 - center) * .5f);
            center = ImGui::GetContentRegionAvail().x - leftWidth - rightWidth - 32;
        }
        if (showPanels) {
            leftPanel(height);
            ImGui::SameLine(0, 5);
            splitter("##leftsplit", height, leftWidth, true);
            ImGui::SameLine(0, 5);
        }
        ImGui::BeginChild("Viewport panel", {std::max(250.f, center), height}, ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar);
        ImGui::TextColored(muted, "MODEL VIEW");
        ImGui::SameLine();
        if (renderer.model)
            ImGui::Text("%s", pathString(currentPath.stem()).c_str());
        ImGui::SameLine(std::max(200.f, ImGui::GetWindowWidth() - 152));
        ImGui::TextColored(green, "●  %s",
                           loading                         ? "载入中"
                           : renderer.options.editedLivery ? "实时预览"
                                                           : "未编辑涂装");
        space(4);
        ImVec2 size = ImGui::GetContentRegionAvail();
        const bool narrowControls = ImGui::GetContentRegionAvail().x < 610;
        const float controlsHeight =
            32 + ImGui::GetFrameHeight() * (narrowControls ? 2 : 1) + 5 + ImGui::GetStyle().ItemSpacing.y * 6;
        size.y = std::max(100.f, size.y - controlsHeight);
        renderer.render(int(size.x), int(size.y));
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Image(renderer.view(), size);
        bool hovered = ImGui::IsItemHovered();
        bool connectorMouse = connectorOverlay(origin, size, hovered);
        bool paintMouse = attaching || connectorMouse;
        if (!attaching)
            paintMouse |= paint.viewport(renderer, origin, size, hovered && !connectorMouse);
        cameraMoving = false;
        if (hovered && !io.WantTextInput) {
            if ((!paintMouse && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) ||
                ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
                renderer.camera.orbit(io.MouseDelta.x, io.MouseDelta.y);
                cameraMoving = true;
            }
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                renderer.camera.pan(io.MouseDelta.x, io.MouseDelta.y, size.y);
                cameraMoving = true;
            }
            if (io.MouseWheel) {
                renderer.camera.zoom(io.MouseWheel);
                cameraMoving = true;
            }
        }
        auto* overlay = ImGui::GetWindowDrawList();
        if (!renderer.model) {
            auto center = ImVec2(origin.x + size.x * .5f, origin.y + size.y * .43f);
            overlay->AddCircle(center, 42, IM_COL32(76, 128, 202, 100), 64, 2);
            overlay->AddTriangle({center.x - 19, center.y + 13}, {center.x, center.y - 21},
                                 {center.x + 19, center.y + 13}, IM_COL32(126, 176, 244, 220), 2);
            std::string text = loading ? "正在准备模型…" : "将 EDM 文件拖入此处";
            auto extent = ImGui::CalcTextSize(text.c_str());
            overlay->AddText({center.x - extent.x / 2, center.y + 68}, IM_COL32(193, 211, 236, 255),
                             text.c_str());
            std::string hint = "或点击右上角「打开 EDM」";
            extent = ImGui::CalcTextSize(hint.c_str());
            overlay->AddText({center.x - extent.x / 2, center.y + 99}, IM_COL32(111, 135, 165, 255),
                             hint.c_str());
        } else {
            char stats[140];
            std::snprintf(stats, sizeof(stats), "%s triangles  ·  %zu meshes",
                          compactNumber(renderer.visibleTriangles).c_str(),
                          renderer.model->scene->meshes.size());
            overlay->AddText({origin.x + 16, origin.y + 14}, IM_COL32(151, 178, 211, 230), stats);
            if (renderer.model->scene->collisionOnly())
                overlay->AddText({origin.x + 16, origin.y + 36}, IM_COL32(111, 192, 224, 235),
                                 "碰撞壳体 · 可查看参数动画与导出几何");
            std::string help = renderer.options.connectors ? "点击空挂点加载 EDM · 点击黄色挂点显示卸载按钮"
                               : paint.active() && renderer.options.editedLivery
                                   ? "按位置自动绘制 · 右键旋转 · 中键平移"
                                   : "左 / 右键旋转  ·  中键平移  ·  滚轮缩放";
            overlay->AddText({origin.x + 16, origin.y + size.y - 30}, IM_COL32(111, 139, 173, 220),
                             help.c_str());
        }
        space(3);
        if (ImGui::Button("适应视图", {96, 32}))
            fit();
        ImGui::SameLine();
        ImGui::Checkbox("贴图", &renderer.options.textures);
        ImGui::SameLine();
        ImGui::Checkbox("RoughMet", &renderer.options.roughmet);
        if (ImGui::GetWindowWidth() > 600) {
            ImGui::SameLine();
            ImGui::Checkbox("线框", &renderer.options.wireframe);
            ImGui::SameLine();
            ImGui::Checkbox("网格", &renderer.options.grid);
        } else {
            ImGui::SameLine();
            if (ImGui::Button("更多"))
                ImGui::OpenPopup("视图选项");
            if (ImGui::BeginPopup("视图选项")) {
                ImGui::Checkbox("线框", &renderer.options.wireframe);
                ImGui::Checkbox("网格", &renderer.options.grid);
                ImGui::EndPopup();
            }
        }
        space(2);
        ImGui::Checkbox("浏览已编辑的涂装", &renderer.options.editedLivery);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "取消勾选显示编辑前的涂装；编辑和撤销记录保留。\n保存与导出始终包含已编辑的内容。");
        if (!narrowControls)
            ImGui::SameLine(0, 20);
        ImGui::Checkbox("显示挂点", &renderer.options.connectors);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("蓝色为 Pylon / Point；橙色表示已有外挂。\n点击圆点加载 "
                              "EDM。标记可透过模型查看，绘制时可关闭。");
        ImGui::SameLine(0, 20);
        ImGui::BeginDisabled(attaching || paint.busy());
        if (ImGui::Checkbox("显示外挂物体", &renderer.options.attachments)) {
            paint.invalidateSurface(renderer);
            poseDirty = true;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(
                "隐藏后，外挂不参与画笔拾取和图片投影遮挡。\n绘制记录保留；模型导出仍包含所有已连接的外挂。");
        ImGui::EndChild();
        if (showPanels) {
            ImGui::SameLine(0, 5);
            splitter("##rightsplit", height, rightWidth, false);
            ImGui::SameLine(0, 5);
            rightPanel(height);
        }
        space(2);
        std::string status = notice;
        for (auto& job : jobs)
            if (!job->finished) {
                std::lock_guard lock(job->mutex);
                if (!job->progress.empty())
                    status = job->progress;
                break;
            }
        ImGui::TextColored(muted, "%s", status.c_str());
        if (io.DisplaySize.x > 1100) {
            ImGui::SameLine(io.DisplaySize.x - 305);
            ImGui::TextColored(muted, "%.0f FPS  ·  %.1f MB  ·  F11 聚焦", io.Framerate,
                               renderer.textures.bytes / 1048576.);
        }
        modals();
        if (!error.empty()) {
            ImGui::OpenPopup("操作未完成");
        }
        ImGui::SetNextWindowSize({620, 0}, ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("操作未完成", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s", error.c_str());
            space();
            if (primary("知道了", {120, 36})) {
                error.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::End();
        if (!io.WantTextInput) {
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O))
                openDialog();
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_E))
                exportDialog();
            if (ImGui::IsKeyPressed(ImGuiKey_F))
                fit();
            if (ImGui::IsKeyPressed(ImGuiKey_F11))
                showPanels = !showPanels;
            if (ImGui::IsKeyPressed(ImGuiKey_Space) && selectedArg >= 0 &&
                (!paint.active() || !renderer.options.editedLivery))
                playing = !playing;
        }
    }
    void tick() {
        poll();
        pumpTextures();
        if (renderer.model && texturesReady() && textureReadyTime == 0)
            textureReadyTime = seconds(launched);
        paint.bind(renderer, livery, textureDir);
        if (paint.active() && renderer.options.editedLivery)
            playing = false;
        auto& io = ImGui::GetIO();
        if (playing && renderer.model && selectedArg >= 0) {
            auto [lo, hi] = renderer.model->scene->limits.at(selectedArg);
            playTime += std::min(double(io.DeltaTime), .1);
            double t = std::fmod(playTime, std::max(.1f, duration)) / std::max(.1f, duration);
            args[selectedArg] = lo + (hi - lo) * t;
            poseDirty = true;
        }
        if (poseDirty && renderer.model) {
            renderer.model->update(renderer.context.Get(), args, renderer.options.attachments);
            poseDirty = false;
        }
        int highlight = highlightMotion ? selectedArg : -1;
        if (renderer.model && highlight != lastHighlight) {
            std::vector<size_t> affected;
            if (auto* f = finding(highlight))
                affected.assign(f->meshes.begin(), f->meshes.end());
            renderer.model->highlight(renderer.context.Get(), affected);
            lastHighlight = highlight;
        }
        auto sceneBeforePaint = renderer.model ? renderer.model->scene : nullptr;
        if (!attaching)
            paint.tick(renderer, args);
        if (auto restored = paint.takeRestoredAppearance()) {
            bool sceneChanged = renderer.model && renderer.model->scene != sceneBeforePaint;
            if (sceneChanged) {
                if (sceneBeforePaint)
                    for (const auto& attachment : sceneBeforePaint->attachments)
                        for (auto [original, mapped] : attachment.argumentMap) {
                            args.erase(mapped);
                            baseline.erase(mapped);
                        }
                for (auto [argument, value] : renderer.model->scene->defaultArgs) {
                    args[argument] = value;
                    baseline[argument] = value;
                }
                argNames.clear();
                for (const auto& track : renderer.model->scene->tracks) {
                    auto& names = argNames[track.arg];
                    if (names.size() < 500)
                        names += renderer.model->scene->nodes[track.node].name + "\n";
                }
                if (!renderer.model->scene->limits.contains(selectedArg))
                    selectedArg = renderer.model->scene->limits.empty()
                                      ? -1
                                      : renderer.model->scene->limits.begin()->first;
                lastHighlight = -2;
                paint.invalidateSurface(renderer);
            }
            if (restored->restoreAppearance) {
                cancelKind("livery");
                textureDir = restored->textureDirectory;
                applyLivery(std::move(restored->livery));
                luaContext = livery && livery->evaluation.is_object()
                                 ? livery->evaluation.value("context", Json::object())
                                 : Json::object();
                if (!luaContext.is_object())
                    luaContext = Json::object();
                contextText = luaContext.dump(2);
                paint.bind(renderer, livery, textureDir);
            } else if (sceneChanged) {
                startAnalysis();
                startTextures();
            }
            if (sceneChanged)
                renderer.model->update(renderer.context.Get(), args, renderer.options.attachments);
            for (const auto& warning : restored->warnings)
                record(warning);
        }
    }
#include "attachment_app_test.inc"
#include "detachment_app_test.inc"
    bool diagnostics(double frameSeconds) {
        if (!smoke)
            return false;
        if (!error.empty()) {
            if (!metricsPath.empty())
                writeJson(metricsPath, {{"error", error}});
            return true;
        }
        const double diagnosticTimeout =
            !autoPaintDirectory.empty() && autoPaintDimension >= 2048 ? 900 : 180;
        if (seconds(launched) > diagnosticTimeout) {
            error = "Native preview timed out";
            if (!metricsPath.empty())
                writeJson(metricsPath, {{"error", error}});
            return true;
        }
        if (!renderer.model || loading || !texturesReady())
            return false;
        if (attaching)
            return false;
        for (const auto& job : jobs)
            if (!job->finished &&
                (job->kind == "livery" || job->kind == "catalog" || job->kind == "analysis"))
                return false;
        if (!autoPaintDirectory.empty() &&
            !paint.exerciseAutomatic(renderer, autoPaintImage, autoPaintDirectory, autoPaintDimension))
            return false;
        if (!layersTestDirectory.empty() && !paint.exerciseLayers(renderer, args, layersTestDirectory))
            return false;
        if (!paintMaterial.empty() && !paint.exercise(renderer, paintMaterial, paintResult))
            return false;
        if (attachmentTest && !exerciseAttachmentInteraction())
            return false;
        if (!detachmentTest.empty() && !exerciseDetachmentInteraction())
            return false;
        if (!decalTestDirectory.empty() &&
            !paint.exerciseDecal(renderer, decalTestImage, decalTestDirectory, decalTestDimension,
                                 decalTestConforming))
            return false;
        if (!wrapImage.empty() && !paint.exerciseWrap(renderer, wrapImage, wrapResult))
            return false;
        if (!projectionImage.empty() &&
            !paint.exerciseProjection(renderer, projectionImage, projectionResult))
            return false;
        if (strokeTest && !paint.exerciseStroke(renderer))
            return false;
        if (!paintProjectDirectory.empty() && !paint.exerciseProject(renderer, paintProjectDirectory))
            return false;
        if (!readyTime) {
            readyTime = seconds(launched);
            if (verifyGPU) {
                gpuReport.push_back(renderer.verifyGpu(args));
                Args animated = args;
                for (int a : {0, 9, 10, 11, 12, 38})
                    if (renderer.model->scene->limits.contains(a)) {
                        auto [lo, hi] = renderer.model->scene->limits.at(a);
                        animated[a] = lo + (hi - lo) * .613;
                    }
                gpuReport.push_back(renderer.verifyGpu(animated));
                renderer.model->update(renderer.context.Get(), args, renderer.options.attachments);
            }
            benchUploads = renderer.model->vertexUploads;
            benchEvals = renderer.model->sceneEvaluations;
        }
        if (seconds(launched) - readyTime < 1)
            return false;
        if (!benchStarted) {
            benchStarted = true;
            frames.clear();
        }
        if (benchmarkFrames > 0 && frames.size() < size_t(benchmarkFrames)) {
            renderer.camera.yaw += .008;
            frames.push_back(frameSeconds * 1000);
            return false;
        }
        if (!capturePath.empty())
            renderer.capture(capturePath);
        auto samples = frames;
        std::sort(samples.begin(), samples.end());
        auto stats = renderer.model->scene->summary();
        stats["attachment_interaction"] = attachmentTestReport;
        stats["detachment_interaction"] = detachmentTestReport;
        stats["connector_markers"] = connectorMarkers;
        stats.update(
            {{"model_ready_seconds", loadSeconds},
             {"textures_ready_seconds", textureReadyTime},
             {"workspace_ready_seconds", readyTime},
             {"texture_count", renderer.textures.images.size()},
             {"texture_bytes", renderer.textures.bytes},
             {"geometry_bytes", renderer.model->geometryBytes},
             {"gpu_preparation_seconds", renderer.model->preparationSeconds},
             {"camera_vertex_uploads", renderer.model->vertexUploads - benchUploads},
             {"camera_scene_evaluations", renderer.model->sceneEvaluations - benchEvals},
             {"benchmark_frames", samples.size()},
             {"frame_p50_ms", samples.empty() ? 0 : samples[samples.size() / 2]},
             {"frame_p95_ms",
              samples.empty() ? 0 : samples[std::min(samples.size() - 1, size_t(samples.size() * .95))]},
             {"gpu_validation", gpuReport},
             {"adapter", renderer.adapterName},
             {"livery", livery ? livery->name : ""},
             {"arguments",
              [&] {
                  Json values = Json::object();
                  for (auto [a, v] : args)
                      values[std::to_string(a)] = v;
                  return values;
              }()},
             {"preview_max_dimension", 1024 << textureQuality},
             {"preview_texture_budget_gb", textureBudgetGB},
             {"preview_texture_budget_bytes", size_t(textureBudgetGB * (1024ull * 1024 * 1024))},
             {"viewport", renderer.viewportMetrics()},
             {"animation_analysis", analysis ? analysis->toJson() : Json()},
             {"paint", paint.diagnostic()},
             {"texture_warnings", renderer.textures.warnings},
             {"missing_textures", renderer.textures.missing}});
        if (!autoPaintDirectory.empty())
            stats["automatic_paint"] =
                Json::parse(readFile(autoPaintDirectory / "auto-paint-report.json", 4000000));
        if (!metricsPath.empty())
            writeJson(metricsPath, stats);
        return true;
    }
};
App* active = nullptr;
LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, w, l))
        return true;
    try {
        switch (msg) {
        case WM_SIZE:
            if (active && active->renderer.device && w != SIZE_MINIMIZED)
                active->renderer.resizeBack(LOWORD(l), HIWORD(l));
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(l);
            info->ptMinTrackSize = {1060, 700};
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((w & 0xfff0) == SC_KEYMENU)
                return 0;
            break;
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(w);
            wchar_t path[32768];
            DragQueryFileW(drop, 0, path, 32768);
            DragFinish(drop);
            if (active) {
                fs::path p(path);
                if (lower(pathString(p.extension())) == ".edm")
                    active->openModel(p);
                else
                    active->importLivery(p);
            }
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_CLOSE:
            if (active) {
                active->paint.quiesce();
                active->paint.recover(active->settingsPath.parent_path() / "PaintRecovery");
            }
            break;
        }
    } catch (...) {
        if (active)
            active->error = exceptionText();
        if (msg == WM_CLOSE)
            return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}
} // namespace
int runApp(HINSTANCE instance, int argc, wchar_t** argv) {
    if (argc >= 4 && std::wstring_view(argv[1]) == L"--lua-worker")
        return luaWorker(argv[2], argv[3]);
    ComRuntime imageRuntime;
    ComApartment apartment(COINIT_APARTMENTTHREADED);
    ImGui_ImplWin32_EnableDpiAwareness();
    App app;
    active = &app;
    fs::path initial;
    std::optional<int> initialExportFormat;
    int windowWidth = 1600, windowHeight = 980;
    for (int i = 1; i < argc; i++) {
        std::wstring key = argv[i];
        auto value = [&]() {
            require(i + 1 < argc, "Missing command-line option");
            return std::wstring(argv[++i]);
        };
        if (key == L"--smoke")
            app.smoke = true;
        else if (key == L"--attach") {
            auto spec = utf8(value());
            auto equals = spec.find('=');
            require(equals != std::string::npos && equals > 0 && equals + 1 < spec.size(),
                    "--attach requires connector=model.edm");
            app.initialAttachments.emplace_back(spec.substr(0, equals), wide(spec.substr(equals + 1)));
        } else if (key == L"--attachment-test") {
            auto spec = utf8(value());
            auto equals = spec.find('=');
            require(equals != std::string::npos && equals > 0 && equals + 1 < spec.size(),
                    "--attachment-test requires connector=model.edm");
            app.attachmentTest = std::pair{spec.substr(0, equals), fs::path(wide(spec.substr(equals + 1)))};
            app.smoke = true;
        } else if (key == L"--show-connectors")
            app.renderer.options.connectors = true;
        else if (key == L"--detachment-test") {
            app.detachmentTest = utf8(value());
            app.smoke = true;
            app.renderer.options.connectors = true;
        } else if (key == L"--hide-attachments")
            app.renderer.options.attachments = false;
        else if (key == L"--capture")
            app.capturePath = value();
        else if (key == L"--metrics")
            app.metricsPath = value();
        else if (key == L"--export-panel") {
            auto format = value();
            for (int f = 0; f < 4; ++f)
                if (format == exportExtensions[f])
                    initialExportFormat = f;
            require(initialExportFormat.has_value(), "Expected glb, gltf, obj or fbx for --export-panel");
        } else if (key == L"--benchmark")
            app.benchmarkFrames = std::stoi(value());
        else if (key == L"--verify-gpu")
            app.verifyGPU = true;
        else if (key == L"--no-scan")
            app.noAutoScan = true;
        else if (key == L"--no-textures")
            app.noTextures = true;
        else if (key == L"--texture-budget-gb")
            app.initialTextureBudget = std::stod(value());
        else if (key == L"--diagnostic-settings") {
            app.diagnosticSettings = value();
            app.smoke = true;
        } else if (key == L"--livery")
            app.initialLivery = value();
        else if (key == L"--bort")
            app.initialBort = utf8(value());
        else if (key == L"--highlight-argument")
            app.initialHighlight = std::stoi(value());
        else if (key == L"--paint-material")
            app.paintMaterial = utf8(value());
        else if (key == L"--paint-result")
            app.paintResult = value();
        else if (key == L"--wrap-image")
            app.wrapImage = value();
        else if (key == L"--wrap-result")
            app.wrapResult = value();
        else if (key == L"--projection-image")
            app.projectionImage = value();
        else if (key == L"--projection-result")
            app.projectionResult = value();
        else if (key == L"--paint-project-dir")
            app.paintProjectDirectory = value();
        else if (key == L"--layers-test") {
            app.layersTestDirectory = value();
            app.smoke = true;
            app.rightTab = 3;
        } else if (key == L"--auto-paint-test") {
            app.autoPaintDirectory = value();
            app.smoke = true;
        } else if (key == L"--auto-paint-image")
            app.autoPaintImage = value();
        else if (key == L"--auto-paint-size")
            app.autoPaintDimension = std::stoi(value());
        else if (key == L"--decal-test") {
            app.decalTestDirectory = value();
            app.smoke = true;
        } else if (key == L"--decal-image")
            app.decalTestImage = value();
        else if (key == L"--decal-size")
            app.decalTestDimension = std::stoi(value());
        else if (key == L"--decal-conform")
            app.decalTestConforming = true;
        else if (key == L"--stroke-test")
            app.strokeTest = true;
        else if (key == L"--baseline") {
            std::istringstream input(utf8(value()));
            std::string item;
            while (std::getline(input, item, ',')) {
                auto equals = item.find('=');
                require(equals != std::string::npos, "Expected argument=value");
                auto number = std::stod(item.substr(equals + 1));
                require(std::isfinite(number), "Nonfinite argument");
                app.initialArgs[std::stoi(item.substr(0, equals))] = number;
            }
        } else if (key == L"--window-size") {
            auto size = value();
            auto separator = size.find(L'x');
            require(separator != std::wstring::npos, "Expected WIDTHxHEIGHT");
            windowWidth = std::clamp(std::stoi(size.substr(0, separator)), 1060, 7680);
            windowHeight = std::clamp(std::stoi(size.substr(separator + 1)), 700, 4320);
        } else if (key.starts_with(L"--"))
            throw std::runtime_error("Unknown option: " + utf8(key));
        else
            initial = key;
    }
    if (!app.layersTestDirectory.empty()) {
        require(!initial.empty(), "Layer diagnostic requires an EDM model path");
        require(app.autoPaintDirectory.empty() && app.paintMaterial.empty() && app.wrapImage.empty() &&
                    app.projectionImage.empty() && app.decalTestDirectory.empty() && !app.strokeTest,
                "Run the layer diagnostic independently of other painting diagnostics");
        app.layersTestDirectory = fs::absolute(app.layersTestDirectory).lexically_normal();
        fs::create_directories(app.layersTestDirectory);
        if (app.metricsPath.empty())
            app.metricsPath = app.layersTestDirectory / "metrics.json";
        if (app.capturePath.empty())
            app.capturePath = app.layersTestDirectory / "preview.png";
    }
    if (!app.decalTestDirectory.empty()) {
        require(!initial.empty() && !app.decalTestImage.empty(),
                "Surface decal diagnostic requires an EDM path and --decal-image PNG");
        require(app.decalTestDimension == 0 || app.decalTestDimension == 512 ||
                    app.decalTestDimension == 1024 || app.decalTestDimension == 2048 ||
                    app.decalTestDimension == 4096 || app.decalTestDimension == 8192,
                "Surface decal diagnostic texture size must be 0 (editor default), 512, 1024, 2048, 4096 or 8192");
        require(app.autoPaintDirectory.empty() && app.paintMaterial.empty() && app.wrapImage.empty() &&
                    app.projectionImage.empty() && !app.strokeTest && app.paintProjectDirectory.empty(),
                "Run the surface decal diagnostic independently");
        app.decalTestDirectory = fs::absolute(app.decalTestDirectory).lexically_normal();
        fs::create_directories(app.decalTestDirectory);
        if (app.metricsPath.empty())
            app.metricsPath = app.decalTestDirectory / "metrics.json";
        if (app.capturePath.empty())
            app.capturePath = app.decalTestDirectory / "preview.png";
    } else
        require(app.decalTestImage.empty() && !app.decalTestConforming,
                "--decal-image and --decal-conform require --decal-test");
    if (!app.autoPaintDirectory.empty()) {
        require(!initial.empty(), "Automatic paint diagnostic requires an EDM model path");
        require(!app.autoPaintImage.empty(), "Specify --auto-paint-image with the diagnostic PNG");
        require(app.paintMaterial.empty() && app.wrapImage.empty() && app.projectionImage.empty() &&
                    !app.strokeTest && app.paintProjectDirectory.empty(),
                "Automatic paint diagnostic must run independently of legacy selected-material tests");
        app.autoPaintDirectory = fs::absolute(app.autoPaintDirectory).lexically_normal();
        fs::create_directories(app.autoPaintDirectory);
        if (app.metricsPath.empty())
            app.metricsPath = app.autoPaintDirectory / "metrics.json";
        if (app.capturePath.empty())
            app.capturePath = app.autoPaintDirectory / "preview.png";
    } else
        require(app.autoPaintImage.empty(), "--auto-paint-image requires --auto-paint-test");
    app.restore();
    if (initialExportFormat) {
        app.exportFormat = *initialExportFormat;
        app.rightTab = 1;
    }
    if (app.initialTextureBudget) {
        app.textureBudgetInputGB = *app.initialTextureBudget;
        app.applyTextureBudget();
    }
    if (!app.autoPaintDirectory.empty())
        app.settingsPath = app.autoPaintDirectory / "diagnostic-settings.json";
    if (!app.decalTestDirectory.empty())
        app.settingsPath = app.decalTestDirectory / "diagnostic-settings.json";
    if (!app.layersTestDirectory.empty())
        app.settingsPath = app.layersTestDirectory / "diagnostic-settings.json";
    WNDCLASSEXW wc{sizeof(wc),
                   CS_CLASSDC,
                   windowProc,
                   0,
                   0,
                   instance,
                   nullptr,
                   LoadCursor(nullptr, IDC_ARROW),
                   nullptr,
                   nullptr,
                   L"EDMStudioNative",
                   nullptr};
    wc.hIcon =
        static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_EDM_STUDIO), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_EDM_STUDIO), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                               LR_SHARED));
    RegisterClassExW(&wc);
    const auto windowTitle = wide(std::string("EDM Studio ") + EDM_NATIVE_VERSION + " — 模型、涂装与动画");
    HWND hwnd = CreateWindowW(wc.lpszClassName, windowTitle.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                              CW_USEDEFAULT, windowWidth, windowHeight, nullptr, nullptr, instance, nullptr);
    require(hwnd, "Cannot create application window");
    app.hwnd = hwnd;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    app.renderer.initialize(hwnd);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc", 18);
    app.theme();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(app.renderer.device.Get(), app.renderer.context.Get());
    DragAcceptFiles(hwnd, TRUE);
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    if (!initial.empty())
        app.openModel(initial);
    bool done = false;
    auto previous = Clock::now();
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;
        if (IsIconic(hwnd)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }
        auto now = Clock::now();
        double elapsed = std::chrono::duration<double>(now - previous).count();
        previous = now;
        try {
            app.tick();
            app.renderer.beginFrame();
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            if (app.smoke && app.diagnosticMousePosition) {
                ImGui::GetIO().AddMousePosEvent(app.diagnosticMousePosition->x,
                                                app.diagnosticMousePosition->y);
                if (app.diagnosticMouseDown) {
                    ImGui::GetIO().AddMouseButtonEvent(0, *app.diagnosticMouseDown);
                    app.diagnosticMouseDown.reset();
                }
            }
            ImGui::NewFrame();
            app.ui();
            ImGui::Render();
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            if (app.diagnostics(elapsed))
                done = true;
            // Match interactive pacing during background baking; benchmark uncapped only afterwards.
            app.renderer.endFrame(!app.smoke || app.paint.busy());
        } catch (...) {
            app.error = exceptionText();
            app.record(app.error);
            if (app.smoke) {
                if (!app.metricsPath.empty())
                    writeJson(app.metricsPath, {{"error", app.error}});
                done = true;
            }
        }
    }
    for (auto& job : app.jobs)
        job->cancel = true;
    if (app.stream)
        app.stream->condition.notify_all();
    for (auto& job : app.jobs)
        if (job->thread.joinable())
            job->thread.join();
    app.jobs.clear();
    app.paint.quiesce();
    app.saveSettings();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    active = nullptr;
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, instance);
    return app.error.empty() ? 0 : 1;
}
} // namespace edm
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    try {
        int code = edm::runApp(instance, argc, argv);
        LocalFree(argv);
        return code;
    } catch (...) {
        auto text = edm::exceptionText();
        bool smoke = false;
        edm::fs::path metrics, automaticDirectory;
        for (int i = 1; i < argc; i++) {
            if (std::wstring_view(argv[i]) == L"--smoke")
                smoke = true;
            if (std::wstring_view(argv[i]) == L"--auto-paint-test" ||
                std::wstring_view(argv[i]) == L"--layers-test" ||
                std::wstring_view(argv[i]) == L"--decal-test") {
                smoke = true;
                if (i + 1 < argc)
                    automaticDirectory = argv[++i];
            }
            if (std::wstring_view(argv[i]) == L"--metrics" && i + 1 < argc)
                metrics = argv[++i];
        }
        if (smoke) {
            if (metrics.empty() && !automaticDirectory.empty())
                metrics = automaticDirectory / "metrics.json";
            if (!metrics.empty()) {
                // Invalid/unwritable diagnostic paths may be the original startup error. Reporting
                // that error must not throw again across wWinMain and terminate the process.
                try {
                    edm::writeJson(metrics, {{"error", text}});
                } catch (...) {
                    OutputDebugStringW(L"EDM Studio: cannot write the diagnostic error report.\n");
                }
            }
        } else {
            auto message = edm::wide(text);
            MessageBoxW(nullptr, message.c_str(), L"EDM Studio — 启动失败", MB_OK | MB_ICONERROR);
        }
        LocalFree(argv);
        return 1;
    }
}
