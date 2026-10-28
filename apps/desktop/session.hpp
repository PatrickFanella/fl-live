#pragma once
#include "fl_live/pipewire_backend.hpp"
#include "project.hpp"
#include <QVariantList>
#include <QObject>
#include <QTimer>
#include <QString>
#include <memory>
#include <type_traits>

// QObject and the sole command producer live on the GUI thread. Preview renders
// there too; a connected backend exclusively owns processing instead.
class Session final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList activeClips READ activeClips NOTIFY changed)
    Q_PROPERTY(QVariantList clipStates READ clipStates NOTIFY changed)
    Q_PROPERTY(int stepCount READ stepCount NOTIFY changed)
    Q_PROPERTY(int selectedStep READ selectedStep NOTIFY changed)
    Q_PROPERTY(QVariantList stepEnabled READ stepEnabled NOTIFY changed)
    Q_PROPERTY(QVariantList velocities READ velocities NOTIFY changed)
    Q_PROPERTY(QVariantList gates READ gates NOTIFY changed)
    Q_PROPERTY(int tempo READ tempo NOTIFY changed)
    Q_PROPERTY(QVariantList levels READ levels NOTIFY changed)
    Q_PROPERTY(QVariantList pitches READ pitches NOTIFY changed)
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
    bool dirty() const { return settings_ != saved_settings_; }
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
private:
    struct Prepared {
        const fl_live::SessionPlan plan;
        fl_live::RenderController controller;
        std::unique_ptr<fl_live::PipeWireBackend> backend;
        explicit Prepared(const ProjectSettings& settings)
            : plan(prepareProject(settings)), controller(plan) {}
    };
    ProjectSettings settings_;
    const fl_live::PipeWireConfig config_;
    std::unique_ptr<Prepared> prepared_;
    QString project_path_, owned_path_;
    enum class EditKind { none, tempo, level, pitch, velocity, gate, enabled, length };
    struct HistoryEntry {
        ProjectSettings settings;
        EditKind kind = EditKind::none;
        int index = -1;
    };
    static constexpr std::size_t history_capacity = 128;
    static_assert(std::is_trivially_copyable_v<HistoryEntry>);
    std::array<HistoryEntry, history_capacity + 1> history_{};
    std::size_t history_cursor_ = 0, history_size_ = 1;
    ProjectSettings saved_settings_;
    static QString editName(const HistoryEntry& entry);
    void edit(const ProjectSettings& next, EditKind kind, int index = -1);
    int selected_step_ = 0;
    bool replace(const ProjectSettings& settings);
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
