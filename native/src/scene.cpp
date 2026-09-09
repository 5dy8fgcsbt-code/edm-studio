#include "model.h"
#include <Eigen/SVD>
namespace edm {
V4 Track::sample(double v) const {
    if (visibility) {
        bool yes = false;
        for (auto [lo, hi] : ranges)
            yes |= lo <= v && v < hi;
        return V4(yes, yes, yes, 0);
    }
    require(!keys.empty(), "Empty track");
    auto it =
        std::upper_bound(keys.begin(), keys.end(), v, [](double x, const Key& k) { return x < k.time; });
    size_t i = it - keys.begin();
    const auto& a = keys[i ? i - 1 : 0];
    const auto& b = keys[std::min(i, keys.size() - 1)];
    double t = a.time == b.time ? 0 : std::clamp((v - a.time) / (b.time - a.time), 0., 1.);
    V4 out;
    if (channel == Channel::Rotation)
        out = slerp(a.value, b.value, t);
    else
        out = a.value * (1 - t) + b.value * t;
    return conjugate ? qinverse(out) : out;
}
std::pair<double, double> Track::domain() const {
    if (!visibility)
        return {keys.front().time, keys.back().time};
    double lo = 0, hi = 1;
    for (auto [a, b] : ranges)
        for (double x : {a, b})
            if (std::isfinite(x) && std::abs(x) <= 1e4) {
                lo = std::min(lo, x);
                hi = std::max(hi, x);
            }
    return {lo, hi};
}
namespace {
struct Build {
    Scene& s;
    Document& d;
    Progress progress;
    const std::atomic_bool* cancel;
    std::map<int, std::vector<Visibility>> visibility;
    std::map<std::pair<int, std::vector<int>>, int> gates;
    std::map<std::vector<int>, std::map<int, int>> clones;
    std::map<int, Track> trackMap;
    int add(std::string name, int parent, Json extras = Json::object()) {
        Node n;
        n.name = std::move(name);
        n.parent = parent;
        n.extras = std::move(extras);
        s.nodes.push_back(std::move(n));
        return int(s.nodes.size() - 1);
    }
    int factor(int p, std::string name, Channel c, const V4& v) {
        int i = add(name, p);
        set(s.nodes[i], c, v);
        return i;
    }
    static void set(Node& n, Channel c, const V4& v) {
        require(v.allFinite(), "Nonfinite transform");
        if (c == Channel::Position)
            n.t = v.head<3>();
        else if (c == Channel::Scale)
            n.s = v.head<3>();
        else
            n.q = qnormalize(v);
    }
    static V4 v4(const V3& v) {
        return {v[0], v[1], v[2], 0};
    }
    int affine(int p, const std::string& name, const Mat& m) {
        require(m.allFinite() && m.row(3).head<3>().cwiseAbs().maxCoeff() < 1e-7 &&
                    std::abs(m(3, 3) - 1) < 1.01e-5,
                "Invalid affine matrix: " + name);
        if ((m - Mat::Identity()).cwiseAbs().maxCoeff() < 1e-12)
            return p;
        Eigen::JacobiSVD<Eigen::Matrix3d> svd(m.block<3, 3>(0, 0), Eigen::ComputeFullU | Eigen::ComputeFullV);
        Eigen::Matrix3d u = svd.matrixU(), vt = svd.matrixV().transpose();
        V3 scale = svd.singularValues();
        if (u.determinant() < 0) {
            u.col(2) *= -1;
            scale[2] *= -1;
        }
        if (vt.determinant() < 0) {
            vt.row(2) *= -1;
            scale[2] *= -1;
        }
        int n = add(name, p);
        s.nodes[n].t = m.block<3, 1>(0, 3);
        s.nodes[n].q = quatMatrix(u);
        s.nodes[n].s = scale;
        return factor(n, name + " / basis", Channel::Rotation, quatMatrix(vt));
    }
    int animated(int p, const std::string& name, Channel c, int arg, const std::vector<Key>& keys,
                 bool inverse = false) {
        if (keys.empty())
            return p;
        for (size_t i = 0; i < keys.size(); i++)
            require(keys[i].value.allFinite() && std::isfinite(keys[i].time) &&
                        (!i || keys[i].time > keys[i - 1].time),
                    "Animation times must be finite and strictly increasing");
        int n = add(name, p, {{"edm_argument", arg}, {"edm_channel", channelName(c)}});
        Track t;
        t.node = n;
        t.arg = arg;
        t.channel = c;
        t.keys = keys;
        t.conjugate = inverse;
        set(s.nodes[n], c, t.sample(0));
        s.tracks.push_back(t);
        return n;
    }
    void nodes() {
        add(pathString(s.source.stem()), -1,
            {{"edm_version", s.version}, {"units", "metres"}, {"up_axis", "Y"}});
        for (int i = 0; i < int(d.nodes.size()); i++) {
            auto& n = d.nodes[i];
            auto name = n.name.empty() ? n.type + " " + std::to_string(i) : n.name;
            int h = add(name, 0, {{"edm_node", i}, {"edm_type", n.type}, {"edm_properties", n.props}}), p = h;
            if (n.animated()) {
                p = affine(p, name + " / matrix", n.matrix);
                p = factor(p, name + " / position", Channel::Position, v4(n.position));
                for (auto& a : n.positions)
                    p = animated(p, name + " / position " + std::to_string(a.arg), Channel::Position, a.arg,
                                 a.keys);
                p = factor(p, name + " / Q1", Channel::Rotation, n.q1);
                for (auto it = n.rotations.rbegin(); it != n.rotations.rend(); ++it)
                    p = animated(p, name + " / rotation " + std::to_string(it->arg), Channel::Rotation,
                                 it->arg, it->keys);
                p = factor(p, name + " / inverse scale basis", Channel::Rotation, qinverse(n.q2));
                p = factor(p, name + " / base scale", Channel::Scale, v4(n.scale));
                p = factor(p, name + " / scale basis", Channel::Rotation, n.q2);
                for (auto it = n.scales.rbegin(); it != n.scales.rend(); ++it) {
                    require(!it->second.empty(), "Missing scale vector keys");
                    auto suffix = std::to_string(it->arg);
                    p = animated(p, name + " / inverse scale basis " + suffix, Channel::Rotation, it->arg,
                                 it->keys, true);
                    p = animated(p, name + " / scale " + suffix, Channel::Scale, it->arg, it->second);
                    p = animated(p, name + " / scale basis " + suffix, Channel::Rotation, it->arg, it->keys);
                }
            } else if (n.type == "TransformNode" || n.type == "Bone")
                p = affine(p, name + " / matrix", n.matrix);
            else if (n.type == "ArgVisibilityNode") {
                visibility[i] = n.visibility;
                s.nodes[h].extras["edm_visibility_matrix"] = matJson(n.matrix);
                require((n.matrix - Mat::Identity()).cwiseAbs().maxCoeff() < 1e-7,
                        "Non-identity visibility extension matrix is not yet verified");
            } else if (n.type == "LodNode") {
                s.nodes[h].extras["edm_lod_levels"] = n.extras["levels"];
                s.warn("内部 LOD 保留所有层级；可在目标软件中筛选。");
            } else if (n.type == "BillboardNode")
                s.warn("Billboard 随相机朝向的行为未导出。");
            s.heads.push_back(h);
            s.tails.push_back(p);
        }
        for (int i = 0; i < int(d.nodes.size()); i++)
            s.nodes[s.heads[i]].parent = d.nodes[i].parent >= 0 ? s.tails[d.nodes[i].parent] : 0;
        for (auto& t : s.tracks)
            trackMap[t.node] = t;
    }
    std::vector<int> conditions(int n) {
        std::vector<int> out;
        while (n >= 0) {
            require(n < int(d.nodes.size()), "Invalid render parent/bone");
            if (visibility.contains(n))
                out.push_back(n);
            n = d.nodes[n].parent;
        }
        std::sort(out.begin(), out.end());
        return out;
    }
    int gate(int parent, const std::vector<int>& cond) {
        if (cond.empty())
            return parent;
        auto key = std::make_pair(parent, cond);
        if (gates.contains(key))
            return gates[key];
        int p = parent;
        for (int id : cond)
            for (auto& v : visibility[id]) {
                int n = add("Visibility " + std::to_string(id) + " / " + std::to_string(v.arg), p,
                            {{"edm_visibility_ranges", v.ranges}, {"edm_argument", v.arg}});
                Track t;
                t.node = n;
                t.arg = v.arg;
                t.channel = Channel::Scale;
                t.visibility = true;
                t.ranges = v.ranges;
                set(s.nodes[n], Channel::Scale, t.sample(0));
                s.tracks.push_back(t);
                p = n;
            }
        return gates[key] = p;
    }
    int skinJoint(int node, const std::vector<int>& cond) {
        if (cond.empty())
            return node;
        auto& map = clones[cond];
        if (map.contains(node))
            return map[node];
        if (node == 0)
            return gate(0, cond);
        int parent = skinJoint(s.nodes[node].parent, cond);
        Node copy = s.nodes[node];
        copy.name += " / skin visibility";
        copy.parent = parent;
        int idx = int(s.nodes.size());
        s.nodes.push_back(std::move(copy));
        if (trackMap.contains(node)) {
            auto t = trackMap[node];
            t.node = idx;
            s.tracks.push_back(t);
        }
        return map[node] = idx;
    }
    static V3 point(const F3& p) {
        return V3(p[0], p[1], p[2]);
    }
    void mesh(const RawRender& r, const Material& mat, int ri, int pi, int parts, const Parent* parent,
              const std::vector<uint32_t>& ids) {
        if (ids.empty())
            return;
        Mesh m;
        m.name = r.name.empty() ? r.type + " " + std::to_string(ri) : r.name;
        if (parts > 1)
            m.name += " / " + std::to_string(pi);
        m.material = r.material;
        m.numbers = r.numbers;
        int p = parent ? parent->node : -1;
        require(p >= -1 && p < int(s.tails.size()), "Invalid mesh parent");
        m.extras = {{"edm_render_index", ri},
                    {"edm_type", r.type},
                    {"edm_parent", p},
                    {"edm_damage_argument", parent ? parent->damage : -1},
                    {"edm_properties", r.props}};
        if (r.type == "ShellNode") {
            m.extras["edm_is_collision"] = true;
            m.extras["edm_shell_name"] = r.name;
            m.extras["edm_shell_index"] = ri;
            m.extras["edm_shell_vertex_format"] = r.shellFormat;
        }
        if (!m.numbers.empty()) {
            Json controls = Json::array();
            for (auto& c : m.numbers) {
                require(std::isfinite(c.su) && std::isfinite(c.sv), "Invalid number multiplier");
                controls.push_back({c.u, c.su, c.v, c.sv});
            }
            m.extras["edm_properties"]["number_controls"] = controls;
        }
        bool skin = r.type == "SkinNode";
        if (skin)
            require(!r.bones.empty(), "Missing skin bones");
        auto cond = conditions(skin ? r.bones[0] : p);
        m.node = add(m.name, skin ? -1 : gate(p >= 0 ? s.tails[p] : 0, cond), m.extras);
        std::vector<uint32_t> used = ids;
        std::sort(used.begin(), used.end());
        used.erase(std::unique(used.begin(), used.end()), used.end());
        std::vector<uint32_t> remap(r.vertexCount);
        for (uint32_t i = 0; i < used.size(); i++)
            remap[used[i]] = i;
        m.indices.reserve(ids.size());
        for (auto id : ids)
            m.indices.push_back(remap[id]);
        int po = mat.offset(0), no = mat.offset(1), wo = mat.offset(21);
        std::vector<int> uvOff;
        for (int c = 4; c < 9; c++)
            if (mat.channel(c) >= 2)
                uvOff.push_back(mat.offset(c));
        m.uvs.resize(uvOff.size());
        for (auto& u : m.uvs)
            u.reserve(used.size());
        m.positions.reserve(used.size());
        m.normals.reserve(used.size());
        for (uint32_t id : used) {
            F3 p3{r.value(id, po), r.value(id, po + 1), r.value(id, po + 2)};
            require(point(p3).allFinite(), "Nonfinite mesh position");
            m.positions.push_back(p3);
            V3 n(0, 1, 0);
            if (no >= 0 && mat.channel(1) >= 3) {
                n = {r.value(id, no), r.value(id, no + 1), r.value(id, no + 2)};
                if (!n.allFinite() || n.norm() < 1e-10)
                    n = {0, 1, 0};
                else
                    n.normalize();
            }
            m.normals.push_back({float(n[0]), float(n[1]), float(n[2])});
            for (size_t j = 0; j < uvOff.size(); j++) {
                F2 uv{r.value(id, uvOff[j]), r.value(id, uvOff[j] + 1)};
                require(std::isfinite(uv[0]) && std::isfinite(uv[1]), "Nonfinite UV");
                m.uvs[j].push_back(uv);
            }
            if (r.type == "NumberNode" && wo >= 0 && !r.numbers.empty()) {
                float selector = r.value(id, wo);
                require(std::isfinite(selector) && selector >= 0 && selector < r.numbers.size() &&
                            selector == std::floor(selector),
                        "Invalid number control selector");
                m.selectors.push_back(int(selector));
            }
            if (skin) {
                require(mat.channel(0) == 4 && mat.channel(21) == 4, "Unsupported skin layout");
                std::array<uint16_t, 8> joints{};
                std::array<float, 8> weights{};
                uint32_t word = r.word(id, po + 3);
                float sum = 0;
                for (int k = 0; k < 4; k++) {
                    weights[k] = r.value(id, wo + k);
                    require(std::isfinite(weights[k]) && weights[k] >= 0, "Invalid skin weight");
                    joints[k] = uint16_t(((word >> (k * 8)) & 255) + 1);
                    if (weights[k] > 0)
                        require(joints[k] < r.bones.size(), "Invalid skin joint index");
                    else
                        joints[k] = 0;
                    sum += weights[k];
                }
                weights[4] = std::max(0.f, 1.f - sum);
                sum += weights[4];
                for (auto& w : weights)
                    w /= sum;
                m.joints.push_back(joints);
                m.weights.push_back(weights);
            }
        }
        if (no < 0 || mat.channel(1) < 3) {
            std::vector<V3> normals(used.size(), V3::Zero());
            for (size_t i = 0; i < m.indices.size(); i += 3) {
                auto a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
                V3 cross = (point(m.positions[b]) - point(m.positions[a]))
                               .cross(point(m.positions[c]) - point(m.positions[a]));
                normals[a] += cross;
                normals[b] += cross;
                normals[c] += cross;
            }
            for (size_t i = 0; i < normals.size(); i++) {
                auto n = normals[i];
                if (n.norm() < 1e-20)
                    n = {0, 1, 0};
                else
                    n.normalize();
                m.normals[i] = {float(n[0]), float(n[1]), float(n[2])};
            }
        }
        double total = 0, evidence = 0, reverse = 0;
        for (size_t i = 0; i < m.indices.size(); i += 3) {
            auto a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
            V3 cross = (point(m.positions[b]) - point(m.positions[a]))
                           .cross(point(m.positions[c]) - point(m.positions[a]));
            double area = cross.norm(),
                   dot = cross.dot((point(m.normals[a]) + point(m.normals[b]) + point(m.normals[c])) / 3);
            total += area;
            if (std::abs(dot) > area * .2) {
                evidence += area;
                if (dot < 0)
                    reverse += area;
            }
        }
        if (total > 0 && evidence > total * .5 && reverse > evidence * .95) {
            for (size_t i = 0; i < m.indices.size(); i += 3)
                std::swap(m.indices[i + 1], m.indices[i + 2]);
            m.extras["edm_winding_reversed"] = true;
        }
        if (skin)
            for (int b : r.bones) {
                require(b >= 0 && b < int(d.nodes.size()) && d.nodes[b].hasBind, "Invalid bone inverse bind");
                m.skinNodes.push_back(skinJoint(s.tails[b], cond));
                m.inverseBind.push_back(d.nodes[b].matrix2);
            }
        s.meshes.push_back(std::move(m));
    }
    void meshes() {
        for (int ri = 0; ri < int(d.renders.size()); ri++) {
            if (cancel && *cancel)
                throw std::runtime_error("Cancelled");
            auto& r = d.renders[ri];
            require(r.material >= 0 && r.material < int(s.materials.size()), "Invalid material index");
            auto& mat = s.materials[r.material];
            if (r.indices.empty() || !r.vertexCount)
                continue;
            require(r.indices.size() % 3 == 0 &&
                        *std::max_element(r.indices.begin(), r.indices.end()) < r.vertexCount,
                    "Invalid triangle indices");
            require(mat.stride() == int(r.stride) && mat.channel(0) >= 3, "Vertex format mismatch");
            if (r.type == "SkinNode" || r.parents.empty())
                mesh(r, mat, ri, 0, 1, nullptr, r.indices);
            else {
                auto n = r.parents.size();
                std::vector<std::vector<uint32_t>> parts(n);
                bool groups = n > 1 && mat.channel(0) == 4;
                bool ends = n > 1 && r.parents.back().start == int(r.indices.size());
                if (groups) {
                    int off = mat.offset(0) + 3;
                    for (size_t i = 0; i < r.indices.size(); i += 3) {
                        float g = r.value(r.indices[i], off);
                        require(std::isfinite(g) && g >= 0 && g < n && g == std::floor(g) &&
                                    r.value(r.indices[i + 1], off) == g &&
                                    r.value(r.indices[i + 2], off) == g,
                                "Invalid per-vertex transform index");
                        parts[int(g)].insert(parts[int(g)].end(), r.indices.begin() + i,
                                             r.indices.begin() + i + 3);
                    }
                } else {
                    require(ends || r.parents[0].start == 0, "Missing leading parent range");
                    for (size_t i = 0; i < n; i++) {
                        int begin = ends ? (i ? r.parents[i - 1].start : 0) : r.parents[i].start;
                        int end = ends ? r.parents[i].start
                                       : (i + 1 < n ? r.parents[i + 1].start : int(r.indices.size()));
                        require(begin >= 0 && end >= begin && end <= int(r.indices.size()) &&
                                    begin % 3 == 0 && end % 3 == 0,
                                "Invalid parent index range");
                        parts[i].assign(r.indices.begin() + begin, r.indices.begin() + end);
                    }
                }
                for (int i = 0; i < int(n); i++)
                    mesh(r, mat, ri, i, int(n), &r.parents[i], parts[i]);
            }
            if (progress && ri % 50 == 0)
                progress("构建网格 " + std::to_string(ri + 1) + " / " + std::to_string(d.renders.size()));
        }
    }
    void collisionMeshes() {
        if (!s.meshes.empty() || d.collisionShells.empty())
            return;
        // A collision-only EDM may still declare unused appearance materials. Its
        // shells have independent layouts and no source livery to paint or resolve.
        s.materials.clear();
        constexpr std::array<F3, 5> colors{{{.48f, .65f, .76f},
                                            {.58f, .72f, .71f},
                                            {.66f, .70f, .79f},
                                            {.69f, .72f, .66f},
                                            {.59f, .67f, .77f}}};
        for (int index = 0; index < int(d.collisionShells.size()); ++index) {
            if (cancel && *cancel)
                throw std::runtime_error("Cancelled");
            auto& shell = d.collisionShells[index];
            if (shell.indices.empty())
                continue;
            Material material;
            material.name = "Collision / " + (shell.name.empty() ? std::to_string(index) : shell.name);
            material.source = s.source;
            material.shader = "edm_collision";
            material.culling = 1; // Double-sided in both the native renderer and glTF exporter.
            material.format = shell.shellFormat;
            material.uniforms = {{"diffuseColor", colors[size_t(index) % colors.size()]},
                                 {"opacityValue", 1.}};
            material.extras = {{"edm_is_collision", true},
                               {"edm_shell_name", shell.name},
                               {"edm_shell_index", index},
                               {"edm_properties", shell.props},
                               {"edm_shell_vertex_format", shell.shellFormat}};
            shell.material = int(s.materials.size());
            s.materials.push_back(std::move(material));
            mesh(shell, s.materials.back(), index, 0, 1, &shell.parents.at(0), shell.indices);
            // Some shells carry extra channels; a collision surface still has no paintable UV map.
            s.meshes.back().uvs.clear();
            if (progress && !(index % 50))
                progress("构建碰撞壳体 " + std::to_string(index + 1) + " / " +
                         std::to_string(d.collisionShells.size()));
        }
    }
    void run() {
        nodes();
        meshes();
        collisionMeshes();
        for (auto& c : d.connectors) {
            require(c.parent >= -1 && c.parent < int(s.tails.size()), "Invalid connector");
            add(c.name.empty() ? "Connector" : c.name, c.parent >= 0 ? s.tails[c.parent] : 0,
                {{"edm_type", "Connector"}, {"edm_properties", c.props}});
        }
        for (auto& t : s.tracks) {
            auto [lo, hi] = t.domain();
            auto it = s.limits.find(t.arg);
            if (it == s.limits.end())
                s.limits[t.arg] = {lo, hi};
            else {
                it->second.first = std::min(lo, it->second.first);
                it->second.second = std::max(hi, it->second.second);
            }
        }
        std::set<int> numbers;
        for (auto& m : s.meshes)
            if (!m.selectors.empty())
                for (auto& c : m.numbers)
                    for (int a : {c.u, c.v})
                        if (a != -1)
                            numbers.insert(a);
        s.numberArgs.assign(numbers.begin(), numbers.end());
        for (int a : numbers)
            if (!s.limits.contains(a))
                s.limits[a] = {0, 1};
        s.staticLocal.reserve(s.nodes.size());
        std::vector<std::vector<int>> children(s.nodes.size());
        std::vector<int> pending;
        for (int i = 0; i < int(s.nodes.size()); i++) {
            s.staticLocal.push_back(s.nodes[i].local());
            int p = s.nodes[i].parent;
            if (p >= 0)
                children[p].push_back(i);
            else
                pending.push_back(i);
        }
        while (!pending.empty()) {
            int i = pending.back();
            pending.pop_back();
            s.order.push_back(i);
            pending.insert(pending.end(), children[i].begin(), children[i].end());
        }
        require(s.order.size() == s.nodes.size(), "Scene graph cycle");
        s.defaultWorld = s.evaluate({});
        require(!s.meshes.empty(), "文件没有可导出的三角网格（外观与碰撞壳体均无三角形）");
        if (d.collisionCount && !s.collisionOnly())
            s.warn("碰撞壳体未加入可见模型。");
        if (d.collisionLineCount)
            s.warn("另有 " + std::to_string(d.collisionLineCount) + " 组碰撞线段，当前未显示或导出。");
        if (d.lightCount)
            s.warn("DCS 专用灯光效果未转换。");
        for (auto& m : s.materials)
            if (!m.animatedUniforms.empty())
                s.warn("材质参数动画以元数据保留；DCS 专用着色器无法完整映射为 glTF PBR。");
    }
};
} // namespace
std::shared_ptr<Scene> Scene::load(const fs::path& path, Progress progress, const std::atomic_bool* cancel) {
    auto t = Clock::now();
    auto d = parseEdm(path, progress, cancel);
    auto s = std::make_shared<Scene>();
    s->parseSeconds = seconds(t);
    t = Clock::now();
    s->source = path;
    s->version = d.version;
    s->sourceNodes = int(d.nodes.size());
    s->materials = std::move(d.materials);
    for (auto& material : s->materials)
        material.source = path;
    s->collisionCount = d.collisionCount;
    s->connectorCount = int(d.connectors.size());
    s->renderTypes = d.renderTypes;
    Build{*s, d, progress, cancel}.run();
    s->buildSeconds = seconds(t);
    return s;
}
std::vector<Mat> Scene::evaluate(const Args& args, bool attachmentsVisible) const {
    auto world = staticLocal;
    for (auto& t : tracks) {
        V4 v = t.sample(args.contains(t.arg) ? args.at(t.arg) : argValue(defaultArgs, t.arg));
        if (t.channel == Channel::Position)
            world[t.node] = translation(v.head<3>());
        else if (t.channel == Channel::Rotation)
            world[t.node] = rotation(v);
        else
            world[t.node] = scaling(v.head<3>());
    }
    if (!attachmentsVisible)
        for (const auto& attachment : attachments)
            world.at(attachment.root) *= scaling(V3::Zero());
    for (int i : order) {
        int p = nodes[i].parent;
        if (p >= 0)
            world[i] = world[p] * world[i];
    }
    return world;
}
std::vector<F3> Scene::transformed(const Mesh& m, const std::vector<Mat>& world) const {
    std::vector<Mat> palette;
    for (size_t i = 0; i < m.skinNodes.size(); i++)
        palette.push_back(world[m.skinNodes[i]] * m.inverseBind[i]);
    std::vector<F3> result;
    result.reserve(m.positions.size());
    for (size_t i = 0; i < m.positions.size(); i++) {
        auto& p = m.positions[i];
        V4 v(p[0], p[1], p[2], 1), out = V4::Zero();
        if (m.skinned()) {
            for (int k = 0; k < 8; k++)
                if (m.weights[i][k])
                    out += palette[m.joints[i][k]] * v * m.weights[i][k];
        } else
            out = world[m.node] * v;
        result.push_back({float(out[0]), float(out[1]), float(out[2])});
    }
    return result;
}
bool Scene::collisionOnly() const {
    return !meshes.empty() && std::all_of(meshes.begin(), meshes.end(), [](const Mesh& mesh) {
        return mesh.extras.is_object() && mesh.extras.value("edm_is_collision", false);
    });
}
Json Scene::summary() const {
    size_t tris = 0, vertices = 0, skins = 0, winding = 0, collisionMeshes = 0, collisionTris = 0;
    for (auto& m : meshes) {
        tris += m.indices.size() / 3;
        vertices += m.positions.size();
        skins += m.skinned();
        winding += m.extras.value("edm_winding_reversed", false);
        if (m.extras.value("edm_is_collision", false)) {
            ++collisionMeshes;
            collisionTris += m.indices.size() / 3;
        }
    }
    Json args = Json::object();
    for (auto [a, r] : limits)
        args[std::to_string(a)] = {r.first, r.second};
    Json attached = Json::array();
    for (const auto& item : attachments) {
        Json mapping = Json::object();
        for (auto [original, remapped] : item.argumentMap)
            mapping[std::to_string(original)] = remapped;
        attached.push_back({{"source", pathString(item.source)},
                            {"target_node", item.targetNode},
                            {"root", item.root},
                            {"attach_node", item.attachNode},
                            {"node_begin", item.nodeBegin},
                            {"node_count", item.nodeCount},
                            {"material_begin", item.materialBegin},
                            {"material_count", item.materialCount},
                            {"mesh_begin", item.meshBegin},
                            {"mesh_count", item.meshCount},
                            {"argument_map", mapping}});
    }
    return {
        {"source", pathString(source)},
        {"edm_version", version},
        {"scene_nodes", sourceNodes},
        {"graph_nodes", nodes.size()},
        {"render_types", renderTypes},
        {"export_meshes", meshes.size()},
        {"triangles", tris},
        {"vertices", vertices},
        {"skinned_meshes", skins},
        {"materials", materials.size()},
        {"arguments", args},
        {"animation_tracks", tracks.size()},
        {"number_arguments", numberArgs},
        {"winding_normalized_meshes", winding},
        {"connectors", connectorCount},
        {"attachments", attached},
        {"collision_nodes", collisionCount},
        {"collision_model", collisionOnly()},
        {"collision_meshes", collisionMeshes},
        {"collision_triangles", collisionTris},
        {"warnings", warnings},
        {"parse_seconds", parseSeconds},
        {"scene_build_seconds", buildSeconds},
        {"animation_formula", "M T(p+sum(pos)) Q1 Rn...R1 Q2^-1 Sbase Q2 Qsn^-1 Sn Qsn ... Qs1^-1 S1 Qs1"}};
}
MaterialAlphaMode materialAlphaMode(const Material& m) {
    // DCS blend modes: NONE, TRANSPARENT, ALPHA_TEST, ADDITIVE, DECAL,
    // DECAL_DEFERRED, SHADOWED_TRANSPARENT. Scalar opacity never changes NONE.
    if (m.blending == 0)
        return MaterialAlphaMode::Opaque;
    if (m.blending == 2)
        return MaterialAlphaMode::Mask;
    if (m.blending >= 1 && m.blending <= 6)
        return MaterialAlphaMode::Blend;
    // Preserve the previous fallback for unrecognized material modes.
    const auto opacity = m.uniforms.find("opacityValue");
    return opacity != m.uniforms.end() && opacity->is_number() && opacity->get<float>() < .999f
               ? MaterialAlphaMode::Blend
               : MaterialAlphaMode::Opaque;
}
F4 materialColor(const Material& m) {
    float opacity = m.uniforms.contains("opacityValue") && m.uniforms["opacityValue"].is_number()
                        ? m.uniforms["opacityValue"].get<float>()
                        : 1;
    F4 c{.66f, .7f, .75f,
         materialAlphaMode(m) == MaterialAlphaMode::Opaque ? 1.f : std::clamp(opacity, 0.f, 1.f)};
    if (m.uniforms.contains("diffuseColor")) {
        auto& v = m.uniforms["diffuseColor"];
        if (v.is_array() && v.size() >= 3)
            for (int i = 0; i < 3; i++)
                c[i] = std::clamp(v[i].get<float>(), 0.f, 1.f);
    }
    return c;
}
std::vector<F2> textureUV(const Mesh& mesh, const Material& mat, int slot, const Args& args) {
    int uvSlot = slot;
    if (slot == 13 && (mat.uvChannels.size() <= 13 || mat.uvChannels[13] == 0xffffffff))
        uvSlot = 2;
    uint32_t ch = uvSlot < int(mat.uvChannels.size()) ? mat.uvChannels[uvSlot] : 0;
    if (ch == 0xffffffff)
        ch = 0;
    std::vector<F2> uv = ch < mesh.uvs.size() ? mesh.uvs[ch]
                         : !mesh.uvs.empty()  ? mesh.uvs[0]
                                              : std::vector<F2>(mesh.positions.size(), F2{});
    auto* ref = mat.texture(slot);
    if (slot == 13 && !ref)
        ref = mat.texture(2);
    auto shift = mat.uniforms.value(slot == 3 ? "decalShift" : "diffuseShift", Json());
    for (size_t i = 0; i < uv.size(); i++) {
        double u = uv[i][0], v = uv[i][1];
        if (ref) {
            u = ref->matrix(0, 0) * uv[i][0] + ref->matrix(0, 1) * uv[i][1] + ref->matrix(0, 3);
            v = ref->matrix(1, 0) * uv[i][0] + ref->matrix(1, 1) * uv[i][1] + ref->matrix(1, 3);
        }
        if (shift.is_array() && shift.size() >= 2) {
            u += shift[0].get<double>();
            v += shift[1].get<double>();
        }
        if (slot == 3 && !mesh.selectors.empty()) {
            auto& c = mesh.numbers[mesh.selectors[i]];
            if (c.u != -1)
                u += argValue(args, c.u) * c.su;
            if (c.v != -1)
                v += argValue(args, c.v) * c.sv;
        }
        uv[i] = {float(u), float(v)};
    }
    return uv;
}
std::vector<int> bortMapping(const Scene& s) {
    for (auto order : std::vector<std::vector<int>>{{442, 31, 32}, {443, 444, 445}, {30, 31, 32}, {31, 32}}) {
        bool all = true;
        for (int a : order)
            all &= std::find(s.numberArgs.begin(), s.numberArgs.end(), a) != s.numberArgs.end();
        if (all)
            return order;
    }
    return {};
}
Args bortArguments(const Scene& s, const std::string& text) {
    auto order = bortMapping(s);
    require(!order.empty(), "模型没有已识别的快捷编号布局，请使用编号参数调节。");
    require(!text.empty() && text.size() <= order.size() &&
                std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }),
            "请输入正确位数的数字");
    std::string digits(order.size() - text.size(), '0');
    digits += text;
    Args args;
    for (size_t i = 0; i < order.size(); i++)
        args[order[i]] = (digits[i] - '0') / 10.;
    return args;
}
} // namespace edm
