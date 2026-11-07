#include "project.hpp"
#include "fl_live/document_io.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
void validate(const ProjectSettings& s) {
    if (s.tempo < 20 || s.tempo > 300) throw std::invalid_argument("Tempo must be 20 to 300 BPM");
    for (int level : s.levels)
        if (level < 0 || level > 100) throw std::invalid_argument("Track level must be 0 to 100 percent");
    if (s.step_count < 1 || s.step_count > 16) throw std::invalid_argument("Pattern length must be 1 to 16 steps");
    for (const auto& step : s.steps) {
        if (step.pitch < 0 || step.pitch > 127) throw std::invalid_argument("MIDI pitch must be 0 to 127");
        if (step.velocity < 0 || step.velocity > 100) throw std::invalid_argument("Velocity must be 0 to 100 percent");
        if (step.gate < 1 || step.gate > 100) throw std::invalid_argument("Gate must be 1 to 100 percent");
    }
}
int integer(const QJsonValue& value, int low, int high) {
    if (!value.isDouble()) throw std::invalid_argument("Project requires numeric settings");
    const double n = value.toDouble();
    if (!std::isfinite(n) || n < low || n > high || std::floor(n) != n)
        throw std::invalid_argument("Project setting is out of range or not an integer");
    return static_cast<int>(n);
}
}
namespace {
fl_live::Frame boundary(int tempo, unsigned hundredths_of_a_beat) {
    // Each boundary rounds independently, rather than accumulating rounded beats.
    const auto numerator = 48000ULL * 60 * hundredths_of_a_beat;
    const auto denominator = static_cast<unsigned>(tempo) * 100;
    return (numerator + denominator - 1) / denominator;
}
}
void validatePattern(const StepPattern& pattern) {
    ProjectSettings s;
    s.step_count = pattern.step_count;
    s.steps = pattern.steps;
    validate(s);
}
fl_live::AudioClip pulseClip(int tempo) {
    fl_live::AudioClip pulse;
    pulse.samples.resize(static_cast<std::size_t>(boundary(tempo, 100)));
    for (std::size_t n = 0; n < pulse.samples.size(); ++n) {
        const double t = static_cast<double>(n) / 48000.0;
        pulse.samples[n] = static_cast<float>(0.18 * std::sin(2 * 3.141592653589793 * 110 * t) * std::exp(-22 * t));
    }
    return pulse;
}
fl_live::MidiClip patternClip(const StepPattern& pattern, int tempo) {
    validatePattern(pattern);
    fl_live::MidiClip midi{boundary(tempo, static_cast<unsigned>(pattern.step_count) * 100), {}, true};
    for (unsigned n = 0; n < static_cast<unsigned>(pattern.step_count); ++n) {
        const auto& step = pattern.steps[n];
        if (!step.enabled) continue;
        const auto start = boundary(tempo, n * 100);
        midi.notes.push_back({start, boundary(tempo, n * 100 + static_cast<unsigned>(step.gate)) - start,
            static_cast<std::uint8_t>(step.pitch), static_cast<float>(step.velocity) / 100});
    }
    return midi;
}
void validateSynth(const SynthSettings& s) {
    if (s.waveform < 0 || s.waveform > 3) throw std::invalid_argument("Waveform must be sine, triangle, square or saw");
    for (int ms : {s.attack_ms, s.decay_ms, s.release_ms})
        if (ms < 0 || ms > 10000) throw std::invalid_argument("Envelope times must be 0 to 10000 ms");
    if (s.sustain < 0 || s.sustain > 100) throw std::invalid_argument("Sustain must be 0 to 100 percent");
}
fl_live::MidiClip patternClip(const StepPattern& pattern, int tempo, const SynthSettings& synth) {
    validateSynth(synth);
    auto clip = patternClip(pattern, tempo);
    clip.instrument = {static_cast<fl_live::Waveform>(synth.waveform), static_cast<fl_live::Frame>(synth.attack_ms) * 48,
        static_cast<fl_live::Frame>(synth.decay_ms) * 48, static_cast<float>(synth.sustain) / 100, static_cast<fl_live::Frame>(synth.release_ms) * 48};
    return clip;
}
void sortPiano(PianoRoll& roll) {
    std::sort(roll.notes.begin(), roll.notes.end(), [](const PianoNote& a, const PianoNote& b) {
        return a.start != b.start ? a.start < b.start : a.id < b.id; });
}
void validatePiano(const PianoRoll& roll) {
    if (roll.length < 1 || roll.length > piano_length_limit) throw std::invalid_argument("Piano-roll length must be 1 tick to 16 bars");
    if (roll.notes.size() > fl_live::Engine::notes_per_clip_capacity) throw std::invalid_argument("A clip holds at most 1024 notes");
    std::vector<std::uint64_t> ids;
    for (std::size_t i = 0; i < roll.notes.size(); ++i) {
        const auto& note = roll.notes[i];
        if (note.id < 1 || note.id >= roll.next_id) throw std::invalid_argument("Note IDs must come from the clip's counter");
        if (note.pitch < 0 || note.pitch > 127) throw std::invalid_argument("Note pitch must be 0 to 127");
        if (note.velocity < 1 || note.velocity > 100) throw std::invalid_argument("Note velocity must be 1 to 100 percent");
        if (note.start < 0 || note.length < 1 || note.start >= roll.length || note.length > roll.length - note.start)
            throw std::invalid_argument("A note must lie inside the clip");
        if (i && (roll.notes[i - 1].start > note.start || (roll.notes[i - 1].start == note.start && roll.notes[i - 1].id >= note.id)))
            throw std::invalid_argument("Notes must be ordered by start, then ID");
        ids.push_back(note.id);
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) throw std::invalid_argument("Note IDs must be unique");
    // At most 16 notes sound at once (the synth's voice count): count notes
    // still sounding at each note start.
    for (const auto& note : roll.notes) {
        const auto sounding = std::count_if(roll.notes.begin(), roll.notes.end(), [&](const PianoNote& other) {
            return other.start <= note.start && other.start + other.length > note.start; });
        if (sounding > static_cast<std::ptrdiff_t>(fl_live::Engine::voices_per_track))
            throw std::invalid_argument("At most 16 notes can sound at once");
    }
}
fl_live::MidiClip pianoClip(const PianoRoll& roll, int tempo, const SynthSettings& synth) {
    validatePiano(roll);
    validateSynth(synth);
    // frame = ceil(ticks * 48000 * 60 / (tempo * 96)), the shared upward rounding.
    const auto frame = [tempo](int ticks) {
        const auto numerator = static_cast<std::uint64_t>(ticks) * 30000U;
        const auto denominator = static_cast<std::uint64_t>(tempo);
        return static_cast<fl_live::Frame>((numerator + denominator - 1) / denominator);
    };
    fl_live::MidiClip midi{frame(roll.length), {}, true};
    for (const auto& note : roll.notes)
        midi.notes.push_back({frame(note.start), frame(note.start + note.length) - frame(note.start),
            static_cast<std::uint8_t>(note.pitch), static_cast<float>(note.velocity) / 100});
    midi.instrument = {static_cast<fl_live::Waveform>(synth.waveform), static_cast<fl_live::Frame>(synth.attack_ms) * 48,
        static_cast<fl_live::Frame>(synth.decay_ms) * 48, static_cast<float>(synth.sustain) / 100, static_cast<fl_live::Frame>(synth.release_ms) * 48};
    return midi;
}
fl_live::SessionPlan prepareProject(const ProjectSettings& s) {
    validate(s);
    return {48000, static_cast<std::uint32_t>(s.tempo * 1000),
        {{static_cast<float>(s.levels[0]) / 100, {pulseClip(s.tempo)}},
         {static_cast<float>(s.levels[1]) / 100, {patternClip({s.step_count, s.steps}, s.tempo)}}}};
}
ProjectSettings readProject(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot open project for reading");
    constexpr qint64 limit = 65536;
    if (file.size() > limit) throw std::invalid_argument("Project exceeds the 64 KiB settings limit");
    const auto bytes = file.read(limit + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > limit)
        throw std::runtime_error("Project read failed or exceeded the size limit");
    return decodeProjectSettings(bytes);
}
ProjectSettings decodeProjectSettings(const QByteArray& bytes) {
    if (bytes.size() > 65536) throw std::invalid_argument("Project exceeds the 64 KiB settings limit");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw std::invalid_argument("Project is not a valid JSON object");
    const auto object = document.object();
    if (object.size() != 6 || object.value("format") != "fl-live-project")
        throw std::invalid_argument("Unsupported project format");
    const int version = integer(object.value("version"), 1, 2);
    const auto expected_template = version == 1 ? "pulse-steps-v1" : "pulse-step-sequencer-v1";
    if (object.value("template") != expected_template)
        throw std::invalid_argument("Unsupported project template");
    ProjectSettings s;
    s.tempo = integer(object.value("tempo"), 20, 300);
    const auto levels = object.value("levels").toArray();
    if (levels.size() != 2) throw std::invalid_argument("Project requires two track levels");
    for (int i = 0; i < 2; ++i) s.levels[static_cast<std::size_t>(i)] = integer(levels[i], 0, 100);
    if (version == 1) {
        const auto pitches = object.value("pitches").toArray();
        if (pitches.size() != 4) throw std::invalid_argument("Version 1 requires four MIDI pitches");
        for (int i = 0; i < 4; ++i) s.steps[static_cast<std::size_t>(i)].pitch = integer(pitches[i], 0, 127);
    } else {
        const auto pattern = object.value("pattern").toObject();
        if (pattern.size() != 2) throw std::invalid_argument("Invalid pattern object");
        s.step_count = integer(pattern.value("length"), 1, 16);
        const auto steps = pattern.value("steps").toArray();
        if (steps.size() != 16) throw std::invalid_argument("Project requires 16 stored steps");
        for (int i = 0; i < 16; ++i) {
            const auto value = steps[i].toObject();
            if (value.size() != 4 || !value.value("enabled").isBool())
                throw std::invalid_argument("Invalid step object");
            s.steps[static_cast<std::size_t>(i)] = {value.value("enabled").toBool(),
                integer(value.value("pitch"), 0, 127), integer(value.value("velocity"), 0, 100),
                integer(value.value("gate"), 1, 100)};
        }
    }
    validate(s);
    return s;
}
QByteArray encodeProjectSettings(const ProjectSettings& s) {
    validate(s);
    QJsonArray levels, steps;
    for (int n : s.levels) levels.append(n);
    for (const auto& step : s.steps)
        steps.append(QJsonObject{{"enabled", step.enabled}, {"pitch", step.pitch},
            {"velocity", step.velocity}, {"gate", step.gate}});
    return QJsonDocument(QJsonObject{{"format", "fl-live-project"}, {"version", 2},
        {"template", "pulse-step-sequencer-v1"}, {"tempo", s.tempo}, {"levels", levels},
        {"pattern", QJsonObject{{"length", s.step_count}, {"steps", steps}}}}).toJson();
}
void writeProject(const QString& path, const ProjectSettings& s) {
    const auto bytes = encodeProjectSettings(s);
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || !fl_live::commitDurably(file, bytes))
        throw std::runtime_error("Cannot save project atomically; previous file was preserved");
}
