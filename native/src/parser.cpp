#include "model.h"
namespace edm {
namespace {
class Parser {
    Document d;
    size_t pos = 0;
    std::vector<std::string> strings;
    Progress progress;
    const std::atomic_bool* cancel;
    template <class T> T get() {
        auto bytes = take(sizeof(T));
        T x;
        std::memcpy(&x, bytes.data(), sizeof(T));
        return x;
    }
    std::span<const uint8_t> take(size_t n) {
        require(pos <= d.file->size && n <= d.file->size - pos,
                "Truncated EDM at byte " + std::to_string(pos));
        auto b = std::span(d.file->data + pos, n);
        pos += n;
        return b;
    }
    uint32_t count(size_t minimum = 1) {
        auto n = get<uint32_t>();
        require(n <= 100000000 && n <= (d.file->size - pos) / minimum,
                "Invalid EDM count at byte " + std::to_string(pos));
        return n;
    }
    void check() {
        if (cancel && *cancel)
            throw std::runtime_error("Cancelled");
    }
    std::string literal() {
        auto b = take(count());
        // Newer DCS assets also use UTF-8 in EDM 8 literals. Keep valid UTF-8
        // intact, with Windows-1251 as the fallback for legacy model strings.
        return decodeText(b);
    }
    std::string str() {
        if (d.version == 8)
            return literal();
        auto i = get<uint32_t>();
        require(i < strings.size(), "EDM string index out of range");
        return strings[i];
    }
    std::string type() {
        auto t = str();
        require(t.starts_with("model::"), "Invalid EDM type " + t);
        return t.substr(7);
    }
    template <int N> Eigen::Matrix<double, N, 1> vec() {
        Eigen::Matrix<double, N, 1> v;
        for (int i = 0; i < N; i++)
            v[i] = get<double>();
        return v;
    }
    Mat matrix(bool f = false) {
        Mat m;
        for (int i = 0; i < 16; i++)
            m.data()[i] = f ? get<float>() : get<double>();
        require(m.allFinite(), "Nonfinite EDM matrix");
        return m;
    }
    Json value(int n) {
        if (n == 1)
            return get<float>();
        Json j = Json::array();
        for (int i = 0; i < n; i++)
            j.push_back(get<float>());
        return j;
    }
    Json props() {
        Json j = Json::object();
        for (auto n = count(8); n--;) {
            auto t = type(), name = str();
            if (t == "ArgumentProperty") {
                j[name] = {{"name", name}, {"argument", get<uint32_t>()}};
                continue;
            }
            bool anim = t.starts_with("AnimatedProperty");
            int dim = t.find("Vec2f") != std::string::npos   ? 2
                      : t.find("Vec3f") != std::string::npos ? 3
                      : t.find("Vec4f") != std::string::npos ? 4
                                                             : 1;
            if (anim) {
                Json p = {{"name", name}, {"argument", get<uint32_t>()}, {"keys", Json::array()}};
                for (auto k = count(12); k--;) {
                    double time = get<double>();
                    p["keys"].push_back({{"frame", time}, {"value", value(dim)}});
                }
                j[name] = std::move(p);
            } else if (t == "Property<unsigned int>")
                j[name] = get<uint32_t>();
            else if (t == "Property<const char*>")
                j[name] = str();
            else if (t == "Property<float>" || t == "Property<osg::Vec2f>" || t == "Property<osg::Vec3f>" ||
                     t == "Property<osg::Vec4f>")
                j[name] = value(dim);
            else
                throw std::runtime_error("Unsupported property " + t);
        }
        return j;
    }
    template <class T> void base(T& n) {
        n.name = literal();
        n.version = get<uint32_t>();
        n.props = props();
    }
    std::vector<int> format() {
        std::vector<int> f;
        for (auto n = count(); n--;)
            f.push_back(get<uint8_t>());
        return f;
    }
    Material material() {
        Material m;
        for (auto n = count(4); n--;) {
            auto k = str();
            if (k == "BLENDING")
                m.blending = get<uint8_t>();
            else if (k == "CULLING")
                m.culling = get<uint8_t>();
            else if (k == "DECAL")
                m.decal = get<uint8_t>();
            else if (k == "VERTEX_FORMAT")
                m.format = format();
            else if (k == "TEXTURE_COORDINATES_CHANNELS") {
                for (auto c = count(4); c--;)
                    m.uvChannels.push_back(get<uint32_t>());
            } else if (k == "NAME")
                m.name = str();
            else if (k == "MATERIAL_NAME")
                m.shader = str();
            else if (k == "UNIFORMS")
                m.uniforms = props();
            else if (k == "ANIMATED_UNIFORMS")
                m.animatedUniforms = props();
            else if (k == "TEXTURES") {
                for (auto c = count(88); c--;) {
                    TextureRef t;
                    t.slot = get<uint32_t>();
                    get<int32_t>();
                    t.name = str();
                    take(16);
                    t.matrix = matrix(true);
                    m.textures.push_back(std::move(t));
                }
            } else if (k == "DEPTH_BIAS")
                m.extras[k] = get<uint32_t>();
            else if (k == "DAMAGE_TEXTURE_OFFSET" || k == "Z_OFFSET")
                m.extras[k] = get<float>();
            else if (k == "SHADOWS" || k == "FLAT_COLOR_RENDERING" || k == "HAS_ALPHA_CHANNEL" ||
                     k == "DAMAGE_REQUIRED" || k == "NIGHT_LIGHTING_ALPHA" || k == "LIGHT_MAP")
                m.extras[k] = get<uint8_t>();
            else
                throw std::runtime_error("Unsupported material field " + k);
        }
        return m;
    }
    std::vector<Key> keys(int dim) {
        std::vector<Key> out;
        auto n = count(8 + dim * 8);
        out.reserve(n);
        while (n--) {
            Key k;
            k.time = get<double>();
            for (int i = 0; i < dim; i++)
                k.value[i] = get<double>();
            require(std::isfinite(k.time) && k.value.allFinite(), "Nonfinite animation key");
            out.push_back(k);
        }
        return out;
    }
    std::vector<ArgKeys> arguments(int dim, bool scale = false) {
        std::vector<ArgKeys> out;
        for (auto n = count(8); n--;) {
            ArgKeys a;
            a.arg = int(get<uint32_t>());
            a.keys = keys(dim);
            if (scale)
                a.second = keys(3);
            out.push_back(std::move(a));
        }
        return out;
    }
    RawNode node(std::string t) {
        RawNode n;
        n.type = t;
        base(n);
        if (t == "Node") {
        } else if (t == "TransformNode")
            n.matrix = matrix();
        else if (t == "Bone") {
            n.matrix = matrix();
            n.matrix2 = matrix();
            n.hasBind = true;
        } else if (n.animated()) {
            n.matrix = matrix();
            n.position = vec<3>();
            n.q1 = vec<4>();
            n.q2 = vec<4>();
            n.scale = vec<3>();
            n.positions = arguments(3);
            n.rotations = arguments(4);
            n.scales = arguments(4, true);
            if (t == "ArgAnimatedBone") {
                n.matrix2 = matrix();
                n.hasBind = true;
            }
        } else if (t == "ArgVisibilityNode") {
            int layout = n.props.value("__VERSION__", 0);
            require(layout == 0 || layout == 1, "Unsupported visibility layout " + std::to_string(layout));
            for (auto c = count(8); c--;) {
                Visibility v;
                v.arg = int(get<uint32_t>());
                for (auto r = count(16); r--;) {
                    double lo = get<double>(), hi = get<double>();
                    require(std::isfinite(lo) && std::isfinite(hi) && lo <= hi,
                            "Invalid visibility interval");
                    v.ranges.emplace_back(lo, hi);
                }
                n.visibility.push_back(std::move(v));
            }
            if (layout)
                n.matrix = matrix(true);
        } else if (t == "LodNode") {
            n.extras["levels"] = Json::array();
            for (auto c = count(16); c--;) {
                double lo = get<double>(), hi = get<double>();
                n.extras["levels"].push_back({std::sqrt(std::max(0., lo)), std::sqrt(std::max(0., hi))});
            }
        } else if (t == "BillboardNode")
            take(154);
        else if (t == "Connector") {
            n.parent = get<uint32_t>();
            get<uint32_t>();
        } else if (t == "LightNode") {
            n.parent = get<uint32_t>();
            get<uint8_t>();
            n.extras = props();
            get<uint8_t>();
        } else if (t == "FakeOmniLightsNode") {
            take(20);
            take(size_t(count(48)) * 48);
        } else if (t == "FakeSpotLightsNode") {
            int layout = n.props.value("__VERSION__", 0);
            require(layout >= 0 && layout <= 2, "Unsupported spot-light layout");
            take(8);
            take(size_t(count(20)) * 20);
            take(size_t(count(layout == 2 ? 77 : 65)) * (layout == 2 ? 77 : 65));
        } else if (t == "FakeALSNode") {
            take(12);
            take(size_t(count(80)) * 80);
        } else if (t == "SegmentsNode") {
            take(4);
            take(size_t(count(24)) * 24);
        } else
            throw std::runtime_error("Unsupported scene node " + t + " at " + std::to_string(pos));
        return n;
    }
    void geometry(RawRender& r) {
        r.vertexCount = count();
        r.stride = get<uint32_t>();
        require(r.stride <= 128 && (!r.vertexCount || r.stride >= 3), "Invalid vertex stride");
        r.vertices = take(size_t(r.vertexCount) * r.stride * 4);
        auto fmt = get<uint8_t>();
        auto n = count();
        get<uint32_t>();
        if (!n)
            return;
        require(fmt <= 2, "Unsupported EDM index format");
        require(n <= (d.file->size - pos) / (1u << fmt), "Truncated indices");
        r.indices.resize(n);
        if (fmt == 2) {
            auto b = take(size_t(n) * 4);
            std::memcpy(r.indices.data(), b.data(), b.size());
        } else
            for (auto& i : r.indices)
                i = fmt == 1 ? get<uint16_t>() : get<uint8_t>();
    }
    RawRender render(std::string t) {
        RawRender r;
        r.type = t;
        base(r);
        if (t == "ShellNode") {
            get<uint32_t>();
            format();
            geometry(r);
            return r;
        }
        get<uint32_t>();
        r.material = get<uint32_t>();
        if (t == "SkinNode") {
            for (auto n = count(4); n--;)
                r.bones.push_back(get<uint32_t>());
            get<uint32_t>();
        } else {
            auto n = count(8);
            for (auto c = n; c--;) {
                Parent p;
                p.node = get<uint32_t>();
                if (n > 1)
                    p.start = get<int32_t>();
                p.damage = get<int32_t>();
                r.parents.push_back(p);
            }
        }
        geometry(r);
        if (t == "NumberNode") {
            for (auto n = count(16); n--;) {
                NumberControl c;
                c.u = get<int32_t>();
                c.su = get<float>();
                c.v = int(get<uint32_t>());
                c.sv = get<float>();
                r.numbers.push_back(c);
            }
        }
        return r;
    }

  public:
    Parser(const fs::path& path, Progress p, const std::atomic_bool* c) : progress(p), cancel(c) {
        d.source = path;
        d.file = std::make_shared<MappedFile>(path);
    }
    Document run() {
        auto magic = take(3);
        require(std::memcmp(magic.data(), "EDM", 3) == 0, "Not an EDM file");
        d.version = get<uint16_t>();
        require(d.version == 8 || d.version == 10, "Unsupported EDM version " + std::to_string(d.version));
        if (d.version == 10) {
            auto b = take(count());
            size_t start = 0;
            for (size_t i = 0; i <= b.size(); i++)
                if (i == b.size() || b[i] == 0) {
                    strings.push_back(decodeText(b.subspan(start, i - start)));
                    start = i + 1;
                }
        }
        for (int k = 0; k < 2; k++)
            for (auto n = count(8); n--;) {
                str();
                get<uint32_t>();
            }
        require(type() == "RootNode", "EDM root missing");
        RawNode root;
        base(root);
        d.rootProps = root.props;
        if (d.version == 8)
            get<uint8_t>();
        take(18 * 8);
        for (auto n = count(4); n--;)
            d.materials.push_back(material());
        take(8);
        auto n = count(12);
        d.nodes.reserve(n);
        while (n--) {
            check();
            auto t = type();
            d.nodes.push_back(node(t));
        }
        for (auto& a : d.nodes)
            a.parent = get<int32_t>();
        d.extraItems = Json::object();
        for (auto cat = count(8); cat--;) {
            auto name = str();
            auto c = count(12);
            d.extraItems[name] = c;
            while (c--) {
                check();
                auto t = type();
                if (name == "RENDER_NODES")
                    d.renderTypes[t] = d.renderTypes.value(t, 0) + 1;
                if (t == "RenderNode" || t == "SkinNode" || t == "NumberNode" || t == "ShellNode") {
                    auto r = render(t);
                    if (name == "RENDER_NODES")
                        d.renders.push_back(std::move(r));
                    else if (name == "SHELL_NODES")
                        d.collisionCount++;
                } else {
                    auto a = node(t);
                    if (name == "CONNECTORS")
                        d.connectors.push_back(std::move(a));
                    else if (name == "LIGHT_NODES" || name == "RENDER_NODES")
                        d.lightCount++;
                }
            }
            if (progress)
                progress("读取 " + name);
        }
        require(pos == d.file->size, "Unconsumed EDM bytes: " + std::to_string(d.file->size - pos));
        std::vector<uint8_t> state(d.nodes.size());
        std::function<void(int)> visit = [&](int i) {
            require(i >= 0 && i < int(d.nodes.size()), "Invalid EDM parent index");
            require(state[i] != 1, "Cycle in EDM hierarchy");
            if (state[i] == 2)
                return;
            state[i] = 1;
            int p = d.nodes[i].parent;
            require(p >= -1, "Invalid negative EDM parent");
            if (p >= 0)
                visit(p);
            state[i] = 2;
        };
        for (int i = 0; i < int(d.nodes.size()); i++)
            visit(i);
        return std::move(d);
    }
};
} // namespace
Document parseEdm(const fs::path& path, Progress progress, const std::atomic_bool* cancel) {
    return Parser(path, progress, cancel).run();
}
} // namespace edm
