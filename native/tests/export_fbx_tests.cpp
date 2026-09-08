#include "export.h"
#include "paint.h"
#include <ufbx.h>
#include <iostream>
#include <iomanip>
#include <sstream>

using namespace edm;
namespace {
size_t checks = 0;
double maximumError = 0;
double argumentAtFraction(double lo, double hi, double fraction) {
    // Endpoint arithmetic such as -1.01 + (1.5 - -1.01) lands one ULP below 1.5.
    // The source/export contract samples exact endpoints, especially for upper-exclusive visibility.
    return fraction == 0. ? lo : fraction == 1. ? hi : lo + (hi - lo) * fraction;
}
void check(bool value, const std::string& message) {
    ++checks;
    require(value, "FBX regression: " + message);
}
template <class F> void fails(F action, const std::string& message) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, message);
}
std::string string(ufbx_string value) {
    return {value.data, value.length};
}
std::string errorText(const ufbx_error& error) {
    char text[4096]{};
    ufbx_format_error(text, sizeof(text), &error);
    return text;
}
using Imported = std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)>;
Imported load(const fs::path& path) {
    ufbx_load_opts options{};
    options.strict = true;
    options.force_single_thread_ascii_parsing = true;
    options.index_error_handling = UFBX_INDEX_ERROR_HANDLING_ABORT_LOADING;
    options.evaluate_skinning = true;
    options.target_unit_meters = 1;
    ufbx_error error{};
    Imported scene(ufbx_load_file(pathString(path).c_str(), &options, &error), ufbx_free_scene);
    check(bool(scene), "strict ufbx load: " + errorText(error));
    check(scene->metadata.warnings.count == 0, "independent importer reports no malformed-file warnings");
    return scene;
}
V4 quaternion(double x, double y, double z) {
    constexpr double radians = 3.14159265358979323846 / 180;
    Eigen::Quaterniond q = Eigen::AngleAxisd(z * radians, V3::UnitZ()) *
                           Eigen::AngleAxisd(y * radians, V3::UnitY()) *
                           Eigen::AngleAxisd(x * radians, V3::UnitX());
    return V4(q.x(), q.y(), q.z(), q.w());
}
int node(Scene& scene, const std::string& name, int parent = -1, V3 t = V3::Zero(), V4 q = V4(0, 0, 0, 1),
         V3 s = V3::Ones()) {
    Node value;
    value.name = name;
    value.parent = parent;
    value.t = t;
    value.q = q;
    value.s = s;
    scene.nodes.push_back(value);
    return int(scene.nodes.size() - 1);
}
void track(Scene& scene, int target, int argument, Channel channel, std::vector<Key> keys) {
    Track value;
    value.node = target;
    value.arg = argument;
    value.channel = channel;
    value.keys = std::move(keys);
    scene.tracks.push_back(value);
    scene.limits[argument] = value.domain();
}
void finish(Scene& scene) {
    for (const auto& track : scene.tracks) {
        auto value = track.sample(0);
        auto& node = scene.nodes[track.node];
        if (track.channel == Channel::Position)
            node.t = value.head<3>();
        else if (track.channel == Channel::Scale)
            node.s = value.head<3>();
        else
            node.q = value;
    }
    scene.order.clear();
    scene.staticLocal.clear();
    for (size_t index = 0; index < scene.nodes.size(); ++index) {
        require(scene.nodes[index].parent < int(index), "Fixture hierarchy must be topological");
        scene.order.push_back(int(index));
        scene.staticLocal.push_back(scene.nodes[index].local());
    }
    scene.defaultWorld = scene.evaluate({});
    scene.sourceNodes = int(scene.nodes.size());
}
void material(Scene& scene) {
    Material value;
    value.name = "Fixture coating";
    value.shader = "fixture";
    value.format = {3, 3, 2, 2};
    value.uvChannels = {1};
    TextureRef diffuse{0, "missing-default-diffuse"};
    diffuse.matrix(0, 0) = .7;
    diffuse.matrix(1, 1) = .8;
    diffuse.matrix(0, 3) = .12;
    diffuse.matrix(1, 3) = -.08;
    value.textures.push_back(diffuse);
    scene.materials.push_back(value);
}
Mesh quad(const std::string& name, int target) {
    Mesh mesh;
    mesh.name = name;
    mesh.node = target;
    mesh.positions = {{-.9f, -.3f, .2f}, {1.2f, -.5f, .1f}, {.7f, 1.1f, -.2f}, {-.4f, .8f, .4f}};
    mesh.normals.assign(4, F3{0, 0, 1});
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.uvs = {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}, {{.05f, .1f}, {.8f, .2f}, {.9f, .85f}, {.15f, .95f}}};
    mesh.extras = Json::object();
    return mesh;
}
Scene rigidFixture(const fs::path& root) {
    Scene scene;
    scene.source = root / "rigid-fixture.edm";
    scene.version = 10;
    material(scene);
    const int base = node(scene, "offset root", -1, V3(4, -2, 3), quaternion(13, -7, 21), V3(1.2, .8, 1.1));
    const int slide = node(scene, "slide", base);
    track(scene, slide, 0, Channel::Position,
          {{-1, V4(-2, 1, .5, 0)}, {0, V4(0, 0, 0, 0)}, {1, V4(3, -.4, 1, 0)}});
    const int crossing = node(scene, "crossing 180 degrees", slide);
    track(scene, crossing, 1, Channel::Rotation, {{0, quaternion(0, 170, 0)}, {1, quaternion(0, 190, 0)}});
    const int compound = node(scene, "compound quaternion", crossing);
    track(scene, compound, 2, Channel::Rotation,
          {{0, quaternion(-70, 78, 100)}, {.37, quaternion(30, 91, -15)}, {1, quaternion(40, 112, -30)}});
    const int mirror =
        node(scene, "negative scale", compound, V3::Zero(), V4(0, 0, 0, 1), V3(-1.25, .6, 1.4));
    const int surface = node(scene, "Rigid surface", mirror, V3(.3, -.7, 2.1), quaternion(10, 20, 30));
    scene.meshes.push_back(quad("Rigid surface", surface));
    const int stationary = node(scene, "Stationary reference", base, V3(-3, 1, 0), quaternion(20, -10, 3));
    scene.meshes.push_back(quad("Stationary reference", stationary));
    const int gateOffset = node(scene, "gate offset", base, V3(2, 4, -1));
    const int gate = node(scene, "visibility gate", gateOffset);
    Track visibility;
    visibility.node = gate;
    visibility.arg = 7;
    visibility.channel = Channel::Scale;
    visibility.visibility = true;
    visibility.ranges = {{.125, .25}, {.375, .375 + 1.0 / 524288}, {.75, .875}};
    scene.tracks.push_back(visibility);
    scene.limits[7] = {0, 1};
    const int visible = node(scene, "Visibility surface", gate, V3(.4, .8, -.3));
    scene.meshes.push_back(quad("Visibility surface", visible));
    const int scaleOffset = node(scene, "animated scale offset", base, V3(-1, -3, 2));
    const int scaled = node(scene, "Animated mirror", scaleOffset);
    track(scene, scaled, 3, Channel::Scale, {{0, V4(-1.1, .8, 1, 0)}, {1, V4(-.7, 1.4, .2, 0)}});
    scene.meshes.push_back(quad("Animated mirror", scaled));
    finish(scene);
    return scene;
}
Scene skinFixture(const fs::path& root, bool hiddenAncestor = false) {
    Scene scene;
    scene.source = root / (hiddenAncestor ? "hidden-skin-fixture.edm" : "skin-fixture.edm");
    scene.version = 10;
    material(scene);
    const int base = node(scene, "skin root", -1, V3(-4, 2, 1), quaternion(10, 15, 20));
    const int skeletonParent = hiddenAncestor ? node(scene, "Visibility2509 / 23", base) : base;
    if (hiddenAncestor) {
        Track visibility;
        visibility.node = skeletonParent;
        visibility.arg = 23;
        visibility.channel = Channel::Scale;
        visibility.visibility = true;
        visibility.ranges = {{.5, 1.01}};
        scene.tracks.push_back(visibility);
        scene.limits[23] = {0, 1};
    }
    std::vector<int> bones;
    for (int index = 0; index < 8; ++index) {
        const int offset =
            node(scene, "bone offset " + std::to_string(index), skeletonParent,
                 V3(index * .3, -.2 * index, .1 * index), quaternion(index * 3, index * -2, 5));
        const int bone = node(scene, "Bone " + std::to_string(index), offset);
        if (index % 2 == 0)
            track(scene, bone, 4, Channel::Rotation,
                  {{-1, quaternion(7 * index, -13, 5)}, {1, quaternion(30 + 9 * index, 25, -17)}});
        else
            track(scene, bone, 5, Channel::Position,
                  {{0, V4(.1 * index, -.2, .3, 0)}, {1, V4(-.15 * index, .5, -.1, 0)}});
        bones.push_back(bone);
    }
    const int meshNode =
        node(scene, "Eight weight skin", base, V3(7, -3, 2), quaternion(27, -18, 13), V3(1.1, .8, 1.3));
    finish(scene);
    auto mesh = quad("Eight weight skin", meshNode);
    mesh.positions.push_back(mesh.positions[0]);
    mesh.positions.push_back({-.5f, -.1f, 1.2f});
    mesh.normals.resize(6, {0, 0, 1});
    for (auto& uv : mesh.uvs) {
        uv.push_back(uv[0]);
        uv.push_back({.4f, .7f});
    }
    mesh.indices = {0, 1, 2, 4, 2, 3, 3, 5, 1};
    mesh.skinNodes = bones;
    // The true skin bind is visible. The exported baseline deliberately hides its
    // ancestor, reproducing Blender dropping a zero-length helper rest bone.
    const auto bind = scene.evaluate({{4, -.15}, {5, .7}, {23, 1}});
    for (int bone : bones)
        mesh.inverseBind.push_back(bind[bone].inverse() * bind[meshNode]);
    mesh.joints.resize(mesh.positions.size());
    mesh.weights.resize(mesh.positions.size());
    for (size_t vertex = 0; vertex < mesh.positions.size(); ++vertex)
        for (size_t weight = 0; weight < 8; ++weight) {
            mesh.joints[vertex][weight] = uint16_t(weight);
            mesh.weights[vertex][weight] = .125f;
        }
    mesh.weights[4].fill(0);
    mesh.weights[4][7] = 1;
    mesh.weights[1].fill(0);
    mesh.weights[1][0] = .25f;
    mesh.weights[1][6] = .75f;
    scene.meshes.push_back(std::move(mesh));
    return scene;
}
Args baseline(const ExportOptions& options) {
    auto values = options.livery ? options.livery->args : Args{};
    for (auto [key, value] : options.baseline)
        values[key] = value;
    return values;
}
ufbx_node* importedMesh(const Scene& source, const Mesh& mesh, const ufbx_scene& scene) {
    const auto expected = source.nodes[mesh.node].name + " / geometry 0";
    for (auto* node : scene.nodes)
        if (node->mesh && (string(node->name) == expected || string(node->name) == mesh.name))
            return node;
    throw std::runtime_error("FBX geometry node missing: " + expected);
}
double compareGeometry(const Scene& source, const ufbx_scene& scene, const Args& arguments,
                       const std::string& label, double tolerance = 2e-5) {
    const auto world = source.evaluate(arguments);
    double error = 0;
    for (const auto& mesh : source.meshes) {
        auto* node = importedMesh(source, mesh, scene);
        const auto& imported = *node->mesh;
        check(imported.num_indices == mesh.indices.size(), label + ": polygon corner count");
        const auto expected = source.transformed(mesh, world);
        const auto& positions =
            imported.skinned_position.exists ? imported.skinned_position : imported.vertex_position;
        require(positions.indices.count == imported.num_indices, "ufbx position corner bounds");
        for (size_t corner = 0; corner < mesh.indices.size(); ++corner) {
            auto actual = ufbx_get_vertex_vec3(&positions, corner);
            if (!imported.skinned_position.exists || imported.skinned_is_local)
                actual = ufbx_transform_position(&node->geometry_to_world, actual);
            const auto& value = expected[mesh.indices[corner]];
            const double delta = (V3(actual.x, actual.y, actual.z) - V3(value[0], value[1], value[2])).norm();
            error = std::max(error, delta);
            if (!(delta <= tolerance)) {
                std::ostringstream detail;
                detail << std::setprecision(12) << label << ": " << mesh.name << " corner " << corner
                       << " world error " << delta << " m; expected " << value[0] << ',' << value[1] << ','
                       << value[2] << " actual " << actual.x << ',' << actual.y << ',' << actual.z;
                check(false, detail.str());
            }
        }
        check(true, label + ": transformed world vertices match the EDM evaluator");
    }
    maximumError = std::max(maximumError, error);
    return error;
}
std::vector<double> sampleValues(const Scene& scene, int argument) {
    const auto [lo, hi] = scene.limits.at(argument);
    std::set<double> points;
    for (int sample = 0; sample <= 73; ++sample)
        points.insert(lo + (hi - lo) * sample / 73.0);
    for (const auto& track : scene.tracks)
        if (track.arg == argument) {
            for (const auto& key : track.keys)
                points.insert(key.time);
            for (auto [a, b] : track.ranges)
                for (double edge : {a, b})
                    for (double offset : {-(b - a) / 8, 0.0, (b - a) / 8})
                        if (edge + offset >= lo && edge + offset <= hi)
                            points.insert(edge + offset);
        }
    return {points.begin(), points.end()};
}
double representableTime(double seconds) {
    // FBX KTime has integer ticks. Query the representable boundary rather than
    // a sub-tick time just before it, while keeping the source argument/pose exact.
    constexpr double ticksPerSecond = 46186158000.0;
    const double result = double(std::llround(seconds * ticksPerSecond)) / ticksPerSecond;
    check(std::abs(result - seconds) <= .5 / ticksPerSecond + 2e-15,
          "FBX sampling time differs by at most half an integer KTime tick");
    return result;
}
void writeReference(const Scene& source, const fs::path& path, const ExportOptions& options) {
    Json reference = {{"source", pathString(source.source)},
                      {"fbx", pathString(path)},
                      {"duration_seconds", options.duration},
                      {"tolerance_meters", 2e-5},
                      {"samples", Json::array()}};
    auto append = [&](const std::string& clip, double time, const Args& arguments) {
        Json row = {{"clip", clip},
                    {"source_time_seconds", time},
                    {"time_seconds", representableTime(time)},
                    {"meshes", Json::array()}};
        const auto world = source.evaluate(arguments);
        for (const auto& mesh : source.meshes) {
            const auto vertices = source.transformed(mesh, world);
            Json corners = Json::array();
            for (auto index : mesh.indices)
                corners.push_back(Json::array({vertices[index][0], vertices[index][1], vertices[index][2]}));
            row["meshes"].push_back({{"node", source.nodes[mesh.node].name + " / geometry 0"},
                                     {"world_corners", std::move(corners)}});
        }
        reference["samples"].push_back(std::move(row));
    };
    const auto base = baseline(options);
    append("", 0, base);
    std::set<int> arguments;
    if (options.arguments)
        arguments.insert(options.arguments->begin(), options.arguments->end());
    else
        for (const auto& [argument, limits] : source.limits)
            arguments.insert(argument);
    for (int argument : arguments) {
        std::ostringstream name;
        name << "Argument " << std::setw(3) << std::setfill('0') << argument;
        const auto [lo, hi] = source.limits.at(argument);
        for (double value : sampleValues(source, argument)) {
            auto expected = base;
            expected[argument] = value;
            append(name.str(), hi == lo ? 0 : (value - lo) / (hi - lo) * options.duration, expected);
        }
    }
    auto target = path;
    target.replace_extension(".reference.json");
    writeJson(target, reference);
}
void validate(const Scene& source, const fs::path& path, const ExportOptions& options) {
    writeReference(source, path, options);
    auto scene = load(path);
    const auto originalBaseline = baseline(options);
    compareGeometry(source, *scene, originalBaseline, pathString(path.filename()) + " baseline");
    std::set<int> arguments;
    if (options.arguments)
        arguments.insert(options.arguments->begin(), options.arguments->end());
    else
        for (const auto& [argument, range] : source.limits)
            arguments.insert(argument);
    check(scene->anim_stacks.count == arguments.size(), "all/selected/static animation stack count");
    for (int argument : arguments) {
        std::ostringstream name;
        name << "Argument " << std::setw(3) << std::setfill('0') << argument;
        auto* stack = ufbx_find_anim_stack(scene.get(), name.str().c_str());
        check(stack && stack->anim, "independent argument animation stack exists");
        check(std::abs(stack->time_begin) < 1e-9 && std::abs(stack->time_end - options.duration) < 1e-9,
              "animation stack uses the requested exact seconds duration");
        const auto [lo, hi] = source.limits.at(argument);
        for (double value : sampleValues(source, argument)) {
            const double time = representableTime(hi == lo ? 0 : (value - lo) / (hi - lo) * options.duration);
            ufbx_evaluate_opts evaluate{};
            evaluate.evaluate_skinning = true;
            ufbx_error error{};
            Imported pose(ufbx_evaluate_scene(scene.get(), stack->anim, time, &evaluate, &error),
                          ufbx_free_scene);
            check(bool(pose), "ufbx scene evaluation: " + errorText(error));
            auto expected = originalBaseline;
            expected[argument] = value;
            compareGeometry(source, *pose, expected, name.str() + " at " + std::to_string(time));
        }
    }
}
std::vector<uint8_t> embedded(const ufbx_texture& texture, const fs::path& directory) {
    ufbx_blob content = texture.content;
    if (!content.size && texture.video)
        content = texture.video->content;
    if (content.size) {
        auto* first = static_cast<const uint8_t*>(content.data);
        return {first, first + content.size};
    }
    const auto relative = fs::path(wide(string(texture.relative_filename)));
    require(!relative.empty() && !relative.is_absolute(), "Exported texture must have bounded relative path");
    const auto path = fs::weakly_canonical(directory / relative);
    require(within(path, directory), "Exported texture path escapes the fixture directory");
    return readFile(path);
}
void numberTests(const fs::path& directory) {
    Scene source;
    source.version = 10;
    source.source = directory / "number-fixture.edm";
    material(source);
    source.materials[0].uvChannels.resize(4, 0);
    source.materials[0].textures.push_back({3, "fixture-number-atlas"});
    source.numberArgs = {32, 33};
    source.limits = {{32, {0, 1}}, {33, {0, 1}}};
    const int parent = node(source, "number root", -1, V3(2.7, -.8, 1.3), quaternion(12, -19, 31));
    const int target = node(source, "Number plate", parent, V3(.4, .7, -.3), quaternion(9, 3, -5));
    auto plate = quad("Number plate", target);
    for (size_t vertex = 0; vertex < 4; ++vertex) {
        auto position = plate.positions[vertex];
        position[0] += 3;
        plate.positions.push_back(position);
        plate.normals.push_back(plate.normals[vertex]);
    }
    plate.indices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    for (auto& set : plate.uvs) {
        set = {{.01f, .01f}, {.09f, .01f}, {.09f, .09f}, {.01f, .09f}};
        auto second = set;
        set.insert(set.end(), second.begin(), second.end());
    }
    plate.selectors = {0, 0, 0, 0, 1, 1, 1, 1};
    plate.numbers = {{32, -1, 1, 0}, {-1, 33, 0, 1}};
    plate.extras = {{"edm_type", "NumberNode"}};
    source.meshes.push_back(plate);
    finish(source);
    PaintImage atlas(20, 20, {0, 0, 0, 0});
    for (uint32_t y = 0; y < 20; ++y)
        for (uint32_t x = 0; x < 20; ++x) {
            const size_t pixel = (size_t(y) * 20 + x) * 4;
            atlas.rgba[pixel] = uint8_t(20 + x * 10);
            atlas.rgba[pixel + 1] = uint8_t(20 + y * 10);
            atlas.rgba[pixel + 2] = 130;
            atlas.rgba[pixel + 3] = (x + y) % 2 ? 255 : 0;
        }
    const auto atlasBytes = atlas.pngBytes();
    writeFile(directory / "fixture-number-atlas.png", atlasBytes);
    ExportOptions options;
    options.duration = 2.5;
    options.textureDirectory = directory;
    options.baseline = {{32, .425}, {33, .65}};
    options.diffuseOverrides[0] = PaintImage(4, 4, {62, 127, 200, 255}).pngBytes();
    const auto numberWorld = source.evaluate(options.baseline);
    auto raised = plate;
    for (size_t vertex = 0; vertex < raised.positions.size(); ++vertex)
        for (int axis = 0; axis < 3; ++axis)
            raised.positions[vertex][axis] += raised.normals[vertex][axis] * .0002f;
    const auto raisedWorld = source.transformed(raised, numberWorld);
    const V4 collapsed = numberWorld[target] * V4(0, 0, 0, 1);
    for (bool animated : {false, true}) {
        auto config = options;
        if (!animated)
            config.arguments = std::vector<int>{};
        const auto path = directory / (animated ? "numbers-animated.fbx" : "numbers-static.fbx");
        exportScene(source, path, config);
        auto imported = load(path);
        check(imported->anim_stacks.count == (animated ? 2 : 0),
              "NumberNode exports independent selector clips or only the requested static baseline");
        check(imported->meshes.count == (animated ? 24 : 2),
              "each number selector has a static variant or eleven digit states plus its exact custom "
              "baseline");
        bool diffusePreserved = false, atlasPreserved = false;
        for (auto* texture : imported->textures) {
            if (texture->type != UFBX_TEXTURE_FILE)
                continue;
            const auto bytes = embedded(*texture, directory);
            diffusePreserved |= bytes == options.diffuseOverrides[0];
            atlasPreserved |= bytes == atlasBytes;
        }
        for (auto* video : imported->videos) {
            const auto& original = options.diffuseOverrides[0];
            diffusePreserved |= video->content.size == original.size() &&
                                std::memcmp(video->content.data, original.data(), original.size()) == 0;
        }
        check(diffusePreserved && atlasPreserved,
              "number atlas and edited base texture resources are both retained");
        Json reference = {{"fbx", pathString(path)},
                          {"duration_seconds", config.duration},
                          {"tolerance_meters", 2e-5},
                          {"samples", Json::array()}};
        auto validateNumberPose = [&](const ufbx_scene& pose, int activeArgument, double value,
                                      double originalTime) {
            Args desired = options.baseline;
            if (activeArgument >= 0) {
                // Portable registration clips switch through digit states; a custom baseline is
                // also a state so opening/exporting a fractional selector remains lossless.
                std::set<double> grid{options.baseline.at(activeArgument)};
                for (int digit = 0; digit <= 10; ++digit)
                    grid.insert(digit / 10.0);
                auto upper = grid.upper_bound(value);
                desired[activeArgument] = upper == grid.begin() ? *upper : *std::prev(upper);
            }
            std::array<int, 2> visible{};
            std::string clip;
            if (activeArgument >= 0) {
                std::ostringstream stream;
                stream << "Argument " << std::setw(3) << std::setfill('0') << activeArgument;
                clip = stream.str();
            }
            Json row = {{"clip", clip},
                        {"source_time_seconds", originalTime},
                        {"time_seconds", representableTime(originalTime)},
                        {"meshes", Json::array()}};
            for (auto* item : pose.nodes) {
                if (!item->mesh)
                    continue;
                const auto name = string(item->name);
                const auto numberStart = name.find(" / number ");
                require(numberStart != std::string::npos, "Unexpected geometry in number fixture");
                const int selector = std::stoi(name.substr(numberStart + 10));
                require(selector >= 0 && selector < 2, "Number fixture selector bounds");
                const auto begin = name.find('{'), end = name.rfind('}');
                require(begin != std::string::npos && end != std::string::npos, "Number variant state name");
                const auto state = Json::parse(name.substr(begin, end - begin + 1));
                bool shouldShow = true;
                for (auto binding = state.begin(); binding != state.end(); ++binding)
                    shouldShow &= binding.value().get<double>() == desired.at(std::stoi(binding.key()));
                const auto* mesh = item->mesh;
                require(mesh->num_indices == 6 && item->materials.count != 0,
                        "Number geometry/material structure");
                auto* texture = item->materials.data[0]->fbx.diffuse_color.texture;
                if (!texture)
                    texture = item->materials.data[0]->pbr.base_color.texture;
                check(texture && string(texture->uv_set) == "UV2",
                      "registration atlas explicitly selects UV2");
                const ufbx_uv_set* selected = nullptr;
                for (const auto& set : mesh->uv_sets)
                    if (string(set.name) == "UV2")
                        selected = &set;
                check(selected && mesh->vertex_uv.exists,
                      "registration has an active and named atlas UV stream");
                Args variant = options.baseline;
                for (auto binding = state.begin(); binding != state.end(); ++binding)
                    variant[std::stoi(binding.key())] = binding.value().get<double>();
                const auto expectedUv = textureUV(plate, source.materials[0], 3, variant);
                Json expectedCorners = Json::array();
                std::array<V3, 3> triangle;
                for (size_t corner = 0; corner < 6; ++corner) {
                    const auto actual = ufbx_transform_position(
                        &item->geometry_to_world, ufbx_get_vertex_vec3(&mesh->vertex_position, corner));
                    const auto index = plate.indices[size_t(selector) * 6 + corner];
                    const V3 expected =
                        shouldShow ? V3(raisedWorld[index][0], raisedWorld[index][1], raisedWorld[index][2])
                                   : V3(collapsed[0], collapsed[1], collapsed[2]);
                    const double delta = (V3(actual.x, actual.y, actual.z) - expected).norm();
                    maximumError = std::max(maximumError, delta);
                    check(delta < 2e-5,
                          "NumberNode STEP variant geometry and exact baseline match expected state");
                    expectedCorners.push_back(Json::array({expected.x(), expected.y(), expected.z()}));
                    if (corner < 3)
                        triangle[corner] = V3(actual.x, actual.y, actual.z);
                    const auto uv = ufbx_get_vertex_vec2(&selected->vertex_uv, corner);
                    const auto active = ufbx_get_vertex_vec2(&mesh->vertex_uv, corner);
                    check(std::abs(uv.x - expectedUv[index][0]) < 1e-6 &&
                              std::abs(uv.y - (1 - expectedUv[index][1])) < 1e-6 &&
                              std::abs(active.x - uv.x) < 1e-9 && std::abs(active.y - uv.y) < 1e-9,
                          "UV2 atlas state is also the default active UV stream for Blender");
                }
                visible[size_t(selector)] +=
                    (triangle[1] - triangle[0]).cross(triangle[2] - triangle[0]).norm() > 1e-8;
                row["meshes"].push_back({{"node", name}, {"world_corners", std::move(expectedCorners)}});
            }
            check(visible[0] == 1 && visible[1] == 1,
                  "exactly one registration variant per selector remains visible");
            reference["samples"].push_back(std::move(row));
        };
        validateNumberPose(*imported, -1, 0, 0);
        if (animated)
            for (int argument : {32, 33}) {
                const auto name = std::string("Argument 0") + std::to_string(argument);
                auto* stack = ufbx_find_anim_stack(imported.get(), name.c_str());
                check(stack && std::abs(stack->time_end - config.duration) < 1e-9,
                      "number selector animation stack retains exact requested duration");
                std::set<double> values{0, 1, options.baseline.at(argument)};
                for (int digit = 1; digit < 10; ++digit)
                    for (double offset : {-1e-6, 0.0, 1e-6})
                        values.insert(digit / 10.0 + offset);
                values.insert(options.baseline.at(argument) - 1e-6);
                values.insert(options.baseline.at(argument) + 1e-6);
                for (double value : values) {
                    ufbx_evaluate_opts evaluate{};
                    evaluate.evaluate_skinning = true;
                    ufbx_error error{};
                    Imported pose(ufbx_evaluate_scene(imported.get(), stack->anim,
                                                      representableTime(value * config.duration), &evaluate,
                                                      &error),
                                  ufbx_free_scene);
                    check(bool(pose), "evaluate dynamic number selector: " + errorText(error));
                    validateNumberPose(*pose, argument, value, value * config.duration);
                }
            }
        auto referencePath = path;
        referencePath.replace_extension(".reference.json");
        writeJson(referencePath, reference);
    }
}
void endpointVisibilityTests(const fs::path& directory) {
    constexpr double lo = -1.01, hi = 1.5;
    check(argumentAtFraction(lo, hi, 0.) == lo && argumentAtFraction(lo, hi, 1.) == hi,
          "fraction endpoint mapping preserves the exact [-1.01, 1.5] argument limits");
    Scene source;
    source.source = directory / "endpoint-visibility-fixture.edm";
    source.version = 10;
    material(source);
    const int gate = node(source, "Endpoint visibility");
    Track visible;
    visible.node = gate;
    visible.arg = 114;
    visible.channel = Channel::Scale;
    visible.visibility = true;
    visible.ranges = {{lo, hi}};
    source.tracks.push_back(visible);
    source.limits[114] = {lo, hi};
    source.meshes.push_back(quad("Endpoint visibility", gate));
    finish(source);
    const auto before =
        source.transformed(source.meshes[0], source.evaluate({{114, std::nextafter(hi, lo)}}));
    const auto at = source.transformed(source.meshes[0], source.evaluate({{114, hi}}));
    check(before[0] != at[0] && at[0] == F3{0, 0, 0},
          "upper-exclusive visibility distinguishes exact high from the preceding double");
    ExportOptions options;
    options.textures = false;
    options.arguments = std::vector<int>{114};
    auto path = directory / "endpoint-visibility.fbx";
    exportScene(source, path, options);
    auto scene = load(path);
    auto* stack = ufbx_find_anim_stack(scene.get(), "Argument 114");
    check(stack && stack->anim, "endpoint visibility animation stack survives export");
    for (double fraction : {0., .5, 1.}) {
        ufbx_evaluate_opts evaluate{};
        evaluate.evaluate_skinning = true;
        ufbx_error error{};
        Imported pose(ufbx_evaluate_scene(scene.get(), stack->anim,
                                          representableTime(options.duration * fraction), &evaluate, &error),
                      ufbx_free_scene);
        check(bool(pose), "evaluate exact endpoint visibility pose: " + errorText(error));
        compareGeometry(source, *pose, {{114, argumentAtFraction(lo, hi, fraction)}},
                        "exact endpoint visibility fraction " + std::to_string(fraction));
    }
}
void verifyModel(const fs::path& edmPath, const fs::path& fbxPath, const fs::path& reportPath) {
    const auto report = Json::parse(readFile(reportPath));
    require(report.value("number_animation_nodes", size_t(0)) == 0 &&
                report.value("number_meshes", size_t(0)) == 0,
            "Real model verification requires an FBX exported with --no-textures (number variants excluded)");
    const auto source = Scene::load(edmPath);
    auto imported = load(fbxPath);
    Args base;
    for (auto it = report.at("baseline_arguments").begin(); it != report.at("baseline_arguments").end(); ++it)
        base[std::stoi(it.key())] = it.value().get<double>();
    const auto arguments = report.at("exported_arguments").get<std::vector<int>>();
    const double duration = report.at("clip_duration_seconds").get<double>();
    require(std::isfinite(duration) && duration > 0, "Invalid real-model report duration");
    check(imported->anim_stacks.count == arguments.size(), "real-model exported animation stack count");
    // The payload keeps original nodes first and appends geometry children. Following
    // each parent's source child order disambiguates repeated EDM node names.
    std::vector<std::vector<int>> children(source->nodes.size() + 1);
    for (size_t index = 0; index < source->nodes.size(); ++index) {
        const int parent = source->nodes[index].parent;
        children[parent < 0 ? source->nodes.size() : size_t(parent)].push_back(int(index));
    }
    std::vector<ufbx_node*> mapped(source->nodes.size(), nullptr);
    std::vector<std::pair<size_t, ufbx_node*>> queue{{source->nodes.size(), imported->root_node}};
    for (size_t cursor = 0; cursor < queue.size(); ++cursor) {
        auto [index, parent] = queue[cursor];
        std::vector<ufbx_node*> descendants;
        for (auto* child : parent->children)
            if (!child->mesh)
                descendants.push_back(child);
        check(descendants.size() == children[index].size(), "real-model source hierarchy child count");
        for (size_t offset = 0; offset < descendants.size(); ++offset) {
            const auto sourceChild = children[index][offset];
            auto* child = descendants[offset];
            check(string(child->name) == source->nodes[sourceChild].name,
                  "real-model child order/name identity at source node " + std::to_string(sourceChild));
            mapped[sourceChild] = child;
            queue.push_back({size_t(sourceChild), child});
        }
    }
    std::vector<uint32_t> geometry;
    for (const auto& mesh : source->meshes) {
        auto* parent = mapped.at(size_t(mesh.node));
        require(parent != nullptr, "Real model mesh parent was not mapped");
        ufbx_node* found = nullptr;
        for (auto* child : parent->children)
            if (child->mesh) {
                require(!found, "Real model source mesh maps to multiple FBX geometry primitives");
                found = child;
            }
        check(found && found->mesh->num_indices == mesh.indices.size(),
              "real-model geometry topology at parent " + string(parent->name));
        geometry.push_back(found->typed_id);
    }
    size_t points = 0, poses = 0;
    auto compare = [&](const ufbx_scene& pose, const Args& config, const std::string& label) {
        const auto world = source->evaluate(config);
        ++poses;
        for (size_t index = 0; index < source->meshes.size(); ++index) {
            const auto& mesh = source->meshes[index];
            auto* instance = pose.nodes.data[geometry[index]];
            const auto* actualMesh = instance->mesh;
            require(actualMesh && actualMesh->num_indices == mesh.indices.size(),
                    "Evaluated real-model topology changed");
            const auto expected = source->transformed(mesh, world);
            const auto& actualPositions = actualMesh->skinned_position.exists ? actualMesh->skinned_position
                                                                              : actualMesh->vertex_position;
            const size_t count = std::min<size_t>(16, mesh.indices.size());
            double error = 0;
            for (size_t sample = 0; sample < count; ++sample) {
                const size_t corner = count <= 1 ? 0 : sample * (mesh.indices.size() - 1) / (count - 1);
                auto actual = ufbx_get_vertex_vec3(&actualPositions, corner);
                if (!actualMesh->skinned_position.exists || actualMesh->skinned_is_local)
                    actual = ufbx_transform_position(&instance->geometry_to_world, actual);
                const auto& value = expected[mesh.indices[corner]];
                const double delta =
                    (V3(actual.x, actual.y, actual.z) - V3(value[0], value[1], value[2])).norm();
                error = std::max(error, delta);
                ++points;
                if (!(delta <= 2e-5)) {
                    std::ostringstream text;
                    text << std::setprecision(12) << label << " source mesh " << index << " (" << mesh.name
                         << ") corner " << corner << " world error " << delta << " m; expected " << value[0]
                         << ',' << value[1] << ',' << value[2] << " actual " << actual.x << ',' << actual.y
                         << ',' << actual.z;
                    check(false, text.str());
                }
            }
            maximumError = std::max(maximumError, error);
            check(true, label + ": sampled real-model world geometry");
        }
    };
    compare(*imported, base, "loaded baseline");
    for (int argument : arguments) {
        std::ostringstream name;
        name << "Argument " << std::setw(3) << std::setfill('0') << argument;
        auto* stack = ufbx_find_anim_stack(imported.get(), name.str().c_str());
        check(stack && stack->anim && std::abs(stack->time_end - duration) < 1e-9,
              "real-model argument clip identity and exact duration");
        const auto [lo, hi] = source->limits.at(argument);
        for (double fraction : {0.0, .25, .5, .75, 1.0}) {
            ufbx_evaluate_opts evaluate{};
            evaluate.evaluate_skinning = true;
            ufbx_error error{};
            Imported pose(ufbx_evaluate_scene(imported.get(), stack->anim,
                                              representableTime(duration * fraction), &evaluate, &error),
                          ufbx_free_scene);
            check(bool(pose), "evaluate real-model clip: " + errorText(error));
            auto values = base;
            values[argument] = argumentAtFraction(lo, hi, fraction);
            compare(*pose, values, name.str() + " at fraction " + std::to_string(fraction));
        }
    }
    auto output = fbxPath;
    output.replace_extension(".verification.json");
    writeJson(output, {{"passed", true},
                       {"model", pathString(edmPath)},
                       {"fbx", pathString(fbxPath)},
                       {"source_meshes", source->meshes.size()},
                       {"source_nodes", source->nodes.size()},
                       {"exported_arguments", arguments},
                       {"poses_checked", poses},
                       {"points_checked", points},
                       {"checks", checks},
                       {"tolerance_meters", 2e-5},
                       {"maximum_world_error_meters", maximumError}});
    std::cout << "FBX real model: " << checks << " checks passed, " << poses << " poses, " << points
              << " points; maximum independent world error " << maximumError << " m\n";
}
void tests(const fs::path& directory) {
    fs::create_directories(directory);
    auto rigid = rigidFixture(directory), skin = skinFixture(directory),
         hiddenSkin = skinFixture(directory, true);
    ExportOptions options;
    options.textures = false;
    options.duration = 2.5;
    options.livery = std::make_shared<Livery>();
    options.livery->args = {{0, -.45}, {1, .8}, {2, .6}, {3, .4}, {7, .8}, {4, -.6}, {5, .2}, {23, 0}};
    options.baseline = {{0, .27}, {4, .45}, {5, .35}};
    for (auto* fixture : {&rigid, &skin, &hiddenSkin}) {
        const auto stem = fixture == &rigid ? "rigid" : fixture == &skin ? "skin" : "hidden-skin";
        auto path = directory / (std::string(stem) + "-all.fbx");
        exportScene(*fixture, path, options);
        validate(*fixture, path, options);
        auto selected = options;
        selected.arguments = fixture == &rigid  ? std::vector<int>{7, 2, 2}
                             : fixture == &skin ? std::vector<int>{4}
                                                : std::vector<int>{23};
        path = directory / (std::string(stem) + "-selected.fbx");
        exportScene(*fixture, path, selected);
        validate(*fixture, path, selected);
        selected.arguments = std::vector<int>{};
        path = directory / (std::string(stem) + "-static.fbx");
        exportScene(*fixture, path, selected);
        validate(*fixture, path, selected);
    }
    auto skinImported = load(directory / "skin-all.fbx");
    const auto* skinMesh = importedMesh(skin, skin.meshes[0], *skinImported)->mesh;
    check(skinMesh->skin_deformers.count == 1 &&
              skinMesh->skin_deformers.data[0]->max_weights_per_vertex == 8,
          "skin retains all eight influences");
    const auto expectedSkin = skin.transformed(skin.meshes[0], skin.evaluate(baseline(options)));
    check(expectedSkin[0] != expectedSkin[4],
          "fixture identical source positions deform differently by weights");
    auto hiddenBaseline = baseline(options);
    const auto hiddenVertices =
        hiddenSkin.transformed(hiddenSkin.meshes[0], hiddenSkin.evaluate(hiddenBaseline));
    hiddenBaseline[23] = 1;
    const auto shownVertices =
        hiddenSkin.transformed(hiddenSkin.meshes[0], hiddenSkin.evaluate(hiddenBaseline));
    check(std::all_of(hiddenVertices.begin(), hiddenVertices.end(),
                      [&](const auto& point) { return point == hiddenVertices[0]; }) &&
              std::any_of(shownVertices.begin(), shownVertices.end(),
                          [&](const auto& point) { return point != shownVertices[0]; }),
          "hidden-skin fixture has a zero-scale weighted-bone ancestor at baseline and a nondegenerate "
          "visible true bind");

    auto textured = options;
    textured.arguments = std::vector<int>{};
    textured.textures = true;
    PaintImage image(8, 4, {47, 171, 93, 255});
    image.rgba[0] = 9;
    image.rgba[7] = 48;
    const auto png = image.pngBytes();
    textured.diffuseOverrides[0] = png;
    const auto texturePath = directory / "edited-texture.fbx";
    auto alphaTextured = rigid;
    alphaTextured.materials[0].blending = 1;
    exportScene(alphaTextured, texturePath, textured);
    validate(alphaTextured, texturePath, textured);
    auto textureScene = load(texturePath);
    auto* meshNode = importedMesh(rigid, rigid.meshes[0], *textureScene);
    require(meshNode->materials.count != 0, "FBX fixture material connection");
    auto* texture = meshNode->materials.data[0]->fbx.diffuse_color.texture;
    if (!texture)
        texture = meshNode->materials.data[0]->pbr.base_color.texture;
    check(texture && embedded(*texture, directory) == png, "FBX material links exact edited PNG bytes");
    const auto uv = textureUV(rigid.meshes[0], rigid.materials[0], 0);
    const ufbx_uv_set* selectedUv = nullptr;
    for (const auto& set : meshNode->mesh->uv_sets)
        if (string(set.name) == string(texture->uv_set))
            selectedUv = &set;
    check(selectedUv != nullptr, "diffuse texture references an exported UV layer");
    for (size_t corner = 0; corner < rigid.meshes[0].indices.size(); ++corner) {
        const auto actual = ufbx_get_vertex_vec2(&selectedUv->vertex_uv, corner);
        const auto expected = uv[rigid.meshes[0].indices[corner]];
        check(std::abs(actual.x - expected[0]) < 1e-6 && std::abs(actual.y - (1 - expected[1])) < 1e-6,
              "selected UV channel and EDM texture transform survive FBX V convention");
    }

    for (double duration :
         {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        auto invalid = options;
        invalid.duration = duration;
        fails([&] { exportScene(rigid, directory / "invalid-duration.fbx", invalid); },
              "invalid duration rejected");
    }
    auto invalid = options;
    invalid.arguments = std::vector<int>{9999};
    fails([&] { exportScene(rigid, directory / "unknown-argument.fbx", invalid); },
          "unknown animation argument rejected");
    invalid = options;
    invalid.baseline[0] = std::numeric_limits<double>::quiet_NaN();
    fails([&] { exportScene(rigid, directory / "invalid-baseline.fbx", invalid); },
          "nonfinite baseline rejected");
    std::atomic_bool cancel = true;
    const auto cancelled = directory / "cancelled.fbx";
    fails([&] { exportScene(rigid, cancelled, options, {}, &cancel); }, "pre-cancelled FBX export rejected");
    check(!fs::exists(cancelled), "pre-cancelled export leaves no completed FBX");
    cancel = false;
    fails([&] { exportScene(skin, cancelled, options, [&](const std::string&) { cancel = true; }, &cancel); },
          "cancellation during export is observed");
    check(!fs::exists(cancelled), "cancelled export does not publish a partial model");
    numberTests(directory);
    endpointVisibilityTests(directory);
    std::cout << "FBX: " << checks << " checks passed; maximum independent world error " << maximumError
              << " m\n";
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc >= 4 && std::wstring_view(argv[1]) == L"--lua-worker")
        return luaWorker(argv[2], argv[3]);
    try {
        ComRuntime runtime;
        if (argc == 5 && std::wstring_view(argv[1]) == L"--verify-model") {
            verifyModel(fs::absolute(argv[2]), fs::absolute(argv[3]), fs::absolute(argv[4]));
            return 0;
        }
        const auto directory =
            argc == 3 && std::wstring_view(argv[1]) == L"--export-fixtures"
                ? fs::absolute(argv[2])
                : fs::current_path() / "validation" / ("fbx-tests-" + std::to_string(GetCurrentProcessId()));
        tests(directory);
        return 0;
    } catch (...) {
        std::cerr << exceptionText() << '\n';
        return 1;
    }
}
