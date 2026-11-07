#include "session.hpp"
#include <array>
#include <algorithm>
#include <QDir>
#include <QFileInfo>
#include <stdexcept>
#include <cmath>

namespace {
std::uint64_t parseIdentity(const QString& text) {
    bool valid=false;const auto id=text.toULongLong(&valid);
    if(!valid || !id || QString::number(id)!=text)throw std::invalid_argument("Invalid document identity");
    return id;
}
QString identityText(std::uint64_t id){return QString::number(static_cast<qulonglong>(id));}
// The desktop always renders the stereo mix; mono remains an engine option.
fl_live::PipeWireConfig stereo(fl_live::PipeWireConfig config){config.channels=2;return config;}
}

Session::Session(fl_live::PipeWireConfig config, QObject* parent)
    : QObject(parent), config_(stereo(std::move(config))), prepared_(std::make_unique<Prepared>(*content_, *identities_)),
      project_path_(QDir::home().filePath("fl-live-project.json")), preview_(config_.remote.empty()) {
    history_[0] = {content_, identities_, EditKind::none, -1, selected_scene_};
    saved_identities_ = identities_;
    saved_content_ = content_;
    restorePatternTarget();
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
    if (export_job_) export_job_->progress.cancel.store(true);
    if (export_thread_.joinable()) export_thread_.join();
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
        auto candidate = std::make_unique<Prepared>(*content_, *identities_);
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
    QVariantList result;for(std::size_t i=0;i<identities_->tracks.size();++i)result.append(status_.tracks[i].active_clip);return result;
}
QVariantList Session::trackNames() const {
    QVariantList result;for(const auto& track:identities_->tracks)result.append(QString::fromUtf8(track.name));return result;
}
QVariantList Session::trackIds() const {
    QVariantList result;for(const auto& track:identities_->tracks)result.append(identityText(track.id.value));return result;
}
QVariantList Session::clipIds() const {
    QVariantList result;for(std::size_t i=0;i<identities_->tracks.size();++i)result.append(identityText(prepared_->mapped.clipId(i,0)->value));return result;
}
QVariantList Session::clipNames() const {
    QVariantList result;
    for(std::size_t i=0;i<identities_->tracks.size();++i) {
        const auto id=*prepared_->mapped.clipId(i,0);
        const auto found=std::find_if(identities_->clips.begin(),identities_->clips.end(),[id](const auto& clip){return clip.id==id;});
        result.append(QString::fromUtf8(found->name));
    }
    return result;
}
QVariantList Session::activeClipIds() const {
    QVariantList result;for(std::size_t i=0;i<identities_->tracks.size();++i) {
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
        if(!replace(content_,identities))return false;
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
        if(!replace(content_,identities))return false;
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
        if(!replace(content_,identities))return false;
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
        if(!replace(content_,identities))return false;
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
        if(!replace(content_,identities))return false;
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
    for (std::size_t i = 0; i < identities_->tracks.size(); ++i) {
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
    QVariantList result;
    for (const auto& track : identities_->tracks) result.append(trackContent(*content_, track.id).level);
    return result;
}
QVariantList Session::pans() const {
    QVariantList result;for(const auto& track:identities_->tracks)result.append(track.mix.pan);return result;
}
QVariantList Session::mutes() const {
    QVariantList result;for(const auto& track:identities_->tracks)result.append(track.mix.mute);return result;
}
QVariantList Session::solos() const {
    QVariantList result;for(const auto& track:identities_->tracks)result.append(track.mix.solo);return result;
}
bool Session::setPan(const QString& id,int value){return editMix(id,MixField::pan,value);}
bool Session::setMute(const QString& id,bool value){return editMix(id,MixField::mute,value);}
bool Session::setSolo(const QString& id,bool value){return editMix(id,MixField::solo,value);}
bool Session::editMix(const QString& trackId,MixField field,int value) {
    if(!editable())return false;
    try {
        const fl_live::TrackId id{parseIdentity(trackId)};
        const auto index=prepared_->mapped.trackIndex(id);
        if(!index)throw std::invalid_argument("Unknown track");
        auto mix=identities_->tracks[*index].mix;
        if(field==MixField::pan)mix.pan=value;else if(field==MixField::mute)mix.mute=value!=0;else mix.solo=value!=0;
        fl_live::SessionDocument candidate(*identities_);candidate.setTrackMix(id,mix);
        if(candidate.snapshot()==*identities_)return true;
        auto identities=std::make_shared<const fl_live::DocumentSnapshot>(candidate.snapshot());
        if(!replace(content_,identities))return false;
        pushHistory(field==MixField::pan ? EditKind::track_pan : field==MixField::mute ? EditKind::track_mute : EditKind::track_solo,static_cast<int>(*index));
        emit changed();return true;
    }catch(const std::exception& error){message_=QString::fromUtf8(error.what());emit changed();return false;}
}
QVariantList Session::pitches() const {
    QVariantList result;
    for (const auto& step : pattern().steps) result.append(step.pitch);
    return result;
}
QVariantList Session::stepEnabled() const {
    QVariantList result;
    for (const auto& step : pattern().steps) result.append(step.enabled);
    return result;
}
QVariantList Session::velocities() const {
    QVariantList result;
    for (const auto& step : pattern().steps) result.append(step.velocity);
    return result;
}
QVariantList Session::gates() const {
    QVariantList result;
    for (const auto& step : pattern().steps) result.append(step.gate);
    return result;
}
void Session::selectStep(int index) {
    if (index < 0 || index >= pattern().step_count) return;
    selected_step_ = index;
    emit changed();
}
void Session::setStepCount(int value) {
    editPatternStep(EditKind::length, -1, [value](StepPattern& p) { p.step_count = value; });
}
void Session::setStepEnabled(int index, bool value) {
    if (index < 0 || index >= pattern().step_count) return;
    editPatternStep(EditKind::enabled, index, [&](StepPattern& p) { p.steps[static_cast<std::size_t>(index)].enabled = value; });
}
void Session::setVelocity(int index, int value) {
    if (index < 0 || index >= pattern().step_count) return;
    editPatternStep(EditKind::velocity, index, [&](StepPattern& p) { p.steps[static_cast<std::size_t>(index)].velocity = value; });
}
void Session::setGate(int index, int value) {
    if (index < 0 || index >= pattern().step_count) return;
    editPatternStep(EditKind::gate, index, [&](StepPattern& p) { p.steps[static_cast<std::size_t>(index)].gate = value; });
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
    case EditKind::track_pan: return QString("track %1 pan").arg(entry.index+1);
    case EditKind::track_mute: return QString("track %1 mute").arg(entry.index+1);
    case EditKind::track_solo: return QString("track %1 solo").arg(entry.index+1);
    case EditKind::track_create: return "new track";
    case EditKind::track_duplicate: return QString("track %1 duplicate").arg(entry.index+1);
    case EditKind::track_delete: return "track deletion";
    case EditKind::audio_import: return QString("track %1 audio import").arg(entry.index+1);
    case EditKind::clip_range: return QString("track %1 clip range").arg(entry.index+1);
    case EditKind::audio_relink: return QString("track %1 audio relink").arg(entry.index+1);
    case EditKind::assets_collect: return "audio collection";
    default: return {};
    }
}
QString Session::undoText() const {
    return history_cursor_ ? "Undo " + editName(history_[history_cursor_]) : "Undo";
}
QString Session::redoText() const {
    return history_cursor_ + 1 < history_size_ ? "Redo " + editName(history_[history_cursor_ + 1]) : "Redo";
}
void Session::edit(const SessionContent& next, EditKind kind, int index) {
    if (next == *content_ || !replace(std::make_shared<const SessionContent>(next))) return;
    pushHistory(kind, index);
    selected_step_ = std::min(selected_step_, pattern().step_count - 1);
    emit changed();
}
void Session::pushHistory(EditKind kind, int index) {
    // Scalar settings and immutable shared metadata copy without allocation.
    if (history_cursor_ == history_capacity) {
        for (std::size_t n = 1; n <= history_capacity; ++n) history_[n - 1] = history_[n];
        --history_cursor_;
    }
    history_[++history_cursor_] = {content_, identities_, kind, index, selected_scene_, selected_scene_};
    history_size_ = history_cursor_ + 1; // discard the old redo branch
}
void Session::undo() {
    if (!history_cursor_) return;
    const auto name = editName(history_[history_cursor_]);
    const auto selection=history_[history_cursor_].undo_selection;
    if (!replace(history_[history_cursor_ - 1].content, history_[history_cursor_ - 1].identities)) return;
    --history_cursor_;
    restoreSceneSelection(selection);
    selected_step_ = std::min(selected_step_, pattern().step_count - 1);
    message_ = "Undid " + name + "; session returned to the start.";
    emit changed();
}
void Session::redo() {
    if (history_cursor_ + 1 >= history_size_) return;
    const auto name = editName(history_[history_cursor_ + 1]);
    if (!replace(history_[history_cursor_ + 1].content, history_[history_cursor_ + 1].identities)) return;
    ++history_cursor_;
    restoreSceneSelection(history_[history_cursor_].selection);
    selected_step_ = std::min(selected_step_, pattern().step_count - 1);
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
bool Session::replace(Content content, Identities identities) {
    if (!identities) identities = identities_;
    if (!editable()) return false;
    try {
        // Allocate and validate a candidate before interrupting the existing backend.
        auto candidate = std::make_unique<Prepared>(*content, *identities);
        auto preparedMessage = QStringLiteral("Session prepared at the start. Launch clips, then press Play.");
        prepared_->backend.reset();
        try {
            if (!preview_) candidate->backend = std::make_unique<fl_live::PipeWireBackend>(candidate->controller, config_);
        } catch (...) {
            audioFailed("Cannot prepare audio"); // old document survives
            throw;
        }
        prepared_.swap(candidate); // old plan is destroyed only on the GUI thread
        content_ = std::move(content);
        identities_ = std::move(identities);
        restorePatternTarget();
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
    auto next = *content_; next.tempo = value;
    edit(next, EditKind::tempo);
}
void Session::setLevel(int track, int value) {
    if (track < 0 || static_cast<std::size_t>(track) >= identities_->tracks.size()) return;
    auto next = *content_;
    const auto id = identities_->tracks[static_cast<std::size_t>(track)].id;
    std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto& t) { return t.track == id; })->level = value;
    edit(next, EditKind::level, track);
}
void Session::setPitch(int note, int value) {
    if (note < 0 || note >= pattern().step_count) return;
    editPatternStep(EditKind::pitch, note, [&](StepPattern& p) { p.steps[static_cast<std::size_t>(note)].pitch = value; });
}
bool Session::saveProject(const QString& path) {
    try {
        if (path.trimmed().isEmpty()) throw std::invalid_argument("Enter a project file path");
        const auto target = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        if (QFileInfo::exists(target) && target != owned_path_)
            throw std::invalid_argument("File already exists. Open that project first or choose a new path.");
        auto persisted=*identities_;persisted.next_id=next_identity_;
        writeAuthoredProject(target, {*content_, persisted});
        project_path_ = owned_path_ = target;
        saved_content_ = content_;
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
        auto next = readAuthoredProject(target);
        // Imported WAVs decode here, on the control thread, before the plan is
        // prepared. A missing or refused asset opens as a silent clip.
        const auto report = resolveAssets(next.content, next.relative, QFileInfo(target).absolutePath());
        const auto& missing = report.missing;
        auto identities = std::make_shared<const fl_live::DocumentSnapshot>(next.identities);
        pattern_clip_ = {};
        if (!replace(std::make_shared<const SessionContent>(std::move(next.content)), identities)) return false;
        next_identity_=next.identities.next_id;
        restoreSceneSelection(next.identities.scenes[0].id);
        project_path_ = owned_path_ = target;
        saved_content_ = content_;
        if (!report.relocated.isEmpty()) {
            // The file on disk still names the old locations: unsaved until Save.
            auto saved = *content_;
            for (const auto& [clip, old] : report.previous)
                std::find_if(saved.clips.begin(), saved.clips.end(), [&](const auto& c) { return c.clip == clip; })->asset = old;
            saved_content_ = std::make_shared<const SessionContent>(std::move(saved));
        }
        saved_identities_ = identities_;
        selected_step_ = 0;
        history_[0] = {content_, identities_, EditKind::none, -1, selected_scene_};
        history_cursor_ = 0;
        history_size_ = 1;
        QStringList notes;
        if (!report.relocated.isEmpty()) notes.append("Found moved audio inside the project folder: " + report.relocated.join("; ") + ". Save to keep the new locations.");
        if (!missing.isEmpty()) notes.append("Missing audio plays as silence; relink or import it: " + missing.join("; "));
        message_ = notes.isEmpty() ? QStringLiteral("Project opened. Launch clips, then press Play.") : "Project opened. " + notes.join(" ");
        emit projectPathChanged();
        emit changed();
        return true;
    } catch (const std::exception& error) { message_ = QString::fromUtf8(error.what()); emit changed(); return false; }
}
QString Session::position() const {
    constexpr std::uint64_t frames_per_minute = 48000ULL * 60;
    // Split the product so a long-running playhead cannot overflow the display.
    const auto tempo = static_cast<std::uint64_t>(content_->tempo);
    const auto beat = status_.position / frames_per_minute * tempo + (status_.position % frames_per_minute) * tempo / frames_per_minute;
    return QString("%1.%2").arg(beat / 4 + 1).arg(beat % 4 + 1);
}
void Session::send(fl_live::ControlKind kind, std::size_t track, std::size_t clip) {
    if (failed_) return;
    const auto next = serial_ + 1;
    if (prepared_->controller.submit({kind, {fl_live::CommandKind::launch, 0, track, clip}, next})) {
        serial_ = next;
        if ((kind == fl_live::ControlKind::launch_next_bar || kind == fl_live::ControlKind::stop_next_bar) && track < track_serials_.size()) {
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
    if (track < 0 || static_cast<std::size_t>(track) >= identities_->tracks.size() || clip != 0) { message_ = "Unknown clip"; emit changed(); return; }
    launchClip(clipIds()[track].toString());
}
void Session::stopTrack(int track) {
    if (track < 0 || static_cast<std::size_t>(track) >= identities_->tracks.size()) return;
    stopTrackId(trackIds()[track].toString());
}
void Session::panic() {
    if (failed_) return;
    prepared_->controller.panic();
    message_ = "Silence requested; awaiting the rendering thread";
    emit changed();
}
void Session::tick() {
    finishExport();
    pollImport();
    if (preview_) {
        std::array<float, 960> discarded{};
        prepared_->controller.process(discarded, 2); // 480 stereo frames; simulated clock, no audio output
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
    if (export_job_) { message_ = "An export is already running. Cancel it or wait for it to finish."; emit changed(); return false; }
    if (!editable()) return false;
    try {
        if (bars < 1 || bars > 16) throw std::invalid_argument("Export length must be 1 to 16 bars");
        if (path.trimmed().isEmpty()) throw std::invalid_argument("Enter a new WAV file path");
        auto job = std::make_shared<ExportJob>();
        job->path = path; job->bars = bars;
        job->progress.total.store(exportFrames(content_->tempo, bars));
        // Copies keep the job independent of later edits on the GUI thread.
        export_thread_ = std::thread([job, content = content_, identities = identities_] {
            try {
                const auto outcome = exportProjectWav(job->path, *content, *identities, job->bars, &job->progress);
                job->state.store(outcome == ExportOutcome::completed ? 1 : 2);
            } catch (const std::exception& error) {
                job->error = QString::fromUtf8(error.what());
                job->state.store(3);
            }
        });
        export_job_ = std::move(job);
        message_ = QString("Exporting %1 bars in the background").arg(bars);
        emit changed();
        return true;
    } catch (const std::exception& error) {
        message_ = QString::fromUtf8(error.what());
        emit changed();
        return false;
    }
}
void Session::cancelExport() {
    if (!export_job_) return;
    export_job_->progress.cancel.store(true);
    message_ = "Cancelling export";
    emit changed();
}
double Session::exportProgress() const {
    if (!export_job_) return 0;
    const auto total = export_job_->progress.total.load();
    return total ? static_cast<double>(export_job_->progress.frames.load()) / static_cast<double>(total) : 0;
}
void Session::finishExport() {
    if (!export_job_ || export_job_->state.load() == 0) return;
    export_thread_.join();
    const auto job = std::move(export_job_);
    export_job_.reset();
    switch (job->state.load()) {
    case 1: message_ = QString("Exported %1 bars: every track from the start through the mixer, stereo 48 kHz / 16-bit WAV").arg(job->bars); break;
    case 2: message_ = "Export cancelled; no file was written"; break;
    default: message_ = job->error; break;
    }
}

// ---- General track management -------------------------------------------
namespace {
QString kindText(fl_live::TrackKind kind){return kind==fl_live::TrackKind::audio ? QStringLiteral("audio") : QStringLiteral("midi");}
fl_live::ClipId firstClip(const fl_live::DocumentSnapshot& identities,fl_live::TrackId track) {
    const auto found=std::find_if(identities.clips.begin(),identities.clips.end(),[track](const auto& c){return c.track==track;});
    return found==identities.clips.end() ? fl_live::ClipId{} : found->id;
}
}
QVariantList Session::trackKinds() const {
    QVariantList result;for(const auto& track:identities_->tracks)result.append(kindText(track.kind));return result;
}
QVariantList Session::trackSources() const {
    QVariantList result;
    for(const auto& track:identities_->tracks) {
        const auto& clip=clipContent(*content_,firstClip(*identities_,track.id));
        if(import_ticket_ && track.id==import_track_) { result.append("Importing " + QFileInfo(import_path_).fileName() + "…"); continue; }
        if(track.kind==fl_live::TrackKind::midi) { result.append(QString("MIDI pattern · %1 steps").arg(clip.pattern.step_count)); continue; }
        switch(clip.audio) {
        case AudioSource::pulse: result.append("Built-in pulse"); break;
        case AudioSource::empty: result.append("Empty · import a WAV"); break;
        case AudioSource::wav:
            if(!clip.media) result.append("Missing: " + QFileInfo(clip.asset).fileName());
            else {
                const auto frames=clip.media->samples.size()/clip.media->channels;
                result.append(QString("%1 · %2 s · %3").arg(QFileInfo(clip.asset).fileName())
                    .arg(static_cast<double>(frames)/48000.0,0,'f',2).arg(clip.media->channels==2 ? "stereo" : "mono"));
            }
            break;
        }
    }
    return result;
}
const StepPattern& Session::pattern() const {
    static const StepPattern none{};
    const auto found=std::find_if(content_->clips.begin(),content_->clips.end(),[&](const auto& c){return c.clip==pattern_clip_;});
    return pattern_clip_.value && found!=content_->clips.end() ? found->pattern : none;
}
int Session::patternTrackIndex() const {
    const auto clip=std::find_if(identities_->clips.begin(),identities_->clips.end(),[&](const auto& c){return c.id==pattern_clip_;});
    if(!pattern_clip_.value || clip==identities_->clips.end())return -1;
    const auto index=prepared_->mapped.trackIndex(clip->track);
    return index ? static_cast<int>(*index) : -1;
}
void Session::restorePatternTarget() {
    // Keep the chosen MIDI clip while it exists; otherwise use the first one.
    const auto exists=std::any_of(identities_->clips.begin(),identities_->clips.end(),[&](const auto& c){return c.id==pattern_clip_;});
    if(pattern_clip_.value && exists)return;
    pattern_clip_={};
    for(const auto& track:identities_->tracks)
        if(track.kind==fl_live::TrackKind::midi) { pattern_clip_=firstClip(*identities_,track.id); break; }
    selected_step_=0;
}
bool Session::editPattern(const QString& trackId) {
    try {
        const fl_live::TrackId id{parseIdentity(trackId)};
        const auto track=std::find_if(identities_->tracks.begin(),identities_->tracks.end(),[id](const auto& t){return t.id==id;});
        if(track==identities_->tracks.end() || track->kind!=fl_live::TrackKind::midi)throw std::invalid_argument("Choose a MIDI track to edit its pattern");
        pattern_clip_=firstClip(*identities_,id);selected_step_=0;
        emit changed();return true;
    }catch(const std::exception& error){message_=QString::fromUtf8(error.what());emit changed();return false;}
}
void Session::editPatternStep(EditKind kind,int index,const std::function<void(StepPattern&)>& change) {
    if(patternTrackIndex()<0) { message_="Add or choose a MIDI track to edit a pattern."; emit changed(); return; }
    auto next=*content_;
    auto& clip=*std::find_if(next.clips.begin(),next.clips.end(),[&](const auto& c){return c.clip==pattern_clip_;});
    change(clip.pattern);
    edit(next,kind,index);
}
bool Session::commitStructure(SessionContent next,const fl_live::DocumentSnapshot& identities,EditKind kind,int index) {
    validateContent(next,identities);
    auto snapshot=std::make_shared<const fl_live::DocumentSnapshot>(identities);
    if(!replace(std::make_shared<const SessionContent>(std::move(next)),snapshot))return false;
    pushHistory(kind,index);
    selected_step_=std::min(selected_step_,pattern().step_count-1);
    emit changed();
    return true;
}
bool Session::addTrack(const QString& kind) {
    if(!editable())return false;
    try {
        if(kind!="audio" && kind!="midi")throw std::invalid_argument("Track kind must be audio or MIDI");
        const auto trackKind=kind=="audio" ? fl_live::TrackKind::audio : fl_live::TrackKind::midi;
        auto snapshot=*identities_;snapshot.next_id=next_identity_;
        fl_live::SessionDocument candidate(std::move(snapshot));
        const auto number=candidate.snapshot().tracks.size()+1;
        const auto track=candidate.createTrack((trackKind==fl_live::TrackKind::audio ? "Audio " : "MIDI ")+std::to_string(number),trackKind);
        const auto clip=candidate.createClip(track,trackKind==fl_live::TrackKind::audio ? "Audio clip" : "Pattern");
        auto next=*content_;
        next.tracks.push_back({track,70});
        ClipContent value{clip};
        if(trackKind==fl_live::TrackKind::audio)value.audio=AudioSource::empty;
        else for(auto& step:value.pattern.steps)step.enabled=false; // new patterns start as rests
        next.clips.push_back(std::move(value));
        if(!commitStructure(std::move(next),candidate.snapshot(),EditKind::track_create,static_cast<int>(number-1)))return false;
        if(trackKind==fl_live::TrackKind::midi && patternTrackIndex()<0)pattern_clip_=clip;
        message_=QString("Added %1 track %2").arg(kind=="audio" ? "an audio" : "a MIDI").arg(number);
        emit changed();return true;
    }catch(const std::exception& error){message_=QString::fromUtf8(error.what());emit changed();return false;}
}
bool Session::duplicateTrack(const QString& id) {
    if(!editable())return false;
    try {
        const fl_live::TrackId source{parseIdentity(id)};
        auto snapshot=*identities_;snapshot.next_id=next_identity_;
        fl_live::SessionDocument candidate(std::move(snapshot));
        const auto before=candidate.snapshot().clips.size();
        const auto copy=candidate.duplicateTrack(source);
        const auto& copied=candidate.snapshot();
        auto name=std::find_if(copied.tracks.begin(),copied.tracks.end(),[copy](const auto& t){return t.id==copy;})->name+" copy";
        if(name.size()<=fl_live::SessionDocument::name_byte_limit)candidate.renameTrack(copy,name);
        auto next=*content_;
        next.tracks.push_back({copy,trackContent(*content_,source).level});
        // duplicateTrack appends the new clips in the source track's order.
        std::size_t n=before;
        for(const auto& clip:identities_->clips) if(clip.track==source) {
            auto value=clipContent(*content_,clip.id);value.clip=candidate.snapshot().clips[n++].id;
            next.clips.push_back(std::move(value));
        }
        const auto index=static_cast<int>(candidate.snapshot().tracks.size()-1);
        if(!commitStructure(std::move(next),candidate.snapshot(),EditKind::track_duplicate,index))return false;
        message_="Duplicated track; the copy has new IDs and shares imported audio";emit changed();return true;
    }catch(const std::exception& error){message_=QString::fromUtf8(error.what());emit changed();return false;}
}
bool Session::deleteTrack(const QString& id) {
    if(!editable())return false;
    try {
        const fl_live::TrackId target{parseIdentity(id)};
        fl_live::SessionDocument candidate(*identities_);
        candidate.deleteTrack(target);
        auto next=*content_;
        std::erase_if(next.tracks,[target](const auto& t){return t.track==target;});
        std::erase_if(next.clips,[&](const auto& c){return !std::any_of(candidate.snapshot().clips.begin(),candidate.snapshot().clips.end(),[&](const auto& d){return d.id==c.clip;});});
        if(!commitStructure(std::move(next),candidate.snapshot(),EditKind::track_delete,-1))return false;
        message_="Deleted track; Undo restores it with its IDs";emit changed();return true;
    }catch(const std::exception& error){message_=QString::fromUtf8(error.what());emit changed();return false;}
}
bool Session::relinkAudio(const QString& trackId,const QString& path) {
    try {
        const fl_live::TrackId id{parseIdentity(trackId)};
        const auto clip=firstClip(*identities_,id);
        if(!clip.value || clipContent(*content_,clip).audio!=AudioSource::wav)throw std::invalid_argument("Relink applies to an imported audio clip; use Import for other tracks");
    }catch(const std::exception& error){message_=QString::fromUtf8(error.what());emit changed();return false;}
    if(!importAudio(trackId,path))return false;
    import_relink_=true;
    return true;
}
bool Session::importAudio(const QString& trackId,const QString& path) {
    try {
        if(importing())throw std::invalid_argument("An import is already running. Cancel it or wait for it to finish.");
        const fl_live::TrackId id{parseIdentity(trackId)};
        const auto track=std::find_if(identities_->tracks.begin(),identities_->tracks.end(),[id](const auto& t){return t.id==id;});
        if(track==identities_->tracks.end() || track->kind!=fl_live::TrackKind::audio)throw std::invalid_argument("Choose an audio track to import a WAV");
        if(path.trimmed().isEmpty())throw std::invalid_argument("Enter a WAV file path");
        if(!importer_) {
            // One stereo import fits the whole preparation budget exactly.
            importer_=std::make_unique<fl_live::WavImportWorker>(fl_live::WavImportPolicy{48000, fl_live::PreparationBudget::audio_frame_limit/2});
        }
        import_path_=QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        import_track_=id;import_relink_=false;
        import_ticket_=importer_->submit(import_path_.toStdString());
        message_="Importing " + QFileInfo(import_path_).fileName() + " in the background";
        emit changed();return true;
    }catch(const std::exception& error){message_=QString::fromUtf8(error.what());emit changed();return false;}
}
void Session::cancelImport() {
    if(import_ticket_ && importer_)importer_->cancel(import_ticket_);
    if(ready_import_) { ready_import_.reset(); import_ticket_=0; message_="Import cancelled; the session was not changed"; emit changed(); }
}
void Session::pollImport() {
    if(importer_ && import_ticket_ && !ready_import_) {
        if(auto result=importer_->poll(); result && result->ticket==import_ticket_) {
            if(!result->import) {
                import_ticket_=0;
                message_=result->import.error==fl_live::ImportError::cancelled ? QString::fromStdString(result->import.message)
                    : "Import failed: " + QString::fromStdString(result->import.message);
                emit changed();return;
            }
            ready_import_=std::move(result->import);
        }
    }
    // Apply only while stopped; a finished import waits for Stop.
    if(!ready_import_ || failed_ || running() || pending())return;
    auto import=std::move(*ready_import_);ready_import_.reset();import_ticket_=0;
    const auto track=std::find_if(identities_->tracks.begin(),identities_->tracks.end(),[&](const auto& t){return t.id==import_track_;});
    if(track==identities_->tracks.end()) { message_="Import finished, but its track was deleted; nothing changed"; emit changed(); return; }
    auto next=*content_;
    auto& clip=*std::find_if(next.clips.begin(),next.clips.end(),[&](const auto& c){return c.clip==firstClip(*identities_,import_track_);});
    const bool relink=import_relink_;import_relink_=false;
    const auto frames=import.frames;
    if(relink && (clip.audio!=AudioSource::wav || clip.range_start>=frames || clip.range_length>frames-clip.range_start)) {
        message_="Relink refused: " + QFileInfo(import_path_).fileName() + " is shorter than the clip's range. Import it instead to replace the clip.";
        emit changed();return;
    }
    clip.audio=AudioSource::wav;clip.asset=import_path_;
    if(!relink) { clip.range_start=0;clip.range_length=0;clip.loop=true; } // a new source plays whole
    clip.media=std::make_shared<const fl_live::AudioClip>(std::move(import.clip));
    const auto before=content_;
    edit(next,relink ? EditKind::audio_relink : EditKind::audio_import,static_cast<int>(track-identities_->tracks.begin()));
    if(content_!=before)message_=(relink ? "Relinked " : "Imported ") + QFileInfo(import_path_).fileName() + ". Launch the clip, then press Play.";
    emit changed();
}
Session::AssetReport Session::resolveAssets(SessionContent& content,const std::vector<RelativeAsset>& relative,const QString& projectDirectory) const {
    AssetReport report;
    const fl_live::WavImportPolicy policy{48000,fl_live::PreparationBudget::audio_frame_limit/2};
    for(auto& clip:content.clips) {
        if(clip.audio!=AudioSource::wav)continue;
        // The saved absolute path wins whenever it exists. Only when it is
        // gone is the recorded project-relative location tried; nothing is
        // searched for by name, so a different file is never substituted.
        auto path=clip.asset;
        const auto inside=std::find_if(relative.begin(),relative.end(),[&](const auto& r){return r.clip==clip.clip;});
        if(!QFileInfo::exists(path) && inside!=relative.end()) {
            const auto candidate=QDir::cleanPath(QDir(projectDirectory).filePath(inside->path));
            if(QFileInfo(candidate).isFile())path=candidate;
        }
        auto result=fl_live::importWav(path.toStdString(),policy);
        if(!result) { report.missing.append(QFileInfo(clip.asset).fileName() + " (" + QString::fromStdString(result.message) + ")"); continue; }
        if(path!=clip.asset) {
            report.relocated.append(QFileInfo(path).fileName() + " at " + path + " (was " + clip.asset + ")");
            report.previous.append({clip.clip,clip.asset});
            clip.asset=path;
        }
        clip.media=std::make_shared<const fl_live::AudioClip>(std::move(result.clip));
        const auto frames=mediaFrames(clip);
        if(clip.range_start>=frames || clip.range_length>frames-clip.range_start) {
            // The file changed since saving; play it whole rather than refuse.
            clip.range_start=0;clip.range_length=0;
            report.missing.append(QFileInfo(clip.asset).fileName() + " (shorter than its saved range; range reset to the whole file)");
        }
    }
    return report;
}
QVariantList Session::clipFrames() const {
    QVariantList result;
    for(const auto& track:identities_->tracks)result.append(static_cast<double>(mediaFrames(clipContent(*content_,firstClip(*identities_,track.id)))));
    return result;
}
QVariantList Session::clipRanges() const {
    QVariantList result;
    for(const auto& track:identities_->tracks) {
        const auto& clip=clipContent(*content_,firstClip(*identities_,track.id));
        const auto frames=mediaFrames(clip);
        const auto end=clip.range_length ? clip.range_start+clip.range_length : frames;
        result.append(QVariant(QVariantList{static_cast<double>(clip.range_start),static_cast<double>(end),clip.loop}));
    }
    return result;
}
bool Session::setClipRange(const QString& trackId,double startFrame,double endFrame,bool loop) {
    if(!editable())return false;
    try {
        const fl_live::TrackId id{parseIdentity(trackId)};
        const auto index=prepared_->mapped.trackIndex(id);
        if(!index)throw std::invalid_argument("Unknown track");
        auto next=*content_;
        auto& clip=*std::find_if(next.clips.begin(),next.clips.end(),[&](const auto& c){return c.clip==firstClip(*identities_,id);});
        const auto frames=mediaFrames(clip);
        if(clip.audio!=AudioSource::wav || !frames)throw std::invalid_argument("Import a WAV into this track before setting its range");
        if(!std::isfinite(startFrame) || !std::isfinite(endFrame) || startFrame<0 || endFrame>static_cast<double>(frames) ||
           startFrame!=std::floor(startFrame) || endFrame!=std::floor(endFrame) || endFrame<=startFrame)
            throw std::invalid_argument("Range must start before it ends, inside the imported audio");
        clip.range_start=static_cast<std::uint64_t>(startFrame);
        const auto end=static_cast<std::uint64_t>(endFrame);
        clip.range_length=end==frames ? 0 : end-clip.range_start; // 0 keeps "to the end"
        clip.loop=loop;
        if(next==*content_)return true;
        edit(next,EditKind::clip_range,static_cast<int>(*index));
        return *content_==next;
    }catch(const std::exception& error){message_=QString::fromUtf8(error.what());emit changed();return false;}
}
QVariantList Session::waveform(int track,int bins) const {
    if(track<0 || static_cast<std::size_t>(track)>=identities_->tracks.size() || bins<1 || bins>4096)return {};
    const auto& clip=clipContent(*content_,firstClip(*identities_,identities_->tracks[static_cast<std::size_t>(track)].id));
    if(!clip.media)return {};
    const auto key=std::make_pair(clip.media.get(),bins);
    if(const auto found=waveform_cache_.find(key);found!=waveform_cache_.end() && found->second.first.lock()==clip.media)return found->second.second;
    if(waveform_cache_.size()>256)waveform_cache_.clear();
    const auto& media=*clip.media;
    const auto frames=mediaFrames(clip);
    QVariantList result;
    for(int bin=0;bin<bins;++bin) {
        const auto first=frames*static_cast<std::uint64_t>(bin)/static_cast<std::uint64_t>(bins);
        const auto last=std::max(first+1,frames*static_cast<std::uint64_t>(bin+1)/static_cast<std::uint64_t>(bins));
        float low=0,high=0;bool any=false;
        for(auto frame=first;frame<last && frame<frames;++frame)
            for(std::uint32_t c=0;c<media.channels;++c) {
                const float v=media.samples[frame*media.channels+c];
                low=any ? std::min(low,v) : v;high=any ? std::max(high,v) : v;any=true;
            }
        result.append(QVariant(QVariantList{low,high}));
    }
    waveform_cache_[key]={clip.media,result};
    return result;
}
bool Session::collectAssets() {
    if(!editable())return false;
    try {
        if(owned_path_.isEmpty())throw std::invalid_argument("Save the project first; collected audio goes into a folder beside it");
        const QFileInfo project(owned_path_);
        const auto folder=project.absoluteDir().filePath(project.completeBaseName()+"-assets");
        auto next=*content_;
        int copied=0,reused=0;
        for(auto& clip:next.clips) {
            if(clip.audio!=AudioSource::wav || !clip.media || !assetRelativePath(clip.asset,project.absolutePath()).isEmpty())continue;
            if(!QFileInfo(clip.asset).isFile())continue;
            if(!QDir().mkpath(folder))throw std::runtime_error("Cannot create " + folder.toStdString());
            QFile source(clip.asset);
            if(!source.open(QIODevice::ReadOnly))throw std::runtime_error("Cannot read " + clip.asset.toStdString());
            const auto bytes=source.readAll();source.close();
            const QFileInfo name(clip.asset);
            QString target;
            for(int n=1;n<10000;++n) {
                const auto candidate=QDir(folder).filePath(n==1 ? name.fileName() : QString("%1-%2.%3").arg(name.completeBaseName()).arg(n).arg(name.suffix()));
                if(!QFileInfo::exists(candidate)) {
                    // Exclusive create: never overwrite a file that appears meanwhile.
                    QFile out(candidate);
                    if(!out.open(QIODevice::WriteOnly|QIODevice::NewOnly))continue;
                    if(out.write(bytes)!=bytes.size() || !out.flush()) { out.remove(); throw std::runtime_error("Cannot copy " + name.fileName().toStdString()); }
                    target=candidate;++copied;break;
                }
                QFile existing(candidate);
                if(existing.open(QIODevice::ReadOnly) && existing.readAll()==bytes) { target=candidate;++reused;break; } // same bytes already collected
            }
            if(target.isEmpty())throw std::runtime_error("No free name for " + name.fileName().toStdString());
            clip.asset=QDir::cleanPath(target);
        }
        if(next==*content_) { message_="All imported audio is already inside the project folder"; emit changed(); return true; }
        edit(next,EditKind::assets_collect,-1);
        message_=QString("Collected audio into %1: %2 copied, %3 already there. Source files were not changed.")
            .arg(QFileInfo(folder).fileName()).arg(copied).arg(reused);
        emit changed();return true;
    }catch(const std::exception& error){message_=QString::fromUtf8(error.what());emit changed();return false;}
}
