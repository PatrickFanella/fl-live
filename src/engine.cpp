#include "fl_live/engine.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <type_traits>

namespace fl_live {
namespace {
std::optional<Frame> ceil_mul_div(Frame value, Frame multiplier, Frame divisor) noexcept {
    // Quotient/remainder split keeps supported clock products exact in uint64.
    const auto q = value / divisor;
    const auto r = value % divisor;
    const auto limit = std::numeric_limits<Frame>::max();
    if (q > limit / multiplier || (r != 0 && multiplier > limit / r))
        return std::nullopt;
    const auto product = r * multiplier;
    const auto tail = product / divisor + (product % divisor != 0 ? 1U : 0U);
    const auto base = q * multiplier;
    if (base > limit - tail) return std::nullopt;
    return base + tail;
}

void validate(const SessionPlan& plan) {
    (void)TempoGrid(plan.sample_rate, plan.bpm_milli);
    if (plan.tracks.size() > Engine::track_capacity) throw std::invalid_argument("at most 64 tracks in engine slice");
    for (const auto& track : plan.tracks) {
        if (!std::isfinite(track.gain) || track.gain < 0 || track.gain > 4)
            throw std::invalid_argument("track gain must be finite in [0,4]");
        if (track.clips.size() > Engine::clips_per_track_capacity) throw std::invalid_argument("at most 64 clips per track");
        for (const auto& clip : track.clips) {
            if (const auto* audio = std::get_if<AudioClip>(&clip)) {
                if (audio->samples.empty() || !std::all_of(audio->samples.begin(), audio->samples.end(),
                        [](float x) { return std::isfinite(x); }))
                    throw std::invalid_argument("audio clip must contain finite samples");
            } else {
                const auto& midi = std::get<MidiClip>(clip);
                if (midi.length == 0 || midi.notes.size() > Engine::notes_per_clip_capacity)
                    throw std::invalid_argument("invalid MIDI length or note count");
                Frame previous = 0;
                for (const auto& note : midi.notes) {
                    if (note.start < previous || note.start >= midi.length || note.duration == 0 ||
                        note.duration > midi.length - note.start || note.pitch > 127 ||
                        !std::isfinite(note.velocity) || note.velocity < 0 || note.velocity > 1)
                        throw std::invalid_argument("invalid or unordered MIDI note");
                    previous = note.start;
                    const auto overlapping = std::count_if(midi.notes.begin(), midi.notes.end(),
                        [&](const Note& n) { return n.start <= note.start &&
                            n.duration > note.start - n.start; });
                    if (overlapping > static_cast<std::ptrdiff_t>(Engine::voices_per_track))
                        throw std::invalid_argument("MIDI clip exceeds 16 simultaneous voices");
                }
            }
        }
    }
}
} // namespace

TempoGrid::TempoGrid(std::uint32_t sample_rate, std::uint32_t bpm_milli)
    : numerator_(static_cast<Frame>(sample_rate) * 240000U), denominator_(bpm_milli) {
    if (sample_rate < 8000 || sample_rate > 384000 || bpm_milli < 20000 || bpm_milli > 300000)
        throw std::invalid_argument("supported sample rate 8000..384000; tempo 20..300 BPM");
}

Frame TempoGrid::next_bar(Frame at) const {
    const auto result = try_next_bar(at);
    if (!result) throw std::overflow_error("musical clock exceeds frame range");
    return *result;
}

std::optional<Frame> TempoGrid::try_next_bar(Frame at) const noexcept {
    // k = floor((at-1)*D/N)+1 is the first k whose ceil(k*N/D) >= at.
    // Using ceil(at*D/N) would skip a rounded-up, nonintegral boundary.
    if (at == 0) return 0;
    const auto value = at - 1;
    const auto q = value / numerator_;
    const auto r = value % numerator_;
    if (q > (std::numeric_limits<Frame>::max() - 1) / denominator_)
        return std::nullopt;
    const auto k = q * denominator_ + r * denominator_ / numerator_ + 1;
    return ceil_mul_div(k, numerator_, denominator_);
}

Engine::Engine(const SessionPlan& plan) : plan_(plan) {
    validate(plan);
    runtime_.resize(plan.tracks.size());
    for (std::size_t pitch = 0; pitch < steps_.size(); ++pitch)
        steps_[pitch] = 440.0 * std::exp2((static_cast<double>(pitch) - 69.0) / 12.0) / plan.sample_rate;
}
void Engine::start() noexcept { running_ = true; }
void Engine::stop() noexcept {
    running_ = false;
    count_ = 0;
    head_ = 0;
    for (auto& state : runtime_) state = Runtime{};
}
void Engine::rewind() noexcept { if (!running_) position_ = 0; }

ScheduleResult Engine::schedule(Command command) noexcept {
    if (command.kind != CommandKind::launch && command.kind != CommandKind::stop_track)
        return ScheduleResult::invalid;
    if (command.track >= plan_.tracks.size() ||
        (command.kind == CommandKind::launch && command.clip >= plan_.tracks[command.track].clips.size()))
        return ScheduleResult::invalid;
    if (command.at < position_) return ScheduleResult::in_past;
    if (count_ == command_capacity) return ScheduleResult::full;
    if (count_ && command.at < commands_[(head_ + count_ - 1) % command_capacity].at)
        return ScheduleResult::out_of_order;
    commands_[(head_ + count_) % command_capacity] = command;
    ++count_;
    return ScheduleResult::accepted;
}
void Engine::apply(const Command& command) noexcept {
    auto& state = runtime_[command.track];
    state = Runtime{};
    if (command.kind == CommandKind::launch) {
        state.clip = command.clip;
        state.launched = position_;
    }
}

float Engine::render_track(std::size_t index) noexcept {
    auto& state = runtime_[index];
    if (!state.clip) return 0;
    const auto& track = plan_.tracks[index];
    const auto& clip = track.clips[*state.clip];
    const auto age = position_ - state.launched;
    if (const auto* audio = std::get_if<AudioClip>(&clip)) {
        const auto length = audio->samples.size();
        if (!audio->loop && age >= length) { state = Runtime{}; return 0; }
        return track.gain * audio->samples[age % length];
    }
    const auto& midi = std::get<MidiClip>(clip);
    if (!midi.loop && age >= midi.length) { state = Runtime{}; return 0; }
    const auto local = age % midi.length;
    if (local == 0) { state.next_note = 0; state.voices = {}; }
    while (state.next_note < midi.notes.size() && midi.notes[state.next_note].start == local) {
        const auto& note = midi.notes[state.next_note++];
        auto it = std::find_if(state.voices.begin(), state.voices.end(), [](const Voice& v) { return v.remaining == 0; });
        // Validated plans never exceed the fixed voice capacity.
        if (it != state.voices.end()) *it = Voice{note.duration, 0, steps_[note.pitch], note.velocity * 0.1F};
    }
    float value = 0;
    for (auto& voice : state.voices) {
        if (!voice.remaining) continue;
        value += static_cast<float>(std::sin(voice.phase * 2.0 * std::numbers::pi)) * voice.level;
        voice.phase += voice.step;
        voice.phase -= std::floor(voice.phase);
        --voice.remaining;
    }
    return track.gain * value;
}

void Engine::process(std::span<float> output) noexcept {
    std::fill(output.begin(), output.end(), 0);
    if (!running_) return;
    for (auto& sample : output) {
        if (position_ == std::numeric_limits<Frame>::max()) { stop(); return; }
        while (count_ && commands_[head_].at <= position_) {
            apply(commands_[head_]);
            head_ = (head_ + 1) % command_capacity;
            --count_;
        }
        for (std::size_t track = 0; track < runtime_.size(); ++track) sample += render_track(track);
        ++position_;
    }
}
std::array<TrackStatus, Engine::track_capacity> Engine::track_statuses() const noexcept {
    std::array<TrackStatus, track_capacity> result{};
    for (std::size_t i = 0; i < runtime_.size(); ++i) {
        const auto& state = runtime_[i];
        if (!state.clip) continue;
        const auto& clip = plan_.tracks[i].clips[*state.clip];
        const auto age = position_ - state.launched;
        const bool active = std::visit([&](const auto& value) {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, AudioClip>)
                return value.loop || age < value.samples.size();
            else return value.loop || age < value.length;
        }, clip);
        if (active) result[i].active_clip = static_cast<int>(*state.clip);
    }
    // Only the earliest pending action per track is exposed; queue order is
    // unchanged, including multiple actions at the same sample.
    for (std::size_t n = 0; n < count_; ++n) {
        const auto& command = commands_[(head_ + n) % command_capacity];
        auto& status = result[command.track];
        if (status.pending != PendingAction::none) continue;
        status.pending = command.kind == CommandKind::launch ? PendingAction::launch : PendingAction::stop;
        status.pending_clip = command.kind == CommandKind::launch ? static_cast<int>(command.clip) : -1;
        status.pending_at = command.at;
    }
    return result;
}
std::size_t Engine::active_voices() const noexcept {
    std::size_t count = 0;
    for (const auto& state : runtime_)
        count += static_cast<std::size_t>(std::count_if(state.voices.begin(), state.voices.end(),
            [](const Voice& v) { return v.remaining != 0; }));
    return count;
}
} // namespace fl_live
