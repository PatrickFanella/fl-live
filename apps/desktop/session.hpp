#pragma once
#include "fl_live/pipewire_backend.hpp"
#include "project.hpp"
#include "authored_project.hpp"
#include "fl_live/media_import.hpp"
#include <QVariantList>
#include <QObject>
#include <QTimer>
#include <QString>
#include <memory>
#include <thread>
#include <optional>
#include <type_traits>
#include <functional>
#include <map>
#include <QStringList>
#include <QLockFile>

// QObject and the sole command producer live on the GUI thread. Preview renders
// there too; a connected backend exclusively owns processing instead.
class Session final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList activeClips READ activeClips NOTIFY changed)
    Q_PROPERTY(QVariantList clipStates READ clipStates NOTIFY changed)
    Q_PROPERTY(QVariantList trackNames READ trackNames NOTIFY changed)
    Q_PROPERTY(QVariantList clipNames READ clipNames NOTIFY changed)
    Q_PROPERTY(QVariantList trackIds READ trackIds NOTIFY changed)
    Q_PROPERTY(QVariantList clipIds READ clipIds NOTIFY changed)
    Q_PROPERTY(QVariantList activeClipIds READ activeClipIds NOTIFY changed)
    Q_PROPERTY(QString sceneId READ sceneId NOTIFY changed)
    Q_PROPERTY(QString sceneName READ sceneName NOTIFY changed)
    Q_PROPERTY(QString sceneState READ sceneState NOTIFY changed)
    Q_PROPERTY(QVariantList sceneIncluded READ sceneIncluded NOTIFY changed)
    Q_PROPERTY(QVariantList sceneIds READ sceneIds NOTIFY documentChanged)
    Q_PROPERTY(QVariantList sceneNames READ sceneNames NOTIFY documentChanged)
    Q_PROPERTY(int selectedSceneIndex READ selectedSceneIndex NOTIFY sceneSelectionChanged)
    Q_PROPERTY(int stepCount READ stepCount NOTIFY changed)
    Q_PROPERTY(int selectedStep READ selectedStep NOTIFY changed)
    Q_PROPERTY(QVariantList stepEnabled READ stepEnabled NOTIFY changed)
    Q_PROPERTY(QVariantList velocities READ velocities NOTIFY changed)
    Q_PROPERTY(QVariantList gates READ gates NOTIFY changed)
    Q_PROPERTY(int tempo READ tempo NOTIFY changed)
    Q_PROPERTY(QVariantList levels READ levels NOTIFY changed)
    Q_PROPERTY(QVariantList pitches READ pitches NOTIFY changed)
    Q_PROPERTY(QVariantList pans READ pans NOTIFY changed)
    Q_PROPERTY(QVariantList mutes READ mutes NOTIFY changed)
    Q_PROPERTY(QVariantList solos READ solos NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    Q_PROPERTY(QString undoText READ undoText NOTIFY changed)
    Q_PROPERTY(QString redoText READ redoText NOTIFY changed)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY projectPathChanged)
    Q_PROPERTY(bool canEdit READ canEdit NOTIFY changed)
    Q_PROPERTY(bool canChange READ canChange NOTIFY changed)
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(bool pending READ pending NOTIFY changed)
    Q_PROPERTY(QString position READ position NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString mode READ mode NOTIFY changed)
    Q_PROPERTY(bool audioAvailable READ audioAvailable NOTIFY changed)
    Q_PROPERTY(bool exporting READ exporting NOTIFY changed)
    Q_PROPERTY(int trackCount READ trackCount NOTIFY changed)
    Q_PROPERTY(QVariantList trackKinds READ trackKinds NOTIFY changed)
    Q_PROPERTY(QVariantList trackSources READ trackSources NOTIFY changed)
    Q_PROPERTY(int patternTrackIndex READ patternTrackIndex NOTIFY changed)
    Q_PROPERTY(QVariantList synth READ synth NOTIFY changed)
    Q_PROPERTY(bool pianoMode READ pianoMode NOTIFY changed)
    Q_PROPERTY(int pianoLength READ pianoLength NOTIFY changed)
    Q_PROPERTY(QVariantList pianoNotes READ pianoNotes NOTIFY changed)
    Q_PROPERTY(QVariantList selectedNotes READ selectedNotes NOTIFY changed)
    Q_PROPERTY(bool importing READ importing NOTIFY changed)
    Q_PROPERTY(QVariantList clipFrames READ clipFrames NOTIFY changed)
    Q_PROPERTY(QVariantList recoveryCandidates READ recoveryCandidates NOTIFY recoveryChanged)
    Q_PROPERTY(QVariantList clipRanges READ clipRanges NOTIFY changed)
    Q_PROPERTY(double exportProgress READ exportProgress NOTIFY changed)
public:
    explicit Session(fl_live::PipeWireConfig config = {}, QObject* parent = nullptr);
    ~Session() override;
    QVariantList activeClips() const;
    QVariantList clipStates() const;
    QVariantList trackNames() const;
    QVariantList clipNames() const;
    QVariantList trackIds() const;
    QVariantList clipIds() const;
    QVariantList activeClipIds() const;
    Q_INVOKABLE bool renameTrack(const QString& id, const QString& name);
    Q_INVOKABLE bool renameClip(const QString& id, const QString& name);
    Q_INVOKABLE bool launchClip(const QString& id);
    Q_INVOKABLE bool stopTrackId(const QString& id);
    QString sceneId() const;
    QString sceneName() const;
    QString sceneState() const;
    QVariantList sceneIncluded() const;
    QVariantList sceneIds() const;
    QVariantList sceneNames() const;
    int selectedSceneIndex() const;
    Q_INVOKABLE bool selectScene(const QString& id);
    Q_INVOKABLE bool createScene();
    Q_INVOKABLE bool copyScene(const QString& id);
    Q_INVOKABLE bool moveScene(const QString& id, int direction);
    Q_INVOKABLE bool deleteScene(const QString& id);
    Q_INVOKABLE bool setSceneIncluded(const QString& trackId, bool included);
    Q_INVOKABLE bool renameScene(const QString& id, const QString& name);
    Q_INVOKABLE bool launchScene(const QString& id, const QString& policy);
    Q_INVOKABLE bool stopScene(const QString& id, const QString& policy);
    // General track management (one clip per track). Stopped edits with Undo.
    int trackCount() const { return static_cast<int>(identities_->tracks.size()); }
    QVariantList trackKinds() const;   // "audio" or "midi"
    QVariantList trackSources() const; // human-readable clip source per track
    Q_INVOKABLE bool addTrack(const QString& kind);
    Q_INVOKABLE bool duplicateTrack(const QString& id);
    Q_INVOKABLE bool deleteTrack(const QString& id);
    // The step editor shows one MIDI clip; choose it by track. -1 when none.
    Q_INVOKABLE bool editPattern(const QString& trackId);
    int patternTrackIndex() const;
    // Built-in synth of the MIDI clip in the step editor:
    // [waveform, attack_ms, decay_ms, sustain, release_ms]. Stopped edit, one Undo step.
    QVariantList synth() const;
    // Piano roll of the MIDI clip in the step editor (docs/piano-roll.md).
    // Positions are ticks of 1/96 beat. pianoNotes lists
    // {id, start, length, pitch, velocity} in start order; IDs are strings.
    bool pianoMode() const;
    int pianoLength() const;
    QVariantList pianoNotes() const;
    // Switches the clip between its step pattern and piano-roll notes. Turning
    // it on converts the pattern's sounding steps once; turning it off keeps
    // only the pattern. Each is one Undo step.
    Q_INVOKABLE bool usePianoRoll(bool on);
    // Adds a note and selects it; refused with a message when out of range or
    // above 16 simultaneous notes.
    Q_INVOKABLE bool addNote(int start, int length, int pitch, int velocity);
    Q_INVOKABLE bool deleteNotes(const QVariantList& ids);
    // Selection is view state: not saved or undone, kept by note ID across
    // unrelated edits, cleared when the edited clip changes.
    QVariantList selectedNotes() const;
    Q_INVOKABLE void selectNote(const QString& id, bool additive);
    Q_INVOKABLE void clearNoteSelection();
    Q_INVOKABLE bool setSynth(const QString& field, int value);
    // WAV decoding runs on a worker; the result is applied as one Undo step
    // once transport is stopped. One import at a time.
    Q_INVOKABLE bool importAudio(const QString& trackId, const QString& path);
    Q_INVOKABLE void cancelImport();
    // Points an imported clip at another file, keeping its range; refused
    // when the file is shorter than the range. Decodes on the worker.
    Q_INVOKABLE bool relinkAudio(const QString& trackId, const QString& path);
    // Copies imported files that live outside the project folder into
    // NAME-assets/ beside the saved project, never overwriting a file, and
    // repoints the clips as one Undo step. Source files are not modified.
    Q_INVOKABLE bool collectAssets();
    // Crash recovery. While there are unsaved edits, the session writes an
    // autosave snapshot to the recovery folder (FL_LIVE_RECOVERY_DIR, or the
    // per-user application data folder) every autosave_interval_ms, never to
    // the project file. A manual save or a normal shutdown removes it. Each
    // snapshot from a session that is no longer running is a candidate:
    // {file, project, time, valid, reason}. Restore loads a valid one as
    // unsaved work for its original project path; Discard deletes one file.
    // Invalid snapshots are listed with a reason and never removed silently.
    static constexpr int autosave_interval_ms = 30000;
    static QString recoveryDirectory();
    QVariantList recoveryCandidates() const;
    Q_INVOKABLE bool restoreRecovery(const QString& file);
    Q_INVOKABLE bool discardRecovery(const QString& file);
    Q_INVOKABLE void refreshRecovery();
    bool autosaveNow();
    QString autosaveFile() const { return autosave_file_; }
    // Imported clips: decoded frames per track (0 if none), and [start, end,
    // loop] in source frames. setClipRange takes an exclusive end frame and is
    // one Undo step. waveform returns bins x [min, max] of the whole source,
    // prepared on the GUI thread and cached per decoded clip.
    QVariantList clipFrames() const;
    QVariantList clipRanges() const;
    Q_INVOKABLE bool setClipRange(const QString& trackId, double startFrame, double endFrame, bool loop);
    Q_INVOKABLE QVariantList waveform(int track, int bins) const;
    // Automation lanes (see docs/automation.md). parameter is "level" or
    // "pan"; positions are hundredths of a beat from the song start. Each
    // change is one Undo step; a point at an existing position replaces it.
    Q_INVOKABLE QVariantList automationPoints(int track, const QString& parameter) const; // [[at, value], ...]
    Q_INVOKABLE bool setAutomationPoint(const QString& trackId, const QString& parameter, double at, double value);
    Q_INVOKABLE bool removeAutomationPoint(const QString& trackId, const QString& parameter, double at);
    Q_INVOKABLE bool clearAutomation(const QString& trackId, const QString& parameter);
    bool importing() const { return import_ticket_ != 0 || ready_import_.has_value(); }
    int stepCount() const { return pattern().step_count; }
    int selectedStep() const { return selected_step_; }
    QVariantList stepEnabled() const;
    QVariantList velocities() const;
    QVariantList gates() const;
    Q_INVOKABLE void selectStep(int index);
    Q_INVOKABLE void setStepCount(int value);
    Q_INVOKABLE void setStepEnabled(int index, bool value);
    Q_INVOKABLE void setVelocity(int index, int value);
    Q_INVOKABLE void setGate(int index, int value);
    int tempo() const { return content_->tempo; }
    QVariantList levels() const;
    QVariantList pitches() const;
    QVariantList pans() const;
    QVariantList mutes() const;
    QVariantList solos() const;
    // Stopped mixer edits on track IDs; each is one Undo step and persists.
    Q_INVOKABLE bool setPan(const QString& trackId, int value);
    Q_INVOKABLE bool setMute(const QString& trackId, bool value);
    Q_INVOKABLE bool setSolo(const QString& trackId, bool value);
    bool dirty() const { return *content_ != *saved_content_ || identities_->tracks != saved_identities_->tracks ||
        identities_->clips != saved_identities_->clips || identities_->scenes != saved_identities_->scenes; }
    bool canUndo() const { return canChange() && history_cursor_ > 0; }
    bool canRedo() const { return canChange() && history_cursor_ + 1 < history_size_; }
    QString undoText() const;
    QString redoText() const;
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    QString projectPath() const { return project_path_; }
    // canEdit: stopped-only operations (tempo, Open, New, Restore, Collect,
    // Export). canChange: document edits, which also apply while playing
    // through a plan replacement; see docs/general-session.md#playback-edits.
    bool canEdit() const { return !failed_ && !running() && !pending(); }
    bool canChange() const { return !failed_; }
    Q_INVOKABLE void setTempo(int value);
    Q_INVOKABLE void setLevel(int track, int value);
    Q_INVOKABLE void setPitch(int note, int value);
    // Starts a background export of the current document while stopped. The
    // job renders an immutable copy, so later edits, playback and history do
    // not affect it. Completion, cancellation and failure are reported by tick().
    Q_INVOKABLE bool exportWav(const QString& path, int bars); // bars 1..16 from bar 1, stereo PCM16
    // startBar is 1-based; channels 1 (mono downmix) or 2; format pcm16,
    // pcm24 or float32. See ExportOptions for the render policy.
    Q_INVOKABLE bool exportAudio(const QString& path, int startBar, int bars, int tailMs, int channels, const QString& format);
    Q_INVOKABLE void cancelExport();
    bool exporting() const { return static_cast<bool>(export_job_); }
    double exportProgress() const;
    Q_INVOKABLE bool saveProject(const QString& path);
    Q_INVOKABLE bool loadProject(const QString& path);
    // Starts the default template project, unsaved and with empty history.
    // Refused while there are unsaved edits, like Open.
    Q_INVOKABLE bool newProject();
    bool running() const { return status_.running; }
    // Plan replacements the render owner has activated in this runtime.
    std::uint64_t playbackEdits() const { return status_.generation; }
    bool pending() const { return status_.handled_serial < serial_; }
    QString position() const;
    QString message() const { return message_; }
    bool audioAvailable() const { return !failed_; }
    QString mode() const { return failed_ ? "Audio unavailable" : preview_ ? "Silent preview" : "PipeWire output"; }
    Q_INVOKABLE bool reconnectAudio();
    Q_INVOKABLE void play();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void rewind();
    Q_INVOKABLE void launch(int track, int clip);
    Q_INVOKABLE void stopTrack(int track);
    Q_INVOKABLE void panic();
    void tick(); // shared polling path, also exercised deterministically in tests
signals:
    void changed();
    void projectPathChanged();
    void documentChanged();
    void sceneSelectionChanged();
    void recoveryChanged();
private:
    using Plan = std::shared_ptr<const fl_live::DocumentRenderPlan>;
    static Plan preparePlan(const SessionContent& content, const fl_live::DocumentSnapshot& identities);
    struct Prepared {
        // mapped: the latest plan the controller accepted; controls resolve
        // against it and carry its generation. waiting: a newer playback edit
        // prepared but not yet offered. Retired plans come back through
        // collect() and are released on the GUI thread.
        Plan mapped;
        const Plan initial; // the controller's constructor plan
        fl_live::RenderController controller;
        std::unique_ptr<fl_live::PipeWireBackend> backend;
        Plan waiting;
        Prepared(const SessionContent& content, const fl_live::DocumentSnapshot& identities)
            : mapped(preparePlan(content, identities)), initial(mapped), controller(initial->plan()) {}
        // The plan matching the current document, for display and ID lookups.
        const fl_live::DocumentRenderPlan& view() const { return waiting ? *waiting : *mapped; }
    };
    using Content = std::shared_ptr<const SessionContent>;
    Content content_ = std::make_shared<const SessionContent>(defaultProject().content);
    using Identities = std::shared_ptr<const fl_live::DocumentSnapshot>;
    Identities identities_ = std::make_shared<const fl_live::DocumentSnapshot>(defaultTemplateIdentities());
    fl_live::SceneId selected_scene_ = identities_->scenes[0].id;
    std::uint64_t next_identity_ = identities_->next_id;
    const fl_live::PipeWireConfig config_;
    std::unique_ptr<Prepared> prepared_;
    QString project_path_, owned_path_;
    enum class EditKind { none, tempo, level, pitch, velocity, gate, enabled, length, track_name, clip_name, scene_name, scene_slot, scene_create, scene_delete, scene_copy, scene_order, track_pan, track_mute, track_solo,
        track_create, track_duplicate, track_delete, audio_import, clip_range, audio_relink, assets_collect, automation, synth, piano_mode, note_add, note_delete };
    struct HistoryEntry {
        Content content;
        Identities identities;
        EditKind kind = EditKind::none;
        int index = -1;
        fl_live::SceneId selection{};
        fl_live::SceneId undo_selection{};
    };
    static constexpr std::size_t history_capacity = 128;
    static_assert(std::is_nothrow_copy_assignable_v<HistoryEntry>);
    std::array<HistoryEntry, history_capacity + 1> history_{};
    std::size_t history_cursor_ = 0, history_size_ = 1;
    Content saved_content_;
    Identities saved_identities_;
    static QString editName(const HistoryEntry& entry);
    void edit(const SessionContent& next, EditKind kind, int index = -1);
    // Applies a document+content change (track management) as one Undo step.
    bool commitStructure(SessionContent next, const fl_live::DocumentSnapshot& identities, EditKind kind, int index);
    const StepPattern& pattern() const;
    void editPatternStep(EditKind kind, int index, const std::function<void(StepPattern&)>& change);
    fl_live::ClipId pattern_clip_{};
    std::vector<std::uint64_t> selected_notes_;
    int history_index_hint_ = -1; // note count for the delete Undo label
    const PianoRoll* piano() const;
    bool editPiano(EditKind kind, const std::function<void(PianoRoll&)>& change);
    void restorePatternTarget();
    std::unique_ptr<fl_live::WavImportWorker> importer_;
    std::uint64_t import_ticket_ = 0;
    fl_live::TrackId import_track_{};
    QString import_path_;
    std::optional<fl_live::WavImport> ready_import_;
    void pollImport();
    bool import_relink_ = false;
    struct AssetReport { QStringList missing, relocated; QList<std::pair<fl_live::ClipId, QString>> previous; };
    AssetReport resolveAssets(SessionContent& content, const std::vector<RelativeAsset>& relative, const QString& projectDirectory) const;
    // Keyed by decoded clip and bin count; the weak pointer rejects a stale
    // entry if a freed clip's address is reused.
    mutable std::map<std::pair<const fl_live::AudioClip*, int>, std::pair<std::weak_ptr<const fl_live::AudioClip>, QVariantList>> waveform_cache_;
    void pushHistory(EditKind kind, int index);
    enum class NameKind { track, clip, scene };
    bool rename(const QString& id, const QString& name, NameKind kind);
    enum class MixField { pan, mute, solo };
    bool editMix(const QString& trackId, MixField field, int value);
    bool sceneAction(const QString& id, const QString& policy, bool stop);
    std::uint64_t scene_serial_ = 0;
    bool scene_stop_ = false;
    fl_live::SceneId requested_scene_{};
    const fl_live::DocumentScene& selectedScene() const;
    bool addScene(std::optional<fl_live::SceneId> source);
    void restoreSceneSelection(fl_live::SceneId id, bool notify = true);
    int selected_step_ = 0;
    bool replace(Content content, Identities identities = {});
    bool replaceLive(Content content, Identities identities);
    bool live_ = false; // the last replace() applied while playing
    void offerWaiting();
    bool controlsReady(); // no playback edit awaits its offer
    bool editable();
    bool changeable();
    struct ExportJob {
        ExportProgress progress;
        std::atomic<int> state{0}; // 0 running, 1 completed, 2 cancelled, 3 failed
        QString path, error;
        int bars = 0;
    };
    std::shared_ptr<ExportJob> export_job_;
    std::thread export_thread_;
    void finishExport();
    QTimer autosave_timer_;
    QString autosave_file_;
    std::unique_ptr<QLockFile> autosave_lock_;
    Content autosaved_content_;
    Identities autosaved_identities_;
    QVariantList recovery_candidates_;
    void removeAutosave();
    QTimer timer_;
    fl_live::RenderStatus status_{};
    std::uint64_t serial_ = 0;
    std::array<std::uint64_t, fl_live::Engine::track_capacity> track_serials_{};
    std::array<fl_live::ControlKind, fl_live::Engine::track_capacity> track_requests_{};
    QString message_ = "Ready. Launches use the next 4/4 bar.";
    const bool preview_;
    bool failed_ = false;
    void audioFailed(const QString& reason);
    void send(fl_live::ControlKind kind, std::size_t track = 0, std::size_t clip = 0);
};
