#pragma once
#include "fl_live/engine.hpp"
#include <QString>
#include <QByteArray>
#include <array>

struct MidiStep {
    bool enabled = false;
    int pitch = 60;
    int velocity = 70; // percent of the engine's normalized velocity
    int gate = 75; // percent of one quarter-note step
    bool operator==(const MidiStep&) const = default;
};
struct ProjectSettings {
    int tempo = 120;
    std::array<int, 2> levels{70, 25};
    int step_count = 4;
    std::array<MidiStep, 16> steps = [] {
        std::array<MidiStep, 16> value{};
        const std::array<int, 4> pitches{60, 64, 67, 72};
        for (std::size_t i = 0; i < pitches.size(); ++i) value[i] = {true, pitches[i], 70, 75};
        return value;
    }();
    bool operator==(const ProjectSettings&) const = default;
};
// One MIDI clip's 1-16 quarter-note steps. Hidden steps beyond step_count are
// retained so shortening and lengthening a pattern is lossless.
struct StepPattern {
    int step_count = ProjectSettings{}.step_count;
    std::array<MidiStep, 16> steps = ProjectSettings{}.steps;
    bool operator==(const StepPattern&) const = default;
};
void validatePattern(const StepPattern& pattern);
// Authored content shared by the template and general projects. Tempo is in
// whole BPM; every boundary is rounded upward independently.
fl_live::AudioClip pulseClip(int tempo);
fl_live::MidiClip patternClip(const StepPattern& pattern, int tempo);
// Built-in synth settings of a MIDI clip in user units; see engine Instrument.
struct SynthSettings {
    int waveform = 0;   // 0 sine, 1 triangle, 2 square, 3 saw
    int attack_ms = 0;  // 0..10000
    int decay_ms = 0;   // 0..10000
    int sustain = 100;  // percent
    int release_ms = 0; // 0..10000
    bool operator==(const SynthSettings&) const = default;
};
void validateSynth(const SynthSettings& synth);
fl_live::MidiClip patternClip(const StepPattern& pattern, int tempo, const SynthSettings& synth);
// Piano-roll notes of a MIDI clip, in ticks of 1/96 beat from the clip start.
// A clip that uses the piano roll renders these notes instead of its step
// pattern, which is retained. Note IDs are stable within the clip; next_id is
// a counter saved with the notes, so later edits never reissue a deleted ID
// (Undo restores the counter together with the notes it removes).
inline constexpr int piano_ticks_per_beat = 96;
inline constexpr int piano_length_limit = 16 * 4 * piano_ticks_per_beat; // 16 bars
struct PianoNote {
    std::uint64_t id = 0;
    int start = 0;     // ticks from the clip start
    int length = 24;   // ticks, at least 1
    int pitch = 60;    // MIDI 0..127
    int velocity = 80; // percent 1..100
    bool operator==(const PianoNote&) const = default;
};
// The clip plays [0, length) and loops there unless loop is false. Shortening
// is non-destructive: notes starting at or after the length stay in the
// document but are silent, and a note crossing the end is cut there when
// rendered. Lengthening plays them again.
struct PianoRoll {
    int length = 4 * piano_ticks_per_beat; // ticks, 1..piano_length_limit
    bool loop = true;
    std::uint64_t next_id = 1;
    std::vector<PianoNote> notes; // ordered by start, then ID
    bool operator==(const PianoRoll&) const = default;
};
// Throws std::invalid_argument for out-of-range values, notes outside 16 bars,
// duplicate or unissued IDs, wrong order, more than 1024 notes or more than
// 16 notes sounding at once (counting silent notes past the length too).
void validatePiano(const PianoRoll& roll);
// Sorts notes into the canonical order (start, then ID).
void sortPiano(PianoRoll& roll);
fl_live::MidiClip pianoClip(const PianoRoll& roll, int tempo, const SynthSettings& synth);
fl_live::SessionPlan prepareProject(const ProjectSettings& settings);
ProjectSettings readProject(const QString& path);
void writeProject(const QString& path, const ProjectSettings& settings);
QByteArray encodeProjectSettings(const ProjectSettings& settings);
ProjectSettings decodeProjectSettings(const QByteArray& bytes);

