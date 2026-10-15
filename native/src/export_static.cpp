#include "export_internal.h"
#include <cfloat>

namespace edm {
namespace {
// This reader consumes only our own generated, uncompressed glTF payload.
struct Attribute {
    std::span<const uint8_t> bytes;
    size_t count = 0, components = 0, size = 0, stride = 0;
    int type = 0;
    double value(size_t vertex, size_t component = 0) const {
        require(vertex < count && component < components, "Static export attribute index out of range");
        const auto* data = bytes.data() + vertex * stride + component * size;
        if (type == 5126) {
            float result;
            std::memcpy(&result, data, 4);
            require(std::isfinite(result), "Static export has nonfinite geometry");
            return result;
        }
        if (type == 5125) {
            uint32_t result;
            std::memcpy(&result, data, 4);
            return result;
        }
        if (type == 5123) {
            uint16_t result;
            std::memcpy(&result, data, 2);
            return result;
        }
        return *data;
    }
    V3 vector(size_t i) const {
        return {value(i, 0), value(i, 1), value(i, 2)};
    }
};
struct StaticBaker {
    const ExportPayload& source;
    const std::atomic_bool* cancel;
    ExportPayload output;
    std::map<int, int> materials, textures, images, samplers;
    size_t triangles = 0, vertices = 0, skipped = 0, reversed = 0;
    explicit StaticBaker(const ExportPayload& input, const std::atomic_bool* stop)
        : source(input), cancel(stop) {
        output.document = {{"asset", {{"version", "2.0"}}},
                           {"scene", 0},
                           {"scenes", Json::array({{{"nodes", Json::array()}}})},
                           {"nodes", Json::array()},
                           {"meshes", Json::array()},
                           {"materials", Json::array()},
                           {"accessors", Json::array()},
                           {"bufferViews", Json::array()}};
    }
    void check() const {
        if (cancel && cancel->load())
            throw std::runtime_error("Cancelled");
    }
    std::span<const uint8_t> view(int id) const {
        const auto& item = source.document.at("bufferViews").at(id);
        const size_t offset = item.value("byteOffset", size_t(0)), length = item.at("byteLength");
        require(offset <= source.buffer.size() && length <= source.buffer.size() - offset,
                "Static export buffer out of range");
        return std::span(source.buffer).subspan(offset, length);
    }
    Attribute attribute(int id) const {
        const auto& item = source.document.at("accessors").at(id);
        Attribute result;
        result.type = item.at("componentType");
        require(result.type == 5126 || result.type == 5125 || result.type == 5123 || result.type == 5121,
                "Static export unsupported attribute type");
        result.size = result.type == 5123 ? 2 : result.type == 5121 ? 1 : 4;
        const std::string shape = item.at("type");
        result.components = shape == "SCALAR" ? 1
                            : shape == "VEC2" ? 2
                            : shape == "VEC3" ? 3
                            : shape == "VEC4" ? 4
                            : shape == "MAT4" ? 16
                                              : 0;
        require(result.components > 0, "Static export unsupported attribute shape");
        const int vi = item.at("bufferView");
        result.bytes = view(vi);
        const size_t offset = item.value("byteOffset", size_t(0));
        require(offset <= result.bytes.size(), "Static export attribute offset out of range");
        result.bytes = result.bytes.subspan(offset);
        result.stride =
            source.document.at("bufferViews").at(vi).value("byteStride", result.size * result.components);
        result.count = item.at("count");
        const auto width = result.size * result.components;
        require(result.stride >= width &&
                    (!result.count || (result.count - 1 <= result.bytes.size() / result.stride &&
                                       width <= result.bytes.size() - (result.count - 1) * result.stride)),
                "Static export attribute data out of range");
        return result;
    }
    int blob(std::span<const uint8_t> data, int target = 0) {
        check();
        while (output.buffer.size() % 4)
            output.buffer.push_back(0);
        Json item = {{"buffer", 0}, {"byteOffset", output.buffer.size()}, {"byteLength", data.size()}};
        if (target)
            item["target"] = target;
        int id = int(output.document["bufferViews"].size());
        output.document["bufferViews"].push_back(item);
        output.buffer.insert(output.buffer.end(), data.begin(), data.end());
        return id;
    }
    int accessor(Json item, std::span<const uint8_t> data, int target) {
        item["bufferView"] = blob(data, target);
        item.erase("byteOffset");
        item.erase("extras");
        const int id = int(output.document["accessors"].size());
        output.document["accessors"].push_back(std::move(item));
        return id;
    }
    template <class T>
    int array(const std::vector<T>& data, int type, const char* shape, int target, bool bounds = false) {
        Json item = {{"componentType", type}, {"count", data.size()}, {"type", shape}};
        if (bounds) {
            const auto* values = reinterpret_cast<const float*>(data.data());
            F3 lo{FLT_MAX, FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
            for (size_t i = 0; i < data.size() * 3; ++i) {
                require(std::isfinite(values[i]), "Static export exceeds float position range");
                lo[i % 3] = std::min(lo[i % 3], values[i]);
                hi[i % 3] = std::max(hi[i % 3], values[i]);
            }
            item["min"] = lo;
            item["max"] = hi;
        }
        return accessor(std::move(item),
                        {reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T)}, target);
    }
    int copyAttribute(int id, const std::vector<uint32_t>& keep) {
        const auto input = attribute(id);
        const auto width = input.components * input.size;
        std::vector<uint8_t> bytes(keep.size() * width);
        for (size_t i = 0; i < keep.size(); ++i) {
            if (!(i % 4096))
                check();
            require(keep[i] < input.count, "Static export mismatched vertex attributes");
            std::memcpy(bytes.data() + i * width, input.bytes.data() + keep[i] * input.stride, width);
        }
        Json item = source.document.at("accessors").at(id);
        item["count"] = keep.size();
        item.erase("min");
        item.erase("max");
        return accessor(std::move(item), bytes, 34962);
    }
    int texture(int id) {
        if (textures.contains(id))
            return textures.at(id);
        Json item = source.document.at("textures").at(id);
        const int image = item.at("source");
        if (!images.contains(image)) {
            Json imageItem = source.document.at("images").at(image);
            imageItem["bufferView"] = blob(view(imageItem.at("bufferView")));
            imageItem.erase("extras");
            if (!output.document.contains("images"))
                output.document["images"] = Json::array();
            images[image] = int(output.document["images"].size());
            output.document["images"].push_back(std::move(imageItem));
        }
        item["source"] = images.at(image);
        if (item.contains("sampler")) {
            const int sampler = item.at("sampler");
            if (!samplers.contains(sampler)) {
                if (!output.document.contains("samplers"))
                    output.document["samplers"] = Json::array();
                samplers[sampler] = int(output.document["samplers"].size());
                output.document["samplers"].push_back(source.document.at("samplers").at(sampler));
            }
            item["sampler"] = samplers.at(sampler);
        }
        item.erase("extras");
        if (!output.document.contains("textures"))
            output.document["textures"] = Json::array();
        const int target = int(output.document["textures"].size());
        output.document["textures"].push_back(std::move(item));
        return textures[id] = target;
    }
    int material(int id) {
        if (materials.contains(id))
            return materials.at(id);
        Json item = source.document.at("materials").at(id);
        item.erase("extras");
        auto remap = [&](Json& owner, const char* key) {
            if (owner.contains(key))
                owner[key]["index"] = texture(owner[key].at("index"));
        };
        auto& pbr = item["pbrMetallicRoughness"];
        remap(pbr, "baseColorTexture");
        remap(pbr, "metallicRoughnessTexture");
        remap(item, "occlusionTexture");
        remap(item, "normalTexture");
        remap(item, "emissiveTexture");
        const int target = int(output.document["materials"].size());
        output.document["materials"].push_back(std::move(item));
        return materials[id] = target;
    }
    static Eigen::Matrix3d normalMatrix(const Mat& matrix) {
        const Eigen::Matrix3d linear = matrix.block<3, 3>(0, 0);
        if (std::abs(linear.determinant()) <= 1e-20)
            return Eigen::Matrix3d::Zero();
        return linear.inverse().transpose();
    }
    void mesh(const Json& node, const Json& mesh, const Json& primitive, const std::vector<Mat>& world,
              size_t nodeIndex) {
        check();
        const auto& attrs = primitive.at("attributes");
        const auto positions = attribute(attrs.at("POSITION")), normals = attribute(attrs.at("NORMAL"));
        require(positions.count == normals.count, "Static export normal count mismatch");
        const bool skinned = node.contains("skin");
        std::vector<Mat> palette;
        std::vector<Eigen::Matrix3d> normalPalette;
        std::vector<std::pair<Attribute, Attribute>> weights;
        if (skinned) {
            const auto& skin = source.document.at("skins").at(node.at("skin").get<size_t>());
            const auto binds = attribute(skin.at("inverseBindMatrices"));
            for (size_t i = 0; i < skin.at("joints").size(); ++i) {
                Mat bind;
                for (size_t k = 0; k < 16; ++k)
                    bind.data()[k] = binds.value(i, k);
                palette.push_back(world.at(skin.at("joints").at(i).get<size_t>()) * bind);
            }
            for (int set = 0; set < 2; ++set) {
                const auto key = std::to_string(set);
                if (attrs.contains("JOINTS_" + key))
                    weights.emplace_back(attribute(attrs.at("JOINTS_" + key)),
                                         attribute(attrs.at("WEIGHTS_" + key)));
            }
            require(!weights.empty(), "Static export skin has no weights");
        } else
            palette.push_back(world.at(nodeIndex));
        for (const auto& matrix : palette) {
            require(matrix.allFinite(), "Static export nonfinite transform");
            normalPalette.push_back(normalMatrix(matrix));
        }
        std::vector<V3> points(positions.count), directions(positions.count);
        for (size_t i = 0; i < positions.count; ++i) {
            if (!(i % 4096))
                check();
            const V3 p = positions.vector(i), n = normals.vector(i);
            const V4 homogeneous(p[0], p[1], p[2], 1);
            V4 transformed = V4::Zero();
            V3 normal = V3::Zero();
            if (skinned) {
                for (const auto& [joints, amounts] : weights)
                    for (size_t k = 0; k < 4; ++k) {
                        const double weight = amounts.value(i, k);
                        require(weight >= 0, "Static export negative skin weight");
                        if (!weight)
                            continue;
                        const size_t joint = size_t(joints.value(i, k));
                        transformed += palette.at(joint) * homogeneous * weight;
                        normal += normalPalette.at(joint) * n * weight;
                    }
            } else {
                transformed = palette[0] * homogeneous;
                normal = normalPalette[0] * n;
            }
            require(transformed.allFinite() && normal.allFinite(), "Static export nonfinite posed vertex");
            points[i] = transformed.head<3>();
            directions[i] = normal.squaredNorm() > 1e-30 ? V3(normal.normalized()) : V3::Zero();
        }
        const auto inputIndices = attribute(primitive.at("indices"));
        require(inputIndices.count % 3 == 0, "Static export needs triangle geometry");
        std::vector<uint32_t> indices;
        std::vector<bool> used(positions.count, false);
        const bool mirror = !skinned && palette[0].block<3, 3>(0, 0).determinant() < 0;
        for (size_t i = 0; i < inputIndices.count; i += 3) {
            if (!(i % 12288))
                check();
            uint32_t a = uint32_t(inputIndices.value(i)), b = uint32_t(inputIndices.value(i + 1)),
                     c = uint32_t(inputIndices.value(i + 2));
            V3 face = (points.at(b) - points.at(a)).cross(points.at(c) - points.at(a));
            if (face.squaredNorm() <= 1e-40)
                continue;
            if (mirror || (skinned && face.dot(directions[a] + directions[b] + directions[c]) < 0)) {
                std::swap(b, c);
                face = -face;
                ++reversed;
            }
            indices.insert(indices.end(), {a, b, c});
            for (auto vertex : {a, b, c}) {
                used[vertex] = true;
                if (directions[vertex].squaredNorm() <= 1e-30)
                    directions[vertex] = face.normalized();
            }
        }
        if (indices.empty()) {
            ++skipped;
            return;
        }
        std::vector<uint32_t> keep, remap(positions.count);
        std::vector<F3> bakedPoints, bakedNormals;
        for (size_t i = 0; i < used.size(); ++i)
            if (used[i]) {
                remap[i] = uint32_t(keep.size());
                keep.push_back(uint32_t(i));
                bakedPoints.push_back({float(points[i][0]), float(points[i][1]), float(points[i][2])});
                bakedNormals.push_back(
                    {float(directions[i][0]), float(directions[i][1]), float(directions[i][2])});
            }
        for (auto& index : indices)
            index = remap[index];
        Json attributes = {{"POSITION", array(bakedPoints, 5126, "VEC3", 34962, true)},
                           {"NORMAL", array(bakedNormals, 5126, "VEC3", 34962)}};
        for (const auto& [key, value] : attrs.items())
            if (key.starts_with("TEXCOORD_") || key.starts_with("COLOR_"))
                attributes[key] = copyAttribute(value, keep);
        Json result = {{"attributes", attributes},
                       {"indices", array(indices, 5125, "SCALAR", 34963)},
                       {"mode", 4},
                       {"material", material(primitive.at("material"))}};
        const std::string name = mesh.value("name", node.value("name", "Mesh"));
        const size_t index = output.document["meshes"].size();
        output.document["meshes"].push_back({{"name", name}, {"primitives", Json::array({result})}});
        output.document["nodes"].push_back({{"name", name}, {"mesh", index}});
        output.document["scenes"][0]["nodes"].push_back(index);
        triangles += indices.size() / 3;
        vertices += keep.size();
    }
};
} // namespace

ExportPayload bakeModelOnlyPayload(ExportPayload source, Progress progress, const std::atomic_bool* cancel) {
    StaticBaker baker(source, cancel);
    const auto& nodes = source.document.at("nodes");
    std::vector<Mat> world(nodes.size());
    std::vector<bool> visited(nodes.size(), false);
    std::vector<std::pair<size_t, Mat>> pending;
    for (const auto& root : source.document.at("scenes").at(0).at("nodes"))
        pending.emplace_back(root.get<size_t>(), Mat::Identity());
    while (!pending.empty()) {
        baker.check();
        auto [index, parent] = std::move(pending.back());
        pending.pop_back();
        require(index < nodes.size() && !visited[index], "Static export invalid node hierarchy");
        visited[index] = true;
        const auto& node = nodes.at(index);
        auto vector = [&](const char* key, V3 fallback) {
            if (!node.contains(key))
                return fallback;
            const auto& value = node.at(key);
            return V3(value.at(0), value.at(1), value.at(2));
        };
        V4 q(0, 0, 0, 1);
        if (node.contains("rotation"))
            for (size_t k = 0; k < 4; ++k)
                q[k] = node.at("rotation").at(k);
        Mat local = translation(vector("translation", V3::Zero())) * rotation(q) *
                    scaling(vector("scale", V3::Ones()));
        if (node.contains("matrix"))
            for (size_t k = 0; k < 16; ++k)
                local.data()[k] = node.at("matrix").at(k);
        world[index] = parent * local;
        if (node.contains("children"))
            for (const auto& child : node.at("children"))
                pending.emplace_back(child.get<size_t>(), world[index]);
    }
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!visited[i] || !nodes[i].contains("mesh"))
            continue;
        const auto& mesh = source.document.at("meshes").at(nodes[i].at("mesh").get<size_t>());
        for (const auto& primitive : mesh.at("primitives"))
            baker.mesh(nodes[i], mesh, primitive, world, i);
        if (progress && i % 100 == 0)
            progress(edm::tr("烘焙纯模型网格 ") + std::to_string(baker.output.document["meshes"].size()));
    }
    require(baker.triangles > 0, edm::tr("当前姿态没有可导出的三角网格"));
    baker.output.document["buffers"] = Json::array({{{"byteLength", baker.output.buffer.size()}}});
    baker.output.report = std::move(source.report);
    baker.output.report.update({{"model_only", true},
                                {"exported_arguments", Json::array()},
                                {"clip_duration_seconds", 0},
                                {"exported_meshes", baker.output.document["meshes"].size()},
                                {"exported_vertices", baker.vertices},
                                {"exported_triangles", baker.triangles},
                                {"exported_skins", 0},
                                {"skipped_hidden_or_degenerate_meshes", baker.skipped},
                                {"reversed_triangles", baker.reversed},
                                {"exported_materials", baker.materials.size()},
                                {"exported_textures", baker.images.size()},
                                {"geometry_encoding", "current pose baked in world coordinates"}});
    baker.check();
    return std::move(baker.output);
}
} // namespace edm
