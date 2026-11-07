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
fl_live::SessionPlan prepareProject(const ProjectSettings& settings);
ProjectSettings readProject(const QString& path);
void writeProject(const QString& path, const ProjectSettings& settings);
QByteArray encodeProjectSettings(const ProjectSettings& settings);
ProjectSettings decodeProjectSettings(const QByteArray& bytes);

