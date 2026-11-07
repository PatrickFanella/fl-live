#pragma once
#include "project.hpp"
#include "fl_live/document_render.hpp"
#include <atomic>

struct AuthoredProject {
    ProjectSettings settings;
    fl_live::DocumentSnapshot identities;
};
fl_live::DocumentSnapshot defaultTemplateIdentities();
void validateTemplateIdentities(const fl_live::DocumentSnapshot& identities);
std::vector<fl_live::DocumentClipSource> templateSources(const ProjectSettings&, const fl_live::DocumentSnapshot&);
std::vector<fl_live::DocumentTrackGain> templateGains(const ProjectSettings&, const fl_live::DocumentSnapshot&);
QByteArray encodeAuthoredProject(const AuthoredProject&);
AuthoredProject decodeAuthoredProject(const QByteArray&);
AuthoredProject readAuthoredProject(const QString& path);
void writeAuthoredProject(const QString& path, const AuthoredProject&);
// Stereo 48 kHz PCM16 through the track mixer (level, pan, mute, solo). Each
// track's first clip starts at frame zero; hard end, no effect tails. Renders
// into NAME.part and publishes without replacing an existing file. Runs on
// any thread; progress (optional) is shared with the caller.
struct ExportProgress {
    std::atomic<std::uint64_t> frames{0}, total{0};
    std::atomic<bool> cancel{false};
};
enum class ExportOutcome { completed, cancelled };
std::uint64_t exportFrames(int tempo, int bars);
QString exportPartialPath(const QString& path);
ExportOutcome exportProjectWav(const QString& path, const ProjectSettings& settings, const fl_live::DocumentSnapshot& identities,
    int bars, ExportProgress* progress = nullptr);
