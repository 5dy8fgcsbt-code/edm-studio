#include "renderer.h"
#include "paint_conform.h"
#include "shaders.h"
#include <d3dcompiler.h>
#include <limits>
#include <wincodec.h>
namespace edm {
namespace {
void dx(HRESULT r, const char* what) {
    require(SUCCEEDED(r), std::string(what) + " (HRESULT " + std::to_string(uint32_t(r)) + ")");
}
Com<ID3DBlob> shader(const char* entry, const char* profile) {
    Com<ID3DBlob> code, error;
    HRESULT hr = D3DCompile(ModelShader, std::strlen(ModelShader), "EDM Studio renderer", nullptr, nullptr,
                            entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
    require(SUCCEEDED(hr), error ? std::string((const char*)error->GetBufferPointer(), error->GetBufferSize())
                                 : "Shader compilation failed");
    return code;
}
Com<ID3D11Buffer> buffer(ID3D11Device* device, size_t bytes, UINT bind, const void* data = nullptr,
                         UINT stride = 0, bool dynamic = false) {
    require(bytes > 0 && bytes <= UINT_MAX, "Invalid GPU buffer size");
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = UINT(bytes);
    d.BindFlags = bind;
    d.Usage = dynamic ? D3D11_USAGE_DEFAULT : (data ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT);
    if (stride) {
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        d.StructureByteStride = stride;
    }
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = data;
    Com<ID3D11Buffer> out;
    dx(device->CreateBuffer(&d, data ? &init : nullptr, &out), "Create GPU buffer");
    return out;
}
Com<ID3D11ShaderResourceView> structured(ID3D11Device* device, ID3D11Buffer* b, UINT count) {
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    d.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.Buffer.NumElements = count;
    Com<ID3D11ShaderResourceView> out;
    dx(device->CreateShaderResourceView(b, &d, &out), "Create buffer view");
    return out;
}
std::array<float, 16> row(const Mat& m) {
    std::array<float, 16> r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = float(m(i, j));
    return r;
}
GpuPose pose(const Mat& m) {
    Mat normal = Mat::Identity();
    auto linear = m.block<3, 3>(0, 0);
    if (std::abs(linear.determinant()) > 1e-20)
        normal.block<3, 3>(0, 0) = linear.inverse().transpose();
    else
        normal.setZero();
    return {row(m), row(normal)};
}
struct FrameData {
    std::array<float, 16> vp, inverse;
    F4 eye, settings, ground;
};
struct SurfaceDecalData {
    std::array<float, 16> vp;
    F4 centerWidth, rightHeight, upDepth, normalOpacity, options, shadow;
    F4 conformEye;
};
struct SurfaceConformRange {
    uint32_t first = 0, count = 0;
};
struct SurfaceConformTriangle {
    F4 mappingX{}, mappingY{};
    uint32_t triangle = 0, pad0 = 0, pad1 = 0, pad2 = 0;
};
static_assert(sizeof(SurfaceConformRange) == 8 && sizeof(SurfaceConformTriangle) == 48);
static_assert(sizeof(SurfaceDecalData) % 16 == 0);
constexpr UINT surfaceDepthSize = 2048;
} // namespace
V3 Camera::eye() const {
    return target +
           V3(std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)) * distance;
}
Mat Camera::view() const {
    V3 e = eye(), z = (e - target).normalized(), x = V3::UnitY().cross(z).normalized(), y = z.cross(x);
    Mat m = Mat::Identity();
    m.block<1, 3>(0, 0) = x.transpose();
    m.block<1, 3>(1, 0) = y.transpose();
    m.block<1, 3>(2, 0) = z.transpose();
    m(0, 3) = -x.dot(e);
    m(1, 3) = -y.dot(e);
    m(2, 3) = -z.dot(e);
    return m;
}
Mat Camera::projection(float aspect) const {
    double nearPlane = std::max({1e-5, radius * 1e-5, distance * .005, distance - radius * 1.2}),
           farPlane = std::max(nearPlane * 2, distance + radius * 5);
    double f = 1 / std::tan(.65 / 2);
    Mat m = Mat::Zero();
    m(0, 0) = f / aspect;
    m(1, 1) = f;
    m(2, 2) = farPlane / (nearPlane - farPlane);
    m(2, 3) = nearPlane * farPlane / (nearPlane - farPlane);
    m(3, 2) = -1;
    return m;
}
void Camera::fit(const Scene& scene, const Args& args) {
    auto world = scene.evaluate(args);
    V3 lo = V3::Constant(1e100), hi = V3::Constant(-1e100);
    for (auto& mesh : scene.meshes) {
        auto positions = scene.transformed(mesh, world);
        for (auto& p : positions) {
            V3 v(p[0], p[1], p[2]);
            lo = lo.cwiseMin(v);
            hi = hi.cwiseMax(v);
        }
    }
    target = (lo + hi) * .5;
    radius = std::max(.01, (hi - lo).norm() * .5);
    distance = radius * 2.15;
}
void Camera::orbit(float dx, float dy) {
    yaw -= dx * .007;
    pitch = std::clamp(pitch + dy * .007, -1.54, 1.54);
}
void Camera::pan(float dx, float dy, float height) {
    double scale = distance * .68 / std::max(1.f, height);
    Mat v = view();
    target += (-v.block<1, 3>(0, 0).transpose() * dx + v.block<1, 3>(1, 0).transpose() * dy) * scale;
}
void Camera::zoom(float delta) {
    distance = std::clamp(distance * std::exp(-double(delta) * .12), radius * .015, radius * 1000);
}
std::shared_ptr<GpuModel> GpuModel::prepare(ID3D11Device* device, std::shared_ptr<Scene> scene,
                                            const std::atomic_bool* cancel, Progress progress) {
    auto start = Clock::now();
    auto out = std::make_shared<GpuModel>();
    out->scene = scene;
    size_t vertexCount = 0, indexCount = 0;
    for (auto& m : scene->meshes) {
        vertexCount += m.positions.size();
        indexCount += m.indices.size();
    }
    require(vertexCount <= INT_MAX && indexCount <= UINT_MAX, "Model exceeds native buffer addressing limit");
    std::vector<GpuVertex> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve(vertexCount);
    indices.reserve(indexCount);
    out->data.reserve(scene->meshes.size());
    out->draws.reserve(scene->meshes.size());
    for (size_t mi = 0; mi < scene->meshes.size(); mi++) {
        if (cancel && *cancel)
            throw std::runtime_error("Cancelled");
        auto& mesh = scene->meshes[mi];
        auto& mat = scene->materials[mesh.material];
        GpuMeshData data{};
        data.transform = UINT(out->poses.size());
        data.skinned = mesh.skinned();
        data.number = UINT(out->numbers.size());
        data.color = materialColor(mat);
        data.alphaMode = uint32_t(materialAlphaMode(mat));
        data.decal = !mesh.selectors.empty();
        data.pad = !mesh.uvs.empty() && mesh.numbers.empty() ? 2u : 0u;
        data.twoSided = mat.culling == 1;
        data.material = mesh.material;
        out->poses.resize(out->poses.size() + (mesh.skinned() ? mesh.skinNodes.size() : 1));
        out->numbers.resize(out->numbers.size() + std::max(size_t(1), mesh.numbers.size()));
        GpuDraw draw{};
        draw.count = UINT(mesh.indices.size());
        draw.first = UINT(indices.size());
        draw.base = UINT(vertices.size());
        draw.index = UINT(mi);
        draw.maximumWeightMagnitude = mesh.skinned() ? 0 : 1;
        draw.transparent = !data.decal && materialAlphaMode(mat) == MaterialAlphaMode::Blend;
        auto uv = textureUV(mesh, mat, 0), rm = textureUV(mesh, mat, 13), decal = textureUV(mesh, mat, 3);
        V3 lo = V3::Constant(1e100), hi = V3::Constant(-1e100);
        for (size_t i = 0; i < mesh.positions.size(); i++) {
            GpuVertex v{};
            v.position = mesh.positions[i];
            v.normal = mesh.normals[i];
            v.uv = uv[i];
            v.roughUV = rm[i];
            v.decalUV = decal[i];
            if (mesh.skinned()) {
                v.joints = mesh.joints[i];
                v.weights = mesh.weights[i];
                double magnitude = 0;
                for (float weight : v.weights)
                    magnitude += std::abs(weight);
                draw.maximumWeightMagnitude = std::max(draw.maximumWeightMagnitude, magnitude);
            }
            if (!mesh.selectors.empty())
                v.selector = mesh.selectors[i];
            vertices.push_back(v);
            V3 p(v.position[0], v.position[1], v.position[2]);
            lo = lo.cwiseMin(p);
            hi = hi.cwiseMax(p);
        }
        draw.center = (lo + hi) * .5;
        draw.radius = mesh.positions.empty() ? 0 : (hi - lo).norm() * .5;
        indices.insert(indices.end(), mesh.indices.begin(), mesh.indices.end());
        out->data.push_back(data);
        out->draws.push_back(draw);
        if (progress && mi % 100 == 0)
            progress("准备 GPU 网格 " + std::to_string(mi + 1) + " / " +
                     std::to_string(scene->meshes.size()));
    }
    out->geometryBytes = vertices.size() * sizeof(GpuVertex) + indices.size() * 4;
    out->vertices =
        buffer(device, vertices.size() * sizeof(GpuVertex), D3D11_BIND_VERTEX_BUFFER, vertices.data());
    out->indices = buffer(device, indices.size() * 4, D3D11_BIND_INDEX_BUFFER, indices.data());
    out->vertexUploads = 1;
    std::vector<uint32_t> instanceIds(scene->meshes.size());
    std::iota(instanceIds.begin(), instanceIds.end(), 0);
    out->instances = buffer(device, instanceIds.size() * 4, D3D11_BIND_VERTEX_BUFFER, instanceIds.data());
    out->meshData = buffer(device, out->data.size() * sizeof(GpuMeshData), D3D11_BIND_SHADER_RESOURCE,
                           out->data.data(), sizeof(GpuMeshData), true);
    out->meshView = structured(device, out->meshData.Get(), UINT(out->data.size()));
    out->pose = buffer(device, out->poses.size() * sizeof(GpuPose), D3D11_BIND_SHADER_RESOURCE, nullptr,
                       sizeof(GpuPose), true);
    out->poseView = structured(device, out->pose.Get(), UINT(out->poses.size()));
    out->number = buffer(device, out->numbers.size() * sizeof(F4), D3D11_BIND_SHADER_RESOURCE, nullptr,
                         sizeof(F4), true);
    out->numberView = structured(device, out->number.Get(), UINT(out->numbers.size()));
    out->preparationSeconds = seconds(start);
    return out;
}
void GpuModel::highlight(ID3D11DeviceContext* context, std::span<const size_t> meshes) {
    std::vector<bool> selected(data.size(), false);
    for (size_t index : meshes)
        if (index < selected.size())
            selected[index] = true;
    bool changed = false;
    for (size_t i = 0; i < data.size(); ++i) {
        uint32_t value = (data[i].pad & ~1u) | (selected[i] ? 1u : 0u);
        changed |= data[i].pad != value;
        data[i].pad = value;
    }
    if (changed)
        context->UpdateSubresource(meshData.Get(), 0, nullptr, data.data(), 0, 0);
}
void GpuModel::update(ID3D11DeviceContext* context, const Args& args, bool attachmentsVisible) {
    world = scene->evaluate(args, attachmentsVisible);
    Args effective = scene->defaultArgs;
    for (auto [argument, value] : args)
        effective[argument] = value;
    sceneEvaluations++;
    posedMin = V3::Constant(std::numeric_limits<double>::infinity());
    posedMax = -posedMin;
    for (size_t mi = 0; mi < scene->meshes.size(); mi++) {
        auto& m = scene->meshes[mi];
        draws[mi].visible = attachmentsVisible || !m.extras.contains("edm_attachment");
        size_t base = data[mi].transform;
        if (m.skinned()) {
            for (size_t j = 0; j < m.skinNodes.size(); j++)
                poses[base + j] = edm::pose(world[m.skinNodes[j]] * m.inverseBind[j]);
            draws[mi].mirrored = false;
        } else {
            poses[base] = edm::pose(world[m.node]);
            draws[mi].mirrored = world[m.node].block<3, 3>(0, 0).determinant() < 0;
        }
        // Conservative posed bounds cost O(bones), not O(vertices), and include arbitrary skin
        // weight magnitudes. They locate the decal's emitter in front of the complete model.
        auto& draw = draws[mi];
        auto transformedSphere = [&](const Mat& transform) {
            V3 center = (transform * V4(draw.center.x(), draw.center.y(), draw.center.z(), 1)).head<3>();
            V3 extent;
            for (int axis = 0; axis < 3; ++axis)
                extent[axis] = transform.block<1, 3>(axis, 0).norm() * draw.radius;
            return std::pair<V3, V3>{center, extent};
        };
        if (m.skinned()) {
            V3 extent = V3::Zero();
            for (size_t j = 0; j < m.skinNodes.size(); ++j) {
                auto [center, radius] = transformedSphere(world[m.skinNodes[j]] * m.inverseBind[j]);
                extent = extent.cwiseMax(center.cwiseAbs() + radius);
            }
            extent *= draw.maximumWeightMagnitude;
            draw.posedMin = -extent;
            draw.posedMax = extent;
        } else {
            auto [center, radius] = transformedSphere(world[m.node]);
            draw.posedMin = center - radius;
            draw.posedMax = center + radius;
        }
        if (draw.visible && draw.count && m.numbers.empty() &&
            (m.skinned() || world[m.node].block<3, 3>(0, 0).cwiseAbs().maxCoeff() >= 1e-20)) {
            posedMin = posedMin.cwiseMin(draw.posedMin);
            posedMax = posedMax.cwiseMax(draw.posedMax);
        }
        for (size_t j = 0; j < m.numbers.size(); j++) {
            auto& c = m.numbers[j];
            numbers[data[mi].number + j] = {float(c.u != -1 ? argValue(effective, c.u) * c.su : 0),
                                            float(c.v != -1 ? argValue(effective, c.v) * c.sv : 0), 0, 0};
        }
    }
    if (!posedMin.allFinite() || !posedMax.allFinite())
        posedMin = posedMax = V3::Zero();
    context->UpdateSubresource(pose.Get(), 0, nullptr, poses.data(), 0, 0);
    context->UpdateSubresource(number.Get(), 0, nullptr, numbers.data(), 0, 0);
}
std::shared_ptr<GpuTexture> uploadTexture(ID3D11Device* device, const TextureImage& image) {
    auto meta = image.pixels.GetMetadata();
    auto* first = image.pixels.GetImage(image.firstMip, 0, 0);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = UINT(first->width);
    d.Height = UINT(first->height);
    d.MipLevels = UINT(meta.mipLevels - image.firstMip);
    d.ArraySize = 1;
    d.Format = DirectX::MakeTypeless(meta.format);
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<D3D11_SUBRESOURCE_DATA> initial;
    for (size_t mip = image.firstMip; mip < meta.mipLevels; mip++) {
        auto* p = image.pixels.GetImage(mip, 0, 0);
        initial.push_back({p->pixels, UINT(p->rowPitch), UINT(p->slicePitch)});
    }
    Com<ID3D11Texture2D> texture;
    dx(device->CreateTexture2D(&d, initial.data(), &texture), "Upload material texture");
    auto out = std::make_shared<GpuTexture>();
    out->sourceWidth = int(meta.width);
    out->sourceHeight = int(meta.height);
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = d.MipLevels;
    srv.Format = DirectX::MakeSRGB(DirectX::MakeTypelessUNORM(meta.format));
    dx(device->CreateShaderResourceView(texture.Get(), &srv, &out->view), "Create sRGB texture view");
    srv.Format = DirectX::MakeTypelessUNORM(meta.format);
    if (DirectX::IsSRGB(srv.Format)) {
        switch (srv.Format) {
        case DXGI_FORMAT_BC1_UNORM_SRGB:
            srv.Format = DXGI_FORMAT_BC1_UNORM;
            break;
        case DXGI_FORMAT_BC2_UNORM_SRGB:
            srv.Format = DXGI_FORMAT_BC2_UNORM;
            break;
        case DXGI_FORMAT_BC3_UNORM_SRGB:
            srv.Format = DXGI_FORMAT_BC3_UNORM;
            break;
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            srv.Format = DXGI_FORMAT_BC7_UNORM;
            break;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            break;
        default:
            break;
        }
    }
    dx(device->CreateShaderResourceView(texture.Get(), &srv, &out->linearView), "Create linear texture view");
    out->bytes = image.bytes();
    out->width = d.Width;
    out->height = d.Height;
    return out;
}
void Renderer::initialize(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC swapDesc{};
    swapDesc.BufferCount = 2;
    swapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.OutputWindow = hwnd;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.Windowed = TRUE;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_0}, level;
    dx(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested, 1, D3D11_SDK_VERSION,
                                     &swapDesc, &swap, &device, &level, &context),
       "Initialize DirectX 11");
    Com<IDXGIDevice> dxgi;
    device.As(&dxgi);
    Com<IDXGIAdapter> adapter;
    dxgi->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC desc{};
    adapter->GetDesc(&desc);
    adapterName = utf8(desc.Description);
    Com<IDXGIDevice1> latency;
    if (SUCCEEDED(device.As(&latency)))
        latency->SetMaximumFrameLatency(1);
    UINT quality = 0;
    if (FAILED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 4, &quality)) ||
        quality == 0)
        msaaSamples = 1;
    auto vertex = shader("VS", "vs_5_0"), pixel = shader("PS", "ps_5_0"), gv = shader("GridVS", "vs_5_0"),
         gp = shader("GridPS", "ps_5_0"), surfaceVertex = shader("SurfaceDepthVS", "vs_5_0");
    dx(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vs),
       "Vertex shader");
    dx(device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &ps),
       "Pixel shader");
    dx(device->CreateVertexShader(gv->GetBufferPointer(), gv->GetBufferSize(), nullptr, &gridVs),
       "Grid vertex shader");
    dx(device->CreatePixelShader(gp->GetBufferPointer(), gp->GetBufferSize(), nullptr, &gridPs),
       "Grid pixel shader");
    dx(device->CreateVertexShader(surfaceVertex->GetBufferPointer(), surfaceVertex->GetBufferSize(), nullptr,
                                  &surfaceDepthVs),
       "Surface decal depth shader");
    D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, UINT(offsetof(GpuVertex, position)),
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, UINT(offsetof(GpuVertex, normal)),
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, UINT(offsetof(GpuVertex, uv)),
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, UINT(offsetof(GpuVertex, roughUV)),
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, UINT(offsetof(GpuVertex, decalUV)),
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, UINT(offsetof(GpuVertex, joints)),
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 1, DXGI_FORMAT_R16G16B16A16_UINT, 0, UINT(offsetof(GpuVertex, joints) + 8),
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, UINT(offsetof(GpuVertex, weights)),
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, UINT(offsetof(GpuVertex, weights) + 16),
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 3, DXGI_FORMAT_R32_UINT, 0, UINT(offsetof(GpuVertex, selector)),
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 4, DXGI_FORMAT_R32_UINT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1}};
    dx(device->CreateInputLayout(elements, UINT(std::size(elements)), vertex->GetBufferPointer(),
                                 vertex->GetBufferSize(), &layout),
       "Vertex input layout");
    frame = buffer(device.Get(), sizeof(FrameData), D3D11_BIND_CONSTANT_BUFFER);
    surfaceDecalFrame = buffer(device.Get(), sizeof(SurfaceDecalData), D3D11_BIND_CONSTANT_BUFFER);
    for (int wire = 0; wire < 2; wire++)
        for (int cull = 0; cull < 3; cull++) {
            D3D11_RASTERIZER_DESC r{};
            r.FillMode = wire ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
            r.CullMode = cull == 2 ? D3D11_CULL_NONE : D3D11_CULL_BACK;
            r.FrontCounterClockwise = cull == 0;
            r.DepthClipEnable = TRUE;
            r.MultisampleEnable = TRUE;
            dx(device->CreateRasterizerState(&r, &raster[wire][cull]), "Raster state");
        }
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    device->CreateDepthStencilState(&dd, &depthWrite);
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    device->CreateDepthStencilState(&dd, &depthRead);
    dd.DepthEnable = FALSE;
    device->CreateDepthStencilState(&dd, &depthOff);
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bd, &blendOpaque);
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    device->CreateBlendState(&bd, &blendAlpha);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.MaxAnisotropy = 8;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sd, &sampler);
    auto pixelTexture = [&](std::array<uint8_t, 4> p) {
        TextureImage i;
        i.pixels.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1);
        std::memcpy(i.pixels.GetPixels(), p.data(), 4);
        return uploadTexture(device.Get(), i);
    };
    white = pixelTexture({255, 255, 255, 255});
    rough = pixelTexture({255, 166, 0, 255});
    RECT rect;
    GetClientRect(hwnd, &rect);
    resizeBack(rect.right, rect.bottom);
}
void Renderer::resizeBack(int width, int height) {
    if (width <= 0 || height <= 0)
        return;
    context->OMSetRenderTargets(0, nullptr, nullptr);
    back.Reset();
    dx(swap->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0), "Resize window");
    Com<ID3D11Texture2D> buffer;
    swap->GetBuffer(0, IID_PPV_ARGS(&buffer));
    dx(device->CreateRenderTargetView(buffer.Get(), nullptr, &back), "Window render target");
    backWidth = width;
    backHeight = height;
}
void Renderer::beginFrame() {
    float color[]{.035f, .045f, .063f, 1};
    context->ClearRenderTargetView(back.Get(), color);
}
void Renderer::updateSurfaceConform() {
    const auto& patch = decalPreview.conformPatch;
    if (!patch || !model || patch->sourceScene != model->scene.get() || patch->triangles.empty())
        return;
    if (surfaceConformPatch == patch && surfaceConformModel.lock() == model)
        return;
    std::vector<const SurfaceDecalPatchTriangle*> ordered;
    ordered.reserve(patch->triangles.size());
    for (const auto& triangle : patch->triangles)
        ordered.push_back(&triangle);
    std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
        if (a->triangle.mesh != b->triangle.mesh)
            return a->triangle.mesh < b->triangle.mesh;
        return a->triangle.index < b->triangle.index;
    });
    std::vector<SurfaceConformRange> ranges(model->scene->meshes.size());
    std::vector<SurfaceConformTriangle> triangles;
    triangles.reserve(ordered.size());
    for (const auto* source : ordered) {
        const auto& triangle = source->triangle;
        require(triangle.mesh < ranges.size() &&
                    triangle.index < model->scene->meshes[triangle.mesh].indices.size() / 3,
                "Curved decal preview has a stale source triangle");
        auto& range = ranges[triangle.mesh];
        if (!range.count)
            range.first = uint32_t(triangles.size());
        else
            require(triangles.back().triangle != triangle.index,
                    "Curved decal preview contains a duplicate source triangle");
        V3 a = triangle.world[1] - triangle.world[0], b = triangle.world[2] - triangle.world[0];
        V3 cross = a.cross(b);
        double determinant = cross.squaredNorm();
        require(std::isfinite(determinant) && determinant > 1e-30,
                "Curved decal preview contains degenerate geometry");
        V3 g1 = b.cross(cross) / determinant, g2 = cross.cross(a) / determinant;
        SurfaceConformTriangle mapped;
        for (int axis = 0; axis < 2; ++axis) {
            V3 gradient = g1 * (source->coordinates[1][axis] - source->coordinates[0][axis]) +
                          g2 * (source->coordinates[2][axis] - source->coordinates[0][axis]);
            double intercept = source->coordinates[0][axis] - gradient.dot(triangle.world[0]);
            require(gradient.allFinite() && std::isfinite(intercept),
                    "Curved decal preview contains invalid chart coordinates");
            require(gradient.cwiseAbs().maxCoeff() <= std::numeric_limits<float>::max() &&
                        std::abs(intercept) <= std::numeric_limits<float>::max(),
                    "Curved decal preview chart exceeds GPU coordinate precision");
            F4 coefficients{float(gradient.x()), float(gradient.y()), float(gradient.z()),
                            float(intercept)};
            if (axis == 0)
                mapped.mappingX = coefficients;
            else
                mapped.mappingY = coefficients;
        }
        mapped.triangle = triangle.index;
        triangles.push_back(mapped);
        ++range.count;
    }
    // Build all resources before publication. The scene's geometry buffers and source UVs stay intact.
    auto rangeBuffer = buffer(device.Get(), ranges.size() * sizeof(SurfaceConformRange),
                              D3D11_BIND_SHADER_RESOURCE, ranges.data(), sizeof(SurfaceConformRange));
    auto triangleBuffer = buffer(device.Get(), triangles.size() * sizeof(SurfaceConformTriangle),
                                 D3D11_BIND_SHADER_RESOURCE, triangles.data(),
                                 sizeof(SurfaceConformTriangle));
    auto rangeView = structured(device.Get(), rangeBuffer.Get(), UINT(ranges.size()));
    auto triangleView = structured(device.Get(), triangleBuffer.Get(), UINT(triangles.size()));
    surfaceConformRanges = std::move(rangeBuffer);
    surfaceConformTriangles = std::move(triangleBuffer);
    surfaceConformRangesView = std::move(rangeView);
    surfaceConformTrianglesView = std::move(triangleView);
    surfaceConformPatch = patch;
    surfaceConformModel = model;
    surfaceConformProjectionValid = false;
    ++decalPatchUploads;
}
void Renderer::updateSurfaceDecal() {
    const auto& preview = decalPreview;
    if (surfaceConformPatch && (!model || surfaceConformModel.lock() != model)) {
        surfaceConformPatch.reset();
        surfaceConformModel.reset();
        surfaceConformRanges.Reset();
        surfaceConformTriangles.Reset();
        surfaceConformRangesView.Reset();
        surfaceConformTrianglesView.Reset();
        surfaceConformProjectionValid = false;
    }
    SurfaceDecalData data{};
    ID3D11ShaderResourceView* resources[4]{};
    bool conform = bool(preview.conformPatch);
    bool active = options.editedLivery && preview.active && model && preview.image && preview.image->view &&
                  preview.center.allFinite() && preview.right.allFinite() && preview.up.allFinite() &&
                  preview.normal.allFinite() && std::isfinite(preview.width) && preview.width > 1e-8 &&
                  std::isfinite(preview.height) && preview.height > 1e-8 && std::isfinite(preview.depth) &&
                  preview.depth > 1e-8 && std::isfinite(preview.opacity) && preview.opacity > 0 &&
                  std::abs(preview.right.squaredNorm() - 1) < 1e-4 &&
                  std::abs(preview.up.squaredNorm() - 1) < 1e-4 &&
                  std::abs(preview.normal.squaredNorm() - 1) < 1e-4 &&
                  (preview.right.cross(preview.up) - preview.normal).norm() < 1e-4;
    if (active && conform) {
        active = preview.conformPatch->sourceScene == model->scene.get() &&
                 !preview.conformPatch->triangles.empty() && preview.projectionVP.allFinite() &&
                 ((!preview.occlusion && !preview.frontFacesOnly) ||
                  (preview.conformPatch->projectionEye && preview.conformPatch->projectionEye->allFinite()));
        if (active)
            updateSurfaceConform();
    }
    if (active) {
        V3 boundsCenter = (model->posedMin + model->posedMax) * .5;
        V3 boundsExtent = (model->posedMax - model->posedMin) * .5;
        double margin = std::max(1e-4, boundsExtent.norm() * 2e-5);
        double front = std::max(preview.depth, preview.normal.dot(boundsCenter - preview.center) +
                                                   preview.normal.cwiseAbs().dot(boundsExtent)) +
                       margin;
        double range = front + preview.depth;
        Mat matrix = Mat::Identity();
        matrix.block<1, 3>(0, 0) = preview.right.transpose() * (2 / preview.width);
        matrix.block<1, 3>(1, 0) = preview.up.transpose() * (2 / preview.height);
        matrix.block<1, 3>(2, 0) = -preview.normal.transpose() / range;
        matrix(0, 3) = -2 * preview.right.dot(preview.center) / preview.width;
        matrix(1, 3) = -2 * preview.up.dot(preview.center) / preview.height;
        matrix(2, 3) = (front + preview.normal.dot(preview.center)) / range;
        if (conform) {
            matrix = preview.projectionVP;
            // A placed chart can extend beyond the viewport. Enlarge only the frozen camera's
            // horizontal/vertical coverage so these texels still receive placement-eye occlusion.
            if (!surfaceConformProjectionValid ||
                !(surfaceConformProjectionInput.array() == preview.projectionVP.array()).all()) {
                double extentX = 1, extentY = 1;
                for (const auto& item : preview.conformPatch->triangles)
                    for (const auto& point : item.triangle.world) {
                        V4 clip = matrix * V4(point.x(), point.y(), point.z(), 1);
                        if (clip.w() > 1e-8) {
                            extentX = std::max(extentX, std::abs(clip.x() / clip.w()) * 1.01);
                            extentY = std::max(extentY, std::abs(clip.y() / clip.w()) * 1.01);
                        }
                    }
                matrix.row(0) /= extentX;
                matrix.row(1) /= extentY;
                surfaceConformProjectionInput = preview.projectionVP;
                surfaceConformProjection = matrix;
                surfaceConformProjectionValid = true;
            } else
                matrix = surfaceConformProjection;
            if (preview.conformPatch->projectionEye) {
                const V3& eye = *preview.conformPatch->projectionEye;
                data.conformEye = {float(eye.x()), float(eye.y()), float(eye.z()),
                                   float(preview.conformPatch->normalSign)};
            }
            resources[2] = surfaceConformRangesView.Get();
            resources[3] = surfaceConformTrianglesView.Get();
        }
        data.vp = row(matrix);
        data.centerWidth = {float(preview.center.x()), float(preview.center.y()), float(preview.center.z()),
                            float(preview.width)};
        data.rightHeight = {float(preview.right.x()), float(preview.right.y()), float(preview.right.z()),
                            float(preview.height)};
        data.upDepth = {float(preview.up.x()), float(preview.up.y()), float(preview.up.z()),
                        float(preview.depth)};
        data.normalOpacity = {float(preview.normal.x()), float(preview.normal.y()), float(preview.normal.z()),
                              std::clamp(preview.opacity, 0.f, 1.f)};
        data.options = {conform ? 2.f : 1.f, float(preview.frontFacesOnly), float(preview.occlusion),
                        float(preview.preserveAlpha)};
        // Receiver-plane correction in the PS removes slope acne without a large normal offset.
        // The remaining bias covers floating-point error and the CPU ray's intersection tolerance.
        data.shadow = {conform ? 1e-6f : float(std::max(1e-6, range * 2e-6) / range), 1.f / surfaceDepthSize,
                       preview.image->linearView ? 1.f : 0.f, 0};
        context->UpdateSubresource(surfaceDecalFrame.Get(), 0, nullptr, &data, 0, 0);
        context->VSSetConstantBuffers(1, 1, surfaceDecalFrame.GetAddressOf());
        resources[0] =
            preview.image->linearView ? preview.image->linearView.Get() : preview.image->view.Get();
        if (preview.occlusion) {
            if (!surfaceDecalDepth) {
                // Publish the complete depth resource together; a failed allocation is retryable.
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = desc.Height = surfaceDepthSize;
                desc.MipLevels = desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_R32_TYPELESS;
                desc.SampleDesc.Count = 1;
                desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
                Com<ID3D11Texture2D> texture;
                Com<ID3D11DepthStencilView> depthView;
                Com<ID3D11ShaderResourceView> view;
                dx(device->CreateTexture2D(&desc, nullptr, &texture), "Surface decal depth texture");
                D3D11_DEPTH_STENCIL_VIEW_DESC depthDesc{};
                depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
                depthDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                dx(device->CreateDepthStencilView(texture.Get(), &depthDesc, &depthView),
                   "Surface decal depth target");
                D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
                viewDesc.Format = DXGI_FORMAT_R32_FLOAT;
                viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                viewDesc.Texture2D.MipLevels = 1;
                dx(device->CreateShaderResourceView(texture.Get(), &viewDesc, &view),
                   "Surface decal depth lookup");
                surfaceDecalDepth = std::move(texture);
                surfaceDecalDepthView = std::move(depthView);
                surfaceDecalDepthSrv = std::move(view);
                surfaceDecalDepthValid = false;
            }
            if (!surfaceDecalDepthValid || surfaceDecalDepthModel.lock() != model ||
                surfaceDecalDepthEvaluation != model->sceneEvaluations ||
                !(surfaceDecalDepthMatrix.array() == matrix.array()).all()) {
                ID3D11ShaderResourceView* none = nullptr;
                context->PSSetShaderResources(7, 1, &none);
                context->OMSetRenderTargets(0, nullptr, surfaceDecalDepthView.Get());
                context->ClearDepthStencilView(surfaceDecalDepthView.Get(), D3D11_CLEAR_DEPTH, 1, 0);
                D3D11_VIEWPORT viewport{0, 0, float(surfaceDepthSize), float(surfaceDepthSize), 0, 1};
                context->RSSetViewports(1, &viewport);
                context->RSSetState(raster[0][2].Get()); // Every geometric face occludes, including backs.
                context->OMSetDepthStencilState(depthWrite.Get(), 0);
                context->OMSetBlendState(blendOpaque.Get(), nullptr, 0xffffffff);
                context->VSSetShader(surfaceDepthVs.Get(), nullptr, 0);
                context->PSSetShader(nullptr, nullptr, 0);
                UINT strides[2]{sizeof(GpuVertex), 4}, offsets[2]{};
                ID3D11Buffer* vertices[]{model->vertices.Get(), model->instances.Get()};
                context->IASetVertexBuffers(0, 2, vertices, strides, offsets);
                context->IASetIndexBuffer(model->indices.Get(), DXGI_FORMAT_R32_UINT, 0);
                context->IASetInputLayout(layout.Get());
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ID3D11ShaderResourceView* meshes[]{model->meshView.Get(), model->poseView.Get(),
                                                   model->numberView.Get()};
                context->VSSetShaderResources(0, 3, meshes);
                for (const auto& draw : model->draws) {
                    const auto& mesh = model->scene->meshes[draw.index];
                    if (!draw.visible || !draw.count || !mesh.numbers.empty() ||
                        (!mesh.skinned() &&
                         model->world[mesh.node].block<3, 3>(0, 0).cwiseAbs().maxCoeff() < 1e-20))
                        continue;
                    V3 center = (draw.posedMin + draw.posedMax) * .5 - preview.center;
                    V3 extent = (draw.posedMax - draw.posedMin) * .5;
                    if (!conform && (std::abs(center.dot(preview.right)) >
                            preview.width * .5 + preview.right.cwiseAbs().dot(extent) + margin ||
                        std::abs(center.dot(preview.up)) >
                            preview.height * .5 + preview.up.cwiseAbs().dot(extent) + margin ||
                        center.dot(preview.normal) + preview.normal.cwiseAbs().dot(extent) <
                            -preview.depth - margin))
                        continue;
                    context->DrawIndexedInstanced(draw.count, 1, draw.first, draw.base, draw.index);
                }
                context->OMSetRenderTargets(0, nullptr, nullptr);
                surfaceDecalDepthModel = model;
                surfaceDecalDepthEvaluation = model->sceneEvaluations;
                surfaceDecalDepthMatrix = matrix;
                surfaceDecalDepthValid = true;
                ++decalDepthUpdates;
            }
            resources[1] = surfaceDecalDepthSrv.Get();
        }
    } else {
        context->UpdateSubresource(surfaceDecalFrame.Get(), 0, nullptr, &data, 0, 0);
        context->VSSetConstantBuffers(1, 1, surfaceDecalFrame.GetAddressOf());
    }
    context->PSSetConstantBuffers(1, 1, surfaceDecalFrame.GetAddressOf());
    context->PSSetShaderResources(6, 4, resources);
}
void Renderer::render(int width, int height) {
    auto start = Clock::now();
    width = std::max(1, width);
    height = std::max(1, height);
    if (width != targetWidth || height != targetHeight) {
        ID3D11ShaderResourceView* none = nullptr;
        context->PSSetShaderResources(0, 1, &none);
        targetView.Reset();
        targetSrv.Reset();
        depthView.Reset();
        target.Reset();
        renderColor.Reset();
        depth.Reset();
        D3D11_TEXTURE2D_DESC t{};
        t.Width = width;
        t.Height = height;
        t.MipLevels = t.ArraySize = 1;
        t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        t.SampleDesc.Count = 1;
        t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        dx(device->CreateTexture2D(&t, nullptr, &target), "Viewport texture");
        device->CreateShaderResourceView(target.Get(), nullptr, &targetSrv);
        t.SampleDesc.Count = msaaSamples;
        t.BindFlags = D3D11_BIND_RENDER_TARGET;
        dx(device->CreateTexture2D(&t, nullptr, &renderColor), "Antialiased viewport");
        device->CreateRenderTargetView(renderColor.Get(), nullptr, &targetView);
        t.Format = DXGI_FORMAT_D32_FLOAT;
        t.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        dx(device->CreateTexture2D(&t, nullptr, &depth), "Viewport depth");
        device->CreateDepthStencilView(depth.Get(), nullptr, &depthView);
        targetWidth = width;
        targetHeight = height;
    }
    updateSurfaceDecal();
    float color[]{.043f, .058f, .081f, 1};
    context->ClearRenderTargetView(targetView.Get(), color);
    context->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_DEPTH, 1, 0);
    context->OMSetRenderTargets(1, targetView.GetAddressOf(), depthView.Get());
    D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    context->RSSetViewports(1, &viewport);
    Mat vp = camera.projection(float(width) / height) * camera.view();
    V3 eye = camera.eye();
    FrameData frameData{row(vp),
                        row(vp.inverse()),
                        {float(eye[0]), float(eye[1]), float(eye[2]), options.exposure},
                        {float(options.textures), float(options.roughmet), 0, 0},
                        {float(camera.target.y() - camera.radius * .22),
                         float(std::pow(10., std::floor(std::log10(camera.radius / 8)))),
                         float(camera.target.x()), float(camera.target.z())}};
    context->UpdateSubresource(frame.Get(), 0, nullptr, &frameData, 0, 0);
    context->VSSetConstantBuffers(0, 1, frame.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, frame.GetAddressOf());
    drawCalls = visibleTriangles = 0;
    if (model) {
        UINT strides[2]{sizeof(GpuVertex), 4}, offsets[2]{};
        ID3D11Buffer* vbs[]{model->vertices.Get(), model->instances.Get()};
        context->IASetVertexBuffers(0, 2, vbs, strides, offsets);
        context->IASetIndexBuffer(model->indices.Get(), DXGI_FORMAT_R32_UINT, 0);
        context->IASetInputLayout(layout.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[]{model->meshView.Get(), model->poseView.Get(),
                                         model->numberView.Get()};
        context->VSSetShaderResources(0, 3, srvs);
        context->PSSetShaderResources(0, 1, srvs);
        context->PSSetSamplers(0, 1, sampler.GetAddressOf());
        std::vector<const GpuDraw*> sorted;
        sorted.reserve(model->draws.size());
        for (auto& draw : model->draws) {
            if (!draw.visible)
                continue;
            auto& mesh = model->scene->meshes[draw.index];
            if (!mesh.skinned() && model->world[mesh.node].block<3, 3>(0, 0).cwiseAbs().maxCoeff() < 1e-20)
                continue;
            sorted.push_back(&draw);
        }
        std::sort(sorted.begin(), sorted.end(), [&](auto* a, auto* b) {
            if (a->transparent != b->transparent)
                return a->transparent < b->transparent;
            auto& ma = model->data[a->index];
            auto& mb = model->data[b->index];
            if (ma.decal != mb.decal)
                return ma.decal < mb.decal;
            if (a->transparent) {
                auto& am = model->scene->meshes[a->index];
                auto& bm = model->scene->meshes[b->index];
                V4 ac(a->center.x(), a->center.y(), a->center.z(), 1),
                    bc(b->center.x(), b->center.y(), b->center.z(), 1);
                double da = ((model->world[am.node] * ac).head<3>() - eye).squaredNorm(),
                       db = ((model->world[bm.node] * bc).head<3>() - eye).squaredNorm();
                return da > db;
            }
            if (ma.material != mb.material)
                return ma.material < mb.material;
            return a->index < b->index;
        });
        int lastMaterial = -1, lastRaster = -1, lastBlend = -1;
        for (auto* draw : sorted) {
            auto& m = model->data[draw->index];
            int rasterIndex = m.twoSided ? 2 : draw->mirrored ? 1 : 0;
            if (rasterIndex != lastRaster) {
                context->RSSetState(raster[options.wireframe][rasterIndex].Get());
                lastRaster = rasterIndex;
            }
            int blend = draw->transparent;
            if (blend != lastBlend) {
                context->OMSetBlendState(blend ? blendAlpha.Get() : blendOpaque.Get(), nullptr, 0xffffffff);
                context->OMSetDepthStencilState(blend ? depthRead.Get() : depthWrite.Get(), 0);
                lastBlend = blend;
            }
            if (int(m.material) != lastMaterial) {
                ID3D11ShaderResourceView* maps[]{white->view.Get(), rough->linearView.Get(),
                                                 white->view.Get()};
                if (m.material < textures.material.size())
                    for (int slot = 0; slot < 3; slot++) {
                        auto it = textures.images.find(textures.material[m.material][slot]);
                        if (it != textures.images.end())
                            maps[slot] = slot == 1 ? it->second->linearView.Get() : it->second->view.Get();
                    }
                if (options.editedLivery)
                    if (auto edited = diffuseOverrides.find(int(m.material));
                        edited != diffuseOverrides.end())
                        maps[0] = edited->second->view.Get();
                context->PSSetShaderResources(3, 3, maps);
                lastMaterial = m.material;
            }
            context->DrawIndexedInstanced(draw->count, 1, draw->first, draw->base, draw->index);
            drawCalls++;
            visibleTriangles += draw->count / 3;
        }
    }
    if (options.grid && model) {
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(gridVs.Get(), nullptr, 0);
        context->PSSetShader(gridPs.Get(), nullptr, 0);
        context->RSSetState(raster[0][2].Get());
        context->OMSetDepthStencilState(depthRead.Get(), 0);
        context->OMSetBlendState(blendAlpha.Get(), nullptr, 0xffffffff);
        context->Draw(3, 0);
    }
    context->OMSetRenderTargets(0, nullptr, nullptr);
    if (msaaSamples > 1)
        context->ResolveSubresource(target.Get(), 0, renderColor.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    else
        context->CopyResource(target.Get(), renderColor.Get());
    context->OMSetRenderTargets(1, back.GetAddressOf(), nullptr);
    renderMs = seconds(start) * 1000;
}
void Renderer::endFrame(bool vsync) {
    dx(swap->Present(vsync ? 1 : 0, 0), "Present frame");
}
Json Renderer::verifyGpu(const Args& args) {
    require(bool(model), "GPU validation needs a model");
    model->update(context.Get(), args, options.attachments);
    auto code = shader("VS", "vs_5_0");
    D3D11_SO_DECLARATION_ENTRY declaration{0, "POSITION", 0, 0, 3, 0};
    UINT streamStride = 12;
    Com<ID3D11GeometryShader> outputShader;
    dx(device->CreateGeometryShaderWithStreamOutput(code->GetBufferPointer(), code->GetBufferSize(),
                                                    &declaration, 1, &streamStride, 1,
                                                    D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &outputShader),
       "GPU validation stream-output shader");
    size_t count = 0;
    for (auto& mesh : model->scene->meshes)
        count += mesh.positions.size();
    auto output = buffer(device.Get(), count * 12, D3D11_BIND_STREAM_OUTPUT);
    UINT strides[2]{sizeof(GpuVertex), 4}, offsets[2]{};
    ID3D11Buffer* vbs[]{model->vertices.Get(), model->instances.Get()};
    context->IASetVertexBuffers(0, 2, vbs, strides, offsets);
    context->IASetInputLayout(layout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->GSSetShader(outputShader.Get(), nullptr, 0);
    context->PSSetShader(nullptr, nullptr, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* srvs[]{model->meshView.Get(), model->poseView.Get(), model->numberView.Get()};
    context->VSSetShaderResources(0, 3, srvs);
    UINT offset = 0;
    context->SOSetTargets(1, output.GetAddressOf(), &offset);
    for (auto& draw : model->draws)
        context->DrawInstanced(UINT(model->scene->meshes[draw.index].positions.size()), 1, draw.base,
                               draw.index);
    ID3D11Buffer* nullBuffer = nullptr;
    context->SOSetTargets(1, &nullBuffer, &offset);
    context->GSSetShader(nullptr, nullptr, 0);
    D3D11_BUFFER_DESC desc{};
    output->GetDesc(&desc);
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Com<ID3D11Buffer> staging;
    dx(device->CreateBuffer(&desc, nullptr, &staging), "GPU validation readback");
    context->CopyResource(staging.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    dx(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "GPU validation map");
    auto* actual = static_cast<const F3*>(mapped.pData);
    size_t cursor = 0;
    double maximum = 0;
    bool finite = true;
    for (auto& mesh : model->scene->meshes) {
        auto expected = model->scene->transformed(mesh, model->world);
        for (auto& vertex : expected) {
            for (int k = 0; k < 3; k++) {
                finite &= std::isfinite(actual[cursor][k]);
                maximum = std::max(maximum, std::abs(double(actual[cursor][k]) - vertex[k]));
            }
            cursor++;
        }
    }
    context->Unmap(staging.Get(), 0);
    return {{"vertices_compared", cursor},
            {"maximum_position_error_metres", maximum},
            {"finite", finite},
            {"passed", finite && maximum < .0005}};
}
void Renderer::capture(const fs::path& path, bool entire) {
    Com<ID3D11Texture2D> source;
    if (entire)
        dx(swap->GetBuffer(0, IID_PPV_ARGS(&source)), "Capture window");
    else
        source = target;
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    Com<ID3D11Texture2D> stage;
    dx(device->CreateTexture2D(&desc, nullptr, &stage), "Capture staging");
    context->CopyResource(stage.Get(), source.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    dx(context->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Capture frame pixels");
    DirectX::Image image{desc.Width,
                         desc.Height,
                         desc.Format,
                         mapped.RowPitch,
                         size_t(mapped.RowPitch) * desc.Height,
                         (uint8_t*)mapped.pData};
    DirectX::Blob png;
    HRESULT hr = DirectX::SaveToWICMemory(image, DirectX::WIC_FLAGS_NONE, GUID_ContainerFormatPng, png);
    context->Unmap(stage.Get(), 0);
    dx(hr, "Encode screenshot");
    writeFile(path, {static_cast<const uint8_t*>(png.GetBufferPointer()), png.GetBufferSize()});
}
} // namespace edm
