#include "animation_analysis.h"
#include <iostream>

using namespace edm;
namespace {
int checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    require(condition, std::string("Animation analysis test: ") + message);
}
Mesh box(int node, V3 center, V3 halfExtent, std::string name = "anonymous") {
    Mesh mesh;
    mesh.node = node;
    mesh.name = name;
    for (int x : {-1, 1})
        for (int y : {-1, 1})
            for (int z : {-1, 1}) {
                V3 p = center + halfExtent.cwiseProduct(V3(x, y, z));
                mesh.positions.push_back({float(p.x()), float(p.y()), float(p.z())});
            }
    return mesh;
}
void finalize(Scene& scene) {
    scene.order.clear();
    scene.staticLocal.clear();
    scene.limits.clear();
    for (int i = 0; i < int(scene.nodes.size()); ++i) {
        scene.order.push_back(i);
        scene.staticLocal.push_back(scene.nodes[i].local());
    }
    for (const auto& track : scene.tracks)
        scene.limits[track.arg] = track.domain();
    scene.defaultWorld = scene.evaluate({});
}
Scene model(int arg, V3 center, V3 size, Channel channel, V4 endpoint, std::string name = "anonymous") {
    Scene scene;
    scene.nodes.resize(2);
    scene.nodes[0].name = "aircraft";
    scene.nodes[1].name = name;
    scene.nodes[1].parent = 0;
    scene.meshes.push_back(box(0, V3::Zero(), V3(10, 2, 7), "body"));
    scene.meshes.push_back(box(1, center, size, name));
    Track track;
    track.arg = arg;
    track.node = 1;
    track.channel = channel;
    V4 first = channel == Channel::Rotation ? V4(0, 0, 0, 1)
               : channel == Channel::Scale  ? V4(1, 1, 1, 0)
                                            : V4::Zero();
    track.keys = {{0, first}, {1, endpoint}};
    scene.tracks.push_back(track);
    finalize(scene);
    return scene;
}
void tests() {
    auto gear = model(947, V3(3, -1.8, 0), V3(.5, .5, .5), Channel::Position, V4(0, -2, 0, 0));
    auto gearResult = analyzeAnimations(gear);
    auto& g = gearResult.arguments.at(947);
    check(g.part == "landing_gear", "anonymous lowered compact geometry is landing gear");
    check(g.maximumDisplacement > 1.99 && g.maximumDisplacement < 2.01, "movement measured in metres");
    check(g.meshes == std::vector<int>{1}, "only actually moving mesh returned");
    check(g.movingSamples == 8, "moving vertices counted once across poses");
    gear.tracks[0].arg = 2;
    finalize(gear);
    check(analyzeAnimations(gear).arguments.at(2).part == g.part, "argument number does not determine label");

    auto canopy = model(731, V3(4, 1.3, 0), V3(1.7, .2, .6), Channel::Position, V4(0, .6, 0, 0));
    check(analyzeAnimations(canopy).arguments.at(731).part == "canopy",
          "anonymous upper forward central geometry is canopy");
    auto wing =
        model(854, V3(0, 0, 4.5), V3(1, .1, 1.5), Channel::Rotation, V4(0, 0, std::sin(.05), std::cos(.05)));
    check(analyzeAnimations(wing).arguments.at(854).part == "wing_control",
          "anonymous outboard thin surface is wing control");
    auto tail = model(1554, V3(-7, 0, 3.5), V3(1, .1, 1), Channel::Rotation,
                      V4(0, 0, std::sin(.025), std::cos(.025)));
    check(analyzeAnimations(tail).arguments.at(1554).part == "horizontal_tail",
          "rear outboard horizontal surface is elevator");
    auto rudder =
        model(280, V3(-7, 1, 0), V3(.45, .9, .06), Channel::Rotation, V4(0, std::sin(.02), 0, std::cos(.02)));
    check(analyzeAnimations(rudder).arguments.at(280).part == "rudder",
          "rear upper vertical surface is rudder");

    auto unrelated =
        model(18, V3(0, 0, 0), V3(3, 1.5, 1), Channel::Position, V4(1, 0, 0, 0), "CANOPY FLAP GEAR");
    check(analyzeAnimations(unrelated).arguments.at(18).part == "unknown",
          "names cannot override implausible geometric position");
    auto still = model(9, V3(4, 1.3, 0), V3(1.7, .2, .6), Channel::Position, V4::Zero(), "canopy");
    check(analyzeAnimations(still).arguments.at(9).part == "unknown",
          "name alone cannot classify nonmoving geometry");
    auto cockpitKnob = model(1204, V3(4, 1.3, 0), V3(.05, .02, .03), Channel::Position, V4(0, .03, 0, 0));
    check(analyzeAnimations(cockpitKnob).arguments.at(1204).part == "unknown",
          "small cockpit geometry is not mislabelled as canopy");
    auto pilot =
        model(1304, V3(4, 1.3, 0), V3(.2, .25, .2), Channel::Position, V4(0, .1, 0, 0), "pilot head_pose");
    check(analyzeAnimations(pilot).arguments.at(1304).part == "crew",
          "compact cockpit character motion separated from canopy");
    auto cycle = canopy;
    cycle.tracks[0].keys = {{0, V4::Zero()}, {.5, V4(0, .8, 0, 0)}, {1, V4::Zero()}};
    finalize(cycle);
    check(analyzeAnimations(cycle).arguments.at(731).maximumDisplacement > .79,
          "interior motion survives identical endpoints");

    auto visibility = canopy;
    visibility.tracks[0].channel = Channel::Scale;
    visibility.tracks[0].visibility = true;
    visibility.tracks[0].ranges = {{.2, .7}};
    finalize(visibility);
    auto vis = analyzeAnimations(visibility).arguments.at(731);
    check(vis.visibilityChanges && vis.movingSamples == 0 && vis.part == "unknown",
          "visibility collapse is not physical motion");
    check(vis.meshes == std::vector<int>{1}, "visibility-only affected mesh retained for highlighting");

    auto hiddenGear = gear;
    hiddenGear.nodes.resize(3);
    hiddenGear.nodes[2].parent = 0;
    hiddenGear.nodes[1].parent = 2;
    // Establish a valid parent-before-child topological order after finalization.
    Track gate;
    gate.node = 2;
    gate.arg = 600;
    gate.channel = Channel::Scale;
    gate.visibility = true;
    gate.ranges = {{.5, 1.1}};
    hiddenGear.tracks.push_back(gate);
    finalize(hiddenGear);
    hiddenGear.order = {0, 2, 1};
    auto revealed = analyzeAnimations(hiddenGear);
    check(revealed.arguments.at(2).part == "landing_gear",
          "other visibility gates may reveal stowed parts during analysis");
    AnalysisOptions literal;
    literal.revealOtherVisibility = false;
    check(analyzeAnimations(hiddenGear, {}, literal).arguments.at(2).movingSamples == 0,
          "literal context respects hidden parts");
    check((hiddenGear.evaluate({})[1] -
           hiddenGear.staticLocal[2] * scaling(V3::Zero()) * hiddenGear.staticLocal[1])
                  .norm() < 1e-12,
          "analysis does not mutate actual preview pose");

    auto skinned = gear;
    auto& skin = skinned.meshes[1];
    skin.node = 0;
    skin.skinNodes = {0, 1};
    skin.inverseBind = {Mat::Identity(), Mat::Identity()};
    skin.joints.resize(8);
    skin.weights.resize(8);
    for (size_t i = 0; i < 8; ++i) {
        skin.joints[i][4] = 1;
        skin.weights[i][4] = 1;
    }
    auto skinResult = analyzeAnimations(skinned).arguments.at(2);
    check(skinResult.part == "landing_gear" && skinResult.maximumDisplacement > 1.99,
          "fifth bone influence participates in motion inference");
    for (auto& weights : skin.weights) {
        weights.fill(0);
        weights[0] = 1;
    }
    check(analyzeAnimations(skinned).arguments.at(2).movingSamples == 0,
          "unused moving bone does not mark stationary weighted vertices");

    AnalysisOptions budget;
    budget.maxVertexSamples = 10;
    budget.maxSamplesPerMesh = 64;
    check(analyzeAnimations(gear, {}, budget).sampledVertices <= 10, "global sample budget enforced");
    bool denied = false;
    std::atomic_bool cancel = true;
    try {
        analyzeAnimations(gear, {}, {}, {}, &cancel);
    } catch (const std::exception&) {
        denied = true;
    }
    check(denied, "cancellation stops immediately");
    cancel = false;
    denied = false;
    try {
        analyzeAnimations(gear, {}, {}, [&](const std::string&) { cancel = true; }, &cancel);
    } catch (const std::exception&) {
        denied = true;
    }
    check(denied, "progress callback cancellation stops in-flight analysis");
    auto numbers = still;
    numbers.tracks.clear();
    finalize(numbers);
    numbers.limits[3000] = {0, 1};
    numbers.numberArgs = {3000};
    check(analyzeAnimations(numbers).arguments.at(3000).part == "dynamic_number",
          "number texture selectors identified without fixed IDs");
    auto report = gearResult.toJson();
    check(report["arguments"][0]["argument"] == 947 && report["confidence_is_probability"] == false,
          "JSON report exposes evidence and calibrated meaning of score");
    std::cout << "Animation analysis: " << checks << " checks passed\n";
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    try {
        tests();
        if (argc >= 2) {
            auto scene = Scene::load(argv[1]);
            auto report = analyzeAnimations(*scene);
            check(report.sampledVertices <= 12000, "real aircraft remains within bounded vertex budget");
            check(report.arguments.size() == scene->limits.size(),
                  "every real aircraft argument has a result");
            for (const auto& [arg, result] : report.arguments) {
                check(std::isfinite(result.maximumDisplacement) && result.center.allFinite(),
                      "real motion results remain finite");
                for (int mesh : result.meshes)
                    check(mesh >= 0 && mesh < int(scene->meshes.size()),
                          "real highlighting mesh reference valid");
            }
            std::map<int, std::string> expected;
            const std::string filename = lower(pathString(scene->source.filename()));
            // Model-specific expectations belong only in these regression assertions, never the classifier.
            if (filename == "f-14b.edm")
                expected = {{0, "landing_gear"},    {3, "landing_gear"},     {5, "landing_gear"},
                            {38, "canopy"},         {404, "wing_mechanism"}, {405, "wing_mechanism"},
                            {1001, "wing_control"}, {1003, "wing_control"},  {1010, "wing_control"},
                            {1020, "rudder"}};
            else if (filename == "f-100d.edm")
                expected = {{0, "landing_gear"},  {3, "landing_gear"},     {5, "landing_gear"},
                            {9, "wing_control"},  {10, "wing_control"},    {11, "wing_control"},
                            {12, "wing_control"}, {15, "horizontal_tail"}, {16, "horizontal_tail"},
                            {17, "rudder"},       {38, "canopy"}};
            for (const auto& [arg, part] : expected)
                require(report.arguments.contains(arg) && report.arguments.at(arg).part == part,
                        "Real model part regression failed for argument " + std::to_string(arg) +
                            ": expected " + part);
            auto json = report.toJson();
            json["model"] = pathString(scene->source);
            json["regression_expected_parts"] = expected;
            json["regression_expected_parts_passed"] = true;
            if (argc >= 3)
                writeJson(argv[2], json);
            std::cout << "Real model: " << scene->meshes.size() << " meshes, " << report.sampledVertices
                      << " samples, " << report.arguments.size() << " arguments, " << report.seconds
                      << " seconds\n";
            for (const auto& [arg, result] : report.arguments)
                if (result.part != "unknown")
                    std::cout << arg << ": " << result.label << " (" << result.confidence << ")\n";
        }
        return 0;
    } catch (...) {
        std::cerr << exceptionText() << "\n";
        return 1;
    }
}
