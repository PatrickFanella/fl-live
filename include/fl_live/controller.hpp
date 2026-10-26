#pragma once
#include "fl_live/engine.hpp"
#include <algorithm>
#include <atomic>
#include <type_traits>

namespace fl_live {
template<class T, std::size_t Capacity> class SpscQueue {
    static_assert(Capacity > 0);
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
public:
    bool push(T value) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = (head + 1) % slots_.size();
        if (next == tail_.load(std::memory_order_acquire)) return false;
        slots_[head] = value;
        head_.store(next, std::memory_order_release);
        return true;
    }
    bool pop(T& value) noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        value = slots_[tail];
        tail_.store((tail + 1) % slots_.size(), std::memory_order_release);
        return true;
    }
private:
    std::array<T, Capacity + 1> slots_{};
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

enum class ControlKind { start, stop, rewind, schedule };
struct Control { ControlKind kind; Command command{}; std::uint64_t serial = 0; };
struct RenderStatus { Frame position; bool running; std::uint64_t handled_serial; std::uint64_t rejected; };

// Exactly one control producer and one rendering consumer. The immutable plan
// outlives both. Do not call Engine directly while a backend owns rendering.
class RenderController {
public:
    explicit RenderController(const SessionPlan& plan) : engine_(plan), sample_rate_(plan.sample_rate) {}
    [[nodiscard]] std::uint32_t sample_rate() const noexcept { return sample_rate_; }
    bool submit(Control control) noexcept { return queue_.push(control); }
    void panic() noexcept { panic_.store(true); } // out-of-band, even when queue is full
    void process(std::span<float> output) noexcept {
        Control control{};
        if (panic_.exchange(false)) {
            engine_.stop();
            for (std::size_t i = 0; i < Engine::command_capacity && queue_.pop(control); ++i)
                handled_ = control.serial;
            std::fill(output.begin(), output.end(), 0);
        } else {
            // Bound work even if the producer continuously refills the queue.
            for (std::size_t i = 0; i < Engine::command_capacity && queue_.pop(control); ++i) {
                switch (control.kind) {
                case ControlKind::start: engine_.start(); break;
                case ControlKind::stop: engine_.stop(); break;
                case ControlKind::rewind: engine_.rewind(); break;
                case ControlKind::schedule:
                    if (engine_.schedule(control.command) != ScheduleResult::accepted) ++rejected_;
                    break;
                default: ++rejected_; break;
                }
                handled_ = control.serial;
            }
            engine_.process(output);
        }
        publish();
    }
    [[nodiscard]] RenderStatus status() const noexcept {
        for (;;) {
            const auto first = sequence_.load();
            if (first & 1U) continue;
            RenderStatus value{position_.load(), running_.load(), serial_.load(), rejected_count_.load()};
            if (sequence_.load() == first) return value;
        }
    }
    // Call only after the backend processing thread is joined.
    void finish() noexcept {
        engine_.stop();
        Control control{};
        for (std::size_t i = 0; i < Engine::command_capacity && queue_.pop(control); ++i)
            handled_ = control.serial;
        panic_.store(false);
        publish();
    }
private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    Engine engine_;
    const std::uint32_t sample_rate_;
    SpscQueue<Control, Engine::command_capacity> queue_;
    std::atomic<bool> panic_{false};
    std::uint64_t handled_ = 0, rejected_ = 0;
    std::atomic<std::uint64_t> sequence_{0}, serial_{0}, rejected_count_{0}, position_{0};
    std::atomic<bool> running_{false};
    void publish() noexcept {
        // All fields are atomic; seq_cst order gives a coherent snapshot.
        // Readers retry, but never make the rendering writer wait.
        sequence_.fetch_add(1);
        position_.store(engine_.position());
        running_.store(engine_.running());
        serial_.store(handled_);
        rejected_count_.store(rejected_);
        sequence_.fetch_add(1);
    }
};
} // namespace fl_live
