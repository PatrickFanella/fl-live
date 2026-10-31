#include "session.hpp"
#include <array>
#include <algorithm>
#include <QDir>
#include <QFileInfo>
#include <stdexcept>

namespace {
std::uint64_t parseIdentity(const QString& text) {
    bool valid=false;const auto id=text.toULongLong(&valid);
    if(!valid || !id || QString::number(id)!=text)throw std::invalid_argument("Invalid document identity");
    return id;
}
QString identityText(std::uint64_t id){return QString::number(static_cast<qulonglong>(id));}
}

Session::Session(fl_live::PipeWireConfig config, QObject* parent)
    : QObject(parent), config_(std::move(config)), prepared_(std::make_unique<Prepared>(settings_, *identities_)),
      project_path_(QDir::home().filePath("fl-live-project.json")), preview_(config_.remote.empty()) {
    history_[0] = {settings_, identities_, EditKind::none, -1, selected_scene_};
    saved_identities_ = identities_;
    saved_settings_ = settings_;
    if (!preview_) {
        try { prepared_->backend = std::make_unique<fl_live::PipeWireBackend>(prepared_->controller, config_); }
        catch (const std::exception& error) { audioFailed(QString::fromUtf8(error.what())); }
    }
    timer_.setInterval(10);
    connect(&timer_, &QTimer::timeout, this, &Session::tick);
    timer_.start();
}
Session::~Session() {
    timer_.stop();
    // Prepared's backend is destroyed before its controller and immutable plan.
    prepared_.reset();
}
void Session::audioFailed(const QString& reason) {
    // Destruction joins processing before the GUI becomes the controller owner.
    prepared_->backend.reset();
    prepared_->controller.finish();
    status_ = prepared_->controller.status();
    failed_ = true;
    message_ = reason + ". Audio is stopped. Save your project or choose Retry audio after restoring the server/output.";
    emit changed();
}
bool Session::reconnectAudio() {
    if (preview_ || !failed_) return false;
    try {
        // Fresh stopped runtime prevents old launches or notes resuming. Musical
        // settings, saved-content tracking, selection and history stay intact.
        auto candidate = std::make_unique<Prepared>(settings_, *identities_);
        candidate->backend = std::make_unique<fl_live::PipeWireBackend>(candidate->controller, config_);
        prepared_.swap(candidate);
        failed_ = false;
        serial_ = 0;
        scene_serial_ = 0;
        track_serials_.fill(0);
        status_ = prepared_->controller.status();
        message_ = "Audio reconnected and stopped. Launch clips, then press Play.";
        emit changed();
        return true;
    } catch (const std::exception& error) {
        audioFailed(QString::fromUtf8(error.what()));
        return false;
    }
}
QVariantList Session::activeClips() const {
    return {status_.tracks[0].active_clip, status_.tracks[1].active_clip};
}
QVariantList Session::trackNames() const {
    QVariantList result;for(const auto& track:identities_->tracks)result.append(QString::fromUtf8(track.name));return result;
}
QVariantList Session::trackIds() const {
    QVariantList result;for(const auto& track:identities_->tracks)result.append(identityText(track.id.value));return result;
}
QVariantList Session::clipIds() const {
    QVariantList result;for(std::size_t i=0;i<2;++i)result.append(identityText(prepared_->mapped.clipId(i,0)->value));return result;
}
QVariantList Session::clipNames() const {
    QVariantList result;
    for(std::size_t i=0;i<2;++i) {
        const auto id=*prepared_->mapped.clipId(i,0);
        const auto found=std::find_if(identities_->clips.begin(),identities_->clips.end(),[id](const auto& clip){return clip.id==id;});
        result.append(QString::fromUtf8(found->name));
    }
    return result;
}
QVariantList Session::activeClipIds() const {
    QVariantList result;for(std::size_t i=0;i<2;++i) {
        const auto id=prepared_->mapped.clipId(i,status_.tracks[i].active_clip);
        result.append(id ? identityText(id->value) : QString{});
    }return result;
}
const fl_live::DocumentScene& Session::selectedScene() const {
    return *std::find_if(identities_->scenes.begin(),identities_->scenes.end(),[&](const auto& scene){return scene.id==selected_scene_;});
}
QVariantList Session::sceneIds() const {
    QVariantList result;for(const auto& scene:identities_->scenes)result.append(identityText(scene.id.value));return result;
}
QVariantList Session::sceneNames() const {
    QVariantList result;for(const auto& scene:identities_->scenes)result.append(QString::fromUtf8(scene.name));return result;
}
int Session::selectedSceneIndex() const {
    return static_cast<int>(std::find_if(identities_->scenes.begin(),identities_->scenes.end(),[&](const auto& scene){return scene.id==selected_scene_;})-identities_->scenes.begin());
}
void Session::restoreSceneSelection(fl_live::SceneId id,bool notify) {
    const auto found=std::find_if(identities_->scenes.begin(),identities_->scenes.end(),[id](const auto& scene){return scene.id==id;});
    selected_scene_=found==identities_->scenes.end() ? identities_->scenes[0].id : id;
    if(notify)emit sceneSelectionChanged();
}
bool Session::selectScene(const QString& id) {
    try {
        const fl_live::SceneId target{parseIdentity(id)};
        if(std::none_of(identities_->scenes.begin(),identities_->scenes.end(),[target](const auto& scene){return scene.id==target;}))
            throw std::invalid_argument("Unknown scene");
        restoreSceneSelection(target);emit changed();return true;
    }catch(const std::exception& error){message_=QString::fromUtf8(error.what());emit changed();return false;}
}
bool Session::createScene() { return addScene(std::nullopt); }
bool Session::copyScene(const QString& id) {
    try { return addScene(fl_live::SceneId{parseIdentity(id)}); }
    catch(const std::exception& error){message_=QString::fromUtf8(error.what());emit changed();return false;}
}
bool Session::addScene(std::optional<fl_live::SceneId> source) {
    if(!editable())return false;
    try {
        const auto previous=selected_scene_;
        const auto found=std::find_if(identities_->scenes.begin(),identities_->scenes.end(),[source](const auto& scene){return source && scene.id==*source;});
        if(source && found==identities_->scenes.end())throw std::invalid_argument("Unknown scene");
        auto snapshot=*identities_;snapshot.next_id=next_identity_;
        fl_live::SessionDocument candidate(std::move(snapshot));
        const auto id=candidate.createScene(source ? "Scene copy" : "New scene");
        // Copy an arrangement of shared source clips. The general document's
        // duplicateScene operation instead duplicates clip identities too.
        if(source)for(const auto& entry:found->entries)candidate.setSlot(id,entry.track,entry.clip);
        auto identities=std::make_shared<const fl_live::DocumentSnapshot>(candidate.snapshot());
        if(!replace(settings_,identities))return false;
        restoreSceneSelection(id);pushHistory(source ? EditKind::scene_copy : EditKind::scene_create,-1);
        history_[history_cursor_].undo_selection=previous;emit changed();return true;
    }catch(const std::exception& error){message_=QString::fromUtf8(error.what());emit changed();return false;}
}
bool Session::moveScene(const QString& id,int direction) {
    if(!editable())return false;
    try {
        if(direction!=-1 && direction!=1)throw std::invalid_argument("Scene movement must be up or down");
        const fl_live::SceneId target{parseIdentity(id)};
        auto snapshot=*identities_;
        const auto found=std::find_if(snapshot.scenes.begin(),snapshot.scenes.end(),[target](const auto& scene){return scene.id==target;});
        if(found==snapshot.scenes.end())throw std::invalid_argument("Unknown scene");
        const auto index=static_cast<std::size_t>(found-snapshot.scenes.begin());
        if((direction<0 && index==0) || (direction>0 && index+1==snapshot.scenes.size()))
            throw std::invalid_argument("Scene is already at that end of the list");
        const auto previous=selected_scene_;
        std::iter_swap(found,found+direction);
        auto identities=std::make_shared<const fl_live::DocumentSnapshot>(std::move(snapshot));
        if(!replace(settings_,identities))return false;
        restoreSceneSelection(target);pushHistory(EditKind::scene_order,-1);
        history_[history_cursor_].undo_selection=previous;emit changed();return true;
    }catch(const std::exception& error){message_=QString::fromUtf8(error.what());emit changed();return false;}
}
bool Session::deleteScene(const QString& id) {
    if(!editable())return false;
    try {
        const fl_live::SceneId target{parseIdentity(id)};
        if(identities_->scenes.size()==1)throw std::invalid_argument("Keep at least one scene");
        const auto found=std::find_if(identities_->scenes.begin(),identities_->scenes.end(),[target](const auto& scene){return scene.id==target;});
        if(found==identities_->scenes.end())throw std::invalid_argument("Unknown scene");
        const auto index=static_cast<std::size_t>(found-identities_->scenes.begin());
        const auto neighbor=identities_->scenes[index ? index-1 : 1].id;
        fl_live::SessionDocument candidate(*identities_);candidate.deleteScene(target);
        auto identities=std::make_shared<const fl_live::DocumentSnapshot>(candidate.snapshot());
        const bool selected=target==selected_scene_;
        if(!replace(settings_,identities))return false;
        if(selected)restoreSceneSelection(neighbor);
        pushHistory(EditKind::scene_delete,-1);history_[history_cursor_].undo_selection=target;
        emit changed();return true;
    }catch(const std::exception& error){message_=QString::fromUtf8(error.what());emit changed();return false;}
}
QString Session::sceneId() const { return identityText(selectedScene().id.value); }
QString Session::sceneName() const { return QString::fromUtf8(selectedScene().name); }
QVariantList Session::sceneIncluded() const {
    QVariantList result;
    for(const auto& track:identities_->tracks)result.append(std::any_of(selectedScene().entries.begin(),
        selectedScene().entries.end(),[&](const auto& entry){return entry.track==track.id;}));
    return result;
}
bool Session::setSceneIncluded(const QString& trackId,bool included) {
    if(!editable())return false;
    try {
        const fl_live::TrackId id{parseIdentity(trackId)};
        const auto index=prepared_->mapped.trackIndex(id);
        if(!index)throw std::invalid_argument("Unknown scene track");
        fl_live::SessionDocument candidate(*identities_);
        if(included)candidate.setSlot(selectedScene().id,id,*prepared_->mapped.clipId(*index,0));
        else candidate.clearSlot(selectedScene().id,id);
        if(candidate.snapshot()==*identities_)return true;
        auto identities=std::make_shared<const fl_live::DocumentSnapshot>(candidate.snapshot());
        if(!replace(settings_,identities))return false;
        pushHistory(EditKind::scene_slot,static_cast<int>(*index));emit changed();return true;
    }catch(const std::exception& error){message_=QString::fromUtf8(error.what());emit changed();return false;}
}
QString Session::sceneState() const {
    if(requested_scene_==selected_scene_ && scene_serial_>status_.handled_serial)return scene_stop_ ? "Stop requested" : "Launch requested";
    bool active=true,stopped=true,queued=false;
    const auto batch=*prepared_->mapped.scene(selectedScene().id,false);
    for(std::size_t i=0;i<batch.count;++i) {
        const auto& command=batch.commands[i];const auto& state=status_.tracks[command.track];
        const auto expected=command.kind==fl_live::CommandKind::launch ? static_cast<int>(command.clip) : -1;
        active=active && state.active_clip==expected;
        stopped=stopped && state.active_clip<0;queued=queued || state.pending!=fl_live::PendingAction::none;
    }
    const auto current=stopped ? QStringLiteral("Stopped") : active ? QStringLiteral("Active") : QStringLiteral("Partial");
    return queued ? current+QStringLiteral(" · actions queued") : current;
}
bool Session::launchScene(const QString& id,const QString& policy){return sceneAction(id,policy,false);}
bool Session::stopScene(const QString& id,const QString& policy){return sceneAction(id,policy,true);}
bool Session::sceneAction(const QString& id,const QString& policy,bool stop) {
    if(failed_)return false;
    try {
        if(policy!="immediate" && policy!="one-bar")throw std::invalid_argument("Unknown scene timing policy");
        const auto batch=prepared_->mapped.scene(fl_live::SceneId{parseIdentity(id)},stop);
        if(!batch)throw std::invalid_argument("Unknown scene");
        const auto next=serial_+1;
        if(!prepared_->controller.submit({fl_live::ControlKind::scene,{},next,*batch,
            policy=="immediate" ? fl_live::LaunchPolicy::immediate : fl_live::LaunchPolicy::next_bar})) {
            message_="Control queue full. Try again, or use Silence now.";emit changed();return false;
        }
        serial_=scene_serial_=next;scene_stop_=stop;requested_scene_=fl_live::SceneId{parseIdentity(id)};
        for(std::size_t i=0;i<batch->count;++i) {
            const auto& command=batch->commands[i];track_serials_[command.track]=next;
            track_requests_[command.track]=command.kind==fl_live::CommandKind::launch
                ? fl_live::ControlKind::launch_next_bar : fl_live::ControlKind::stop_next_bar;
        }
        message_="Awaiting scene render acknowledgement";emit changed();return true;
    }catch(const std::exception& error){message_=QString::fromUtf8(error.what());emit changed();return false;}
}
bool Session::renameTrack(const QString& id,const QString& name){return rename(id,name,NameKind::track);}
bool Session::renameClip(const QString& id,const QString& name){return rename(id,name,NameKind::clip);}
bool Session::renameScene(const QString& id,const QString& name){return rename(id,name,NameKind::scene);}
bool Session::rename(const QString& id,const QString& name,NameKind kind) {
    if(!editable())return false;
    try {
        const auto value=parseIdentity(id);const auto bytes=name.toUtf8();
        if(QString::fromUtf8(bytes)!=name)throw std::invalid_argument("Name requires valid Unicode");
        fl_live::SessionDocument candidate(*identities_);
        if(kind==NameKind::track)candidate.renameTrack(fl_live::TrackId{value},bytes.toStdString());
        else if(kind==NameKind::clip)candidate.renameClip(fl_live::ClipId{value},bytes.toStdString());
        else candidate.renameScene(fl_live::SceneId{value},bytes.toStdString());
        if(candidate.snapshot()==*identities_)return true;
        auto identities=std::make_shared<const fl_live::DocumentSnapshot>(candidate.snapshot());
        if(!replace(settings_,identities))return false;
        const auto index=kind==NameKind::scene ? 0 : kind==NameKind::track ? *prepared_->mapped.trackIndex(fl_live::TrackId{value}) : prepared_->mapped.launch(fl_live::ClipId{value},0)->track;
        pushHistory(kind==NameKind::scene ? EditKind::scene_name : kind==NameKind::track ? EditKind::track_name : EditKind::clip_name,static_cast<int>(index));
        emit changed();return true;
    }catch(const std::exception& error){message_=QString::fromUtf8(error.what());emit changed();return false;}
}
bool Session::launchClip(const QString& id) {
    if(failed_)return false;
    try {
        const auto command=prepared_->mapped.launch(fl_live::ClipId{parseIdentity(id)},0);
        if(!command)throw std::invalid_argument("Unknown clip");
        const auto before=serial_;send(fl_live::ControlKind::launch_next_bar,command->track,command->clip);return serial_!=before;
    }catch(const std::exception& error){message_=QString::fromUtf8(error.what());emit changed();return false;}
}
bool Session::stopTrackId(const QString& id) {
    if(failed_)return false;
    try {
        const auto command=prepared_->mapped.stop(fl_live::TrackId{parseIdentity(id)},0);
        if(!command)throw std::invalid_argument("Unknown track");
        const auto before=serial_;send(fl_live::ControlKind::stop_next_bar,command->track);return serial_!=before;
    }catch(const std::exception& error){message_=QString::fromUtf8(error.what());emit changed();return false;}
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
    case EditKind::track_name: return QString("track %1 name").arg(entry.index + 1);
    case EditKind::clip_name: return QString("clip %1 name").arg(entry.index + 1);
    case EditKind::scene_name: return "scene name";
    case EditKind::scene_slot: return QString("track %1 scene slot").arg(entry.index+1);
    case EditKind::scene_create: return "new scene";
    case EditKind::scene_delete: return "scene deletion";
    case EditKind::scene_copy: return "scene copy";
    case EditKind::scene_order: return "scene order";
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
    pushHistory(kind, index);
    selected_step_ = std::min(selected_step_, settings_.step_count - 1);
    emit changed();
}
void Session::pushHistory(EditKind kind, int index) {
    // Scalar settings and immutable shared metadata copy without allocation.
    if (history_cursor_ == history_capacity) {
        for (std::size_t n = 1; n <= history_capacity; ++n) history_[n - 1] = history_[n];
        --history_cursor_;
    }
    history_[++history_cursor_] = {settings_, identities_, kind, index, selected_scene_, selected_scene_};
    history_size_ = history_cursor_ + 1; // discard the old redo branch
}
void Session::undo() {
    if (!history_cursor_) return;
    const auto name = editName(history_[history_cursor_]);
    const auto selection=history_[history_cursor_].undo_selection;
    if (!replace(history_[history_cursor_ - 1].settings, history_[history_cursor_ - 1].identities)) return;
    --history_cursor_;
    restoreSceneSelection(selection);
    selected_step_ = std::min(selected_step_, settings_.step_count - 1);
    message_ = "Undid " + name + "; session returned to the start.";
    emit changed();
}
void Session::redo() {
    if (history_cursor_ + 1 >= history_size_) return;
    const auto name = editName(history_[history_cursor_ + 1]);
    if (!replace(history_[history_cursor_ + 1].settings, history_[history_cursor_ + 1].identities)) return;
    ++history_cursor_;
    restoreSceneSelection(history_[history_cursor_].selection);
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
bool Session::replace(const ProjectSettings& settings, Identities identities) {
    if (!identities) identities = identities_;
    if (!editable()) return false;
    try {
        // Allocate and validate a candidate before interrupting the existing backend.
        auto candidate = std::make_unique<Prepared>(settings, *identities);
        auto preparedMessage = QStringLiteral("Session prepared at the start. Launch clips, then press Play.");
        prepared_->backend.reset();
        try {
            if (!preview_) candidate->backend = std::make_unique<fl_live::PipeWireBackend>(candidate->controller, config_);
        } catch (...) {
            audioFailed("Cannot prepare audio"); // old document survives
            throw;
        }
        prepared_.swap(candidate); // old plan is destroyed only on the GUI thread
        settings_ = settings;
        identities_ = std::move(identities);
        next_identity_=std::max(next_identity_,identities_->next_id);
        restoreSceneSelection(selected_scene_,false);
        emit documentChanged();
        emit sceneSelectionChanged();
        serial_ = 0;
        scene_serial_ = 0;
        track_serials_.fill(0);
        status_ = prepared_->controller.status();
        message_.swap(preparedMessage);
        return true;
    } catch (const std::exception& error) {
        message_ = QString::fromUtf8(error.what());
        if (failed_) message_ += ". Audio is stopped. Save your project or choose Retry audio.";
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
        auto persisted=*identities_;persisted.next_id=next_identity_;
        writeAuthoredProject(target, {settings_, persisted});
        project_path_ = owned_path_ = target;
        saved_settings_ = settings_;
        saved_identities_ = identities_;
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
        const auto next = readAuthoredProject(target);
        auto identities = std::make_shared<const fl_live::DocumentSnapshot>(next.identities);
        if (!replace(next.settings, identities)) return false;
        next_identity_=next.identities.next_id;
        restoreSceneSelection(next.identities.scenes[0].id);
        project_path_ = owned_path_ = target;
        saved_settings_ = settings_;
        saved_identities_ = identities_;
        selected_step_ = 0;
        history_[0] = {settings_, identities_, EditKind::none, -1, selected_scene_};
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
    launchClip(clipIds()[track].toString());
}
void Session::stopTrack(int track) {
    if (track < 0 || track >= 2) return;
    stopTrackId(trackIds()[track].toString());
}
void Session::panic() {
    if (failed_) return;
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
        audioFailed("Audio backend disconnected");
    }
    const auto before = status_;
    status_ = prepared_->controller.status();
    if (!failed_ && status_.handled_serial != before.handled_serial)
        message_ = status_.rejected != before.rejected ? "Engine rejected a scheduled command" :
            "Render acknowledged. Track indicators show active clips and queued actions.";
    emit changed();
}

bool Session::exportWav(const QString& path, int bars) {
    if (!editable()) return false;
    try {
        exportProjectWav(path, settings_, bars);
        message_ = QString("Exported %1 bars: both tracks from the start, mono 48 kHz / 16-bit WAV").arg(bars);
        emit changed();
        return true;
    } catch (const std::exception& error) {
        message_ = QString::fromUtf8(error.what());
        emit changed();
        return false;
    }
}
