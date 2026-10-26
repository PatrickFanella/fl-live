#pragma once
#include "fl_live/pipewire_backend.hpp"
#include <QObject>
#include <QTimer>
#include <QString>
#include <memory>

// QObject and the sole command producer live on the GUI thread. Preview renders
// there too; a connected backend exclusively owns processing instead.
class Session final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(bool pending READ pending NOTIFY changed)
    Q_PROPERTY(QString position READ position NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString mode READ mode CONSTANT)
public:
    explicit Session(fl_live::PipeWireConfig config = {}, QObject* parent = nullptr);
    ~Session() override;
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
    const fl_live::SessionPlan plan_;
    fl_live::RenderController controller_;
    std::unique_ptr<fl_live::PipeWireBackend> backend_;
    QTimer timer_;
    fl_live::RenderStatus status_{};
    std::uint64_t serial_ = 0;
    QString message_ = "Ready. Launches use the next 4/4 bar.";
    const bool preview_;
    bool failed_ = false;
    void send(fl_live::ControlKind kind, std::size_t track = 0, std::size_t clip = 0);
};
