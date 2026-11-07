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

// Source-rate samples; no resampling. One channel, or two interleaved as
// left/right. Frame count is samples.size() / channels.
struct AudioClip {
    std::vector<float> samples;
    bool loop = true;
    std::uint32_t channels = 1;
};
struct Note {
    Frame start;
    Frame duration;
    std::uint8_t pitch;
    float velocity;
};
// Built-in instrument of a MIDI clip. Naive (aliasing) oscillators; linear
// envelope segments in frames: attack 0->1, decay 1->sustain, hold at
// sustain until the note ends, then release from the current level to 0.
// Defaults reproduce the original fixed sine: no attack/decay/release.
enum class Waveform : std::uint8_t { sine, triangle, square, saw };
struct Instrument {
    Waveform waveform = Waveform::sine;
    Frame attack = 0;
    Frame decay = 0;
    float sustain = 1.0F;
    Frame release = 0;
    bool operator==(const Instrument&) const = default;
};
struct MidiClip {
    Frame length;
    std::vector<Note> notes; // ordered by start; half-open durations
    bool loop = true;
    Instrument instrument{};
};
using Clip = std::variant<AudioClip, MidiClip>;
// Automation breakpoint at a transport frame (frames since the song start).
// Values between points are linear in frames; before the first point and
// after the last the end value holds. Frames must strictly increase.
struct AutomationPoint {
    Frame at;
    float value;
    bool operator==(const AutomationPoint&) const = default;
};
// Balance pan law, pan in [-1,1]: the far channel is attenuated linearly and
// the near channel stays at unity, so center is 0 dB on both channels. Mono
// sources and the MIDI instrument feed both channels; stereo sources keep
// their left/right channels.
// A non-empty automation list replaces the static gain or pan.
// device_gain is the compiled product of the track's gain devices, applied
// with the gain (static or automated); it is not replaced by automation.
struct Track {
    float gain = 1.0F;
    std::vector<Clip> clips;
    float pan = 0.0F;
    std::vector<AutomationPoint> gain_automation;
    std::vector<AutomationPoint> pan_automation;
    float device_gain = 1.0F;
    // Device automation (docs/devices.md). A non-empty gain lane replaces
    // device_gain (factor 0..16). A transpose lane (semitones -48..48) is read
    // at each note-on, rounded to the nearest semitone and added to the note's
    // pitch; a note it pushes outside 0..127 gets no voice.
    std::vector<AutomationPoint> device_gain_automation;
    std::vector<AutomationPoint> transpose_automation;
};
struct SessionPlan {
    std::uint32_t sample_rate = 48000;
    std::uint32_t bpm_milli = 120000;
    std::vector<Track> tracks;
};
// How engine state carries into a replacement plan. Both vectors index the
// plan being replaced: track_from_old[t] is the new index of old track t, or
// -1 when it was deleted; clip_from_old[t][c] is the new index of its clip c
// within that new track, or -1. New tracks may not share an old source.
struct PlanMapping {
    std::vector<std::int32_t> track_from_old;
    std::vector<std::vector<std::int32_t>> clip_from_old;
};
struct ReplaceResult {
    std::size_t dropped_commands = 0; // queued commands whose track or clip was removed
    std::size_t stopped_tracks = 0;   // playing tracks whose clip was removed or changed kind
};

enum class CommandKind { launch, stop_track };
struct Command {
    CommandKind kind;
    Frame at;
    std::size_t track;
    std::size_t clip = 0;
};
enum class ScheduleResult { accepted, full, out_of_order, in_past, invalid };
enum class PendingAction { none, launch, stop };
struct TrackStatus {
    int active_clip = -1;
    PendingAction pending = PendingAction::none;
    int pending_clip = -1;
    Frame pending_at = 0;
};

// Single-owner engine: transport, scheduling and process are called on one
// rendering owner, at block boundaries. This is NOT a concurrent UI queue.
// The validated plan must remain alive and immutable until the engine is
// destroyed or has replaced it.
class Engine {
public:
    static constexpr std::size_t command_capacity = 64;
    static constexpr std::size_t track_capacity = 64;
    static constexpr std::size_t clips_per_track_capacity = 64;
    static constexpr std::size_t notes_per_clip_capacity = 1024;
    static constexpr std::size_t voices_per_track = 16;
    static constexpr std::size_t max_output_channels = 2;
    static constexpr std::size_t automation_points_capacity = 4096;
    static constexpr Frame envelope_frame_limit = 480000; // each segment, 10 s at 48 kHz
    // Declared interpolation, shared by rendering and tests.
    [[nodiscard]] static float automationValue(std::span<const AutomationPoint> points, Frame at, std::size_t& cursor) noexcept;
    // Throws std::invalid_argument for a plan the engine cannot render.
    static void validate(const SessionPlan& plan);
    // Throws std::invalid_argument unless mapping fits from -> to and both
    // share a sample rate. Call on the preparing thread, never while rendering.
    static void validateMapping(const SessionPlan& from, const SessionPlan& to, const PlanMapping& mapping);
    explicit Engine(const SessionPlan& plan);
    Engine(SessionPlan&&) = delete;
    // Render owner, between frames. next and mapping were validated against
    // the current plan and next outlives its use here. A playing clip that
    // maps to a clip of the same kind keeps its launch time, so audio
    // continues at the same position and sounding MIDI voices keep their
    // pitch and remaining duration under the new instrument; the note cursor
    // moves to the first note at or after the current clip position. Other
    // playing tracks stop. Queued commands are remapped in order or dropped.
    // Automation restarts its cursors. No allocation, locking or destruction;
    // the previous plan is no longer referenced on return.
    ReplaceResult replace(const SessionPlan& next, const PlanMapping& mapping) noexcept;
    [[nodiscard]] const SessionPlan& plan() const noexcept { return *plan_; }
    void start() noexcept;
    void stop() noexcept; // freezes playhead, clears queue and all voices
    void rewind() noexcept; // stopped only; otherwise no operation
    [[nodiscard]] ScheduleResult schedule(Command command) noexcept;
    // All commands share one sample. Admission is all-or-nothing, including
    // capacity and validation. Insert before later events, after existing ties.
    [[nodiscard]] ScheduleResult scheduleTogether(std::span<const Command> commands) noexcept;
    // Mono output is the explicit (left + right) / 2 downmix of the stereo mix.
    void process(std::span<float> mono_output) noexcept;
    // One channel (mono downmix) or two interleaved left/right channels. Any
    // other channel count, or a buffer that is not whole frames, is silenced
    // without advancing the playhead or applying commands; returns false.
    [[nodiscard]] bool process(std::span<float> output, std::size_t channels) noexcept;
    [[nodiscard]] Frame position() const noexcept { return position_; }
    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] std::size_t active_voices() const noexcept;
    [[nodiscard]] std::array<TrackStatus, track_capacity> track_statuses() const noexcept;
private:
    struct Voice {
        Frame remaining = 0;     // held frames left before release
        double phase = 0;
        double step = 0;
        float level = 0;         // velocity scale
        Frame age = 0;           // frames since note start
        Frame released = 0;      // frames into the release
        float release_from = 0;  // envelope level when the release began
        bool releasing = false;
        bool active = false;
    };
    [[nodiscard]] static float heldEnvelope(const Instrument& instrument, Frame age) noexcept;
    struct Runtime {
        std::optional<std::size_t> clip;
        Frame launched = 0;
        std::size_t next_note = 0;
        std::array<Voice, voices_per_track> voices{};
    };
    const SessionPlan* plan_;
    // Sized for track_capacity at construction so replacement never allocates;
    // entries past the plan's track count stay idle.
    std::vector<Runtime> runtime_;
    std::vector<Runtime> spare_;
    std::vector<std::array<std::size_t, 4>> automation_cursor_; // per track: gain, pan, device gain, transpose
    std::array<double, 128> steps_{};
    std::array<Command, command_capacity> commands_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    Frame position_ = 0;
    bool running_ = false;
    void apply(const Command& command) noexcept;
    struct StereoSample { float left = 0; float right = 0; };
    [[nodiscard]] StereoSample render_track(std::size_t index) noexcept;
};
struct CommandBatch {
    std::array<Command, Engine::track_capacity> commands{};
    std::size_t count = 0;
};
} // namespace fl_live
