#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace fl_live {
using Frame = std::uint64_t;

// Constant-tempo 4/4 grid. Boundaries round upward to the first sample at/after
// the exact rational boundary, without accumulating rounded bar lengths.
class TempoGrid {
public:
    TempoGrid(std::uint32_t sample_rate, std::uint32_t bpm_milli);
    [[nodiscard]] Frame next_bar(Frame at) const;
    [[nodiscard]] std::optional<Frame> try_next_bar(Frame at) const noexcept;
private:
    Frame numerator_;
    Frame denominator_;
};

struct AudioClip {
    std::vector<float> samples; // mono, at the session's rate; no resampling
    bool loop = true;
};
struct Note {
    Frame start;
    Frame duration;
    std::uint8_t pitch;
    float velocity;
};
struct MidiClip {
    Frame length;
    std::vector<Note> notes; // ordered by start; half-open durations
    bool loop = true;
};
using Clip = std::variant<AudioClip, MidiClip>;
struct Track {
    float gain = 1.0F;
    std::vector<Clip> clips;
};
struct SessionPlan {
    std::uint32_t sample_rate = 48000;
    std::uint32_t bpm_milli = 120000;
    std::vector<Track> tracks;
};

enum class CommandKind { launch, stop_track };
struct Command {
    CommandKind kind;
    Frame at;
    std::size_t track;
    std::size_t clip = 0;
};
enum class ScheduleResult { accepted, full, out_of_order, in_past, invalid };

// Single-owner engine: transport, scheduling and process are called on one
// rendering owner, at block boundaries. This is NOT a concurrent UI queue.
// The validated plan must remain alive and immutable for the engine lifetime.
class Engine {
public:
    static constexpr std::size_t command_capacity = 64;
    static constexpr std::size_t voices_per_track = 16;
    explicit Engine(const SessionPlan& plan);
    Engine(SessionPlan&&) = delete;
    void start() noexcept;
    void stop() noexcept; // freezes playhead, clears queue and all voices
    void rewind() noexcept; // stopped only; otherwise no operation
    [[nodiscard]] ScheduleResult schedule(Command command) noexcept;
    void process(std::span<float> mono_output) noexcept;
    [[nodiscard]] Frame position() const noexcept { return position_; }
    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] std::size_t active_voices() const noexcept;
private:
    struct Voice { Frame remaining = 0; double phase = 0; double step = 0; float level = 0; };
    struct Runtime {
        std::optional<std::size_t> clip;
        Frame launched = 0;
        std::size_t next_note = 0;
        std::array<Voice, voices_per_track> voices{};
    };
    const SessionPlan& plan_;
    std::vector<Runtime> runtime_;
    std::array<double, 128> steps_{};
    std::array<Command, command_capacity> commands_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    Frame position_ = 0;
    bool running_ = false;
    void apply(const Command& command) noexcept;
    [[nodiscard]] float render_track(std::size_t index) noexcept;
};
} // namespace fl_live
