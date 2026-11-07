#pragma once
#include "project.hpp"
#include "fl_live/document_render.hpp"
#include <QString>
#include <atomic>
#include <memory>
#include <optional>

// General desktop content beside the identity document. Each track has one
// clip in this slice. MIDI clips own a step pattern; audio clips use the
// built-in pulse, nothing yet, or an imported WAV referenced by path.
enum class AudioSource { pulse, empty, wav };
struct ClipContent {
    fl_live::ClipId clip;
    StepPattern pattern;                // MIDI clips
    SynthSettings synth;                // MIDI clips
    std::optional<PianoRoll> piano;     // MIDI clips; replaces the pattern when set
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
// Automatable parameters are identified by (track ID, parameter) and use the
// same units as the static controls: level 0-100 percent, pan -100..100.
// Points sit at hundredths of a beat from the song start (up to 256 bars)
// and must strictly increase; values are linear between points in frames.
// device: the parameter of the device with that ID on the lane's track
// (gain in dB, transpose in whole semitones); see docs/devices.md.
enum class AutomatedParameter { level, pan, device };
struct LanePoint {
    std::int64_t at = 0;
    double value = 0;
    bool operator==(const LanePoint&) const = default;
};
struct AutomationLane {
    fl_live::TrackId track;
    AutomatedParameter parameter = AutomatedParameter::level;
    std::vector<LanePoint> points;
    std::uint64_t device = 0; // device lanes only
    bool operator==(const AutomationLane&) const = default;
};
inline constexpr std::int64_t automation_position_limit = 256LL * 4 * 100;
// Typed devices (docs/devices.md). Ports carry MIDI notes or stereo audio. A
// track's devices form one linear chain: source -> MIDI devices ->
// instrument (MIDI tracks) -> audio devices -> output. Branches, merges,
// cycles, type mismatches and unknown endpoints are refused. A parameter is
// bound by (device ID, kind); device IDs are project-wide, from a counter
// saved with the devices, so later edits never reissue one.
enum class PortType { midi, audio };
enum class DeviceKind { gain, transpose }; // transpose: MIDI in/out; gain: audio in/out
struct Device {
    std::uint64_t id = 0;
    DeviceKind kind = DeviceKind::gain;
    double value = 0; // gain: -60..+12 dB; transpose: whole semitones -48..+48
    bool operator==(const Device&) const = default;
};
struct DeviceEndpoint {
    enum class Kind { source, instrument, output, device };
    Kind kind = Kind::source;
    std::uint64_t device = 0; // Kind::device only
    bool operator==(const DeviceEndpoint&) const = default;
};
struct DeviceConnection {
    DeviceEndpoint from, to;
    bool operator==(const DeviceConnection&) const = default;
};
// Present only for tracks with at least one device; others use the default
// chain source -> (instrument ->) output.
struct TrackDevices {
    fl_live::TrackId track;
    std::vector<Device> devices;
    std::vector<DeviceConnection> connections;
    bool operator==(const TrackDevices&) const = default;
};
inline constexpr std::size_t devices_per_track_limit = 8;
inline constexpr double combined_device_gain_limit_db = 24;
struct SessionContent {
    int tempo = 120;
    std::vector<TrackContent> tracks;
    std::vector<ClipContent> clips;
    std::vector<AutomationLane> automation;
    std::vector<TrackDevices> devices;
    std::uint64_t next_device_id = 1;
    bool operator==(const SessionContent&) const = default;
};
// The chain of a validated entry in order, MIDI stage first.
struct DeviceChain { std::vector<Device> midi, audio; };
DeviceChain deviceChain(const TrackDevices& entry, fl_live::TrackKind kind);
// Connections for devices in order, MIDI devices before the instrument.
std::vector<DeviceConnection> chainConnections(const std::vector<Device>& devices, fl_live::TrackKind kind);
// Transposes notes through the chain; a note leaving 0-127 at any device is
// dropped rather than clamped.
fl_live::MidiClip transposed(fl_live::MidiClip clip, const std::vector<Device>& midi);
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
inline constexpr std::size_t desktop_clips_per_track = 8; // at least one, at most this many

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
// Lanes in engine units at transport frames for the content's tempo.
std::vector<fl_live::DocumentAutomation> contentAutomation(const SessionContent&, const fl_live::DocumentSnapshot&);

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
    // Clip index to play on each track (the desktop's selected clips); a
    // missing entry plays the track's first clip.
    std::vector<std::size_t> clips;
};
void validateExportOptions(const ExportOptions& options);
std::uint64_t exportFrames(int tempo, int bars);
QString exportPartialPath(const QString& path);
ExportOutcome exportProjectWav(const QString& path, const SessionContent& content, const fl_live::DocumentSnapshot& identities,
    int bars, ExportProgress* progress = nullptr);
ExportOutcome exportProjectWav(const QString& path, const SessionContent& content, const fl_live::DocumentSnapshot& identities,
    const ExportOptions& options, ExportProgress* progress = nullptr);
