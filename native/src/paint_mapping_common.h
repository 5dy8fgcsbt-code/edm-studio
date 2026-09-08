#pragma once
#include "paint.h"

namespace edm {
// By default a mapping operation owns one undoable stroke and rolls back on failure.
// externalStroke leaves all history/rollback to a caller managing multiple canvases.
struct PaintMappingLimits {
    uint64_t rasterSamples = 128ull * 1024 * 1024;
    uint64_t rayTests = 64ull * 1024 * 1024;
    double seconds = 120;
};

struct PaintMappingReport {
    uint64_t triangles = 0, rasterSamples = 0, paintedPixels = 0;
    uint64_t reusedTexels = 0, occludedSamples = 0, backfaceSamples = 0, rayTests = 0;
    double seconds = 0;
    bool changed = false;
    std::vector<std::string> warnings;
};

namespace paint_mapping {
F4 sample(const PaintImage& image, double u, double v, float opacity);
void validate(const PaintCanvas& canvas, const PaintImage& image, int material, int mesh, float opacity,
              const PaintMappingLimits& limits, bool externalStroke = false,
              std::span<const int> targetMaterials = {});

// Constant-size per-texel state: an 8K target needs 8 MB, without another floating-point image.
class Operation {
    PaintCanvas& canvas;
    PaintMappingLimits limits;
    Progress progress;
    const std::atomic_bool* cancel;
    std::vector<uint64_t> visited;
    Clock::time_point started = Clock::now();
    bool finished = false;
    bool ownsStroke = true;
    uint64_t nextProgress = 0;

  public:
    PaintMappingReport report;
    Operation(PaintCanvas& canvas, const std::string& label, const PaintMappingLimits& limits,
              Progress progress, const std::atomic_bool* cancel, bool externalStroke = false);
    ~Operation();
    void check();
    void sampleVisited();
    void rayTest();
    void blend(int x, int y, const F4& color, bool preserveAlpha);
    PaintMappingReport finish();
};

bool targetTriangle(const PaintTriangle& triangle, int material, int mesh,
                    std::span<const int> targetMaterials = {});
V3 position(const PaintTriangle& triangle, const V3& barycentric);
double distanceBias(double distance);
} // namespace paint_mapping
} // namespace edm
