#pragma once
#include "fl_live/pipewire_backend.hpp"
#include "project.hpp"
#include "authored_project.hpp"
#include <QVariantList>
#include <QObject>
#include <QTimer>
#include <QString>
#include <memory>
#include <optional>
#include <type_traits>

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
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(bool pending READ pending NOTIFY changed)
    Q_PROPERTY(QString position READ position NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString mode READ mode NOTIFY changed)
    Q_PROPERTY(bool audioAvailable READ audioAvailable NOTIFY changed)
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
    int stepCount() const { return settings_.step_count; }
    int selectedStep() const { return selected_step_; }
    QVariantList stepEnabled() const;
    QVariantList velocities() const;
    QVariantList gates() const;
    Q_INVOKABLE void selectStep(int index);
    Q_INVOKABLE void setStepCount(int value);
    Q_INVOKABLE void setStepEnabled(int index, bool value);
    Q_INVOKABLE void setVelocity(int index, int value);
    Q_INVOKABLE void setGate(int index, int value);
    int tempo() const { return settings_.tempo; }
    QVariantList levels() const;
    QVariantList pitches() const;
    QVariantList pans() const;
    QVariantList mutes() const;
    QVariantList solos() const;
    // Stopped mixer edits on track IDs; each is one Undo step and persists.
    Q_INVOKABLE bool setPan(const QString& trackId, int value);
    Q_INVOKABLE bool setMute(const QString& trackId, bool value);
    Q_INVOKABLE bool setSolo(const QString& trackId, bool value);
    bool dirty() const { return settings_ != saved_settings_ || identities_->tracks != saved_identities_->tracks ||
        identities_->clips != saved_identities_->clips || identities_->scenes != saved_identities_->scenes; }
    bool canUndo() const { return canEdit() && history_cursor_ > 0; }
    bool canRedo() const { return canEdit() && history_cursor_ + 1 < history_size_; }
    QString undoText() const;
    QString redoText() const;
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    QString projectPath() const { return project_path_; }
    bool canEdit() const { return !failed_ && !running() && !pending(); }
    Q_INVOKABLE void setTempo(int value);
    Q_INVOKABLE void setLevel(int track, int value);
    Q_INVOKABLE void setPitch(int note, int value);
    Q_INVOKABLE bool exportWav(const QString& path, int bars);
    Q_INVOKABLE bool saveProject(const QString& path);
    Q_INVOKABLE bool loadProject(const QString& path);
    bool running() const { return status_.running; }
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
private:
    struct Prepared {
        const fl_live::DocumentRenderPlan mapped;
        fl_live::RenderController controller;
        std::unique_ptr<fl_live::PipeWireBackend> backend;
        Prepared(const ProjectSettings& settings, const fl_live::DocumentSnapshot& identities)
            : mapped(identities, templateSources(settings, identities), 48000,
                static_cast<std::uint32_t>(settings.tempo)*1000U, {}, templateGains(settings, identities)), controller(mapped.plan()) {}
    };
    ProjectSettings settings_;
    using Identities = std::shared_ptr<const fl_live::DocumentSnapshot>;
    Identities identities_ = std::make_shared<const fl_live::DocumentSnapshot>(defaultTemplateIdentities());
    fl_live::SceneId selected_scene_ = identities_->scenes[0].id;
    std::uint64_t next_identity_ = identities_->next_id;
    const fl_live::PipeWireConfig config_;
    std::unique_ptr<Prepared> prepared_;
    QString project_path_, owned_path_;
    enum class EditKind { none, tempo, level, pitch, velocity, gate, enabled, length, track_name, clip_name, scene_name, scene_slot, scene_create, scene_delete, scene_copy, scene_order, track_pan, track_mute, track_solo };
    struct HistoryEntry {
        ProjectSettings settings;
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
    ProjectSettings saved_settings_;
    Identities saved_identities_;
    static QString editName(const HistoryEntry& entry);
    void edit(const ProjectSettings& next, EditKind kind, int index = -1);
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
    bool replace(const ProjectSettings& settings, Identities identities = {});
    bool editable();
    QTimer timer_;
    fl_live::RenderStatus status_{};
    std::uint64_t serial_ = 0;
    std::array<std::uint64_t, 2> track_serials_{};
    std::array<fl_live::ControlKind, 2> track_requests_{};
    QString message_ = "Ready. Launches use the next 4/4 bar.";
    const bool preview_;
    bool failed_ = false;
    void audioFailed(const QString& reason);
    void send(fl_live::ControlKind kind, std::size_t track = 0, std::size_t clip = 0);
};
