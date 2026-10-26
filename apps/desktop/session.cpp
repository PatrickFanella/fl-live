#include "session.hpp"
#include <array>
#include <cmath>

namespace {
fl_live::SessionPlan demo() {
    using namespace fl_live;
    AudioClip pulse;
    pulse.samples.resize(24000);
    for (std::size_t n = 0; n < pulse.samples.size(); ++n) {
        const double t = static_cast<double>(n) / 48000.0;
        pulse.samples[n] = static_cast<float>(0.18 * std::sin(2 * 3.141592653589793 * 110 * t) * std::exp(-22 * t));
    }
    return {48000, 120000, {{0.7F, {pulse}}, {0.25F, {MidiClip{96000,
        {{0, 18000, 60, 0.7F}, {24000, 18000, 64, 0.7F},
         {48000, 18000, 67, 0.7F}, {72000, 18000, 72, 0.7F}}, true}}}}};
}
}
Session::Session(fl_live::PipeWireConfig config, QObject* parent)
    : QObject(parent), plan_(demo()), controller_(plan_), preview_(config.remote.empty()) {
    if (!preview_) backend_ = std::make_unique<fl_live::PipeWireBackend>(controller_, std::move(config));
    timer_.setInterval(10);
    connect(&timer_, &QTimer::timeout, this, &Session::tick);
    timer_.start();
}
Session::~Session() {
    timer_.stop();
    backend_.reset(); // joins processing before the immutable plan is destroyed
    controller_.finish();
}
QString Session::position() const {
    const auto beat = status_.position / 24000;
    return QString("%1.%2").arg(beat / 4 + 1).arg(beat % 4 + 1);
}
void Session::send(fl_live::ControlKind kind, std::size_t track, std::size_t clip) {
    if (failed_) return;
    const auto next = serial_ + 1;
    if (controller_.submit({kind, {fl_live::CommandKind::launch, 0, track, clip}, next})) {
        serial_ = next;
        message_ = "Awaiting render acknowledgement";
    } else message_ = "Control queue full. Try again, or use Silence now.";
    emit changed();
}
void Session::play() { send(fl_live::ControlKind::start); }
void Session::stop() { send(fl_live::ControlKind::stop); }
void Session::rewind() {
    if (running() || pending()) { message_ = "Stop transport before returning to the start."; emit changed(); return; }
    send(fl_live::ControlKind::rewind);
}
void Session::launch(int track, int clip) {
    if (track < 0 || track >= 2 || clip != 0) { message_ = "Unknown clip"; emit changed(); return; }
    send(fl_live::ControlKind::launch_next_bar, static_cast<std::size_t>(track), 0);
}
void Session::stopTrack(int track) {
    if (track < 0 || track >= 2) return;
    send(fl_live::ControlKind::stop_next_bar, static_cast<std::size_t>(track));
}
void Session::panic() {
    controller_.panic();
    message_ = "Silence requested; awaiting the rendering thread";
    emit changed();
}
void Session::tick() {
    if (preview_) {
        std::array<float, 480> discarded{};
        controller_.process(discarded); // simulated clock, no audio output
    }
    if (backend_ && backend_->status().error) {
        backend_.reset();
        failed_ = true;
        message_ = "Audio backend disconnected. Restart the application to reconnect.";
    }
    const auto before = status_;
    status_ = controller_.status();
    if (!failed_ && status_.handled_serial != before.handled_serial)
        message_ = status_.rejected != before.rejected ? "Engine rejected a scheduled command" :
            "Render acknowledged. Clip requests are scheduled on the bar grid.";
    emit changed();
}
