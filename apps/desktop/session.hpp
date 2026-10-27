#pragma once
#include "fl_live/pipewire_backend.hpp"
#include "project.hpp"
#include <QVariantList>
#include <QObject>
#include <QTimer>
#include <QString>
#include <memory>

// QObject and the sole command producer live on the GUI thread. Preview renders
// there too; a connected backend exclusively owns processing instead.
class Session final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int tempo READ tempo NOTIFY changed)
    Q_PROPERTY(QVariantList levels READ levels NOTIFY changed)
    Q_PROPERTY(QVariantList pitches READ pitches NOTIFY changed)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY changed)
    Q_PROPERTY(bool canEdit READ canEdit NOTIFY changed)
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(bool pending READ pending NOTIFY changed)
    Q_PROPERTY(QString position READ position NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString mode READ mode CONSTANT)
public:
    explicit Session(fl_live::PipeWireConfig config = {}, QObject* parent = nullptr);
    ~Session() override;
    int tempo() const { return settings_.tempo; }
    QVariantList levels() const;
    QVariantList pitches() const;
    bool dirty() const { return dirty_; }
    QString projectPath() const { return project_path_; }
    bool canEdit() const { return !failed_ && !running() && !pending(); }
    Q_INVOKABLE void setTempo(int value);
    Q_INVOKABLE void setLevel(int track, int value);
    Q_INVOKABLE void setPitch(int note, int value);
    Q_INVOKABLE bool saveProject(const QString& path);
    Q_INVOKABLE bool loadProject(const QString& path);
    bool running() const { return status_.running; }
    bool pending() const { return status_.handled_serial < serial_; }
    QString position() const;
    QString message() const { return message_; }
    QString mode() const { return preview_ ? "Silent preview" : "PipeWire output"; }
    Q_INVOKABLE void play();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void rewind();
    Q_INVOKABLE void launch(int track, int clip);
    Q_INVOKABLE void stopTrack(int track);
    Q_INVOKABLE void panic();
    void tick(); // shared polling path, also exercised deterministically in tests
signals:
    void changed();
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
    bool dirty_ = false;
    bool replace(const ProjectSettings& settings);
    bool editable();
    QTimer timer_;
    fl_live::RenderStatus status_{};
    std::uint64_t serial_ = 0;
    QString message_ = "Ready. Launches use the next 4/4 bar.";
    const bool preview_;
    bool failed_ = false;
    void send(fl_live::ControlKind kind, std::size_t track = 0, std::size_t clip = 0);
};
