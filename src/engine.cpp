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

void validatePlan(const SessionPlan& plan) {
    (void)TempoGrid(plan.sample_rate, plan.bpm_milli);
    if (plan.tracks.size() > Engine::track_capacity) throw std::invalid_argument("at most 64 tracks in engine slice");
    for (const auto& track : plan.tracks) {
        if (!std::isfinite(track.gain) || track.gain < 0 || track.gain > 4)
            throw std::invalid_argument("track gain must be finite in [0,4]");
        if (!std::isfinite(track.pan) || track.pan < -1 || track.pan > 1)
            throw std::invalid_argument("track pan must be finite in [-1,1]");
        const auto lane = [](const std::vector<AutomationPoint>& points, float low, float high) {
            if (points.size() > Engine::automation_points_capacity) throw std::invalid_argument("at most 4096 automation points per lane");
            for (std::size_t i = 0; i < points.size(); ++i) {
                if (!std::isfinite(points[i].value) || points[i].value < low || points[i].value > high)
                    throw std::invalid_argument("automation value outside the parameter range");
                if (i && points[i].at <= points[i - 1].at) throw std::invalid_argument("automation points must strictly increase in time");
            }
        };
        lane(track.gain_automation, 0, 4);
        lane(track.pan_automation, -1, 1);
        if (track.clips.size() > Engine::clips_per_track_capacity) throw std::invalid_argument("at most 64 clips per track");
        for (const auto& clip : track.clips) {
            if (const auto* audio = std::get_if<AudioClip>(&clip)) {
                if (audio->channels < 1 || audio->channels > Engine::max_output_channels)
                    throw std::invalid_argument("audio clip must have one or two channels");
                if (audio->samples.empty() || audio->samples.size() % audio->channels != 0 ||
                    !std::all_of(audio->samples.begin(), audio->samples.end(), [](float x) { return std::isfinite(x); }))
                    throw std::invalid_argument("audio clip must contain whole frames of finite samples");
            } else {
                const auto& midi = std::get<MidiClip>(clip);
                if (midi.length == 0 || midi.notes.size() > Engine::notes_per_clip_capacity)
                    throw std::invalid_argument("invalid MIDI length or note count");
                const auto& in = midi.instrument;
                if (static_cast<int>(in.waveform) > static_cast<int>(Waveform::saw) || in.attack > Engine::envelope_frame_limit ||
                    in.decay > Engine::envelope_frame_limit || in.release > Engine::envelope_frame_limit ||
                    !std::isfinite(in.sustain) || in.sustain < 0 || in.sustain > 1)
                    throw std::invalid_argument("invalid instrument waveform or envelope");
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

void Engine::validate(const SessionPlan& plan) { validatePlan(plan); }
void Engine::validateMapping(const SessionPlan& from, const SessionPlan& to, const PlanMapping& mapping) {
    if (from.sample_rate != to.sample_rate) throw std::invalid_argument("replacement must keep the sample rate");
    if (mapping.track_from_old.size() != from.tracks.size() || mapping.clip_from_old.size() != from.tracks.size())
        throw std::invalid_argument("mapping must cover every track of the replaced plan");
    std::array<bool, track_capacity> used{};
    for (std::size_t t = 0; t < from.tracks.size(); ++t) {
        if (mapping.clip_from_old[t].size() != from.tracks[t].clips.size())
            throw std::invalid_argument("mapping must cover every clip of the replaced plan");
        const auto target = mapping.track_from_old[t];
        if (target < 0) continue;
        if (static_cast<std::size_t>(target) >= to.tracks.size() || used[static_cast<std::size_t>(target)])
            throw std::invalid_argument("mapped track outside the new plan or shared");
        used[static_cast<std::size_t>(target)] = true;
        for (const auto clip : mapping.clip_from_old[t])
            if (clip >= 0 && static_cast<std::size_t>(clip) >= to.tracks[static_cast<std::size_t>(target)].clips.size())
                throw std::invalid_argument("mapped clip outside the new track");
    }
}

Engine::Engine(const SessionPlan& plan) : plan_(&plan) {
    validatePlan(plan);
    runtime_.resize(track_capacity);
    spare_.resize(track_capacity);
    automation_cursor_.resize(track_capacity);
    for (std::size_t pitch = 0; pitch < steps_.size(); ++pitch)
        steps_[pitch] = 440.0 * std::exp2((static_cast<double>(pitch) - 69.0) / 12.0) / plan.sample_rate;
}
void Engine::start() noexcept { running_ = true; }
void Engine::stop() noexcept {
    running_ = false;
    count_ = 0;
    head_ = 0;
    for (std::size_t i = 0; i < plan_->tracks.size(); ++i) runtime_[i] = Runtime{};
}
void Engine::rewind() noexcept { if (!running_) position_ = 0; }

ScheduleResult Engine::schedule(Command command) noexcept {
    if (command.kind != CommandKind::launch && command.kind != CommandKind::stop_track)
        return ScheduleResult::invalid;
    if (command.track >= plan_->tracks.size() ||
        (command.kind == CommandKind::launch && command.clip >= plan_->tracks[command.track].clips.size()))
        return ScheduleResult::invalid;
    if (command.at < position_) return ScheduleResult::in_past;
    if (count_ == command_capacity) return ScheduleResult::full;
    if (count_ && command.at < commands_[(head_ + count_ - 1) % command_capacity].at)
        return ScheduleResult::out_of_order;
    commands_[(head_ + count_) % command_capacity] = command;
    ++count_;
    return ScheduleResult::accepted;
}
ScheduleResult Engine::scheduleTogether(std::span<const Command> commands) noexcept {
    if (commands.empty() || commands.size() > command_capacity) return ScheduleResult::invalid;
    const auto at = commands.front().at;
    for (const auto& command : commands) {
        if (command.at != at || (command.kind != CommandKind::launch && command.kind != CommandKind::stop_track) ||
            command.track >= plan_->tracks.size() ||
            (command.kind == CommandKind::launch && command.clip >= plan_->tracks[command.track].clips.size()))
            return ScheduleResult::invalid;
    }
    if (at < position_) return ScheduleResult::in_past;
    if (commands.size() > command_capacity - count_) return ScheduleResult::full;
    std::size_t insertion = 0;
    while (insertion < count_ && commands_[(head_ + insertion) % command_capacity].at <= at) ++insertion;
    for (auto n = count_; n > insertion; --n)
        commands_[(head_ + n - 1 + commands.size()) % command_capacity] = commands_[(head_ + n - 1) % command_capacity];
    for (std::size_t n = 0; n < commands.size(); ++n)
        commands_[(head_ + insertion + n) % command_capacity] = commands[n];
    count_ += commands.size();
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

ReplaceResult Engine::replace(const SessionPlan& next, const PlanMapping& mapping) noexcept {
    ReplaceResult result;
    for (auto& state : spare_) state = Runtime{};
    for (std::size_t old = 0; old < plan_->tracks.size(); ++old) {
        const auto& state = runtime_[old];
        if (!state.clip) continue;
        const auto target = mapping.track_from_old[old];
        const auto clip = target < 0 ? -1 : mapping.clip_from_old[old][*state.clip];
        if (clip < 0) { ++result.stopped_tracks; continue; }
        const auto& before = plan_->tracks[old].clips[*state.clip];
        const auto& after = next.tracks[static_cast<std::size_t>(target)].clips[static_cast<std::size_t>(clip)];
        if (before.index() != after.index()) { ++result.stopped_tracks; continue; }
        auto& moved = spare_[static_cast<std::size_t>(target)];
        moved = state;
        moved.clip = static_cast<std::size_t>(clip);
        if (const auto* midi = std::get_if<MidiClip>(&after)) {
            const auto local = (position_ - moved.launched) % midi->length;
            moved.next_note = static_cast<std::size_t>(std::lower_bound(midi->notes.begin(), midi->notes.end(), local,
                [](const Note& note, Frame at) { return note.start < at; }) - midi->notes.begin());
            // A release at or past the new release length ends now; this also
            // keeps a zero-length release from dividing by zero.
            for (auto& voice : moved.voices)
                if (voice.active && voice.releasing && voice.released >= midi->instrument.release) voice.active = false;
        }
    }
    std::size_t kept = 0;
    for (std::size_t n = 0; n < count_; ++n) {
        auto command = commands_[(head_ + n) % command_capacity];
        const auto target = mapping.track_from_old[command.track];
        if (target < 0) continue;
        if (command.kind == CommandKind::launch) {
            const auto clip = mapping.clip_from_old[command.track][command.clip];
            if (clip < 0) continue;
            command.clip = static_cast<std::size_t>(clip);
        }
        command.track = static_cast<std::size_t>(target);
        commands_[(head_ + kept++) % command_capacity] = command;
    }
    result.dropped_commands = count_ - kept;
    count_ = kept;
    runtime_.swap(spare_);
    for (auto& cursor : automation_cursor_) cursor = {0, 0};
    plan_ = &next;
    return result;
}

float Engine::automationValue(std::span<const AutomationPoint> points, Frame at, std::size_t& cursor) noexcept {
    if (points.empty()) return 0;
    if (cursor >= points.size() || points[cursor].at > at) cursor = 0; // rewind or first use
    while (cursor + 1 < points.size() && points[cursor + 1].at <= at) ++cursor;
    const auto& a = points[cursor];
    if (at <= a.at || cursor + 1 == points.size()) return a.value;
    const auto& b = points[cursor + 1];
    const double t = static_cast<double>(at - a.at) / static_cast<double>(b.at - a.at);
    return static_cast<float>(static_cast<double>(a.value) + (static_cast<double>(b.value) - static_cast<double>(a.value)) * t);
}

float Engine::heldEnvelope(const Instrument& in, Frame age) noexcept {
    if (age < in.attack) return static_cast<float>(age) / static_cast<float>(in.attack);
    if (age < in.attack + in.decay)
        return 1 - (1 - in.sustain) * static_cast<float>(age - in.attack) / static_cast<float>(in.decay);
    return in.sustain;
}

Engine::StereoSample Engine::render_track(std::size_t index) noexcept {
    auto& state = runtime_[index];
    if (!state.clip) return {};
    const auto& track = plan_->tracks[index];
    const auto& clip = track.clips[*state.clip];
    const auto age = position_ - state.launched;
    auto& cursor = automation_cursor_[index];
    const float gain = track.gain_automation.empty() ? track.gain : automationValue(track.gain_automation, position_, cursor[0]);
    const float pan = track.pan_automation.empty() ? track.pan : automationValue(track.pan_automation, position_, cursor[1]);
    const float left_gain = gain * (pan > 0 ? 1 - pan : 1.0F);
    const float right_gain = gain * (pan < 0 ? 1 + pan : 1.0F);
    if (const auto* audio = std::get_if<AudioClip>(&clip)) {
        const auto length = audio->samples.size() / audio->channels;
        if (!audio->loop && age >= length) { state = Runtime{}; return {}; }
        const auto frame = (age % length) * audio->channels;
        const float left = audio->samples[frame];
        const float right = audio->channels == 2 ? audio->samples[frame + 1] : left;
        return {left_gain * left, right_gain * right};
    }
    const auto& midi = std::get<MidiClip>(clip);
    if (!midi.loop && age >= midi.length) { state = Runtime{}; return {}; }
    const auto& instrument = midi.instrument;
    const auto local = age % midi.length;
    // Loop restart resets the note cursor; sounding voices keep their own
    // durations and releases. Stop and replacement still clear every voice.
    if (local == 0) state.next_note = 0;
    while (state.next_note < midi.notes.size() && midi.notes[state.next_note].start == local) {
        const auto& note = midi.notes[state.next_note++];
        // Overload policy: a free voice; else the quietest releasing voice;
        // else the oldest voice. Ties take the lowest index.
        auto it = std::find_if(state.voices.begin(), state.voices.end(), [](const Voice& v) { return !v.active; });
        if (it == state.voices.end()) {
            float quietest = 2;
            for (auto candidate = state.voices.begin(); candidate != state.voices.end(); ++candidate) {
                if (!candidate->releasing) continue;
                const float now = candidate->release_from * (1 - static_cast<float>(candidate->released) / static_cast<float>(instrument.release));
                if (now < quietest) { quietest = now; it = candidate; }
            }
        }
        if (it == state.voices.end())
            it = std::max_element(state.voices.begin(), state.voices.end(), [](const Voice& a, const Voice& b) { return a.age < b.age; });
        *it = Voice{note.duration, 0, steps_[note.pitch], note.velocity * 0.1F, 0, 0, 0, false, true};
    }
    float value = 0;
    for (auto& voice : state.voices) {
        if (!voice.active) continue;
        double osc = 0;
        switch (instrument.waveform) {
        case Waveform::sine: osc = std::sin(voice.phase * 2.0 * std::numbers::pi); break;
        case Waveform::triangle: osc = 4.0 * std::abs(voice.phase - 0.5) - 1.0; break;
        case Waveform::square: osc = voice.phase < 0.5 ? 1.0 : -1.0; break;
        case Waveform::saw: osc = 2.0 * voice.phase - 1.0; break;
        }
        const float envelope = voice.releasing
            ? voice.release_from * (1 - static_cast<float>(voice.released) / static_cast<float>(instrument.release))
            : heldEnvelope(instrument, voice.age);
        value += static_cast<float>(osc) * voice.level * envelope;
        voice.phase += voice.step;
        voice.phase -= std::floor(voice.phase);
        ++voice.age;
        if (voice.releasing) {
            if (++voice.released >= instrument.release) voice.active = false;
        } else if (--voice.remaining == 0) {
            if (instrument.release == 0) voice.active = false;
            else { voice.releasing = true; voice.released = 0; voice.release_from = heldEnvelope(instrument, voice.age); }
        }
    }
    return {left_gain * value, right_gain * value};
}

void Engine::process(std::span<float> output) noexcept { (void)process(output, 1); }
bool Engine::process(std::span<float> output, std::size_t channels) noexcept {
    std::fill(output.begin(), output.end(), 0);
    if (channels < 1 || channels > max_output_channels || output.size() % channels != 0) return false;
    if (!running_) return true;
    for (std::size_t at = 0; at < output.size(); at += channels) {
        if (position_ == std::numeric_limits<Frame>::max()) { stop(); return true; }
        while (count_ && commands_[head_].at <= position_) {
            apply(commands_[head_]);
            head_ = (head_ + 1) % command_capacity;
            --count_;
        }
        StereoSample mix;
        for (std::size_t track = 0; track < plan_->tracks.size(); ++track) {
            const auto value = render_track(track);
            mix.left += value.left;
            mix.right += value.right;
        }
        if (channels == 2) { output[at] = mix.left; output[at + 1] = mix.right; }
        else output[at] = (mix.left + mix.right) / 2;
        ++position_;
    }
    return true;
}
std::array<TrackStatus, Engine::track_capacity> Engine::track_statuses() const noexcept {
    std::array<TrackStatus, track_capacity> result{};
    for (std::size_t i = 0; i < plan_->tracks.size(); ++i) {
        const auto& state = runtime_[i];
        if (!state.clip) continue;
        const auto& clip = plan_->tracks[i].clips[*state.clip];
        const auto age = position_ - state.launched;
        const bool active = std::visit([&](const auto& value) {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, AudioClip>)
                return value.loop || age < value.samples.size() / value.channels;
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
            [](const Voice& v) { return v.active; }));
    return count;
}
} // namespace fl_live
