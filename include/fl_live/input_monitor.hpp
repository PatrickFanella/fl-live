#pragma once
#include "fl_live/capture.hpp"
#include <algorithm>
#include <array>
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

// MIDI input monitoring: the MIDI input callback keeps note messages in a
// fixed ring; the rendering thread applies them to every track in the mask,
// at the start of its next block, through Target::live() (RenderController).
// Changing the mask silences the live voices first, so a track that stops
// monitoring never keeps a note sounding.
class LiveMidi final : public MidiSink {
public:
    void cycle(std::uint64_t, std::uint32_t) noexcept override {}
    void midi(std::uint64_t, std::span<const std::uint8_t> message) noexcept override {
        if (message.size() < 3 || ((message[0] & 0xF0) != 0x80 && (message[0] & 0xF0) != 0x90)) return;
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = (head + 1) % ring_.size();
        if (next == tail_.load(std::memory_order_acquire)) { dropped_.fetch_add(1, std::memory_order_relaxed); return; }
        ring_[head] = {static_cast<std::uint8_t>(message[1] & 0x7F), static_cast<std::uint8_t>(message[2] & 0x7F),
                       (message[0] & 0xF0) == 0x90 && (message[2] & 0x7F) > 0};
        head_.store(next, std::memory_order_release);
    }
    // Rendering thread, before its block.
    template<class Target> void apply(Target& target) noexcept {
        if (silence_.exchange(false, std::memory_order_acq_rel)) target.silence_live();
        const auto tracks = tracks_.load(std::memory_order_acquire);
        auto tail = tail_.load(std::memory_order_relaxed);
        const auto head = head_.load(std::memory_order_acquire);
        for (; tail != head; tail = (tail + 1) % ring_.size()) {
            const auto& note = ring_[tail];
            for (std::size_t track = 0; track < 64; ++track)
                if (tracks >> track & 1U) target.live(track, note.pitch, note.velocity / 127.0F, note.on);
        }
        tail_.store(tail, std::memory_order_release);
    }
    // Control side: the tracks (bit per plan index) that play live input.
    void setTracks(std::uint64_t mask) noexcept {
        if (tracks_.exchange(mask, std::memory_order_acq_rel) != mask) silence_.store(true, std::memory_order_release);
    }
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_.load(); }
private:
    struct Note { std::uint8_t pitch, velocity; bool on; };
    std::array<Note, 257> ring_{};
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    std::atomic<std::uint64_t> tracks_{0}, dropped_{0};
    std::atomic<bool> silence_{false};
};
} // namespace fl_live
