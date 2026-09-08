#include "paint_stroke_path.h"
#include <algorithm>
#include <iostream>
#include <vector>

using namespace edm;
namespace {
int checks = 0;
void check(bool condition, const char* description) {
    ++checks;
    if (!condition)
        throw std::runtime_error(std::string("Stroke path regression: ") + description);
}
template <class F> void fails(F action, const char* description) {
    bool failed = false;
    try {
        action();
    } catch (const std::exception&) {
        failed = true;
    }
    check(failed, description);
}
bool same(PaintStrokePoint a, PaintStrokePoint b) {
    return std::abs(a.x - b.x) < 1e-9 && std::abs(a.y - b.y) < 1e-9;
}
void fastDragAndBudget() {
    PaintStrokePath path;
    std::vector<PaintStrokeSample> points;
    auto emit = [&](const PaintStrokeSample& sample) { points.push_back(sample); };
    path.begin({0, 0}, .5, 4);
    path.append({1000, 0}, .5, 8);
    check(path.pendingSamples() == 2001, "Fast fine-brush segment is not truncated to 32 samples");
    auto first = path.consume(32, emit, [&] { return points.size() >= 5; });
    check(first == 5 && path.pendingSamples() == 1996, "Frame budget leaves every remaining sample queued");
    check(path.lastConsumed() && same(path.lastConsumed()->point, {2, 0}),
          "Last consumed position is the actual painted sample, not the latest mouse position");
    check(same(path.lastInput(), {1000, 0}), "Input and consumed endpoints are independent");
    size_t untouched = path.consume(32, emit, [] { return true; });
    check(untouched == 0 && path.pendingSamples() == 1996, "Already exhausted frame budget changes no state");
    path.release();
    check(path.released() && !path.drained(), "Release keeps queued samples alive");
    while (!path.drained())
        path.consume(17, emit);
    check(points.size() == 2001 && same(points.back().point, {1000, 0}),
          "Subsequent frames drain through exact final endpoint after release");
    double largestGap = 0;
    for (size_t i = 1; i < points.size(); ++i)
        largestGap = std::max(largestGap, std::hypot(points[i].point.x - points[i - 1].point.x,
                                                     points[i].point.y - points[i - 1].point.y));
    check(largestGap <= .5000001, "Fine brush samples have no holes caused by frame budget");
    check(points.front().context == 4 && points[1].context == 8 && points.back().context == 8,
          "Queued samples retain caller camera/pose context token");
    path.begin({3, 2}, 1);
    check(path.pendingSamples() == 1 && !path.released(), "A drained stroke can begin a new path");
}
void cornersAndRelease() {
    PaintStrokePath path;
    path.begin({0, 0}, 7);
    path.append({100, 0}, 7);
    path.append({100, 100}, 7);
    path.append({20, 100}, 7);
    path.release();
    std::vector<PaintStrokePoint> points;
    while (!path.drained())
        path.consume(3, [&](const PaintStrokeSample& sample) { points.push_back(sample.point); });
    check(std::any_of(points.begin(), points.end(), [](auto p) { return same(p, {100, 0}); }) &&
              std::any_of(points.begin(), points.end(), [](auto p) { return same(p, {100, 100}); }),
          "Every turn is preserved across multiple input events and frames");
    check(std::all_of(points.begin(), points.end(),
                      [](auto p) {
                          return std::abs(p.y) < 1e-9 || std::abs(p.x - 100) < 1e-9 ||
                                 std::abs(p.y - 100) < 1e-9;
                      }),
          "Queued polyline does not cut across corners");
    check(same(points.back(), {20, 100}), "Last corner segment drains fully");
    fails([&] { path.append({2, 2}, 1); }, "Released stroke rejects additional mouse input");
    path.cancel();
    check(!path.started() && path.empty() && !path.lastConsumed(), "Cancel clears all pending state");
    path.begin({9, 11}, 2);
    path.append({9, 11}, 2);
    check(path.pendingSamples() == 1, "Repeated stationary input does not accumulate stamps");
    path.release();
    path.consume(1, [&](const PaintStrokeSample& sample) {
        check(same(sample.point, {9, 11}), "A mouse click paints its initial point exactly once");
    });
    check(path.drained(), "Single-point stroke completes on release");

    PaintStrokePath liveInput;
    std::vector<PaintStrokePoint> livePoints;
    auto liveEmit = [&](const PaintStrokeSample& sample) { livePoints.push_back(sample.point); };
    liveInput.begin({0, 0}, 10);
    liveInput.append({100, 0}, 10);
    liveInput.consume(3, liveEmit);
    liveInput.append({100, 50}, 2);
    liveInput.release();
    while (!liveInput.drained())
        liveInput.consume(4, liveEmit);
    check(livePoints.size() == 36 && same(livePoints.back(), {100, 50}),
          "New input appended while a previous segment is partial retains both sampling densities");
    check(std::all_of(livePoints.begin(), livePoints.end(),
                      [](auto p) { return std::abs(p.y) < 1e-9 || std::abs(p.x - 100) < 1e-9; }),
          "An appended corner starts from the queued input endpoint, not the consumed position");
}
void limitsAndExceptions() {
    PaintStrokePath lazy;
    lazy.begin({2, 4}, 1, 17);
    const auto waiting = lazy.peek();
    lazy.append({8, 4}, 2, 18);
    lazy.release();
    check(waiting && same(waiting->point, {2, 4}) && waiting->context == 17,
          "Lazy texture loading peeks the first hit without consuming it");
    check(lazy.pendingSamples() == 4 && !lazy.lastConsumed() && same(lazy.peek()->point, waiting->point),
          "New input and release during loading retain the paused sample");
    std::vector<PaintStrokePoint> loaded;
    while (!lazy.drained()) {
        auto expected = lazy.peek();
        lazy.consume(1, [&](const PaintStrokeSample& sample) {
            check(expected && same(expected->point, sample.point) && expected->context == sample.context,
                  "Peek exactly matches the sample consumed after texture loading");
            loaded.push_back(sample.point);
        });
    }
    check(!lazy.peek() && loaded.size() == 4 && same(loaded.back(), {8, 4}),
          "Delayed stroke finishes at the released endpoint without lost samples");
    PaintStrokePath samples({16, 8});
    samples.begin({0, 0}, 1);
    samples.append({4, 0}, 1);
    fails([&] { samples.append({8, 0}, 1); }, "Sample backlog overflow is explicit");
    check(samples.pendingSamples() == 5 && same(samples.lastInput(), {4, 0}),
          "Rejected input leaves previous path and endpoint unchanged");
    samples.cancel();
    check(samples.empty() && samples.pendingSamples() == 0, "Caller can cancel entire overflowing stroke");
    PaintStrokePath inputs({2, 100});
    inputs.begin({0, 0}, 1);
    inputs.append({1, 0}, 1);
    fails([&] { inputs.append({1, 1}, 1); }, "Input corner queue has a separate bound");
    check(inputs.queuedInputs() == 2, "Input limit does not silently replace an earlier corner");
    fails([&] { inputs.append({1, 1}, 0); }, "Zero spacing is rejected");
    fails([&] { inputs.append({std::numeric_limits<double>::infinity(), 0}, 1); },
          "Nonfinite cursor coordinates are rejected");
    fails([&] { inputs.begin({4, 4}, 1); }, "Beginning a new stroke cannot discard undrained input");
    PaintStrokePath emission;
    emission.begin({0, 0}, 1);
    emission.append({5, 0}, 1);
    emission.consume(2, [](const PaintStrokeSample&) {});
    auto previous = emission.lastConsumed()->point;
    auto pending = emission.pendingSamples();
    fails(
        [&] {
            emission.consume(1, [](const PaintStrokeSample&) { throw std::runtime_error("brush failure"); });
        },
        "Brush callback error propagates to the caller");
    check(emission.pendingSamples() == pending && same(emission.lastConsumed()->point, previous),
          "Failed sample is not marked consumed before rollback");
    emission.cancel();
    check(emission.consume(8, [](const PaintStrokeSample&) {}) == 0,
          "Canceled stroke never emits delayed pixels");
}
} // namespace
int main() {
    try {
        fastDragAndBudget();
        cornersAndRelease();
        limitsAndExceptions();
        std::cout << "Paint stroke path: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
