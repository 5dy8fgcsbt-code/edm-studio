#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>

namespace edm {

struct PaintStrokePoint {
    double x = 0, y = 0;
};
struct PaintStrokeSample {
    PaintStrokePoint point;
    // Opaque caller-owned camera/pose identifier, retained while a segment waits for a later frame.
    uint64_t context = 0;
};
struct PaintStrokePathLimits {
    size_t queuedInputs = 4096;
    uint64_t pendingSamples = 65536;
};

// A screen-space polyline resampler. Every input corner and the final endpoint is retained.
// The callback performs raycasting/painting; it must not re-enter or mutate this queue.
// A released stroke remains pending until drained(), so the caller can finish its undo transaction
// only after all frames have processed the remaining samples.
class PaintStrokePath {
    struct Segment {
        PaintStrokePoint from, to;
        uint64_t context = 0, samples = 0, next = 1;
    };
    PaintStrokePathLimits limits_;
    std::deque<Segment> queue_;
    std::optional<PaintStrokeSample> lastConsumed_;
    PaintStrokePoint lastInput_;
    uint64_t pending_ = 0;
    bool started_ = false, released_ = false;

    static void validate(PaintStrokePoint point, double spacing) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(spacing) || spacing <= 0)
            throw std::invalid_argument("无效的画笔轨迹坐标或采样间距");
    }
    void checkRoom(uint64_t samples) const {
        if (queue_.size() >= limits_.queuedInputs || samples > limits_.pendingSamples - pending_)
            throw std::length_error("画笔轨迹积压过多，请取消此笔后缩放视角、增大笔刷或降低移动速度");
    }

  public:
    using Consumer = std::function<void(const PaintStrokeSample&)>;
    using YieldPredicate = std::function<bool()>;

    explicit PaintStrokePath(PaintStrokePathLimits limits = {}) : limits_(limits) {
        if (!limits_.queuedInputs || !limits_.pendingSamples || limits_.pendingSamples > 1000000)
            throw std::invalid_argument("画笔轨迹队列预算无效（待处理样点必须在 1 到 1000000 之间）");
    }
    bool started() const {
        return started_;
    }
    bool released() const {
        return started_ && released_;
    }
    bool drained() const {
        return started_ && released_ && queue_.empty();
    }
    bool empty() const {
        return queue_.empty();
    }
    uint64_t pendingSamples() const {
        return pending_;
    }
    size_t queuedInputs() const {
        return queue_.size();
    }
    const std::optional<PaintStrokeSample>& lastConsumed() const {
        return lastConsumed_;
    }
    PaintStrokePoint lastInput() const {
        return lastInput_;
    }

    void begin(PaintStrokePoint point, double spacing, uint64_t context = 0) {
        validate(point, spacing);
        if (started_ && !drained())
            throw std::logic_error("上一笔画笔轨迹尚未完成");
        queue_.push_back({point, point, context, 1, 1});
        lastConsumed_.reset();
        lastInput_ = point;
        pending_ = 1;
        started_ = true;
        released_ = false;
    }

    // spacing is the maximum separation in screen pixels for this input segment. For a world-space
    // brush the caller can derive it from projected brush radius (e.g. 0.35 * radiusInPixels).
    // On overflow/invalid input the previously queued path remains unchanged. Do not ignore the error:
    // cancel the complete canvas transaction and this path, then show the failure to the user.
    void append(PaintStrokePoint point, double spacing, uint64_t context = 0) {
        validate(point, spacing);
        if (!started_ || released_)
            throw std::logic_error("只能向尚未释放的画笔轨迹追加输入");
        const double distance = std::hypot(point.x - lastInput_.x, point.y - lastInput_.y);
        if (!std::isfinite(distance))
            throw std::invalid_argument("画笔轨迹距离超限");
        if (distance <= 1e-9)
            return;
        const double count = std::ceil(distance / spacing);
        if (!std::isfinite(count) || count > double(limits_.pendingSamples))
            throw std::length_error("画笔采样过密，请缩放视角或增大笔刷");
        const uint64_t samples = uint64_t(std::max(1., count));
        checkRoom(samples);
        queue_.push_back({lastInput_, point, context, samples, 1});
        pending_ += samples;
        lastInput_ = point;
    }

    void release() {
        if (started_)
            released_ = true;
    }

    // Consumes only completed callbacks. Budget checks happen before each sample; yielding never
    // discards a segment, its endpoint, or any intervening corner. A single callback cannot be preempted.
    // Example: consume(32, stamp, [&] { return seconds(frameStart) >= .012; });
    std::optional<PaintStrokeSample> peek() const {
        if (queue_.empty())
            return {};
        const auto& segment = queue_.front();
        if (segment.next == segment.samples)
            return PaintStrokeSample{segment.to, segment.context};
        const double t = double(segment.next) / double(segment.samples);
        return PaintStrokeSample{
            {std::lerp(segment.from.x, segment.to.x, t), std::lerp(segment.from.y, segment.to.y, t)},
            segment.context};
    }
    size_t consume(size_t maxSamples, const Consumer& emit, const YieldPredicate& shouldYield = {}) {
        size_t consumed = 0;
        while (consumed < maxSamples && !queue_.empty()) {
            if (shouldYield && shouldYield())
                break;
            auto& segment = queue_.front();
            PaintStrokeSample sample;
            sample.context = segment.context;
            if (segment.next == segment.samples)
                sample.point = segment.to; // Preserve the exact input endpoint, avoiding roundoff drift.
            else {
                const double t = double(segment.next) / double(segment.samples);
                sample.point = {std::lerp(segment.from.x, segment.to.x, t),
                                std::lerp(segment.from.y, segment.to.y, t)};
            }
            emit(sample);
            lastConsumed_ = sample;
            --pending_;
            ++consumed;
            if (++segment.next > segment.samples)
                queue_.pop_front();
        }
        return consumed;
    }

    void cancel() {
        queue_.clear();
        pending_ = 0;
        started_ = released_ = false;
        lastConsumed_.reset();
        lastInput_ = {};
    }
};

} // namespace edm
