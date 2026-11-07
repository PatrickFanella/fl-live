#pragma once
#include "fl_live/engine.hpp"
#include <algorithm>
#include <atomic>
#include <memory>
#include <stdexcept>
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
    // Track and clip indexes refer to this plan generation (see
    // RenderController::generation). A schedule, launch, stop or scene
    // control for another generation is rejected; start, stop and rewind of
    // the transport apply to any.
    std::uint64_t generation = 0;
};
// A replacement plan prepared off the render thread. It owns its plan;
// mapping indexes the plan it replaces. While the transport runs it
// activates exactly before transport frame `at` (or at the next block when
// `at` has passed); while stopped, at the next block.
struct PlanReplacement {
    SessionPlan plan;
    PlanMapping mapping;
    Frame at = 0;
};
enum class OfferResult { accepted, busy, invalid };
// What collect() hands back once the render owner has activated an offer:
// the replacement it retired, or null when that was the constructor's plan.
struct Retirement {
    bool acknowledged = false;
    std::unique_ptr<PlanReplacement> retired;
};
struct RenderStatus {
    Frame position;
    bool running;
    std::uint64_t handled_serial;
    std::uint64_t rejected;
    std::size_t track_count = 0;
    std::uint64_t generation = 0;       // plan replacements activated
    std::uint64_t dropped_commands = 0; // queued commands removed by replacements
    std::array<TrackStatus, Engine::track_capacity> tracks{};
};

// Exactly one control producer and one rendering consumer. The constructor's
// immutable plan outlives both. Do not call Engine directly while a backend
// owns rendering. Destroy only after the rendering thread is joined.
class RenderController {
public:
    explicit RenderController(const SessionPlan& plan) : engine_(plan), sample_rate_(plan.sample_rate), bpm_milli_(plan.bpm_milli),
        grid_(plan.sample_rate, plan.bpm_milli), latest_(&plan), track_count_(plan.tracks.size()) {
        published_tracks_.store(track_count_);
    }
    RenderController(const RenderController&) = delete;
    RenderController& operator=(const RenderController&) = delete;
    ~RenderController() {
        delete pending_.load();
        delete retired_.load();
        delete current_;
    }
    [[nodiscard]] std::uint32_t sample_rate() const noexcept { return sample_rate_; }
    bool submit(Control control) noexcept { return queue_.push(control); }
    // Control thread. The generation that new controls should carry: it
    // counts accepted offers, so controls built for an offered plan are
    // rejected until that plan is active.
    [[nodiscard]] std::uint64_t generation() const noexcept { return offered_; }
    // Control thread. Validates and hands one replacement to the render
    // owner. busy while an earlier offer is unactivated or its retirement is
    // uncollected; invalid (state unchanged) for a plan the engine refuses, a
    // mapping that does not fit the latest accepted plan, or another sample
    // rate or tempo.
    OfferResult offer(std::unique_ptr<PlanReplacement> next) {
        if (!next) return OfferResult::invalid;
        if (swaps_.load(std::memory_order_acquire) != offered_ || collected_ != offered_) return OfferResult::busy;
        if (next->plan.sample_rate != sample_rate_ || next->plan.bpm_milli != bpm_milli_) return OfferResult::invalid;
        try {
            Engine::validate(next->plan);
            Engine::validateMapping(*latest_, next->plan, next->mapping);
        } catch (const std::invalid_argument&) { return OfferResult::invalid; }
        latest_ = &next->plan;
        ++offered_;
        pending_.store(next.release(), std::memory_order_release);
        return OfferResult::accepted;
    }
    // Control thread. After the render owner activates an offer, returns it
    // acknowledged with ownership of the plan it retired, which the render
    // thread no longer references; destroy it here, outside rendering.
    Retirement collect() noexcept {
        if (swaps_.load(std::memory_order_acquire) == collected_) return {};
        ++collected_;
        return {true, std::unique_ptr<PlanReplacement>(retired_.exchange(nullptr, std::memory_order_acquire))};
    }
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
            adopt(); // nothing is playing; activate a pending plan now
            for (std::size_t i = 0; i < Engine::command_capacity && queue_.pop(control); ++i)
                handled_ = control.serial;
            std::fill(output.begin(), output.end(), 0);
        } else {
            if (const auto* next = pending_.load(std::memory_order_acquire);
                next && (!engine_.running() || next->at <= engine_.position())) adopt();
            // Bound work even if the producer continuously refills the queue.
            for (std::size_t i = 0; i < Engine::command_capacity && queue_.pop(control); ++i) {
                const bool indexed = control.kind != ControlKind::start && control.kind != ControlKind::stop && control.kind != ControlKind::rewind;
                if (indexed && control.generation != generation_) { ++rejected_; handled_ = control.serial; continue; }
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
            render(output, channels);
        }
        publish();
    }
    [[nodiscard]] RenderStatus status() const noexcept {
        for (;;) {
            const auto first = sequence_.load();
            if (first & 1U) continue;
            RenderStatus value{position_.load(), running_.load(), serial_.load(), rejected_count_.load()};
            value.track_count = published_tracks_.load();
            value.generation = generation_count_.load();
            value.dropped_commands = dropped_count_.load();
            for (std::size_t i = 0; i < value.track_count; ++i)
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
    const std::uint32_t bpm_milli_;
    const TempoGrid grid_;
    // Control side only.
    const SessionPlan* latest_;
    std::uint64_t offered_ = 0, collected_ = 0;
    // Handoff: control stores pending_; render takes it, stores the plan it
    // retires in retired_, then counts the swap. One offer is in flight at
    // a time, so retired_ is collected before it can be written again.
    std::atomic<PlanReplacement*> pending_{nullptr};
    std::atomic<PlanReplacement*> retired_{nullptr};
    std::atomic<std::uint64_t> swaps_{0};
    // Render side only.
    PlanReplacement* current_ = nullptr; // null while the constructor's plan is active
    std::uint64_t generation_ = 0, dropped_ = 0;
    std::size_t track_count_;
    void adopt() noexcept {
        auto* next = pending_.exchange(nullptr, std::memory_order_acquire);
        if (!next) return;
        dropped_ += engine_.replace(next->plan, next->mapping).dropped_commands;
        track_count_ = next->plan.tracks.size();
        retired_.store(current_, std::memory_order_relaxed);
        current_ = next;
        ++generation_;
        swaps_.fetch_add(1, std::memory_order_release);
    }
    // Activates a pending plan at its frame inside this block, if any.
    void render(std::span<float> output, std::size_t channels) noexcept {
        const auto* next = pending_.load(std::memory_order_acquire);
        const auto frames = output.size() / channels;
        if (next && engine_.running() && next->at > engine_.position() && next->at - engine_.position() < frames) {
            const auto split = static_cast<std::size_t>(next->at - engine_.position()) * channels;
            (void)engine_.process(output.first(split), channels);
            adopt();
            (void)engine_.process(output.subspan(split), channels);
        } else (void)engine_.process(output, channels);
    }
    struct AtomicTrack {
        std::atomic<int> active{-1}, pending{static_cast<int>(PendingAction::none)}, clip{-1};
        std::atomic<Frame> at{0};
    };
    static_assert(std::atomic<int>::is_always_lock_free);
    std::array<AtomicTrack, Engine::track_capacity> tracks_{};
    SpscQueue<Control, Engine::command_capacity> queue_;
    std::atomic<bool> panic_{false};
    std::uint64_t handled_ = 0, rejected_ = 0;
    std::atomic<std::uint64_t> sequence_{0}, serial_{0}, rejected_count_{0}, position_{0}, generation_count_{0}, dropped_count_{0};
    std::atomic<std::size_t> published_tracks_{0};
    std::atomic<bool> running_{false};
    void publish() noexcept {
        // All fields are atomic; seq_cst order gives a coherent snapshot.
        // Readers retry, but never make the rendering writer wait.
        sequence_.fetch_add(1);
        position_.store(engine_.position());
        running_.store(engine_.running());
        serial_.store(handled_);
        rejected_count_.store(rejected_);
        generation_count_.store(generation_);
        dropped_count_.store(dropped_);
        published_tracks_.store(track_count_);
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
