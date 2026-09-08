#include "export_internal.h"
#include <iomanip>
#include <sstream>
namespace edm {
namespace {
struct Builder {
    Json doc = {{"asset", {{"version", "2.0"}, {"generator", "EDM Studio C++ " EDM_NATIVE_VERSION}}},
                {"scene", 0},
                {"scenes", Json::array({{{"nodes", Json::array()}}})},
                {"nodes", Json::array()},
                {"meshes", Json::array()},
                {"materials", Json::array()},
                {"accessors", Json::array()},
                {"bufferViews", Json::array()}};
    std::vector<uint8_t> data;
    std::unordered_map<uint64_t, std::vector<int>> cache;
    int blob(std::span<const uint8_t> raw, int target = 0) {
        while (data.size() % 4)
            data.push_back(0);
        Json j = {{"buffer", 0}, {"byteOffset", data.size()}, {"byteLength", raw.size()}};
        if (target)
            j["target"] = target;
        int idx = int(doc["bufferViews"].size());
        doc["bufferViews"].push_back(j);
        data.insert(data.end(), raw.begin(), raw.end());
        return idx;
    }
    int accessor(std::span<const uint8_t> bytes, int count, int components, int component, const char* type,
                 int target = 0, bool bounds = false) {
        uint64_t hash = 1469598103934665603ull;
        for (auto b : bytes)
            hash = (hash ^ b) * 1099511628211ull;
        hash ^= uint64_t(count) << 32;
        hash ^= uint64_t(component * 31 + components * 17 + target * 13 + bounds);
        auto& entries = cache[hash];
        for (int idx : entries) {
            auto& a = doc["accessors"][idx];
            auto& v = doc["bufferViews"][a["bufferView"].get<int>()];
            size_t offset = v["byteOffset"], size = v["byteLength"];
            if (size == bytes.size() && a["count"] == count && a["type"] == type &&
                a["componentType"] == component && a.contains("min") == bounds &&
                v.value("target", 0) == target && std::memcmp(bytes.data(), data.data() + offset, size) == 0)
                return idx;
        }
        Json a = {{"bufferView", blob(bytes, target)},
                  {"componentType", component},
                  {"count", count},
                  {"type", type}};
        if (component == 5126) {
            std::vector<float> lo(components, std::numeric_limits<float>::max()),
                hi(components, -std::numeric_limits<float>::max());
            for (int i = 0; i < count * components; i++) {
                float f;
                std::memcpy(&f, bytes.data() + size_t(i) * 4, 4);
                require(std::isfinite(f), "Nonfinite glTF accessor");
                if (bounds) {
                    lo[i % components] = std::min(lo[i % components], f);
                    hi[i % components] = std::max(hi[i % components], f);
                }
            }
            if (bounds) {
                a["min"] = lo;
                a["max"] = hi;
            }
        }
        int idx = int(doc["accessors"].size());
        doc["accessors"].push_back(a);
        entries.push_back(idx);
        return idx;
    }
    template <class T>
    int acc(const std::vector<T>& values, int components, int component, const char* type, int target = 0,
            bool bounds = false) {
        return accessor({reinterpret_cast<const uint8_t*>(values.data()), values.size() * sizeof(T)},
                        int(values.size() * sizeof(T) / (components * (component == 5123 ? 2 : 4))),
                        components, component, type, target, bounds);
    }
};
std::string base64(std::span<const uint8_t> bytes) {
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        uint32_t n = uint32_t(bytes[i]) << 16;
        if (i + 1 < bytes.size())
            n |= uint32_t(bytes[i + 1]) << 8;
        if (i + 2 < bytes.size())
            n |= bytes[i + 2];
        result.push_back(chars[n >> 18]);
        result.push_back(chars[(n >> 12) & 63]);
        result.push_back(i + 1 < bytes.size() ? chars[(n >> 6) & 63] : '=');
        result.push_back(i + 2 < bytes.size() ? chars[n & 63] : '=');
    }
    return result;
}
struct NumberVariant {
    int node;
    Args state;
    std::map<int, std::vector<double>> grids;
};
Json argsJson(const Args& args) {
    Json out = Json::object();
    for (auto [a, v] : args)
        out[std::to_string(a)] = v;
    return out;
}
void appendU32(std::vector<uint8_t>& bytes, uint32_t n) {
    for (int i = 0; i < 4; i++)
        bytes.push_back(uint8_t(n >> (8 * i)));
}
} // namespace
ExportPayload buildExportPayload(const Scene& scene, const ExportOptions& options, Progress progress,
                                 const std::atomic_bool* cancel) {
    auto start = Clock::now();
    auto check = [&] {
        if (cancel && *cancel)
            throw std::runtime_error("Cancelled");
    };
    require(std::isfinite(options.duration) && options.duration > 0, "Animation duration must be positive");
    std::vector<int> arguments;
    if (options.arguments)
        arguments = *options.arguments;
    else
        for (auto [a, r] : scene.limits)
            arguments.push_back(a);
    std::sort(arguments.begin(), arguments.end());
    arguments.erase(std::unique(arguments.begin(), arguments.end()), arguments.end());
    for (int a : arguments)
        require(scene.limits.contains(a), "Unknown export argument");
    Args baseline = options.livery ? options.livery->args : Args{};
    for (auto [a, v] : options.baseline) {
        require(a >= 0 && std::isfinite(v), "Invalid baseline argument");
        baseline[a] = v;
    }
    Builder b;
    auto& d = b.doc;
    std::vector<Node> nodes = scene.nodes;
    for (auto& t : scene.tracks)
        if (baseline.contains(t.arg)) {
            auto v = t.sample(baseline[t.arg]);
            if (t.channel == Channel::Position)
                nodes[t.node].t = v.head<3>();
            else if (t.channel == Channel::Scale)
                nodes[t.node].s = v.head<3>();
            else
                nodes[t.node].q = v;
        }
    for (size_t i = 0; i < nodes.size(); i++) {
        auto& n = nodes[i];
        d["nodes"].push_back({{"name", n.name},
                              {"translation", arrayJson(n.t)},
                              {"rotation", arrayJson(qnormalize(n.q))},
                              {"scale", arrayJson(n.s)},
                              {"extras", n.extras}});
        if (n.parent < 0)
            d["scenes"][0]["nodes"].push_back(i);
    }
    for (size_t i = 0; i < nodes.size(); i++)
        if (nodes[i].parent >= 0) {
            auto& parent = d["nodes"][nodes[i].parent];
            if (!parent.contains("children"))
                parent["children"] = Json::array();
            parent["children"].push_back(i);
        }
    std::unique_ptr<TextureResolver> resolver;
    if (options.textures)
        resolver = std::make_unique<TextureResolver>(scene.source, options.textureDirectory, options.livery);
    std::map<std::string, int> images;
    int diffuseCount = 0, roughmetCount = 0, numberCount = 0;
    std::map<int, int> decals;
    std::vector<NumberVariant> variants;
    auto embedPng = [&](std::span<const uint8_t> png, const std::string& name) {
        require(png.size() >= 8 && png[0] == 137 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G',
                "Invalid edited PNG texture");
        if (!d.contains("images")) {
            d["images"] = Json::array();
            d["textures"] = Json::array();
        }
        int image = int(d["images"].size()), texture = int(d["textures"].size());
        d["images"].push_back({{"name", name}, {"mimeType", "image/png"}, {"bufferView", b.blob(png)}});
        d["textures"].push_back({{"source", image}});
        return texture;
    };
    auto embed = [&](std::optional<ImageSource> found, const std::string& name) -> int {
        if (!found)
            return -1;
        check();
        auto key = found->key();
        if (images.contains(key))
            return images[key];
        std::vector<uint8_t> png;
        try {
            png = pngTexture(*found);
        } catch (...) {
            resolver->warnings.push_back(key + ": " + exceptionText());
            resolver->missing.push_back(key);
            images[key] = -1;
            return -1;
        }
        if (!d.contains("images")) {
            d["images"] = Json::array();
            d["textures"] = Json::array();
        }
        int image = int(d["images"].size()), texture = int(d["textures"].size());
        d["images"].push_back({{"name", name}, {"mimeType", "image/png"}, {"bufferView", b.blob(png)}});
        d["textures"].push_back({{"source", image}});
        return images[key] = texture;
    };
    for (int i = 0; i < int(scene.materials.size()); i++) {
        check();
        auto& m = scene.materials[i];
        auto color = materialColor(m);
        Json textures = Json::array();
        for (auto& t : m.textures)
            textures.push_back({{"index", t.slot}, {"name", t.name}, {"matrix", matJson(t.matrix)}});
        Json mat = {{"name", m.name.empty() ? "Material " + std::to_string(i) : m.name},
                    {"pbrMetallicRoughness",
                     {{"baseColorFactor", color}, {"metallicFactor", 0}, {"roughnessFactor", .65}}},
                    {"doubleSided", m.culling == 1},
                    {"extras",
                     {{"edm_shader", m.shader},
                      {"edm_uniforms", m.uniforms},
                      {"edm_animated_uniforms", m.animatedUniforms},
                      {"edm_textures", textures}}}};
        if (materialAlphaMode(m) == MaterialAlphaMode::Blend)
            mat["alphaMode"] = "BLEND";
        else if (materialAlphaMode(m) == MaterialAlphaMode::Mask) {
            mat["alphaMode"] = "MASK";
            mat["alphaCutoff"] = .5;
        }
        if (resolver) {
            auto edited = options.diffuseOverrides.find(i);
            int diffuse = edited == options.diffuseOverrides.end()
                              ? embed(resolver->material(m), m.name + " diffuse")
                              : embedPng(edited->second, m.name + " painted diffuse");
            if (diffuse >= 0) {
                mat["pbrMetallicRoughness"]["baseColorTexture"] = {{"index", diffuse}};
                mat["pbrMetallicRoughness"]["baseColorFactor"] = {1, 1, 1, color[3]};
                diffuseCount++;
            }
            int rm = embed(resolver->material(m, 13), m.name + " RoughMet (linear ORM)");
            if (rm >= 0) {
                mat["pbrMetallicRoughness"]["metallicRoughnessTexture"] = {{"index", rm}, {"texCoord", 1}};
                mat["pbrMetallicRoughness"]["metallicFactor"] = 1;
                mat["pbrMetallicRoughness"]["roughnessFactor"] = 1;
                mat["occlusionTexture"] = {{"index", rm}, {"texCoord", 1}, {"strength", 1}};
                roughmetCount++;
            }
        }
        d["materials"].push_back(mat);
        if (progress)
            progress("导出材质 " + std::to_string(i + 1) + " / " + std::to_string(scene.materials.size()));
    }
    if (resolver)
        for (auto& mesh : scene.meshes)
            if (!mesh.selectors.empty() && !decals.contains(mesh.material)) {
                auto& m = scene.materials[mesh.material];
                int atlas = embed(resolver->material(m, 3), m.name + " number atlas");
                if (atlas < 0)
                    continue;
                Json mat = d["materials"][mesh.material];
                mat["name"] = m.name + " / registration";
                mat["pbrMetallicRoughness"]["baseColorTexture"] = {{"index", atlas}, {"texCoord", 2}};
                mat["pbrMetallicRoughness"]["baseColorFactor"] = {1, 1, 1, 1};
                mat["pbrMetallicRoughness"]["metallicFactor"] = 0;
                mat["alphaMode"] = "MASK";
                mat["alphaCutoff"] = .1;
                decals[mesh.material] = int(d["materials"].size());
                d["materials"].push_back(mat);
            }
    for (size_t mi = 0; mi < scene.meshes.size(); mi++) {
        check();
        auto& mesh = scene.meshes[mi];
        auto& mat = scene.materials[mesh.material];
        auto& material = d["materials"][mesh.material];
        Json attrs = {{"POSITION", b.acc(mesh.positions, 3, 5126, "VEC3", 34962, true)},
                      {"NORMAL", b.acc(mesh.normals, 3, 5126, "VEC3", 34962)}};
        if (!mesh.uvs.empty() || material["pbrMetallicRoughness"].contains("baseColorTexture") ||
            material.contains("occlusionTexture")) {
            attrs["TEXCOORD_0"] = b.acc(textureUV(mesh, mat, 0), 2, 5126, "VEC2", 34962);
            if (material.contains("occlusionTexture"))
                attrs["TEXCOORD_1"] = b.acc(textureUV(mesh, mat, 13), 2, 5126, "VEC2", 34962);
        }
        if (mesh.skinned()) {
            bool second = false;
            for (auto& w : mesh.weights)
                for (int k = 4; k < 8; k++)
                    second |= w[k] > 0;
            for (int set = 0; set < (second ? 2 : 1); set++) {
                std::vector<std::array<uint16_t, 4>> joints(mesh.positions.size());
                std::vector<F4> weights(mesh.positions.size());
                for (size_t j = 0; j < mesh.positions.size(); j++)
                    for (int k = 0; k < 4; k++) {
                        joints[j][k] = mesh.joints[j][set * 4 + k];
                        weights[j][k] = mesh.weights[j][set * 4 + k];
                    }
                attrs["JOINTS_" + std::to_string(set)] = b.acc(joints, 4, 5123, "VEC4", 34962);
                attrs["WEIGHTS_" + std::to_string(set)] = b.acc(weights, 4, 5126, "VEC4", 34962);
            }
            std::vector<std::array<float, 16>> ibm(mesh.inverseBind.size());
            for (size_t i = 0; i < ibm.size(); i++)
                for (int k = 0; k < 16; k++)
                    ibm[i][k] = float(mesh.inverseBind[i].data()[k]);
            if (!d.contains("skins"))
                d["skins"] = Json::array();
            d["nodes"][mesh.node]["skin"] = d["skins"].size();
            d["skins"].push_back({{"name", mesh.name + " / skin"},
                                  {"joints", mesh.skinNodes},
                                  {"inverseBindMatrices", b.acc(ibm, 16, 5126, "MAT4")}});
        }
        if (!mesh.selectors.empty() && decals.contains(mesh.material)) {
            numberCount++;
            std::vector<std::vector<uint32_t>> parts(mesh.numbers.size());
            for (size_t i = 0; i < mesh.indices.size(); i += 3) {
                int selector = mesh.selectors[mesh.indices[i]];
                require(mesh.selectors[mesh.indices[i + 1]] == selector &&
                            mesh.selectors[mesh.indices[i + 2]] == selector,
                        "Number selector changes inside a triangle");
                parts[selector].insert(parts[selector].end(), mesh.indices.begin() + i,
                                       mesh.indices.begin() + i + 3);
            }
            for (size_t ci = 0; ci < parts.size(); ci++) {
                if (parts[ci].empty())
                    continue;
                auto& control = mesh.numbers[ci];
                std::set<int> handles;
                for (int a : {control.u, control.v})
                    if (a != -1 && std::binary_search(arguments.begin(), arguments.end(), a))
                        handles.insert(a);
                std::map<int, std::vector<double>> grids;
                size_t combinations = 1;
                for (int a : handles) {
                    auto [lo, hi] = scene.limits.at(a);
                    require(hi - lo <= 100, "Number animation range exceeds portable limit");
                    std::set<double> grid{lo, hi, argValue(baseline, a)};
                    for (int v = int(std::ceil(lo * 10)); v <= int(std::floor(hi * 10)); v++)
                        grid.insert(v / 10.);
                    grids[a] = {grid.begin(), grid.end()};
                    combinations *= grid.size();
                    require(combinations <= 1024, "Number atlas has too many animation states");
                }
                std::vector<int> active(handles.begin(), handles.end());
                Args state;
                std::function<void(size_t)> variant = [&](size_t i) {
                    if (i < active.size()) {
                        int a = active[i];
                        for (double value : grids[a]) {
                            state[a] = value;
                            variant(i + 1);
                        }
                        return;
                    }
                    Args config = baseline;
                    for (auto [a, v] : state)
                        config[a] = v;
                    auto attributes = attrs;
                    attributes["TEXCOORD_2"] = b.acc(textureUV(mesh, mat, 3, config), 2, 5126, "VEC2", 34962);
                    if (!attributes.contains("TEXCOORD_0"))
                        attributes["TEXCOORD_0"] = b.acc(textureUV(mesh, mat, 0), 2, 5126, "VEC2", 34962);
                    if (!attributes.contains("TEXCOORD_1"))
                        attributes["TEXCOORD_1"] = attributes["TEXCOORD_0"];
                    auto positions = mesh.positions;
                    for (size_t k = 0; k < positions.size(); k++)
                        for (int j = 0; j < 3; j++)
                            positions[k][j] += mesh.normals[k][j] * .0002f;
                    attributes["POSITION"] = b.acc(positions, 3, 5126, "VEC3", 34962, true);
                    Json primitive = {{"attributes", attributes},
                                      {"indices", b.acc(parts[ci], 1, 5125, "SCALAR", 34963)},
                                      {"material", decals.at(mesh.material)},
                                      {"mode", 4}};
                    std::string name =
                        mesh.name + " / number " + std::to_string(ci) + " / " + argsJson(state).dump();
                    int ni = int(d["nodes"].size());
                    bool visible = true;
                    for (auto [a, v] : state)
                        visible &= v == argValue(baseline, a);
                    d["nodes"].push_back({{"name", name},
                                          {"mesh", d["meshes"].size()},
                                          {"scale", visible ? Json{1., 1., 1.} : Json{0., 0., 0.}}});
                    auto& parent = d["nodes"][mesh.node];
                    if (!parent.contains("children"))
                        parent["children"] = Json::array();
                    parent["children"].push_back(ni);
                    d["meshes"].push_back({{"name", name}, {"primitives", Json::array({primitive})}});
                    if (!active.empty())
                        variants.push_back({ni, state, grids});
                };
                variant(0);
            }
            continue;
        }
        d["nodes"][mesh.node]["mesh"] = d["meshes"].size();
        d["meshes"].push_back(
            {{"name", mesh.name},
             {"primitives", Json::array({{{"attributes", attrs},
                                          {"indices", b.acc(mesh.indices, 1, 5125, "SCALAR", 34963)},
                                          {"material", mesh.material},
                                          {"mode", 4}}})}});
        if (progress && mi % 100 == 0)
            progress("导出网格 " + std::to_string(mi + 1) + " / " + std::to_string(scene.meshes.size()));
    }
    d["animations"] = Json::array();
    std::vector<int> exported;
    for (int arg : arguments) {
        check();
        bool number =
            std::find(scene.numberArgs.begin(), scene.numberArgs.end(), arg) != scene.numberArgs.end();
        if (number &&
            std::none_of(variants.begin(), variants.end(), [&](auto& v) { return v.state.contains(arg); }) &&
            std::none_of(scene.tracks.begin(), scene.tracks.end(), [&](auto& t) { return t.arg == arg; }))
            continue;
        auto [lo, hi] = scene.limits.at(arg);
        double span = hi - lo;
        auto others = baseline;
        others.erase(arg);
        std::ostringstream name;
        name << "Argument " << std::setw(3) << std::setfill('0') << arg;
        Json clip = {
            {"name", name.str()},
            {"samplers", Json::array()},
            {"channels", Json::array()},
            {"extras",
             {{"edm_argument", arg},
              {"argument_min", lo},
              {"argument_max", hi},
              {"duration_seconds", options.duration},
              {"mapping",
               "argument = argument_min + (time / duration_seconds) * (argument_max - argument_min)"},
              {"other_arguments", argsJson(others)}}}};
        auto channel = [&](int node, Channel path, const std::set<double>& points, bool step,
                           const std::function<V4(double)>& sample) {
            std::vector<float> times, values;
            std::vector<double> samples(points.begin(), points.end());
            if (!span)
                samples = {lo, lo};
            int components = path == Channel::Rotation ? 4 : 3;
            V4 previous = V4::Zero();
            for (size_t i = 0; i < samples.size(); i++) {
                float time =
                    span ? float((samples[i] - lo) / span * options.duration) : float(i * options.duration);
                V4 v = sample(samples[i]);
                if (path == Channel::Rotation) {
                    v = qnormalize(v);
                    if (i && v.dot(previous) < 0)
                        v = -v;
                    previous = v;
                }
                if (!times.empty() && time == times.back()) {
                    times.pop_back();
                    values.resize(values.size() - components);
                }
                times.push_back(time);
                for (int k = 0; k < components; k++)
                    values.push_back(float(v[k]));
            }
            clip["channels"].push_back({{"sampler", clip["samplers"].size()},
                                        {"target", {{"node", node}, {"path", channelName(path)}}}});
            clip["samplers"].push_back(
                {{"input", b.acc(times, 1, 5126, "SCALAR", 0, true)},
                 {"output", b.acc(values, components, 5126, components == 4 ? "VEC4" : "VEC3")},
                 {"interpolation", step ? "STEP" : "LINEAR"}});
        };
        for (auto& t : scene.tracks) {
            std::set<double> points{lo, hi};
            if (t.arg == arg) {
                if (t.visibility) {
                    for (auto [a, z] : t.ranges)
                        for (double v : {a, z})
                            if (v > lo && v < hi)
                                points.insert(v);
                } else
                    for (auto& k : t.keys)
                        if (k.time > lo && k.time < hi)
                            points.insert(k.time);
            }
            channel(t.node, t.channel, points, t.visibility,
                    [&](double v) { return t.sample(t.arg == arg ? v : argValue(baseline, t.arg)); });
        }
        for (auto& v : variants) {
            std::set<double> points{lo, hi};
            if (v.grids.contains(arg))
                for (double point : v.grids.at(arg))
                    if (point > lo && point < hi)
                        points.insert(point);
            channel(v.node, Channel::Scale, points, true, [&](double point) {
                bool visible = true;
                for (auto [a, target] : v.state) {
                    double value = a == arg ? point : argValue(baseline, a);
                    auto& grid = v.grids.at(a);
                    auto it = std::upper_bound(grid.begin(), grid.end(), value);
                    double chosen = it == grid.begin() ? grid.front() : *std::prev(it);
                    visible &= chosen == target;
                }
                return V4(visible, visible, visible, 0);
            });
        }
        if (!clip["channels"].empty()) {
            d["animations"].push_back(clip);
            exported.push_back(arg);
        }
        if (progress)
            progress("导出动画参数 " + std::to_string(arg));
    }
    if (d["animations"].empty())
        d.erase("animations");
    Json report = scene.summary();
    std::vector<int> skipped;
    for (int a : arguments)
        if (std::find(exported.begin(), exported.end(), a) == exported.end())
            skipped.push_back(a);
    report.update({{"exported_arguments", exported},
                   {"requested_arguments", arguments},
                   {"skipped_arguments", skipped},
                   {"clip_duration_seconds", options.duration},
                   {"embedded_diffuse_materials", diffuseCount},
                   {"embedded_roughmet_materials", roughmetCount},
                   {"number_meshes", numberCount},
                   {"number_animation_nodes", variants.size()},
                   {"baseline_arguments", argsJson(baseline)},
                   {"missing_textures", resolver ? Json(resolver->missing) : Json::array()},
                   {"texture_warnings", resolver ? Json(resolver->warnings) : Json::array()},
                   {"resolved_textures", resolver ? resolver->resolved : Json::object()},
                   {"livery", options.livery ? options.livery->metadata() : Json()},
                   {"visibility_encoding", "STEP scale 0/1, lower-inclusive upper-exclusive"},
                   {"skinning", "packed uint8 indices + 1; residual weight to bone 0"},
                   {"material_limitations",
                    "ORM mapped to glTF PBR; number atlas uses alpha cutout and discrete 0.1 steps. DCS "
                    "shader effects, animated material uniforms and normal maps are not reproduced."},
                   {"export_seconds", seconds(start)}});
    auto extra = report;
    extra.erase("source");
    d["extras"] = {{"edm_studio", extra}};
    d["buffers"] = Json::array({{{"byteLength", b.data.size()}}});
    check();
    return {std::move(d), std::move(b.data), std::move(report)};
}
Json exportScene(const Scene& scene, const fs::path& path, const ExportOptions& options, Progress progress,
                 const std::atomic_bool* cancel) {
    auto extension = lower(pathString(path.extension()));
    if (extension == ".obj")
        return exportObjScene(scene, path, options, std::move(progress), cancel);
    if (extension == ".fbx")
        return exportFbxScene(scene, path, options, std::move(progress), cancel);
    require(extension == ".glb" || extension == ".gltf", "Export format must be .glb, .gltf, .obj or .fbx");
    auto payload = buildExportPayload(scene, options, std::move(progress), cancel);
    auto& d = payload.document;
    auto& data = payload.buffer;
    std::vector<uint8_t> output;
    if (extension == ".gltf") {
        d["buffers"][0]["uri"] = "data:application/octet-stream;base64," + base64(data);
        auto text = d.dump();
        output.assign(text.begin(), text.end());
    } else {
        auto json = d.dump();
        while (json.size() % 4)
            json.push_back(' ');
        while (data.size() % 4)
            data.push_back(0);
        size_t length = 28 + json.size() + data.size();
        require(length <= 0xffffffff, "GLB exceeds 4 GB format limit");
        output.reserve(length);
        appendU32(output, 0x46546c67);
        appendU32(output, 2);
        appendU32(output, uint32_t(length));
        appendU32(output, uint32_t(json.size()));
        appendU32(output, 0x4e4f534a);
        output.insert(output.end(), json.begin(), json.end());
        appendU32(output, uint32_t(data.size()));
        appendU32(output, 0x004e4942);
        output.insert(output.end(), data.begin(), data.end());
    }
    writeFile(path, output);
    auto reportPath = path;
    reportPath.replace_extension(L".report.json");
    writeJson(reportPath, payload.report);
    return payload.report;
}
} // namespace edm
