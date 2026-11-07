#pragma once
#include "fl_live/capture.hpp"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fl_live {
struct MonitorStatus {
    std::uint64_t mixed = 0;     // frames mixed into the output
    std::uint64_t underruns = 0; // audible output cycles that found fewer input frames than they needed
    std::uint64_t overruns = 0;  // input blocks refused by the full ring
    std::uint64_t skipped = 0;   // input frames dropped to keep latency bounded
};
// Software input monitoring (docs/recording.md). The input callback pushes
// interleaved stereo blocks; the output callback adds them to its own output
// at the monitor gains, after the engine has rendered. Both sides are
// lock-free and allocation-free on a ring sized at construction. When more
// than two output cycles of input wait, the oldest are dropped so the
// monitoring delay cannot grow. With both gains at zero the input is still
// consumed, so turning monitoring on never plays stale audio.
class InputMonitor final : public CaptureSink {
public:
    explicit InputMonitor(std::size_t frames = 8192) : ring_((frames + 1) * 2) {}
    // Input callback: one stereo block, all or nothing.
    void capture(std::span<const float> interleaved) noexcept override {
        const auto size = ring_.size();
        const auto head = head_.load(std::memory_order_relaxed);
        const auto tail = tail_.load(std::memory_order_acquire);
        const auto used = (head + size - tail) % size;
        if (interleaved.size() % 2 || interleaved.size() > size - 2 - used) { overruns_.fetch_add(1, std::memory_order_relaxed); return; }
        for (std::size_t i = 0; i < interleaved.size(); ++i) ring_[(head + i) % size] = interleaved[i];
        head_.store((head + interleaved.size()) % size, std::memory_order_release);
    }
    // Output callback: adds up to one cycle of monitored input to `output`
    // (one channel: (left + right) / 2, the engine's downmix; two: left, right).
    void mix(std::span<float> output, std::size_t channels) noexcept {
        if (channels < 1 || channels > 2) return;
        const auto size = ring_.size();
        auto tail = tail_.load(std::memory_order_relaxed);
        const auto head = head_.load(std::memory_order_acquire);
        auto available = ((head + size - tail) % size) / 2;
        const auto frames = output.size() / channels;
        if (available > 2 * frames) { // bound the delay: keep one cycle
            const auto drop = available - frames;
            tail = (tail + drop * 2) % size;
            skipped_.fetch_add(drop, std::memory_order_relaxed);
            available = frames;
        }
        const float left_gain = left_gain_.load(std::memory_order_relaxed), right_gain = right_gain_.load(std::memory_order_relaxed);
        const auto count = std::min(available, frames);
        if (left_gain != 0 || right_gain != 0) {
            for (std::size_t f = 0; f < count; ++f) {
                const float left = left_gain * ring_[(tail + 2 * f) % size], right = right_gain * ring_[(tail + 2 * f + 1) % size];
                if (channels == 2) { output[2 * f] += left; output[2 * f + 1] += right; }
                else output[f] += (left + right) / 2;
            }
            mixed_.fetch_add(count, std::memory_order_relaxed);
            if (count < frames) underruns_.fetch_add(1, std::memory_order_relaxed);
        }
        tail_.store((tail + count * 2) % size, std::memory_order_release);
    }
    // Control side.
    void setGain(float left, float right) noexcept {
        left_gain_.store(left, std::memory_order_relaxed);
        right_gain_.store(right, std::memory_order_relaxed);
    }
    [[nodiscard]] MonitorStatus status() const noexcept {
        return {mixed_.load(), underruns_.load(), overruns_.load(), skipped_.load()};
    }
private:
    std::vector<float> ring_;
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    std::atomic<float> left_gain_{0}, right_gain_{0};
    std::atomic<std::uint64_t> mixed_{0}, underruns_{0}, overruns_{0}, skipped_{0};
};
} // namespace fl_live
