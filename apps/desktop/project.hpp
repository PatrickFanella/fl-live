#pragma once
#include "fl_live/engine.hpp"
#include <QString>
#include <array>

struct ProjectSettings {
    int tempo = 120;
    std::array<int, 2> levels{70, 25};
    std::array<int, 4> pitches{60, 64, 67, 72};
    bool operator==(const ProjectSettings&) const = default;
};
fl_live::SessionPlan prepareProject(const ProjectSettings& settings);
ProjectSettings readProject(const QString& path);
void writeProject(const QString& path, const ProjectSettings& settings);
