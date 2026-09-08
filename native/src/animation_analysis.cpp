#include "animation_analysis.h"
#include <sstream>
#include <iomanip>

namespace edm {
namespace {
Json vectorJson(const V3& v) {
    return {v[0], v[1], v[2]};
}
Json boundsJson(const AnalysisBounds& b) {
    return {{"minimum", vectorJson(b.minimum)}, {"maximum", vectorJson(b.maximum)}};
}
void cancelled(const std::atomic_bool* cancel) {
    require(!cancel || !cancel->load(), "Animation analysis cancelled");
}
struct Box {
    V3 low = V3::Constant(std::numeric_limits<double>::infinity());
    V3 high = V3::Constant(-std::numeric_limits<double>::infinity());
    void add(const V3& p) {
        low = low.cwiseMin(p);
        high = high.cwiseMax(p);
    }
    bool valid() const {
        return low.allFinite() && high.allFinite();
    }
    AnalysisBounds bounds() const {
        return valid() ? AnalysisBounds{low, high} : AnalysisBounds{};
    }
};
struct Point {
    V3 position = V3::Zero();
    bool visible = false;
};
struct SampleMesh {
    int mesh = 0;
    std::vector<size_t> vertices;
    std::set<int> dependencies;
};
std::vector<Mat> evaluate(const Scene& scene, const Args& args, int argument, bool reveal) {
    auto world = scene.staticLocal;
    for (const auto& track : scene.tracks) {
        if (reveal && track.visibility && track.arg != argument) {
            world[track.node] = Mat::Identity();
            continue;
        }
        V4 value = track.sample(argValue(args, track.arg));
        world[track.node] = track.channel == Channel::Position   ? translation(value.head<3>())
                            : track.channel == Channel::Rotation ? rotation(value)
                                                                 : scaling(value.head<3>());
    }
    for (int node : scene.order)
        if (scene.nodes[node].parent >= 0)
            world[node] = world[scene.nodes[node].parent] * world[node];
    return world;
}
// A zero visibility gate collapses vertices to an origin. It must never count as movement.
bool visibleMatrix(const Mat& matrix) {
    return matrix.block<3, 3>(0, 0).squaredNorm() > 1e-18;
}
std::vector<Point> sample(const Mesh& mesh, const SampleMesh& selection, const std::vector<Mat>& world) {
    std::vector<Mat> palette;
    palette.reserve(mesh.skinNodes.size());
    for (size_t i = 0; i < mesh.skinNodes.size(); ++i)
        palette.push_back(world[mesh.skinNodes[i]] * mesh.inverseBind[i]);
    std::vector<Point> out;
    out.reserve(selection.vertices.size());
    for (size_t index : selection.vertices) {
        const auto& p = mesh.positions[index];
        V4 input(p[0], p[1], p[2], 1), position = V4::Zero();
        double visibleWeight = 0;
        if (mesh.skinned()) {
            for (size_t slot = 0; slot < 8; ++slot) {
                double weight = mesh.weights[index][slot];
                if (!weight)
                    continue;
                const auto& transform = palette[mesh.joints[index][slot]];
                position += transform * input * weight;
                if (visibleMatrix(transform))
                    visibleWeight += weight;
            }
        } else {
            position = world[mesh.node] * input;
            visibleWeight = visibleMatrix(world[mesh.node]) ? 1 : 0;
        }
        // Partially hidden skins are excluded too: their collapse is a visibility artefact.
        out.push_back({position.head<3>(), visibleWeight > .999 && position.allFinite()});
    }
    return out;
}
std::vector<double> poses(const Scene& scene, int argument, const Args& baseline, size_t limit) {
    const auto [lo, hi] = scene.limits.at(argument);
    std::set<double> values{lo, hi, std::clamp(argValue(baseline, argument), lo, hi)};
    // Uniform interior samples catch endpoints that intentionally hide a part.
    for (int i = 1; i < 8; ++i)
        values.insert(lo + (hi - lo) * i / 8.);
    std::set<double> boundaries;
    for (const auto& track : scene.tracks)
        if (track.arg == argument) {
            for (const auto& key : track.keys)
                if (key.time >= lo && key.time <= hi)
                    boundaries.insert(key.time);
            for (const auto& range : track.ranges)
                for (double endpoint : {range.first, range.second, (range.first + range.second) * .5})
                    if (endpoint >= lo && endpoint <= hi)
                        boundaries.insert(endpoint);
        }
    if (boundaries.size() <= limit)
        values.insert(boundaries.begin(), boundaries.end());
    std::vector<double> all(values.begin(), values.end()), reduced;
    if (all.size() <= limit)
        return all;
    for (size_t i = 0; i < limit; ++i)
        reduced.push_back(all[i * (all.size() - 1) / (limit - 1)]);
    return reduced;
}
bool anyWord(const std::string& text, std::initializer_list<const char*> words) {
    for (auto word : words)
        if (text.find(word) != std::string::npos)
            return true;
    return false;
}
std::string number(double value, int precision = 2) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}
void classify(AnimationFinding& finding, const AnalysisBounds& model, const std::string& names, bool rotated,
              bool translated, bool scaled) {
    if (finding.textureOnly) {
        finding.part = "dynamic_number";
        finding.label = "动态编号（纹理）";
        finding.motion = "纹理选择";
        finding.confidence = 1;
        finding.evidence.push_back("该参数连接编号纹理选择器，没有空间位移轨道。");
        return;
    }
    if (finding.movingSamples == 0) {
        finding.motion = finding.visibilityChanges ? "显示 / 隐藏" : "无可测空间位移";
        finding.evidence.push_back(
            finding.visibilityChanges
                ? "采样中只发现可见性切换，已排除零缩放造成的伪位移。"
                : "在采样姿态中没有可测位移；可能是材质参数、条件动画或未覆盖的小部件。");
        return;
    }
    const V3 extent = model.extent().cwiseMax(V3::Constant(1e-6));
    const V3 at = (finding.center - model.minimum).cwiseQuotient(extent);
    const V3 span = finding.bounds.extent().cwiseQuotient(extent);
    const V3 shape = finding.shapeBounds.extent().cwiseQuotient(extent);
    const double lateral = std::abs(at.z() - .5) * 2;
    const double outboard = std::max(std::abs(finding.bounds.minimum.z() - model.center().z()),
                                     std::abs(finding.bounds.maximum.z() - model.center().z())) *
                            2 / extent.z();
    const bool rear = at.x() < .27, front = at.x() > .57, low = at.y() < .35, upper = at.y() > .53,
               middle = lateral < .30, horizontal = shape.y() < .28 && shape.z() > shape.y() * .7;
    finding.location = front ? "前部" : rear ? "后部" : "中部";
    finding.location += low ? " · 下方" : upper ? " · 上方" : " · 中层";
    if (finding.bounds.minimum.z() < model.center().z() - extent.z() * .12 &&
        finding.bounds.maximum.z() > model.center().z() + extent.z() * .12)
        finding.location += " · 两侧";
    else if (lateral > .15)
        finding.location += at.z() < .5 ? " · 左侧" : " · 右侧";
    else
        finding.location += " · 中央";
    finding.motion = rotated && translated ? "旋转与平移"
                     : rotated             ? "旋转"
                     : translated          ? "平移"
                     : scaled              ? "形变 / 缩放"
                                           : "复合运动";
    auto add = [&](const char* code, const char* label, double score) {
        if (score > .25)
            finding.candidates.push_back({code, label, std::clamp(score, 0., .96)});
    };
    const bool gearName =
        anyWord(names, {"gear", "landing", "wheel", "tyre", "tire", "стойк", "колес", "起落", "轮胎"});
    const bool canopyName = anyWord(names, {"canopy", "cockpit_glass", "фонар", "座舱盖"});
    const bool flapName =
        anyWord(names, {"flap", "aileron", "_ail_", "spoiler", "закрыл", "элерон", "襟翼", "副翼"});
    const bool rudderName = anyWord(names, {"rudder", "ruder", "direction", "方向舵"});
    const bool elevatorName =
        anyWord(names, {"elevator", "stabilator", "stabilizer", "tailplane", "stab_", "升降舵", "平尾"});
    const bool wingName =
        anyWord(names, {"wingsweep", "wing_sweep", "wingfold", "wing_fold", "后掠", "折叠"});
    const bool brakeName = anyWord(names, {"airbrake", "speedbrake", "speed_brake", "air_brake", "减速板"});
    const bool nozzleName = anyWord(names, {"nozzle", "exhaust", "喷口"});
    const bool crewName = anyWord(names, {"eyelash", "eyebrow", "head_pose", "necktwist", "pilot", "helmet",
                                          "tearduct", "harness", "hand_", "finger", "mouth", "brow_"});
    // Scores are explainable confidence ranks, not learned probabilities. Names only support a
    // spatially plausible candidate; an unrelated name cannot turn a tail into a canopy.
    const bool belowBody = finding.bounds.minimum.y() < model.minimum.y() - extent.y() * .08;
    if (low && lateral < .70 && span.x() < .48 && !rear)
        add("landing_gear", "起落架 / 轮胎", .57 + (belowBody ? .12 : 0) + (gearName ? .22 : 0));
    if (front && middle && at.y() > .40 && shape.x() > .085 && shape.z() > .025 && span.x() < .50)
        add("canopy", "座舱盖", .57 + (upper ? .09 : 0) + (canopyName ? .23 : 0));
    if (front && middle && at.y() > .40 && shape.x() < .12 && crewName)
        add("crew", "机组人物动作", .86);
    if (horizontal && outboard > .24 && !rear)
        add("wing_control", "机翼舵面（襟翼 / 副翼）",
            .57 + (span.x() < .25 ? .10 : 0) + (flapName ? .22 : 0));
    if (rear && (horizontal || elevatorName) && outboard > .12 && rotated)
        add("horizontal_tail", "水平尾翼 / 升降舵", .66 + (elevatorName ? .23 : 0));
    if (rear && (upper || shape.y() > .32) && shape.z() < .20 && rotated)
        add("rudder", "垂直尾翼 / 方向舵", .59 + (shape.y() > shape.x() ? .12 : 0) + (rudderName ? .23 : 0));
    if (outboard > .20 && span.x() > .18 && span.z() > .16 && rotated && !rear)
        add("wing_mechanism", "机翼后掠 / 折叠机构",
            .57 + (finding.dominantAxis.y() > .70 ? .12 : 0) + (wingName ? .23 : 0));
    if (at.x() < .58 && middle && upper && span.x() < .30 && rotated)
        add("airbrake", "减速板", .48 + (brakeName ? .30 : 0));
    if (at.x() < .20 && lateral < .40 && span.x() < .25)
        add("engine_nozzle", "发动机喷口", .47 + (scaled ? .09 : 0) + (nozzleName ? .29 : 0));
    std::sort(finding.candidates.begin(), finding.candidates.end(), [](const auto& a, const auto& b) {
        return a.score > b.score || (a.score == b.score && a.part < b.part);
    });
    if (finding.candidates.size() > 3)
        finding.candidates.resize(3);
    if (!finding.candidates.empty()) {
        auto& best = finding.candidates.front();
        const double margin = finding.candidates.size() > 1 ? best.score - finding.candidates[1].score : 1;
        finding.confidence = best.score;
        if (best.score >= .63 && (margin >= .075 || best.score >= .87)) {
            finding.part = best.part;
            finding.label = best.label;
        } else
            finding.evidence.push_back("空间特征与多个部件相近，保留候选而不强行命名。");
    }
    finding.evidence.push_back("实际检测到 " + std::to_string(finding.meshes.size()) + " 个网格、" +
                               std::to_string(finding.movingSamples) + " 个移动采样点；最大位移 " +
                               number(finding.maximumDisplacement, 3) + " m。");
    finding.evidence.push_back("位置：" + finding.location + "；移动区域占机体长 / 高 / 宽 " +
                               number(span.x() * 100, 0) + "% / " + number(span.y() * 100, 0) + "% / " +
                               number(span.z() * 100, 0) + "%。");
    if (gearName || canopyName || flapName || rudderName || elevatorName || wingName || brakeName ||
        nozzleName)
        finding.evidence.push_back("受影响节点 / 网格名称提供了部件语义佐证，分类同时要求空间位置合理。");
}
} // namespace

Json AnimationFinding::toJson() const {
    Json choices = Json::array();
    for (const auto& c : candidates)
        choices.push_back({{"part", c.part}, {"label", c.label}, {"score", c.score}});
    return {{"argument", argument},
            {"part", part},
            {"label", label},
            {"confidence", confidence},
            {"location", location},
            {"motion", motion},
            {"maximum_displacement_metres", maximumDisplacement},
            {"mean_displacement_metres", meanDisplacement},
            {"moving_samples", movingSamples},
            {"sampled_vertices", sampledVertices},
            {"poses", poses},
            {"visibility_changes", visibilityChanges},
            {"texture_only", textureOnly},
            {"bounds", boundsJson(bounds)},
            {"shape_bounds", boundsJson(shapeBounds)},
            {"center", vectorJson(center)},
            {"dominant_axis", vectorJson(dominantAxis)},
            {"meshes", meshes},
            {"evidence", evidence},
            {"candidates", choices}};
}
Json AnimationAnalysis::toJson() const {
    Json entries = Json::array();
    for (const auto& [argument, finding] : arguments)
        entries.push_back(finding.toJson());
    return {{"arguments", entries},
            {"model_bounds", boundsJson(modelBounds)},
            {"sampled_vertices", sampledVertices},
            {"seconds", seconds},
            {"warnings", warnings},
            {"method", "World-space vertex motion and skinning; spatial rules with supporting node names"},
            {"coordinate_convention", "DCS: +X forward, +Y up, +Z right"},
            {"confidence_is_probability", false}};
}

AnimationAnalysis analyzeAnimations(const Scene& scene, const Args& baseline, const AnalysisOptions& options,
                                    Progress progress, const std::atomic_bool* cancel) {
    const auto start = Clock::now();
    cancelled(cancel);
    require(options.maxVertexSamples > 0 && options.maxSamplesPerMesh > 0 && options.maxPosesPerArgument >= 3,
            "Invalid animation analysis sampling budget");
    require(std::isfinite(options.movementThresholdFraction) && options.movementThresholdFraction > 0,
            "Invalid animation analysis movement threshold");
    AnimationAnalysis result;
    if (options.revealOtherVisibility)
        result.warnings.push_back("识别时临时显示其他参数隐藏的部件；不改变预览或导出状态。");
    result.warnings.push_back("采用 DCS 的 +X 机头、+Y 向上、+Z 右侧坐标；置信度是启发式评分，不是概率。");
    std::vector<std::set<int>> dependencies(scene.nodes.size());
    for (const auto& track : scene.tracks)
        dependencies[track.node].insert(track.arg);
    for (int node : scene.order)
        if (scene.nodes[node].parent >= 0) {
            const auto& parent = dependencies[scene.nodes[node].parent];
            dependencies[node].insert(parent.begin(), parent.end());
        }
    std::vector<SampleMesh> samples;
    const size_t nonEmpty = std::count_if(scene.meshes.begin(), scene.meshes.end(),
                                          [](const auto& mesh) { return !mesh.positions.empty(); });
    const size_t each =
        std::min(options.maxSamplesPerMesh,
                 std::max(size_t(1), options.maxVertexSamples / std::max(size_t(1), nonEmpty)));
    std::map<int, std::vector<size_t>> affected;
    for (size_t m = 0; m < scene.meshes.size(); ++m) {
        cancelled(cancel);
        const auto& mesh = scene.meshes[m];
        if (mesh.positions.empty() || result.sampledVertices >= options.maxVertexSamples)
            continue;
        SampleMesh selection;
        selection.mesh = int(m);
        selection.dependencies = dependencies[mesh.node];
        for (int node : mesh.skinNodes)
            selection.dependencies.insert(dependencies[node].begin(), dependencies[node].end());
        const size_t count =
            std::min({each, mesh.positions.size(), options.maxVertexSamples - result.sampledVertices});
        for (size_t i = 0; i < count; ++i)
            selection.vertices.push_back(count == 1 ? mesh.positions.size() / 2
                                                    : i * (mesh.positions.size() - 1) / (count - 1));
        for (int argument : selection.dependencies)
            affected[argument].push_back(samples.size());
        result.sampledVertices += selection.vertices.size();
        samples.push_back(std::move(selection));
    }
    if (samples.size() < nonEmpty)
        result.warnings.push_back("网格数量超过采样预算，部分网格未覆盖；可增大分析预算后重试。");
    auto neutral = evaluate(scene, baseline, std::numeric_limits<int>::min(), options.revealOtherVisibility);
    const auto defaultWorld = scene.evaluate(baseline);
    std::array<std::vector<double>, 3> coordinate;
    Box all;
    for (const auto& selection : samples)
        for (const auto& point : sample(scene.meshes[selection.mesh], selection, defaultWorld)) {
            if (!point.visible)
                continue;
            all.add(point.position);
            for (size_t axis = 0; axis < 3; ++axis)
                coordinate[axis].push_back(point.position[axis]);
        }
    if (!all.valid()) {
        result.warnings.push_back("当前姿态没有可见几何，机身参照范围改用临时显示的采样。");
        for (const auto& selection : samples)
            for (const auto& point : sample(scene.meshes[selection.mesh], selection, neutral)) {
                if (!point.visible)
                    continue;
                all.add(point.position);
                for (size_t axis = 0; axis < 3; ++axis)
                    coordinate[axis].push_back(point.position[axis]);
            }
    }
    result.modelBounds = all.bounds();
    // Ignore isolated exhaust/billboard outliers, while keeping the entire aircraft silhouette.
    if (coordinate[0].size() >= 100)
        for (size_t axis = 0; axis < 3; ++axis) {
            auto& values = coordinate[axis];
            std::sort(values.begin(), values.end());
            result.modelBounds.minimum[axis] = values[size_t((values.size() - 1) * .005)];
            result.modelBounds.maximum[axis] = values[size_t((values.size() - 1) * .995)];
        }
    const double threshold =
        std::max(1e-7, result.modelBounds.extent().norm() * options.movementThresholdFraction);
    size_t completed = 0;
    for (const auto& [argument, domain] : scene.limits) {
        cancelled(cancel);
        if (progress)
            progress("识别动画部件 " + std::to_string(++completed) + " / " +
                     std::to_string(scene.limits.size()));
        cancelled(cancel);
        AnimationFinding finding;
        finding.argument = argument;
        finding.textureOnly =
            std::find(scene.numberArgs.begin(), scene.numberArgs.end(), argument) != scene.numberArgs.end() &&
            affected[argument].empty();
        bool rotated = false, translated = false, scaled = false;
        std::string names;
        for (const auto& track : scene.tracks)
            if (track.arg == argument && !track.visibility) {
                const auto a = track.sample(domain.first), b = track.sample(domain.second);
                if ((a - b).norm() > 1e-8 || track.keys.size() > 2) {
                    rotated |= track.channel == Channel::Rotation;
                    translated |= track.channel == Channel::Position;
                    scaled |= track.channel == Channel::Scale;
                    if (track.channel == Channel::Rotation) {
                        Eigen::AngleAxisd delta((rotation(b) * rotation(a).transpose()).block<3, 3>(0, 0));
                        V3 axis = delta.axis();
                        const int parent = scene.nodes[track.node].parent;
                        if (parent >= 0)
                            axis = neutral[parent].block<3, 3>(0, 0) * axis;
                        if (axis.norm() > 1e-9 && delta.angle() > 1e-8)
                            finding.dominantAxis += axis.normalized().cwiseAbs() * delta.angle();
                    }
                }
                names += " " + scene.nodes[track.node].name;
            }
        if (finding.dominantAxis.norm() > 1e-9)
            finding.dominantAxis.normalize();
        auto values = affected[argument].empty()
                          ? std::vector<double>{}
                          : poses(scene, argument, baseline, options.maxPosesPerArgument);
        finding.poses = values.size();
        std::vector<std::vector<std::vector<Point>>> trajectories;
        trajectories.resize(affected[argument].size());
        Args args = baseline;
        for (double value : values) {
            cancelled(cancel);
            args[argument] = value;
            auto world = evaluate(scene, args, argument, options.revealOtherVisibility);
            for (size_t m = 0; m < affected[argument].size(); ++m) {
                const auto& selection = samples[affected[argument][m]];
                trajectories[m].push_back(sample(scene.meshes[selection.mesh], selection, world));
            }
        }
        Box movingBounds, shapeBounds;
        double totalDistance = 0;
        for (size_t m = 0; m < affected[argument].size(); ++m) {
            cancelled(cancel);
            const auto& selection = samples[affected[argument][m]];
            const auto& trajectory = trajectories[m];
            bool movedMesh = false, visibilityMesh = false;
            finding.sampledVertices += selection.vertices.size();
            for (size_t point = 0; point < selection.vertices.size(); ++point) {
                double distance = 0;
                bool visible = false, hidden = false;
                for (size_t p = 0; p < trajectory.size(); ++p) {
                    if (!trajectory[p][point].visible) {
                        hidden = true;
                        continue;
                    }
                    visible = true;
                    for (size_t q = p + 1; q < trajectory.size(); ++q)
                        if (trajectory[q][point].visible)
                            distance = std::max(
                                distance,
                                (trajectory[p][point].position - trajectory[q][point].position).norm());
                }
                visibilityMesh |= visible && hidden;
                if (distance <= threshold)
                    continue;
                movedMesh = true;
                ++finding.movingSamples;
                totalDistance += distance;
                finding.maximumDisplacement = std::max(finding.maximumDisplacement, distance);
                // Use all visible poses, not just a single starting configuration.
                V3 centroid = V3::Zero();
                size_t count = 0;
                for (const auto& pose : trajectory)
                    if (pose[point].visible) {
                        if (!count)
                            shapeBounds.add(pose[point].position);
                        movingBounds.add(pose[point].position);
                        centroid += pose[point].position;
                        ++count;
                    }
                finding.center += centroid / double(count);
            }
            finding.visibilityChanges |= visibilityMesh;
            if (movedMesh || visibilityMesh) {
                finding.meshes.push_back(selection.mesh);
                names += " " + scene.meshes[selection.mesh].name;
            }
        }
        finding.bounds = movingBounds.bounds();
        finding.shapeBounds = shapeBounds.bounds();
        if (finding.movingSamples) {
            finding.center /= double(finding.movingSamples);
            finding.meanDisplacement = totalDistance / double(finding.movingSamples);
        }
        classify(finding, result.modelBounds, lower(names), rotated, translated, scaled);
        result.arguments.emplace(argument, std::move(finding));
    }
    result.seconds = edm::seconds(start);
    return result;
}
} // namespace edm
