#pragma once
#include "assets.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
namespace edm {
template <class T> using Com = Microsoft::WRL::ComPtr<T>;
struct Camera {
    V3 target = V3::Zero();
    double yaw = .72, pitch = .28, distance = 30, radius = 10;
    V3 eye() const;
    Mat view() const;
    Mat projection(float aspect) const;
    void fit(const Scene& scene, const Args& args = {});
    void orbit(float dx, float dy);
    void pan(float dx, float dy, float height);
    void zoom(float delta);
};
struct GpuVertex {
    F3 position, normal;
    F2 uv, roughUV, decalUV;
    std::array<uint16_t, 8> joints;
    std::array<float, 8> weights;
    uint32_t selector;
};
struct GpuPose {
    std::array<float, 16> world, normal;
};
struct GpuMeshData {
    uint32_t transform, skinned, number, pad;
    F4 color;
    uint32_t blending, decal, twoSided, material;
};
struct GpuDraw {
    uint32_t count, first, base, index;
    bool transparent = false, mirrored = false;
    V3 center = V3::Zero();
    double radius = 0;
    double maximumWeightMagnitude = 1;
    V3 posedMin = V3::Zero(), posedMax = V3::Zero();
};
struct GpuModel {
    std::shared_ptr<Scene> scene;
    Com<ID3D11Buffer> vertices, indices, instances, meshData, pose, number;
    Com<ID3D11ShaderResourceView> meshView, poseView, numberView;
    std::vector<GpuMeshData> data;
    std::vector<GpuDraw> draws;
    std::vector<GpuPose> poses;
    std::vector<F4> numbers;
    std::vector<Mat> world;
    V3 posedMin = V3::Zero(), posedMax = V3::Zero();
    uint64_t vertexUploads = 0, sceneEvaluations = 0;
    size_t geometryBytes = 0;
    double preparationSeconds = 0;
    static std::shared_ptr<GpuModel> prepare(ID3D11Device* device, std::shared_ptr<Scene> scene,
                                             const std::atomic_bool* cancel = nullptr,
                                             Progress progress = {});
    void update(ID3D11DeviceContext* context, const Args& args);
    void highlight(ID3D11DeviceContext* context, std::span<const size_t> meshes);
};
struct RenderOptions {
    bool textures = true, roughmet = true, wireframe = false, grid = true, editedLivery = true;
    float exposure = 1.05f;
};
struct GpuTexture {
    Com<ID3D11ShaderResourceView> view, linearView;
    size_t bytes = 0;
    int width = 0, height = 0;
};
// Surface-attached preview only: it never modifies texture canvases or model geometry.
// Frame axes must be orthonormal, with right cross up == normal; image V points down.
struct SurfaceDecalPreview {
    bool active = false;
    V3 center = V3::Zero(), right = V3::UnitX(), up = V3::UnitY(), normal = V3::UnitZ();
    double width = 1, height = 1, depth = .1;
    float opacity = 1;
    bool frontFacesOnly = true, occlusion = true, preserveAlpha = true;
    std::shared_ptr<GpuTexture> image;
};
struct TextureBindings {
    std::vector<std::array<std::string, 3>> material;
    std::map<std::string, std::shared_ptr<GpuTexture>> images;
    std::vector<std::string> warnings, missing;
    Json resolved;
    size_t bytes = 0;
};
std::shared_ptr<GpuTexture> uploadTexture(ID3D11Device* device, const TextureImage& image);
class Renderer {
    Com<IDXGISwapChain> swap;
    Com<ID3D11RenderTargetView> back;
    Com<ID3D11Texture2D> target, renderColor, depth;
    Com<ID3D11RenderTargetView> targetView;
    Com<ID3D11ShaderResourceView> targetSrv;
    Com<ID3D11DepthStencilView> depthView;
    Com<ID3D11VertexShader> vs, gridVs;
    Com<ID3D11VertexShader> surfaceDepthVs;
    Com<ID3D11PixelShader> ps, gridPs;
    Com<ID3D11InputLayout> layout;
    Com<ID3D11Buffer> frame;
    Com<ID3D11Buffer> surfaceDecalFrame;
    Com<ID3D11Texture2D> surfaceDecalDepth;
    Com<ID3D11DepthStencilView> surfaceDecalDepthView;
    Com<ID3D11ShaderResourceView> surfaceDecalDepthSrv;
    std::weak_ptr<GpuModel> surfaceDecalDepthModel;
    Mat surfaceDecalDepthMatrix = Mat::Zero();
    uint64_t surfaceDecalDepthEvaluation = 0;
    bool surfaceDecalDepthValid = false;
    void updateSurfaceDecal();
    Com<ID3D11RasterizerState> raster[2][3];
    Com<ID3D11DepthStencilState> depthWrite, depthRead, depthOff;
    Com<ID3D11BlendState> blendOpaque, blendAlpha;
    Com<ID3D11SamplerState> sampler;
    std::shared_ptr<GpuTexture> white, rough;
    int targetWidth = 0, targetHeight = 0, backWidth = 0, backHeight = 0;
    UINT msaaSamples = 4;

  public:
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    std::shared_ptr<GpuModel> model;
    TextureBindings textures;
    std::map<int, std::shared_ptr<GpuTexture>> diffuseOverrides;
    Camera camera;
    RenderOptions options;
    SurfaceDecalPreview decalPreview;
    uint64_t decalDepthUpdates = 0;
    uint64_t drawCalls = 0, visibleTriangles = 0;
    double renderMs = 0;
    std::string adapterName;
    void initialize(HWND hwnd);
    void resizeBack(int width, int height);
    void beginFrame();
    void render(int width, int height);
    void endFrame(bool vsync = true);
    ID3D11ShaderResourceView* view() const {
        return targetSrv.Get();
    }
    void capture(const fs::path& path, bool entireWindow = true);
    Json verifyGpu(const Args& args);
    Json viewportMetrics() const {
        return {{"width", targetWidth},
                {"height", targetHeight},
                {"msaa_samples", msaaSamples},
                {"edited_livery_visible", options.editedLivery},
                {"decal_depth_updates", decalDepthUpdates}};
    }
};
} // namespace edm
