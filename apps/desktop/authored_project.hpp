#pragma once
#include "project.hpp"
#include "fl_live/document_render.hpp"
#include <QString>
#include <atomic>
#include <memory>

// General desktop content beside the identity document. Each track has one
// clip in this slice. MIDI clips own a step pattern; audio clips use the
// built-in pulse, nothing yet, or an imported WAV referenced by path.
enum class AudioSource { pulse, empty, wav };
struct ClipContent {
    fl_live::ClipId clip;
    StepPattern pattern;                // MIDI clips
    AudioSource audio = AudioSource::pulse; // audio clips
    QString asset;                      // absolute path, audio == wav only
    // Decoded asset; not persisted. Null means missing or not yet loaded,
    // which renders silence. Shared and immutable, so history copies are cheap.
    std::shared_ptr<const fl_live::AudioClip> media;
    // Imported clips only: play source frames [range_start, range_start +
    // length), where length 0 means to the end of the source. Source-rate
    // playback, no warping. loop false plays the range once and stops.
    std::uint64_t range_start = 0;
    std::uint64_t range_length = 0;
    bool loop = true;
    bool hasRange() const { return range_start != 0 || range_length != 0 || !loop; }
    bool operator==(const ClipContent&) const = default;
};
// Frames of a decoded clip (samples / channels), 0 when not resolved.
std::uint64_t mediaFrames(const ClipContent& clip);
struct TrackContent {
    fl_live::TrackId track;
    int level = 70; // percent
    bool operator==(const TrackContent&) const = default;
};
struct SessionContent {
    int tempo = 120;
    std::vector<TrackContent> tracks;
    std::vector<ClipContent> clips;
    bool operator==(const SessionContent&) const = default;
};
// An imported file's location relative to the project file's folder, when
// it lies inside that folder. Used only to find files after the project
// folder moved; never to choose between different files.
struct RelativeAsset {
    fl_live::ClipId clip;
    QString path;
    bool operator==(const RelativeAsset&) const = default;
};
struct AuthoredProject {
    SessionContent content;
    fl_live::DocumentSnapshot identities;
    std::vector<RelativeAsset> relative; // filled by decoding; not document state
};
inline constexpr std::size_t desktop_clips_per_track = 1;

fl_live::DocumentSnapshot defaultTemplateIdentities();
AuthoredProject defaultProject();
// Template settings (v1-v3 files) on the two-track template identities.
void validateTemplateIdentities(const fl_live::DocumentSnapshot& identities);
std::vector<fl_live::DocumentClipSource> templateSources(const ProjectSettings&, const fl_live::DocumentSnapshot&);
std::vector<fl_live::DocumentTrackGain> templateGains(const ProjectSettings&, const fl_live::DocumentSnapshot&);
SessionContent migrateTemplate(const ProjectSettings& settings, const fl_live::DocumentSnapshot& identities);
// The first audio track's level, the first MIDI track's level and that MIDI
// clip's pattern, as template settings. Missing tracks keep defaults.
ProjectSettings templateView(const SessionContent& content, const fl_live::DocumentSnapshot& identities);

// Content and identities must describe the same tracks/clips with matching
// kinds, one clip per track, levels 0-100 and valid patterns/sources.
void validateContent(const SessionContent& content, const fl_live::DocumentSnapshot& identities);
const ClipContent& clipContent(const SessionContent& content, fl_live::ClipId clip);
const TrackContent& trackContent(const SessionContent& content, fl_live::TrackId track);
std::vector<fl_live::DocumentClipSource> contentSources(const SessionContent&, const fl_live::DocumentSnapshot&);
std::vector<fl_live::DocumentTrackGain> contentGains(const SessionContent&, const fl_live::DocumentSnapshot&);

// Version 4 is always written; versions 1-3 are read and migrated. Decoding
// never opens assets: media stays null until the caller resolves paths.
// With a base directory, imported files inside it also get a relative path
// (required feature asset-relative-v1). writeAuthoredProject passes the
// destination's folder.
QByteArray encodeAuthoredProject(const AuthoredProject&, const QString& baseDirectory = {});
// Relative path of an asset inside baseDirectory, or empty when outside it.
QString assetRelativePath(const QString& asset, const QString& baseDirectory);
AuthoredProject decodeAuthoredProject(const QByteArray&);
AuthoredProject readAuthoredProject(const QString& path);
void writeAuthoredProject(const QString& path, const AuthoredProject&);

// Stereo 48 kHz PCM16 through the track mixer (level, pan, mute, solo). Each
// track's clip starts at frame zero; hard end, no effect tails. Renders into
// NAME.part and publishes without replacing an existing file. Runs on any
// thread; progress (optional) is shared with the caller.
struct ExportProgress {
    std::atomic<std::uint64_t> frames{0}, total{0};
    std::atomic<bool> cancel{false};
};
enum class ExportOutcome { completed, cancelled };
// Explicit render policy. Clips start at the song start; the file holds bars
// [start_bar, start_bar + bars) followed by tail_ms rendered after every
// track stops at the range end. The engine has no effects or release
// envelopes yet, so the tail is currently silent. Mono is the (L + R) / 2
// downmix. PCM clamps to [-1, 1]; float32 keeps the mix unclipped.
enum class ExportFormat { pcm16, pcm24, float32 };
struct ExportOptions {
    static constexpr int bar_limit = 64, start_limit = 255, tail_limit_ms = 10000;
    int start_bar = 0;
    int bars = 4;
    int tail_ms = 0;
    int channels = 2;
    ExportFormat format = ExportFormat::pcm16;
};
void validateExportOptions(const ExportOptions& options);
std::uint64_t exportFrames(int tempo, int bars);
QString exportPartialPath(const QString& path);
ExportOutcome exportProjectWav(const QString& path, const SessionContent& content, const fl_live::DocumentSnapshot& identities,
    int bars, ExportProgress* progress = nullptr);
ExportOutcome exportProjectWav(const QString& path, const SessionContent& content, const fl_live::DocumentSnapshot& identities,
    const ExportOptions& options, ExportProgress* progress = nullptr);
