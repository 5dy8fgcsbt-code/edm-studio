#pragma once
#include "model.h"

namespace edm {
struct AnalysisBounds {
    V3 minimum = V3::Zero(), maximum = V3::Zero();
    V3 center() const {
        return (minimum + maximum) * .5;
    }
    V3 extent() const {
        return maximum - minimum;
    }
};
struct PartCandidate {
    std::string part, label;
    double score = 0;
};
struct AnimationFinding {
    int argument = 0;
    std::string part = "unknown", label = "未确定部件", location, motion;
    double confidence = 0, maximumDisplacement = 0, meanDisplacement = 0;
    size_t movingSamples = 0, sampledVertices = 0, poses = 0;
    bool visibilityChanges = false, textureOnly = false;
    AnalysisBounds bounds, shapeBounds;
    V3 center = V3::Zero(), dominantAxis = V3::Zero();
    std::vector<int> meshes;
    std::vector<std::string> evidence;
    std::vector<PartCandidate> candidates;
    Json toJson() const;
};
struct AnalysisOptions {
    size_t maxVertexSamples = 12000;
    size_t maxSamplesPerMesh = 64;
    size_t maxPosesPerArgument = 13;
    double movementThresholdFraction = 1e-5;
    // Analyse stowed/hidden parts without changing the preview or exported pose.
    bool revealOtherVisibility = true;
};
struct AnimationAnalysis {
    std::map<int, AnimationFinding> arguments;
    AnalysisBounds modelBounds;
    size_t sampledVertices = 0;
    double seconds = 0;
    std::vector<std::string> warnings;
    Json toJson() const;
};
AnimationAnalysis analyzeAnimations(const Scene& scene, const Args& baseline = {},
                                    const AnalysisOptions& options = {}, Progress progress = {},
                                    const std::atomic_bool* cancel = nullptr);
} // namespace edm
