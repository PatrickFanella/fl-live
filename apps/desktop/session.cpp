#include "session.hpp"
#include <array>
#include <algorithm>
#include <QDir>
#include <QFileInfo>
#include <stdexcept>

Session::Session(fl_live::PipeWireConfig config, QObject* parent)
    : QObject(parent), config_(std::move(config)), prepared_(std::make_unique<Prepared>(settings_)),
      project_path_(QDir::home().filePath("fl-live-project.json")), preview_(config_.remote.empty()) {
    history_[0].settings = settings_;
    saved_settings_ = settings_;
    if (!preview_) prepared_->backend = std::make_unique<fl_live::PipeWireBackend>(prepared_->controller, config_);
    timer_.setInterval(10);
    connect(&timer_, &QTimer::timeout, this, &Session::tick);
    timer_.start();
}
Session::~Session() {
    timer_.stop();
    // Prepared's backend is destroyed before its controller and immutable plan.
    prepared_.reset();
}
QVariantList Session::activeClips() const {
    return {status_.tracks[0].active_clip, status_.tracks[1].active_clip};
}
QVariantList Session::clipStates() const {
    QVariantList result;
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& track = status_.tracks[i];
        QString label;
        if (track_serials_[i] > status_.handled_serial)
            label = track_requests_[i] == fl_live::ControlKind::launch_next_bar ? "Launch requested" : "Stop requested";
        else if (track.pending == fl_live::PendingAction::launch)
            label = track.active_clip >= 0 ? "Active · launch queued" : "Launch queued";
        else if (track.pending == fl_live::PendingAction::stop)
            label = track.active_clip >= 0 ? "Active · stop queued" : "Stop queued";
        else label = track.active_clip >= 0 ? "Active" : "Stopped";
        result.append(label);
    }
    return result;
}
QVariantList Session::levels() const {
    return {settings_.levels[0], settings_.levels[1]};
}
QVariantList Session::pitches() const {
    QVariantList result;
    for (const auto& step : settings_.steps) result.append(step.pitch);
    return result;
}
QVariantList Session::stepEnabled() const {
    QVariantList result;
    for (const auto& step : settings_.steps) result.append(step.enabled);
    return result;
}
QVariantList Session::velocities() const {
    QVariantList result;
    for (const auto& step : settings_.steps) result.append(step.velocity);
    return result;
}
QVariantList Session::gates() const {
    QVariantList result;
    for (const auto& step : settings_.steps) result.append(step.gate);
    return result;
}
void Session::selectStep(int index) {
    if (index < 0 || index >= settings_.step_count) return;
    selected_step_ = index;
    emit changed();
}
void Session::setStepCount(int value) {
    auto next = settings_; next.step_count = value;
    edit(next, EditKind::length);
}
void Session::setStepEnabled(int index, bool value) {
    if (index < 0 || index >= settings_.step_count) return;
    auto next = settings_; next.steps[static_cast<std::size_t>(index)].enabled = value;
    edit(next, EditKind::enabled, index);
}
void Session::setVelocity(int index, int value) {
    if (index < 0 || index >= settings_.step_count) return;
    auto next = settings_; next.steps[static_cast<std::size_t>(index)].velocity = value;
    edit(next, EditKind::velocity, index);
}
void Session::setGate(int index, int value) {
    if (index < 0 || index >= settings_.step_count) return;
    auto next = settings_; next.steps[static_cast<std::size_t>(index)].gate = value;
    edit(next, EditKind::gate, index);
}
QString Session::editName(const HistoryEntry& entry) {
    switch (entry.kind) {
    case EditKind::tempo: return "tempo";
    case EditKind::level: return QString("track %1 level").arg(entry.index + 1);
    case EditKind::pitch: return QString("step %1 pitch").arg(entry.index + 1);
    case EditKind::velocity: return QString("step %1 velocity").arg(entry.index + 1);
    case EditKind::gate: return QString("step %1 gate").arg(entry.index + 1);
    case EditKind::enabled: return QString("step %1 note/rest").arg(entry.index + 1);
    case EditKind::length: return "pattern length";
    default: return {};
    }
}
QString Session::undoText() const {
    return history_cursor_ ? "Undo " + editName(history_[history_cursor_]) : "Undo";
}
QString Session::redoText() const {
    return history_cursor_ + 1 < history_size_ ? "Redo " + editName(history_[history_cursor_ + 1]) : "Redo";
}
void Session::edit(const ProjectSettings& next, EditKind kind, int index) {
    if (next == settings_ || !replace(next)) return;
    // Fixed-capacity plain snapshots: after preparation commits, history cannot
    // fail through an allocation or leave the cursor ahead of the document.
    if (history_cursor_ == history_capacity) {
        for (std::size_t n = 1; n <= history_capacity; ++n) history_[n - 1] = history_[n];
        --history_cursor_;
    }
    history_[++history_cursor_] = {settings_, kind, index};
    history_size_ = history_cursor_ + 1; // discard the old redo branch
    selected_step_ = std::min(selected_step_, settings_.step_count - 1);
    emit changed();
}
void Session::undo() {
    if (!history_cursor_) return;
    const auto name = editName(history_[history_cursor_]);
    if (!replace(history_[history_cursor_ - 1].settings)) return;
    --history_cursor_;
    selected_step_ = std::min(selected_step_, settings_.step_count - 1);
    message_ = "Undid " + name + "; session returned to the start.";
    emit changed();
}
void Session::redo() {
    if (history_cursor_ + 1 >= history_size_) return;
    const auto name = editName(history_[history_cursor_ + 1]);
    if (!replace(history_[history_cursor_ + 1].settings)) return;
    ++history_cursor_;
    selected_step_ = std::min(selected_step_, settings_.step_count - 1);
    message_ = "Redid " + name + "; session returned to the start.";
    emit changed();
}
bool Session::editable() {
    status_ = prepared_->controller.status();
    if (!canEdit()) {
        message_ = "Stop transport and wait for acknowledgement before editing or loading.";
        emit changed();
        return false;
    }
    return true;
}
bool Session::replace(const ProjectSettings& settings) {
    if (!editable()) return false;
    try {
        // Allocate and validate a candidate before interrupting the existing backend.
        auto candidate = std::make_unique<Prepared>(settings);
        prepared_->backend.reset();
        try {
            if (!preview_) candidate->backend = std::make_unique<fl_live::PipeWireBackend>(candidate->controller, config_);
        } catch (...) {
            failed_ = true; // old document survives; processing has been joined
            status_ = prepared_->controller.status();
            throw;
        }
        prepared_.swap(candidate); // old plan is destroyed only on the GUI thread
        settings_ = settings;
        serial_ = 0;
        track_serials_.fill(0);
        status_ = prepared_->controller.status();
        message_ = "Session prepared at the start. Launch clips, then press Play.";
        return true;
    } catch (const std::exception& error) {
        message_ = QString::fromUtf8(error.what());
        if (failed_) message_ += ". Audio is stopped; restart the app to reconnect.";
        emit changed();
        return false;
    }
}
void Session::setTempo(int value) {
    auto next = settings_; next.tempo = value;
    edit(next, EditKind::tempo);
}
void Session::setLevel(int track, int value) {
    if (track < 0 || track >= 2) return;
    auto next = settings_; next.levels[static_cast<std::size_t>(track)] = value;
    edit(next, EditKind::level, track);
}
void Session::setPitch(int note, int value) {
    if (note < 0 || note >= settings_.step_count) return;
    auto next = settings_; next.steps[static_cast<std::size_t>(note)].pitch = value;
    edit(next, EditKind::pitch, note);
}
bool Session::saveProject(const QString& path) {
    try {
        if (path.trimmed().isEmpty()) throw std::invalid_argument("Enter a project file path");
        const auto target = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        if (QFileInfo::exists(target) && target != owned_path_)
            throw std::invalid_argument("File already exists. Open that project first or choose a new path.");
        writeProject(target, settings_);
        project_path_ = owned_path_ = target;
        saved_settings_ = settings_;
        message_ = "Project saved";
        emit projectPathChanged();
        emit changed();
        return true;
    } catch (const std::exception& error) { message_ = QString::fromUtf8(error.what()); emit changed(); return false; }
}
bool Session::loadProject(const QString& path) {
    if (dirty()) { message_ = "Save your changes before opening another project."; emit changed(); return false; }
    if (!editable()) return false;
    try {
        if (!QFileInfo(path).isFile()) throw std::invalid_argument("Choose a regular project file");
        const auto target = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        const auto next = readProject(target);
        if (!replace(next)) return false;
        project_path_ = owned_path_ = target;
        saved_settings_ = settings_;
        selected_step_ = 0;
        history_[0] = {settings_, EditKind::none, -1};
        history_cursor_ = 0;
        history_size_ = 1;
        message_ = "Project opened. Launch clips, then press Play.";
        emit projectPathChanged();
        emit changed();
        return true;
    } catch (const std::exception& error) { message_ = QString::fromUtf8(error.what()); emit changed(); return false; }
}
QString Session::position() const {
    constexpr std::uint64_t frames_per_minute = 48000ULL * 60;
    // Split the product so a long-running playhead cannot overflow the display.
    const auto beat = status_.position / frames_per_minute * static_cast<std::uint64_t>(settings_.tempo) +
        (status_.position % frames_per_minute) * static_cast<std::uint64_t>(settings_.tempo) / frames_per_minute;
    return QString("%1.%2").arg(beat / 4 + 1).arg(beat % 4 + 1);
}
void Session::send(fl_live::ControlKind kind, std::size_t track, std::size_t clip) {
    if (failed_) return;
    const auto next = serial_ + 1;
    if (prepared_->controller.submit({kind, {fl_live::CommandKind::launch, 0, track, clip}, next})) {
        serial_ = next;
        if ((kind == fl_live::ControlKind::launch_next_bar || kind == fl_live::ControlKind::stop_next_bar) && track < 2) {
            track_serials_[track] = next;
            track_requests_[track] = kind;
        }
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
    prepared_->controller.panic();
    message_ = "Silence requested; awaiting the rendering thread";
    emit changed();
}
void Session::tick() {
    if (preview_) {
        std::array<float, 480> discarded{};
        prepared_->controller.process(discarded); // simulated clock, no audio output
    }
    if (prepared_->backend && prepared_->backend->status().error) {
        prepared_->backend.reset();
        failed_ = true;
        message_ = "Audio backend disconnected. Restart the application to reconnect.";
    }
    const auto before = status_;
    status_ = prepared_->controller.status();
    if (!failed_ && status_.handled_serial != before.handled_serial)
        message_ = status_.rejected != before.rejected ? "Engine rejected a scheduled command" :
            "Render acknowledged. Track indicators show active clips and queued actions.";
    emit changed();
}
