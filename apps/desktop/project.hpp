#pragma once
#include "fl_live/engine.hpp"
#include <QString>
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
fl_live::SessionPlan prepareProject(const ProjectSettings& settings);
ProjectSettings readProject(const QString& path);
void writeProject(const QString& path, const ProjectSettings& settings);
