#include "export_internal.h"
#include <numbers>
#include <tuple>
#include <wincodec.h>

namespace edm {
namespace {
// Original, deliberately small FBX 7.4 binary serializer. No SDK or runtime converter.
// Arrays are uncompressed, which allows readers to map large geometry directly.
constexpr int64_t fbxSecond = 46186158000LL;
constexpr double pi = std::numbers::pi;
int64_t keyTime(double seconds) {
    require(std::isfinite(seconds) && seconds >= 0 && seconds < double(INT64_MAX / fbxSecond),
            "FBX animation time is out of range");
    return int64_t(std::llround(seconds * fbxSecond));
}
struct Property {
    std::vector<uint8_t> bytes;
    template <class T> void raw(T value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* p = reinterpret_cast<const uint8_t*>(&value);
        bytes.insert(bytes.end(), p, p + sizeof(T));
    }
    explicit Property(int32_t v) : bytes{'I'} {
        raw(v);
    }
    explicit Property(int64_t v) : bytes{'L'} {
        raw(v);
    }
    explicit Property(double v) : bytes{'D'} {
        require(std::isfinite(v), "Nonfinite FBX property");
        raw(v);
    }
    explicit Property(bool v) : bytes{'C', uint8_t(v)} {}
    explicit Property(std::string_view v) : bytes{'S'} {
        require(v.size() <= UINT32_MAX, "FBX string exceeds 4 GB");
        raw(uint32_t(v.size()));
        bytes.insert(bytes.end(), v.begin(), v.end());
    }
    explicit Property(const char* v) : Property(std::string_view(v)) {}
    template <class T> static Property array(std::span<const T> values, char type) {
        Property p(int32_t(0));
        p.bytes = {uint8_t(type)};
        require(values.size() <= UINT32_MAX && values.size_bytes() <= UINT32_MAX, "FBX array exceeds 4 GB");
        p.raw(uint32_t(values.size()));
        p.raw(uint32_t(0)); // Array encoding: raw little-endian values.
        p.raw(uint32_t(values.size_bytes()));
        auto* first = reinterpret_cast<const uint8_t*>(values.data());
        if (!values.empty())
            p.bytes.insert(p.bytes.end(), first, first + values.size_bytes());
        return p;
    }
    static Property blob(std::span<const uint8_t> values) {
        Property p(int32_t(0));
        p.bytes = {'R'};
        require(values.size() <= UINT32_MAX, "FBX resource exceeds 4 GB");
        p.raw(uint32_t(values.size()));
        p.bytes.insert(p.bytes.end(), values.begin(), values.end());
        return p;
    }
};
Property integers(const std::vector<int32_t>& v) {
    return Property::array<int32_t>(v, 'i');
}
Property longs(const std::vector<int64_t>& v) {
    return Property::array<int64_t>(v, 'l');
}
Property doubles(const std::vector<double>& v) {
    return Property::array<double>(v, 'd');
}
Property floats(const std::vector<float>& v) {
    return Property::array<float>(v, 'f');
}
Property matrix(const Mat& v) {
    require(v.allFinite(), "Nonfinite FBX matrix");
    return Property::array<double>({v.data(), 16}, 'd');
}
struct Binary {
    std::vector<uint8_t> data;
    const std::atomic_bool* cancel;
    explicit Binary(const std::atomic_bool* flag) : cancel(flag) {
        constexpr uint8_t header[] = {'K', 'a', 'y', 'd', 'a', 'r', 'a', ' ', 'F', 'B',  'X', ' ',
                                      'B', 'i', 'n', 'a', 'r', 'y', ' ', ' ', 0,   0x1a, 0};
        data.insert(data.end(), std::begin(header), std::end(header));
        u32(7400);
    }
    void check() const {
        if (cancel && *cancel)
            throw std::runtime_error("Cancelled");
        require(data.size() < UINT32_MAX - 1024,
                "FBX 7.4 output exceeds 4 GB; export GLB or split the model");
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i)
            data.push_back(uint8_t(v >> (8 * i)));
    }
    void patch(size_t at, size_t v) {
        require(v <= UINT32_MAX, "FBX 7.4 offset exceeds 4 GB");
        for (int i = 0; i < 4; ++i)
            data[at + i] = uint8_t(v >> (8 * i));
    }
    size_t begin(std::string_view name, std::vector<Property> props = {}) {
        check();
        require(name.size() < 256, "FBX node identifier is too long");
        size_t mark = data.size();
        data.resize(mark + 13, 0);
        data[mark + 12] = uint8_t(name.size());
        data.insert(data.end(), name.begin(), name.end());
        size_t start = data.size();
        for (auto& p : props) {
            require(data.size() + p.bytes.size() < UINT32_MAX - 1024,
                    "FBX 7.4 output exceeds 4 GB; export GLB or split the model");
            data.insert(data.end(), p.bytes.begin(), p.bytes.end());
        }
        patch(mark + 4, props.size());
        patch(mark + 8, data.size() - start);
        return mark;
    }
    void end(size_t mark, bool children) {
        if (children)
            data.resize(data.size() + 13, 0);
        patch(mark, data.size());
    }
    void leaf(std::string_view name, std::vector<Property> props = {}) {
        auto mark = begin(name, std::move(props));
        end(mark, false);
    }
    template <class F> void node(std::string_view name, std::vector<Property> props, F body) {
        auto mark = begin(name, std::move(props));
        body();
        end(mark, true);
    }
    void prop(std::string_view name, const char* type, std::vector<Property> values, const char* flags = "A",
              const char* label = "") {
        std::vector<Property> p{Property(name), Property(type), Property(label), Property(flags)};
        for (auto& value : values)
            p.push_back(std::move(value));
        leaf("P", std::move(p));
    }
    void scalar(std::string_view name, double value, const char* type = "Number") {
        prop(name, type, {Property(value)});
    }
    void vector(std::string_view name, const V3& value, const char* type = "Vector3D") {
        prop(name, type, {Property(value.x()), Property(value.y()), Property(value.z())});
    }
    void integer(std::string_view name, int32_t value, const char* type = "int") {
        prop(name, type, {Property(value)}, "", std::string_view(type) == "int" ? "Integer" : "");
    }
    void string(std::string_view name, std::string_view value) {
        prop(name, "KString", {Property(value)}, "U");
    }
    void footer() {
        check();
        data.resize(data.size() + 13, 0);
        constexpr uint8_t marker[] = {0xfa, 0xbc, 0xab, 0x09, 0xd0, 0xc8, 0xd4, 0x66,
                                      0xb1, 0x76, 0xfb, 0x83, 0x1c, 0xf7, 0x26, 0x7e};
        data.insert(data.end(), std::begin(marker), std::end(marker));
        data.resize(data.size() + (16 - data.size() % 16), 0);
        u32(0);
        u32(7400);
        data.resize(data.size() + 120, 0);
        constexpr uint8_t magic[] = {0xf8, 0x5a, 0x8c, 0x6a, 0xde, 0xf5, 0xd9, 0x7e,
                                     0xec, 0xe9, 0x0c, 0xe3, 0x75, 0x8f, 0x29, 0x0b};
        data.insert(data.end(), std::begin(magic), std::end(magic));
    }
};
std::string objectName(std::string name, const char* type) {
    std::replace(name.begin(), name.end(), '\0', '_');
    name.append("\0\1", 2);
    name += type;
    return name;
}
V3 vec3(const Json& item, const char* key, V3 fallback) {
    if (!item.contains(key))
        return fallback;
    const auto& j = item.at(key);
    require(j.size() == 3, "Invalid FBX source vector");
    return V3(j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>());
}
V4 nodeRotation(const Json& node) {
    if (!node.contains("rotation"))
        return V4(0, 0, 0, 1);
    const auto& r = node.at("rotation");
    return qnormalize(V4(r.at(0), r.at(1), r.at(2), r.at(3)));
}
double unwrap(double value, double previous) {
    return value + std::round((previous - value) / (2 * pi)) * (2 * pi);
}
V3 eulerXYZ(const V4& q, const std::optional<V3>& previous = {}) {
    Eigen::Matrix3d m = rotation(q).block<3, 3>(0, 0);
    V3 e;
    e.y() = std::asin(std::clamp(-m(2, 0), -1., 1.));
    if (std::abs(std::cos(e.y())) > 1e-7) {
        e.x() = std::atan2(m(2, 1), m(2, 2));
        e.z() = std::atan2(m(1, 0), m(0, 0));
    } else {
        e.z() = previous ? previous->z() : 0.;
        e.x() = e.y() > 0 ? std::atan2(m(0, 1), m(0, 2)) + e.z() : std::atan2(-m(0, 1), -m(0, 2)) - e.z();
    }
    if (previous) {
        V3 alt(e.x() + pi, pi - e.y(), e.z() + pi);
        for (int k = 0; k < 3; ++k) {
            e[k] = unwrap(e[k], (*previous)[k]);
            alt[k] = unwrap(alt[k], (*previous)[k]);
        }
        if ((alt - *previous).squaredNorm() < (e - *previous).squaredNorm())
            e = alt;
    }
    return e;
}
V4 eulerQuaternion(const V3& v) {
    Eigen::Quaterniond q = Eigen::AngleAxisd(v.z(), V3::UnitZ()) * Eigen::AngleAxisd(v.y(), V3::UnitY()) *
                           Eigen::AngleAxisd(v.x(), V3::UnitX());
    return q.coeffs();
}
struct Accessor {
    std::span<const uint8_t> bytes;
    size_t count = 0, components = 0, stride = 0, componentBytes = 0;
    int type = 0;
    double value(size_t index, size_t component = 0) const {
        require(index < count && component < components, "FBX source accessor index out of bounds");
        const uint8_t* p = bytes.data() + index * stride + component * componentBytes;
        if (type == 5126) {
            float v;
            std::memcpy(&v, p, 4);
            require(std::isfinite(v), "Nonfinite FBX source value");
            return v;
        }
        if (type == 5125) {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }
        if (type == 5123) {
            uint16_t v;
            std::memcpy(&v, p, 2);
            return v;
        }
        return *p;
    }
    V4 vector(size_t i) const {
        V4 v = V4::Zero();
        for (size_t k = 0; k < std::min(size_t(4), components); ++k)
            v[k] = value(i, k);
        return v;
    }
};
struct Source {
    const ExportPayload& payload;
    std::span<const uint8_t> view(size_t index) const {
        const auto& v = payload.document.at("bufferViews").at(index);
        size_t start = v.value("byteOffset", size_t(0)), length = v.at("byteLength");
        require(start <= payload.buffer.size() && length <= payload.buffer.size() - start,
                "FBX source buffer view out of bounds");
        return {payload.buffer.data() + start, length};
    }
    Accessor accessor(size_t index) const {
        const auto& a = payload.document.at("accessors").at(index);
        Accessor out;
        out.type = a.at("componentType");
        require(out.type == 5126 || out.type == 5125 || out.type == 5123 || out.type == 5121,
                "Unsupported FBX source accessor component type");
        out.componentBytes = out.type == 5123 ? 2 : (out.type == 5121 ? 1 : 4);
        std::string shape = a.at("type");
        out.components = shape == "SCALAR" ? 1
                         : shape == "VEC2" ? 2
                         : shape == "VEC3" ? 3
                         : shape == "VEC4" ? 4
                         : shape == "MAT4" ? 16
                                           : 0;
        require(out.components > 0, "Unsupported FBX source accessor shape");
        size_t viewIndex = a.at("bufferView");
        out.bytes = view(viewIndex);
        size_t start = a.value("byteOffset", size_t(0));
        require(start <= out.bytes.size(), "FBX source accessor offset out of bounds");
        out.bytes = out.bytes.subspan(start);
        out.stride = payload.document.at("bufferViews")
                         .at(viewIndex)
                         .value("byteStride", out.components * out.componentBytes);
        out.count = a.at("count");
        require(out.stride >= out.components * out.componentBytes &&
                    (!out.count || (out.count - 1 <= out.bytes.size() / out.stride &&
                                    (out.count - 1) * out.stride + out.components * out.componentBytes <=
                                        out.bytes.size())),
                "FBX source accessor data out of bounds");
        return out;
    }
};
struct Connection {
    int64_t from, to;
    std::string property;
};
struct Encoder {
    const ExportPayload& payload;
    Source source;
    Binary w;
    int64_t nextId = 100000;
    std::vector<int64_t> models, materials, videos;
    std::vector<Mat> bindReferenceWorld;
    std::vector<int> nodeParents;
    std::vector<Connection> connections;
    std::map<std::string, int32_t> objectCounts;
    std::map<std::tuple<int, int, bool>, int64_t> textures;
    std::map<size_t, int64_t> opaqueVideos;
    std::set<size_t> boneNodes;
    size_t meshCount = 0, triangleCount = 0, curveCount = 0, skinCount = 0;
    size_t rotationKeyCount = 0;
    explicit Encoder(const ExportPayload& p, const std::atomic_bool* c) : payload(p), source{p}, w(c) {}
    const Json& doc() const {
        return payload.document;
    }
    int64_t id() {
        return nextId++;
    }
    void connect(int64_t from, int64_t to, std::string property = {}) {
        connections.push_back({from, to, std::move(property)});
    }
    template <class F>
    void object(const char* type, int64_t uid, const std::string& name, const char* subtype, F body) {
        ++objectCounts[type];
        std::string_view objectType(type);
        const char* nameClass = objectType == "AnimationStack"       ? "AnimStack"
                                : objectType == "AnimationLayer"     ? "AnimLayer"
                                : objectType == "AnimationCurveNode" ? "AnimCurveNode"
                                : objectType == "AnimationCurve"     ? "AnimCurve"
                                                                     : type;
        w.node(type, {Property(uid), Property(objectName(name, nameClass)), Property(subtype)}, body);
    }
    void metadata() {
        w.node("FBXHeaderExtension", {}, [&] {
            w.leaf("FBXHeaderVersion", {Property(int32_t(1003))});
            w.leaf("FBXVersion", {Property(int32_t(7400))});
            w.leaf("EncryptionType", {Property(int32_t(0))});
            w.node("CreationTimeStamp", {}, [&] {
                SYSTEMTIME time{};
                GetSystemTime(&time);
                const std::pair<const char*, int32_t> fields[] = {
                    {"Version", 1000},        {"Year", time.wYear},
                    {"Month", time.wMonth},   {"Day", time.wDay},
                    {"Hour", time.wHour},     {"Minute", time.wMinute},
                    {"Second", time.wSecond}, {"Millisecond", time.wMilliseconds}};
                for (auto [key, value] : fields)
                    w.leaf(key, {Property(value)});
            });
            w.leaf("Creator", {Property("EDM Studio C++ " EDM_NATIVE_VERSION)});
        });
        w.node("GlobalSettings", {}, [&] {
            w.leaf("Version", {Property(int32_t(1000))});
            w.node("Properties70", {}, [&] {
                w.integer("UpAxis", 1);
                w.integer("UpAxisSign", 1);
                w.integer("FrontAxis", 2);
                w.integer("FrontAxisSign", 1);
                w.integer("CoordAxis", 0);
                w.integer("CoordAxisSign", 1);
                w.integer("OriginalUpAxis", 1);
                w.integer("OriginalUpAxisSign", 1);
                w.scalar("UnitScaleFactor", 100.);
                w.scalar("OriginalUnitScaleFactor", 100.);
                w.integer("TimeMode", 11, "enum"); // 24 frames/s; curve times remain exact seconds.
                w.prop("TimeSpanStart", "KTime", {Property(int64_t(0))});
                double duration = payload.report.value("clip_duration_seconds", 3.);
                w.prop("TimeSpanStop", "KTime", {Property(keyTime(duration))});
            });
        });
        w.node("Documents", {}, [&] {
            w.leaf("Count", {Property(int32_t(1))});
            w.node("Document", {Property(id()), Property("EDM Studio"), Property("Scene")}, [&] {
                w.node("Properties70", {}, [&] { w.string("ActiveAnimStackName", ""); });
                w.leaf("RootNode", {Property(int64_t(0))});
            });
        });
        w.leaf("References");
    }
    std::string imageName(size_t i, bool opaque = false) const {
        return "edm_image_" + std::to_string(i) + (opaque ? "_opaque_rgb.png" : ".png");
    }
    DirectX::Blob opaquePng(size_t imageIndex) {
        w.check();
        const auto& image = doc().at("images").at(imageIndex);
        const auto bytes = source.view(image.at("bufferView"));
        ComApartment apartment;
        DirectX::TexMetadata metadata;
        require(SUCCEEDED(DirectX::GetMetadataFromWICMemory(bytes.data(), bytes.size(),
                                                            DirectX::WIC_FLAGS_NONE, metadata)) &&
                    metadata.width > 0 && metadata.height > 0 && metadata.width <= 32768 &&
                    metadata.height <= 32768,
                "FBX: invalid opaque PNG dimensions");
        DirectX::ScratchImage decoded, converted;
        require(SUCCEEDED(DirectX::LoadFromWICMemory(bytes.data(), bytes.size(), DirectX::WIC_FLAGS_FORCE_RGB,
                                                     nullptr, decoded)),
                "FBX: cannot decode opaque PNG pixels");
        w.check();
        const auto* pixels = decoded.GetImage(0, 0, 0);
        require(pixels != nullptr, "FBX: missing opaque PNG pixels");
        if (pixels->format != DXGI_FORMAT_R8G8B8A8_UNORM &&
            pixels->format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
            const auto format = DirectX::IsSRGB(pixels->format) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                                : DXGI_FORMAT_R8G8B8A8_UNORM;
            require(SUCCEEDED(DirectX::Convert(*pixels, format, DirectX::TEX_FILTER_DEFAULT, 0, converted)),
                    "FBX: cannot convert opaque PNG pixels");
            pixels = converted.GetImage(0, 0, 0);
        }
        // The payload is straight RGBA. Keep every RGB byte, including RGB beneath
        // zero alpha. Making the temporary copy opaque prevents a codec from
        // compositing those colors when dropping alpha; the original stays intact.
        for (size_t y = 0; y < pixels->height; ++y) {
            if (!(y % 64))
                w.check();
            for (size_t x = 0; x < pixels->width; ++x)
                pixels->pixels[y * pixels->rowPitch + x * 4 + 3] = 255;
        }
        DirectX::Blob png;
        require(SUCCEEDED(DirectX::SaveToWICMemory(*pixels, DirectX::WIC_FLAGS_NONE, GUID_ContainerFormatPng,
                                                   png, &GUID_WICPixelFormat24bppBGR)),
                "FBX: cannot encode RGB-only opaque PNG");
        w.check();
        return png;
    }
    int64_t opaqueVideo(size_t imageIndex) {
        if (opaqueVideos.contains(imageIndex))
            return opaqueVideos.at(imageIndex);
        auto png = opaquePng(imageIndex);
        const auto& image = doc().at("images").at(imageIndex);
        const auto name = imageName(imageIndex, true), original = imageName(imageIndex);
        const int64_t uid = id();
        object("Video", uid, name, "Clip", [&] {
            w.leaf("Type", {Property("Clip")});
            w.node("Properties70", {}, [&] {
                w.string("Path", name);
                w.string("EDM_SourceImage", image.value("name", original));
                w.string("EDM_OriginalImage", original);
                w.string("EDM_AlphaPolicy", "OPAQUE RGB derivative; original PNG alpha preserved separately");
            });
            w.leaf("UseMipMap", {Property(int32_t(0))});
            w.leaf("Filename", {Property(name)});
            w.leaf("RelativeFilename", {Property(name)});
            const auto* data = static_cast<const uint8_t*>(png.GetBufferPointer());
            w.leaf("Content", {Property::blob({data, png.GetBufferSize()})});
        });
        opaqueVideos.emplace(imageIndex, uid);
        return uid;
    }
    int64_t texture(int textureIndex, int uvSet, bool opaque = false) {
        auto key = std::make_tuple(textureIndex, uvSet, opaque);
        if (textures.contains(key))
            return textures.at(key);
        const auto& image = doc().at("textures").at(textureIndex);
        size_t imageIndex = image.at("source");
        int64_t uid = id();
        textures[key] = uid;
        const int64_t video = opaque ? opaqueVideo(imageIndex) : videos.at(imageIndex);
        auto name = imageName(imageIndex, opaque);
        object("Texture", uid, name + " / UV" + std::to_string(uvSet), "", [&] {
            w.leaf("Type", {Property("TextureVideoClip")});
            w.leaf("Version", {Property(int32_t(202))});
            w.leaf("TextureName", {Property(objectName(name, "Texture"))});
            w.leaf("Media", {Property(objectName(name, "Video"))});
            w.leaf("FileName", {Property(name)});
            w.leaf("RelativeFilename", {Property(name)});
            w.leaf("ModelUVTranslation", {Property(0.), Property(0.)});
            w.leaf("ModelUVScaling", {Property(1.), Property(1.)});
            w.leaf("Texture_Alpha_Source", {Property("Black")});
            w.node("Properties70", {}, [&] {
                w.string("UVSet", "UV" + std::to_string(uvSet));
                w.vector("Translation", V3::Zero());
                w.vector("Rotation", V3::Zero());
                w.vector("Scaling", V3::Ones());
                w.integer("CurrentTextureBlendMode", 0, "enum");
                w.integer("UseMaterial", 1, "bool");
                if (opaque)
                    w.string("EDM_OriginalImage", imageName(imageIndex));
            });
        });
        connect(video, uid);
        return uid;
    }
    void materialResources() {
        if (doc().contains("images"))
            for (size_t i = 0; i < doc().at("images").size(); ++i) {
                const auto& image = doc().at("images").at(i);
                int64_t uid = id();
                videos.push_back(uid);
                auto name = imageName(i);
                object("Video", uid, name, "Clip", [&] {
                    w.leaf("Type", {Property("Clip")});
                    w.node("Properties70", {}, [&] {
                        w.string("Path", name);
                        w.string("EDM_SourceImage", image.value("name", name));
                    });
                    w.leaf("UseMipMap", {Property(int32_t(0))});
                    w.leaf("Filename", {Property(name)});
                    w.leaf("RelativeFilename", {Property(name)});
                    w.leaf("Content", {Property::blob(source.view(image.at("bufferView")))});
                });
            }
        for (const auto& mat : doc().at("materials")) {
            int64_t uid = id();
            materials.push_back(uid);
            const auto& pbr = mat.at("pbrMetallicRoughness");
            auto color = pbr.value("baseColorFactor", Json{1., 1., 1., 1.});
            const bool opaque = mat.value("alphaMode", "OPAQUE") == "OPAQUE";
            double alpha = opaque ? 1. : color.at(3).get<double>(),
                   roughness = pbr.value("roughnessFactor", 1.);
            object("Material", uid, mat.value("name", "Material"), "", [&] {
                w.leaf("Version", {Property(int32_t(102))});
                w.leaf("ShadingModel", {Property("phong")});
                w.leaf("MultiLayer", {Property(int32_t(0))});
                w.node("Properties70", {}, [&] {
                    w.vector("DiffuseColor", V3(color.at(0), color.at(1), color.at(2)), "Color");
                    w.scalar("DiffuseFactor", 1.);
                    w.scalar("Opacity", alpha);
                    w.scalar("TransparencyFactor", 1. - alpha);
                    w.vector("TransparentColor", V3::Constant(1. - alpha), "Color");
                    w.scalar("Shininess", std::pow(1. - std::clamp(roughness, 0., 1.), 2.) * 100.);
                    w.scalar("SpecularFactor", .25);
                    w.scalar("ReflectionFactor", pbr.value("metallicFactor", 0.));
                    w.string("EDM_AlphaMode", mat.value("alphaMode", "OPAQUE"));
                    w.scalar("EDM_AlphaCutoff", mat.value("alphaCutoff", .5));
                    w.scalar("EDM_RoughnessFactor", roughness);
                    w.scalar("EDM_MetallicFactor", pbr.value("metallicFactor", 0.));
                    if (opaque && pbr.contains("baseColorTexture")) {
                        const auto& base = pbr.at("baseColorTexture");
                        const size_t image =
                            doc().at("textures").at(base.at("index").get<size_t>()).at("source");
                        w.string("EDM_OriginalDiffuseImage", imageName(image));
                        w.string("EDM_OpaqueDiffuseImage", imageName(image, true));
                    }
                    if (pbr.contains("metallicRoughnessTexture")) {
                        auto& orm = pbr.at("metallicRoughnessTexture");
                        size_t image = doc().at("textures").at(orm.at("index").get<size_t>()).at("source");
                        w.string("EDM_ORMImage", imageName(image));
                        w.string("EDM_ORMChannels", "R=occlusion, G=roughness, B=metallic; connect manually");
                    }
                });
            });
            if (pbr.contains("baseColorTexture")) {
                auto& base = pbr.at("baseColorTexture");
                int64_t tex = texture(base.at("index"), base.value("texCoord", 0), opaque);
                connect(tex, uid, "DiffuseColor");
                if (!opaque)
                    connect(tex, uid, "TransparentColor");
            }
            if (pbr.contains("metallicRoughnessTexture")) {
                auto& orm = pbr.at("metallicRoughnessTexture");
                connect(texture(orm.at("index"), orm.value("texCoord", 0)), uid, "EDM_ORMImage");
            }
        }
    }
    void model(int64_t uid, const Json& node, const char* subtype) {
        object("Model", uid, node.value("name", "Node"), subtype, [&] {
            w.leaf("Version", {Property(int32_t(232))});
            w.node("Properties70", {}, [&] {
                w.integer("RotationActive", 1, "bool");
                w.integer("RotationOrder", 0, "enum");
                w.integer("InheritType", 1, "enum"); // RSrs: ordinary parent * local composition.
                w.vector("Lcl Translation", vec3(node, "translation", V3::Zero()), "Lcl Translation");
                w.vector("Lcl Rotation", eulerXYZ(nodeRotation(node)) * (180. / pi), "Lcl Rotation");
                w.vector("Lcl Scaling", vec3(node, "scale", V3::Ones()), "Lcl Scaling");
                w.integer("DefaultAttributeIndex", 0);
            });
            w.leaf("Shading", {Property(true)});
            w.leaf("Culling", {Property("CullingOff")});
        });
    }
    void hierarchy() {
        const auto& nodes = doc().at("nodes");
        models.resize(nodes.size());
        for (auto& uid : models)
            uid = id();
        if (doc().contains("skins"))
            for (auto& skin : doc().at("skins"))
                for (auto& joint : skin.at("joints"))
                    boneNodes.insert(joint.get<size_t>());
        std::vector<int> parents(nodes.size(), -1);
        for (size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].contains("children"))
                for (auto& child : nodes[i].at("children")) {
                    size_t ci = child;
                    require(ci < nodes.size() && parents[ci] < 0, "Invalid FBX source hierarchy");
                    parents[ci] = int(i);
                }
        bindReferenceWorld.resize(nodes.size());
        std::vector<uint8_t> visited(nodes.size(), 0);
        std::function<void(size_t)> world = [&](size_t i) {
            if (visited[i] == 2)
                return;
            require(visited[i] == 0, "Cyclic FBX source hierarchy");
            visited[i] = 1;
            V3 bindScale = vec3(nodes[i], "scale", V3::Ones());
            for (int axis = 0; axis < 3; ++axis)
                if (std::abs(bindScale[axis]) < 1e-10)
                    bindScale[axis] = 1.;
            Mat bindLocal = translation(vec3(nodes[i], "translation", V3::Zero())) *
                            rotation(nodeRotation(nodes[i])) * scaling(bindScale);
            if (parents[i] >= 0) {
                world(size_t(parents[i]));
                bindReferenceWorld[i] = bindReferenceWorld[parents[i]] * bindLocal;
            } else {
                bindReferenceWorld[i] = bindLocal;
            }
            visited[i] = 2;
        };
        for (size_t i = 0; i < nodes.size(); ++i)
            world(i);
        nodeParents = parents;
        // Skeleton intermediary transforms are bones too. Keeping them as Null objects makes some
        // DCC importers create a separate armature per weighted joint and apply deformers in series.
        const auto weightedNodes = boneNodes;
        for (size_t joint : weightedNodes)
            for (int parent = parents.at(joint); parent >= 0 && parents[parent] >= 0;
                 parent = parents[parent])
                boneNodes.insert(size_t(parent));
        for (size_t i = 0; i < nodes.size(); ++i) {
            model(models[i], nodes[i], boneNodes.contains(i) ? "LimbNode" : "Null");
            connect(models[i], parents[i] < 0 ? 0 : models[parents[i]]);
            if (boneNodes.contains(i)) {
                int64_t attr = id();
                object("NodeAttribute", attr, nodes[i].value("name", "Bone"), "LimbNode", [&] {
                    w.leaf("TypeFlags", {Property("Skeleton")});
                    w.node("Properties70", {}, [&] { w.scalar("Size", .1); });
                });
                connect(attr, models[i]);
            }
        }
    }
    void skin(int64_t geometry, int64_t meshModel, const Json& node, const Json& attrs, size_t vertices,
              const Mat& meshWorld) {
        if (!node.contains("skin"))
            return;
        auto& skin = doc().at("skins").at(node.at("skin").get<size_t>());
        auto ibms = source.accessor(skin.at("inverseBindMatrices"));
        auto& joints = skin.at("joints");
        require(ibms.count == joints.size(), "FBX inverse bind count mismatch");
        std::vector<std::vector<int32_t>> indices(joints.size());
        std::vector<std::vector<double>> weights(joints.size());
        for (int set = 0; attrs.contains("JOINTS_" + std::to_string(set)); ++set) {
            auto ji = source.accessor(attrs.at("JOINTS_" + std::to_string(set)));
            auto we = source.accessor(attrs.at("WEIGHTS_" + std::to_string(set)));
            require(ji.count == vertices && we.count == vertices, "FBX skin vertex count mismatch");
            for (size_t vi = 0; vi < vertices; ++vi) {
                if (!(vi % 16384))
                    w.check();
                for (size_t k = 0; k < 4; ++k) {
                    double weight = we.value(vi, k);
                    size_t joint = size_t(ji.value(vi, k));
                    require(joint < joints.size() && weight >= 0, "Invalid FBX source skin weight");
                    if (weight > 0) {
                        indices[joint].push_back(int32_t(vi));
                        weights[joint].push_back(weight);
                    }
                }
            }
        }
        int64_t skinId = id();
        ++skinCount;
        object("Deformer", skinId, "Skin", "Skin", [&] {
            w.leaf("Version", {Property(int32_t(101))});
            w.leaf("Link_DeformAcuracy", {Property(50.)});
            w.leaf("SkinningType", {Property("Linear")});
        });
        connect(skinId, geometry);
        // Any common invertible mesh bind reference gives the same skin equation. Using the mesh's
        // placement keeps bind bones rigid when the mesh has nonuniform scaling; an identity
        // reference would put that inverse scaling/shear into every bone rest pose in DCC tools.
        Mat meshBind = meshWorld;
        require(meshBind.allFinite() && std::abs(meshBind.determinant()) > 1e-16,
                "FBX mesh bind reference is singular");
        std::map<size_t, Mat> poseNodes;
        for (const auto& joint : joints)
            for (int ancestor = joint.get<int>(); ancestor >= 0; ancestor = nodeParents.at(ancestor))
                // Zero-scale visibility controls are valid posed bones, but never valid rest bones:
                // DCC editors may delete a collapsed rest bone and then fail resolving its animation.
                // This metadata reference is nondegenerate; actual local/animated zero scales remain.
                poseNodes[size_t(ancestor)] = bindReferenceWorld.at(ancestor);
        for (size_t i = 0; i < joints.size(); ++i) {
            Mat ibm;
            for (int k = 0; k < 16; ++k)
                ibm.data()[k] = ibms.value(i, k);
            require(std::abs(ibm.determinant()) > 1e-16, "Singular FBX inverse bind matrix");
            Mat bind = meshBind * ibm.inverse();
            poseNodes[joints.at(i).get<size_t>()] = bind;
            int64_t cluster = id();
            object("Deformer", cluster, "Cluster " + std::to_string(i), "Cluster", [&] {
                w.leaf("Version", {Property(int32_t(100))});
                w.leaf("UserData", {Property(""), Property("")});
                w.leaf("Indexes", {integers(indices[i])});
                w.leaf("Weights", {doubles(weights[i])});
                // glTF skin positions are in common bind space, independent of mesh node transforms.
                // FBX serializes Transform in bone space; TransformLink is the bone bind world matrix.
                w.leaf("Transform", {matrix(ibm)});
                w.leaf("TransformLink", {matrix(bind)});
            });
            connect(cluster, skinId);
            connect(models.at(joints.at(i).get<size_t>()), cluster);
        }
        object("Pose", id(), "BindPose", "BindPose", [&] {
            w.leaf("Type", {Property("BindPose")});
            w.leaf("Version", {Property(int32_t(100))});
            w.leaf("NbPoseNodes", {Property(int32_t(poseNodes.size() + 1))});
            auto pose = [&](int64_t uid, const Mat& transform) {
                w.node("PoseNode", {}, [&] {
                    w.leaf("Node", {Property(uid)});
                    w.leaf("Matrix", {matrix(transform)});
                });
            };
            pose(meshModel, meshBind);
            for (const auto& [nodeIndex, transform] : poseNodes)
                pose(models.at(nodeIndex), transform);
        });
    }
    void geometry(size_t ni, const Json& primitive, size_t part) {
        w.check();
        auto& node = doc().at("nodes").at(ni);
        auto& attrs = primitive.at("attributes");
        require(primitive.value("mode", 4) == 4, "FBX export requires triangle primitives");
        auto positions = source.accessor(attrs.at("POSITION"));
        auto indices = source.accessor(primitive.at("indices"));
        require(positions.count <= INT32_MAX && indices.count % 3 == 0, "Invalid FBX triangle geometry");
        std::vector<double> points;
        points.reserve(positions.count * 3);
        for (size_t i = 0; i < positions.count; ++i)
            for (size_t k = 0; k < 3; ++k)
                points.push_back(positions.value(i, k));
        std::vector<int32_t> polygons;
        polygons.reserve(indices.count);
        for (size_t i = 0; i < indices.count; ++i) {
            size_t v = size_t(indices.value(i));
            require(v < positions.count, "FBX triangle index out of bounds");
            polygons.push_back(i % 3 == 2 ? -int32_t(v) - 1 : int32_t(v));
        }
        std::string name = node.value("name", "Mesh") + " / geometry " + std::to_string(part);
        int64_t modelId = id(), geometryId = id();
        model(modelId, Json{{"name", name}}, "Mesh");
        connect(modelId, models.at(ni));
        connect(materials.at(primitive.at("material").get<size_t>()), modelId);
        object("Geometry", geometryId, name, "Mesh", [&] {
            w.leaf("GeometryVersion", {Property(int32_t(124))});
            w.leaf("Vertices", {doubles(points)});
            w.leaf("PolygonVertexIndex", {integers(polygons)});
            if (attrs.contains("NORMAL")) {
                auto normals = source.accessor(attrs.at("NORMAL"));
                require(normals.count == positions.count, "FBX normal count mismatch");
                std::vector<double> values;
                values.reserve(normals.count * 3);
                for (size_t i = 0; i < normals.count; ++i)
                    for (size_t k = 0; k < 3; ++k)
                        values.push_back(normals.value(i, k));
                w.node("LayerElementNormal", {Property(int32_t(0))}, [&] {
                    w.leaf("Version", {Property(int32_t(101))});
                    w.leaf("Name", {Property("")});
                    w.leaf("MappingInformationType", {Property("ByVertice")});
                    w.leaf("ReferenceInformationType", {Property("Direct")});
                    w.leaf("Normals", {doubles(values)});
                });
            }
            std::vector<int> uvSets;
            for (const auto& [key, value] : attrs.items()) {
                if (key.rfind("TEXCOORD_", 0) != 0)
                    continue;
                uvSets.push_back(std::stoi(key.substr(9)));
            }
            const auto& pbr =
                doc().at("materials").at(primitive.at("material").get<size_t>()).at("pbrMetallicRoughness");
            int activeUv =
                pbr.contains("baseColorTexture") ? pbr.at("baseColorTexture").value("texCoord", 0) : 0;
            auto active = std::find(uvSets.begin(), uvSets.end(), activeUv);
            if (active != uvSets.end())
                std::rotate(uvSets.begin(), active, active + 1);
            // Keep all original UV names, but place diffuse UVs on the first physical layer as well.
            // This also serves importers which ignore Texture.UVSet and use their active UV layer.
            for (size_t uvIndex = 0; uvIndex < uvSets.size(); ++uvIndex) {
                int set = uvSets[uvIndex];
                auto uv = source.accessor(attrs.at("TEXCOORD_" + std::to_string(set)));
                require(uv.count == positions.count, "FBX UV count mismatch");
                std::vector<double> values;
                values.reserve(uv.count * 2);
                for (size_t i = 0; i < uv.count; ++i) {
                    values.push_back(uv.value(i, 0));
                    values.push_back(1. -
                                     uv.value(i, 1)); // glTF top-left image convention → FBX bottom-left.
                }
                w.node("LayerElementUV", {Property(int32_t(uvIndex))}, [&] {
                    w.leaf("Version", {Property(int32_t(101))});
                    w.leaf("Name", {Property("UV" + std::to_string(set))});
                    w.leaf("MappingInformationType", {Property("ByVertice")});
                    w.leaf("ReferenceInformationType", {Property("Direct")});
                    w.leaf("UV", {doubles(values)});
                });
            }
            w.node("LayerElementMaterial", {Property(int32_t(0))}, [&] {
                w.leaf("Version", {Property(int32_t(101))});
                w.leaf("Name", {Property("")});
                w.leaf("MappingInformationType", {Property("AllSame")});
                w.leaf("ReferenceInformationType", {Property("IndexToDirect")});
                w.leaf("Materials", {integers({0})});
            });
            if (uvSets.empty())
                uvSets.push_back(-1);
            for (size_t layerIndex = 0; layerIndex < uvSets.size(); ++layerIndex)
                w.node("Layer", {Property(int32_t(layerIndex))}, [&] {
                    int set = uvSets[layerIndex];
                    w.leaf("Version", {Property(int32_t(100))});
                    auto layer = [&](const char* type, int index) {
                        w.node("LayerElement", {}, [&] {
                            w.leaf("Type", {Property(type)});
                            w.leaf("TypedIndex", {Property(int32_t(index))});
                        });
                    };
                    if (layerIndex == 0) {
                        if (attrs.contains("NORMAL"))
                            layer("LayerElementNormal", 0);
                        layer("LayerElementMaterial", 0);
                    }
                    if (attrs.contains("TEXCOORD_" + std::to_string(set)))
                        layer("LayerElementUV", int(layerIndex));
                });
        });
        connect(geometryId, modelId);
        skin(geometryId, modelId, node, attrs, positions.count, bindReferenceWorld.at(ni));
        ++meshCount;
        triangleCount += indices.count / 3;
    }
    void meshes() {
        const auto& nodes = doc().at("nodes");
        for (size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].contains("mesh")) {
                const auto& parts = doc().at("meshes").at(nodes[i].at("mesh").get<size_t>()).at("primitives");
                for (size_t j = 0; j < parts.size(); ++j)
                    geometry(i, parts[j], j);
            }
    }
    void curve(int64_t curveNode, int component, const std::vector<double>& times,
               const std::vector<V3>& values, bool step) {
        std::vector<int64_t> keyTimes;
        std::vector<float> keyValues;
        for (size_t i = 0; i < times.size(); ++i) {
            auto time = keyTime(times[i]);
            if (step && !keyTimes.empty() && float(values[i][component]) != keyValues.back()) {
                // Keep real CONSTANT flags, plus an explicit hold immediately before each jump.
                // Importers that discard FBX interpolation (eg. Blender 4.2) then have only a tiny
                // transition instead of interpolating across the whole visibility interval.
                float frame = float(times[i] * 24. + 1.);
                double frameUlp = double(frame) - double(std::nextafter(frame, -INFINITY));
                double gap = std::max(.011 / 24., frameUlp * 2. / 24.);
                gap = std::min(gap, (times[i] - times[i - 1]) * .25);
                auto hold = keyTime(times[i] - gap);
                if (hold > keyTimes.back() && hold < time) {
                    keyTimes.push_back(hold);
                    keyValues.push_back(keyValues.back());
                }
            }
            if (!keyTimes.empty() && time == keyTimes.back()) {
                keyValues.back() = float(values[i][component]);
            } else {
                keyTimes.push_back(time);
                keyValues.push_back(float(values[i][component]));
            }
        }
        require(!keyTimes.empty(), "FBX animation curve is empty");
        bool constant =
            std::all_of(keyValues.begin(), keyValues.end(), [&](float v) { return v == keyValues[0]; });
        if (constant) {
            keyTimes.resize(1);
            keyValues.resize(1);
        }
        int64_t uid = id();
        object("AnimationCurve", uid, "Curve", "", [&] {
            w.leaf("Default", {Property(double(keyValues.front()))});
            w.leaf("KeyVer", {Property(int32_t(4008))});
            w.leaf("KeyTime", {longs(keyTimes)});
            w.leaf("KeyValueFloat", {floats(keyValues)});
            // FBX eInterpolationConstant (standard: previous value) or eInterpolationLinear.
            w.leaf("KeyAttrFlags", {integers({step ? 2 : 4})});
            w.leaf("KeyAttrDataFloat", {floats({0, 0, 0, 0})});
            w.leaf("KeyAttrRefCount", {integers({int32_t(keyTimes.size())})});
        });
        connect(uid, curveNode, component == 0 ? "d|X" : component == 1 ? "d|Y" : "d|Z");
        ++curveCount;
    }
    void channel(int64_t layer, const Json& ch, const Json& sampler) {
        auto input = source.accessor(sampler.at("input")), output = source.accessor(sampler.at("output"));
        require(input.count == output.count && input.count > 0, "Invalid FBX animation sampler");
        std::string path = ch.at("target").at("path"),
                    interpolation = sampler.value("interpolation", "LINEAR");
        require(interpolation == "LINEAR" || interpolation == "STEP", "FBX source interpolation unsupported");
        bool rotate = path == "rotation", step = interpolation == "STEP";
        require(rotate || path == "translation" || path == "scale", "Unsupported FBX animation channel");
        const char* property = rotate                  ? "Lcl Rotation"
                               : path == "translation" ? "Lcl Translation"
                                                       : "Lcl Scaling";
        std::vector<double> times;
        std::vector<V3> values;
        if (rotate) {
            times.push_back(input.value(0));
            values.push_back(eulerXYZ(output.vector(0)));
            for (size_t i = 1; i < input.count; ++i) {
                w.check();
                double begin = input.value(i - 1), end = input.value(i);
                require(end > begin && begin >= 0, "FBX animation times must increase");
                V4 qa = qnormalize(output.vector(i - 1)), qb = qnormalize(output.vector(i));
                if (step) {
                    times.push_back(end);
                    values.push_back(eulerXYZ(qb, values.back()));
                    continue;
                }
                bool stationary = std::min((qa - qb).norm(), (qa + qb).norm()) < 1e-12;
                double angle = stationary ? 0. : 2. * std::acos(std::clamp(std::abs(qa.dot(qb)), 0., 1.));
                // Constant rotations do not need baking. Moving segments use at least 60 Hz and 5° steps.
                int parts = angle < 1e-10 ? 1
                                          : std::max({1, int(std::ceil((end - begin) * 60.)),
                                                      int(std::ceil(angle / (pi / 36.)))});
                require(parts <= 100000, "FBX animation interval is too long");
                auto sample = [&](double time) { return slerp(qa, qb, (time - begin) / (end - begin)); };
                std::function<void(double, V3, double, V4, int)> refine;
                refine = [&](double a, V3 ea, double z, V4 qz, int depth) {
                    if (!(values.size() % 1024))
                        w.check();
                    V3 ez = eulerXYZ(qz, ea);
                    double mid = (a + z) * .5;
                    V4 qm = sample(mid);
                    double error = 0.;
                    for (double fraction : {.25, .5, .75}) {
                        V4 actual = sample(a + (z - a) * fraction);
                        V4 estimated = eulerQuaternion(ea + (ez - ea) * fraction);
                        error = std::max(error,
                                         2. * std::acos(std::clamp(std::abs(actual.dot(estimated)), 0., 1.)));
                    }
                    if (error > 2e-6 && depth < 14) {
                        refine(a, ea, mid, qm, depth + 1);
                        refine(mid, values.back(), z, qz, depth + 1);
                    } else {
                        require(error <= 2e-6,
                                "FBX rotation cannot be baked accurately near an Euler singularity");
                        times.push_back(z);
                        values.push_back(ez);
                    }
                };
                for (int k = 1; k <= parts; ++k) {
                    double z = begin + (end - begin) * (double(k) / parts);
                    refine(times.back(), values.back(), z, sample(z), 0);
                }
            }
            rotationKeyCount += times.size();
            for (auto& value : values)
                value *= 180. / pi;
        } else {
            for (size_t i = 0; i < input.count; ++i) {
                double time = input.value(i);
                require(time >= 0 && (times.empty() || time > times.back()),
                        "FBX animation times must increase");
                times.push_back(time);
                values.push_back(output.vector(i).head<3>());
            }
        }
        int64_t uid = id();
        object("AnimationCurveNode", uid,
               rotate                  ? "R"
               : path == "translation" ? "T"
                                       : "S",
               "", [&] {
                   w.node("Properties70", {}, [&] {
                       w.scalar("d|X", values.front().x());
                       w.scalar("d|Y", values.front().y());
                       w.scalar("d|Z", values.front().z());
                   });
               });
        connect(uid, layer);
        connect(uid, models.at(ch.at("target").at("node").get<size_t>()), property);
        for (int component = 0; component < 3; ++component)
            curve(uid, component, times, values, step);
    }
    void animations(Progress progress) {
        if (!doc().contains("animations"))
            return;
        for (auto& animation : doc().at("animations")) {
            w.check();
            int64_t stack = id(), layer = id();
            double duration = animation.at("extras").at("duration_seconds");
            int64_t end = keyTime(duration);
            std::string name = animation.value("name", "Animation");
            object("AnimationStack", stack, name, "", [&] {
                w.node("Properties70", {}, [&] {
                    w.prop("LocalStart", "KTime", {Property(int64_t(0))});
                    w.prop("LocalStop", "KTime", {Property(end)});
                    w.prop("ReferenceStart", "KTime", {Property(int64_t(0))});
                    w.prop("ReferenceStop", "KTime", {Property(end)});
                    w.string("EDM_ArgumentMapping", animation.at("extras").dump());
                });
            });
            object("AnimationLayer", layer, name, "", [&] {
                w.node("Properties70", {}, [&] {
                    w.scalar("Weight", 100.);
                    w.integer("BlendMode", 1, "enum"); // Override independent argument clip.
                    w.integer("RotationAccumulationMode", 0, "enum");
                    w.integer("ScaleAccumulationMode", 0, "enum");
                });
            });
            connect(layer, stack);
            for (auto& ch : animation.at("channels"))
                channel(layer, ch, animation.at("samplers").at(ch.at("sampler").get<size_t>()));
            if (progress)
                progress("FBX 动画 " + name);
        }
    }
    void finish() {
        w.node("Definitions", {}, [&] {
            w.leaf("Version", {Property(int32_t(100))});
            int32_t total = 0;
            for (auto& [name, count] : objectCounts)
                total += count;
            w.leaf("Count", {Property(total)});
            for (auto& [name, count] : objectCounts)
                w.node("ObjectType", {Property(name)}, [&] { w.leaf("Count", {Property(count)}); });
        });
        w.node("Connections", {}, [&] {
            for (auto& c : connections) {
                std::vector<Property> p{Property(c.property.empty() ? "OO" : "OP"), Property(c.from),
                                        Property(c.to)};
                if (!c.property.empty())
                    p.push_back(Property(c.property));
                w.leaf("C", std::move(p));
            }
        });
        w.node("Takes", {}, [&] { w.leaf("Current", {Property("")}); });
        w.footer();
    }
};
} // namespace

Json exportFbxScene(const Scene& scene, const fs::path& path, const ExportOptions& options, Progress progress,
                    const std::atomic_bool* cancel) {
    const auto start = Clock::now();
    require(lower(pathString(path.extension())) == ".fbx", "FBX export requires a .fbx extension");
    auto payload = buildExportPayload(scene, options, progress, cancel);
    Encoder encoder(payload, cancel);
    encoder.metadata();
    encoder.w.node("Objects", {}, [&] {
        encoder.materialResources();
        encoder.hierarchy();
        encoder.meshes();
        encoder.animations(progress);
    });
    encoder.finish();
    encoder.w.check();
    Json report = std::move(payload.report);
    report.update(
        {{"format", "fbx"},
         {"fbx_version", 7400},
         {"exported_meshes", encoder.meshCount},
         {"exported_triangles", encoder.triangleCount},
         {"exported_skins", encoder.skinCount},
         {"animation_curves", encoder.curveCount},
         {"baked_rotation_keys", encoder.rotationKeyCount},
         {"opaque_rgb_image_variants", encoder.opaqueVideos.size()},
         {"opaque_alpha_compatibility",
          "OPAQUE diffuse materials use full-resolution RGB PNG variants without an alpha channel. "
          "Original PNG bytes remain embedded and are identified by EDM_OriginalImage / "
          "EDM_OriginalDiffuseImage metadata; transparent and masked materials retain their original PNG."},
         {"rotation_baking", "60 Hz minimum for moving rotations, adaptive quaternion error 0.000002 rad"},
         {"fbx_time_resolution_seconds", 1. / fbxSecond},
         {"step_compatibility",
          "True CONSTANT interpolation plus preceding hold keys. Readers that "
          "discard interpolation may blend over approximately 0.46 ms or omit shorter pulses."},
         {"coordinates", "right-handed Y-up, metres (FBX UnitScaleFactor=100)"},
         {"material_limitations",
          "FBX carries diffuse/alpha and embedded PNG images. ORM remains an "
          "explicit EDM_ORMImage resource (R=occlusion, G=roughness, B=metallic); reconnect its channels "
          "in the receiving application. Alpha cutoff and DCS shader effects are application-dependent."},
         {"export_seconds", seconds(start)},
         {"output_bytes", encoder.w.data.size()}});
    // Embedded resources keep publication atomic: cancellation never replaces a previous complete FBX.
    writeFile(path, encoder.w.data);
    return report;
}
} // namespace edm
