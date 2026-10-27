#include "project.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <cmath>
#include <stdexcept>

namespace {
void validate(const ProjectSettings& s) {
    if (s.tempo < 20 || s.tempo > 300) throw std::invalid_argument("Tempo must be 20 to 300 BPM");
    for (int level : s.levels)
        if (level < 0 || level > 100) throw std::invalid_argument("Track level must be 0 to 100 percent");
    for (int pitch : s.pitches)
        if (pitch < 0 || pitch > 127) throw std::invalid_argument("MIDI pitch must be 0 to 127");
}
int integer(const QJsonValue& value, int low, int high) {
    if (!value.isDouble()) throw std::invalid_argument("Project requires numeric settings");
    const double n = value.toDouble();
    if (!std::isfinite(n) || n < low || n > high || std::floor(n) != n)
        throw std::invalid_argument("Project setting is out of range or not an integer");
    return static_cast<int>(n);
}
}
fl_live::SessionPlan prepareProject(const ProjectSettings& s) {
    validate(s);
    using namespace fl_live;
    // Each boundary rounds independently, rather than accumulating rounded beats.
    const auto boundary = [&](unsigned quarter_beats) -> Frame {
        const auto numerator = 48000ULL * 15 * quarter_beats;
        return (numerator + static_cast<unsigned>(s.tempo) - 1) / static_cast<unsigned>(s.tempo);
    };
    AudioClip pulse;
    pulse.samples.resize(static_cast<std::size_t>(boundary(4)));
    for (std::size_t n = 0; n < pulse.samples.size(); ++n) {
        const double t = static_cast<double>(n) / 48000.0;
        pulse.samples[n] = static_cast<float>(0.18 * std::sin(2 * 3.141592653589793 * 110 * t) * std::exp(-22 * t));
    }
    MidiClip midi{boundary(16), {}, true};
    for (unsigned n = 0; n < 4; ++n)
        midi.notes.push_back({boundary(n * 4), boundary(n * 4 + 3) - boundary(n * 4),
            static_cast<std::uint8_t>(s.pitches[n]), 0.7F});
    return {48000, static_cast<std::uint32_t>(s.tempo * 1000),
        {{static_cast<float>(s.levels[0]) / 100, {std::move(pulse)}},
         {static_cast<float>(s.levels[1]) / 100, {std::move(midi)}}}};
}
ProjectSettings readProject(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot open project for reading");
    constexpr qint64 limit = 65536;
    if (file.size() > limit) throw std::invalid_argument("Project exceeds the 64 KiB settings limit");
    const auto bytes = file.read(limit + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > limit)
        throw std::runtime_error("Project read failed or exceeded the size limit");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw std::invalid_argument("Project is not a valid JSON object");
    const auto object = document.object();
    if (object.size() != 6 || object.value("format") != "fl-live-project" ||
        integer(object.value("version"), 1, 1) != 1 || object.value("template") != "pulse-steps-v1")
        throw std::invalid_argument("Unsupported project format, version or template");
    ProjectSettings s;
    s.tempo = integer(object.value("tempo"), 20, 300);
    const auto levels = object.value("levels").toArray();
    const auto pitches = object.value("pitches").toArray();
    if (levels.size() != 2 || pitches.size() != 4)
        throw std::invalid_argument("Project requires two track levels and four MIDI notes");
    for (int i = 0; i < 2; ++i) s.levels[static_cast<std::size_t>(i)] = integer(levels[i], 0, 100);
    for (int i = 0; i < 4; ++i) s.pitches[static_cast<std::size_t>(i)] = integer(pitches[i], 0, 127);
    validate(s);
    return s;
}
void writeProject(const QString& path, const ProjectSettings& s) {
    validate(s);
    QJsonArray levels, pitches;
    for (int n : s.levels) levels.append(n);
    for (int n : s.pitches) pitches.append(n);
    const auto bytes = QJsonDocument(QJsonObject{{"format", "fl-live-project"}, {"version", 1},
        {"template", "pulse-steps-v1"}, {"tempo", s.tempo}, {"levels", levels}, {"pitches", pitches}}).toJson();
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("Cannot save project atomically; previous file was preserved");
}
