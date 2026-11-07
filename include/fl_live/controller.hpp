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

enum class ControlKind { start, stop, rewind, schedule, launch_next_bar, stop_next_bar, scene };
enum class LaunchPolicy { immediate, next_bar };
struct Control {
    ControlKind kind;
    Command command{};
    std::uint64_t serial = 0;
    CommandBatch batch{};
    LaunchPolicy policy = LaunchPolicy::next_bar;
};
struct RenderStatus {
    Frame position;
    bool running;
    std::uint64_t handled_serial;
    std::uint64_t rejected;
    std::size_t track_count = 0;
    std::array<TrackStatus, Engine::track_capacity> tracks{};
};

// Exactly one control producer and one rendering consumer. The immutable plan
// outlives both. Do not call Engine directly while a backend owns rendering.
class RenderController {
public:
    explicit RenderController(const SessionPlan& plan) : engine_(plan), sample_rate_(plan.sample_rate), grid_(plan.sample_rate, plan.bpm_milli), track_count_(plan.tracks.size()) {}
    [[nodiscard]] std::uint32_t sample_rate() const noexcept { return sample_rate_; }
    bool submit(Control control) noexcept { return queue_.push(control); }
    void panic() noexcept { panic_.store(true); } // out-of-band, even when queue is full
    // channels is 1 (mono downmix) or 2 (interleaved left/right). Unsupported
    // layouts render silence and apply no commands; see Engine::process.
    void process(std::span<float> output, std::size_t channels = 1) noexcept {
        Control control{};
        if (channels < 1 || channels > Engine::max_output_channels || output.size() % channels != 0) {
            std::fill(output.begin(), output.end(), 0);
            publish();
            return;
        }
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
                case ControlKind::launch_next_bar:
                case ControlKind::stop_next_bar:
                    // Resolve time on the render owner, never from a stale UI snapshot.
                    if (const auto boundary = grid_.try_next_bar(engine_.position())) {
                        control.command.at = *boundary;
                        control.command.kind = control.kind == ControlKind::launch_next_bar
                            ? CommandKind::launch : CommandKind::stop_track;
                        if (engine_.schedule(control.command) != ScheduleResult::accepted) ++rejected_;
                    } else ++rejected_;
                    break;
                case ControlKind::scene: {
                    // One queue message, one render-owned time, one admission.
                    // No caller timestamps or UI playhead estimates are used.
                    if (!control.batch.count || control.batch.count > control.batch.commands.size() ||
                        (control.policy != LaunchPolicy::immediate && control.policy != LaunchPolicy::next_bar)) {
                        ++rejected_; break;
                    }
                    const auto at = control.policy == LaunchPolicy::immediate
                        ? std::optional<Frame>{engine_.position()} : grid_.try_next_bar(engine_.position());
                    if (!at) { ++rejected_; break; }
                    auto commands = std::span(control.batch.commands).first(control.batch.count);
                    for (auto& command : commands) command.at = *at;
                    if (engine_.scheduleTogether(commands) != ScheduleResult::accepted) ++rejected_;
                    break;
                }
                default: ++rejected_; break;
                }
                handled_ = control.serial;
            }
            (void)engine_.process(output, channels);
        }
        publish();
    }
    [[nodiscard]] RenderStatus status() const noexcept {
        for (;;) {
            const auto first = sequence_.load();
            if (first & 1U) continue;
            RenderStatus value{position_.load(), running_.load(), serial_.load(), rejected_count_.load()};
            value.track_count = track_count_;
            for (std::size_t i = 0; i < track_count_; ++i)
                value.tracks[i] = {tracks_[i].active.load(), static_cast<PendingAction>(tracks_[i].pending.load()),
                    tracks_[i].clip.load(), tracks_[i].at.load()};
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
    const TempoGrid grid_;
    const std::size_t track_count_;
    struct AtomicTrack {
        std::atomic<int> active{-1}, pending{static_cast<int>(PendingAction::none)}, clip{-1};
        std::atomic<Frame> at{0};
    };
    static_assert(std::atomic<int>::is_always_lock_free);
    std::array<AtomicTrack, Engine::track_capacity> tracks_{};
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
        const auto tracks = engine_.track_statuses();
        for (std::size_t i = 0; i < track_count_; ++i) {
            tracks_[i].active.store(tracks[i].active_clip);
            tracks_[i].pending.store(static_cast<int>(tracks[i].pending));
            tracks_[i].clip.store(tracks[i].pending_clip);
            tracks_[i].at.store(tracks[i].pending_at);
        }
        sequence_.fetch_add(1);
    }
};
} // namespace fl_live
