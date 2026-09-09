#pragma once
#include "common.h"
namespace edm {
struct Key {
    double time = 0;
    V4 value = V4::Zero();
};
struct ArgKeys {
    int arg = 0;
    std::vector<Key> keys, second;
};
struct Visibility {
    int arg = 0;
    std::vector<std::pair<double, double>> ranges;
};
struct TextureRef {
    int slot = 0;
    std::string name;
    Mat matrix = Mat::Identity();
};
struct Material {
    std::string name, shader;
    fs::path source;
    int blending = 0, culling = 0, decal = 0;
    std::vector<int> format;
    std::vector<uint32_t> uvChannels;
    std::vector<TextureRef> textures;
    Json uniforms = Json::object(), animatedUniforms = Json::object(), extras = Json::object();
    int offset(int c) const {
        if (c >= int(format.size()) || format[c] == 0)
            return -1;
        return std::accumulate(format.begin(), format.begin() + c, 0);
    }
    int channel(int c) const {
        return c < int(format.size()) ? format[c] : 0;
    }
    int stride() const {
        return std::accumulate(format.begin(), format.end(), 0);
    }
    const TextureRef* texture(int slot) const {
        for (auto& t : textures)
            if (t.slot == slot)
                return &t;
        return nullptr;
    }
};
struct RawNode {
    std::string type, name;
    int version = 0, parent = -1;
    Json props = Json::object();
    Mat matrix = Mat::Identity(), matrix2 = Mat::Identity();
    bool hasBind = false;
    V3 position = V3::Zero(), scale = V3::Ones();
    V4 q1{0, 0, 0, 1}, q2{0, 0, 0, 1};
    std::vector<ArgKeys> positions, rotations, scales;
    std::vector<Visibility> visibility;
    Json extras = Json::object();
    bool animated() const {
        return type == "ArgAnimationNode" || type == "ArgPositionNode" || type == "ArgRotationNode" ||
               type == "ArgScaleNode" || type == "ArgAnimatedBone";
    }
};
struct Parent {
    int node = -1, start = 0, damage = -1;
};
struct NumberControl {
    int u = -1, v = -1;
    float su = 0, sv = 0;
};
struct RawRender {
    std::string type, name;
    int version = 0, material = 0;
    Json props = Json::object();
    std::vector<Parent> parents;
    std::vector<int> bones;
    uint32_t vertexCount = 0, stride = 0;
    std::span<const uint8_t> vertices;
    std::vector<uint32_t> indices;
    std::vector<NumberControl> numbers;
    float value(uint32_t i, int c) const {
        float v;
        std::memcpy(&v, vertices.data() + (size_t(i) * stride + c) * 4, 4);
        return v;
    }
    uint32_t word(uint32_t i, int c) const {
        uint32_t v;
        std::memcpy(&v, vertices.data() + (size_t(i) * stride + c) * 4, 4);
        return v;
    }
};
struct Document {
    fs::path source;
    int version = 0;
    std::shared_ptr<MappedFile> file;
    std::vector<Material> materials;
    std::vector<RawNode> nodes, connectors;
    std::vector<RawRender> renders;
    Json rootProps, extraItems, renderTypes = Json::object();
    int collisionCount = 0, lightCount = 0;
};
Document parseEdm(const fs::path& path, Progress progress = {}, const std::atomic_bool* cancel = nullptr);
struct Node {
    std::string name;
    int parent = -1;
    V3 t = V3::Zero(), s = V3::Ones();
    V4 q{0, 0, 0, 1};
    Json extras = Json::object();
    Mat local() const {
        return translation(t) * rotation(q) * scaling(s);
    }
};
enum class Channel { Position, Rotation, Scale };
inline const char* channelName(Channel c) {
    return c == Channel::Position ? "translation" : c == Channel::Rotation ? "rotation" : "scale";
}
struct Track {
    int node = 0, arg = 0;
    Channel channel = Channel::Position;
    std::vector<Key> keys;
    std::vector<std::pair<double, double>> ranges;
    bool visibility = false, conjugate = false;
    V4 sample(double value) const;
    std::pair<double, double> domain() const;
};
struct Mesh {
    std::string name;
    int node = 0, material = 0;
    std::vector<F3> positions, normals;
    std::vector<uint32_t> indices;
    std::vector<std::vector<F2>> uvs;
    std::vector<std::array<uint16_t, 8>> joints;
    std::vector<std::array<float, 8>> weights;
    std::vector<int> skinNodes, selectors;
    std::vector<Mat> inverseBind;
    std::vector<NumberControl> numbers;
    Json extras;
    bool skinned() const {
        return !joints.empty();
    }
    size_t byteSize() const {
        return positions.size() * sizeof(F3) * 2 + indices.size() * 4;
    }
};
struct SceneAttachment {
    fs::path source;
    int targetNode = -1, root = -1, attachNode = -1;
    int nodeBegin = 0, nodeCount = 0, materialBegin = 0, materialCount = 0, meshBegin = 0, meshCount = 0;
    std::map<int, int> argumentMap;
    // Runtime bookkeeping only: not part of the persistent paint assembly identity.
    int renderBegin = 0, renderCount = 0, sourceCollisions = 0;
    Json sourceRenderTypes = Json::object();
};
struct Scene {
    fs::path source;
    int version = 0, sourceNodes = 0, collisionCount = 0, connectorCount = 0;
    Json renderTypes;
    std::vector<Material> materials;
    std::vector<Node> nodes;
    std::vector<Mesh> meshes;
    std::vector<Track> tracks;
    std::vector<SceneAttachment> attachments;
    Args defaultArgs;
    std::map<int, std::pair<double, double>> limits;
    std::vector<int> numberArgs, order, heads, tails;
    std::vector<Mat> staticLocal, defaultWorld;
    std::vector<std::string> warnings;
    double parseSeconds = 0, buildSeconds = 0;
    static std::shared_ptr<Scene> load(const fs::path& path, Progress progress = {},
                                       const std::atomic_bool* cancel = nullptr);
    std::vector<Mat> evaluate(const Args& args, bool attachmentsVisible = true) const;
    std::vector<F3> transformed(const Mesh& mesh, const std::vector<Mat>& world) const;
    Json summary() const;
    void warn(std::string message) {
        if (std::find(warnings.begin(), warnings.end(), message) == warnings.end())
            warnings.push_back(std::move(message));
    }
};
enum class MaterialAlphaMode : uint32_t { Opaque, Mask, Blend };
MaterialAlphaMode materialAlphaMode(const Material& mat);
F4 materialColor(const Material& mat);
std::vector<F2> textureUV(const Mesh& mesh, const Material& mat, int slot, const Args& args = {});
std::vector<int> bortMapping(const Scene& scene);
Args bortArguments(const Scene& scene, const std::string& text);
} // namespace edm
