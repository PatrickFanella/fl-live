#include "authored_project.hpp"
#include "fl_live/document_io.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
// Metadata plus bounded content: 64 tracks with 16-step patterns and paths.
constexpr qsizetype limit = fl_live::document_metadata_byte_limit + 1024 * 1024;
constexpr qsizetype asset_path_limit = 4096;
fl_live::ClipId clipFor(const fl_live::DocumentSnapshot& identities, fl_live::TrackId track) {
    const auto found=std::find_if(identities.clips.begin(),identities.clips.end(),[track](const auto& clip){return clip.track==track;});
    if(found==identities.clips.end()) throw std::invalid_argument("Missing authored clip");
    return found->id;
}
const fl_live::DocumentTrack& trackOf(const fl_live::DocumentSnapshot& identities, fl_live::ClipId clip) {
    const auto owner=std::find_if(identities.clips.begin(),identities.clips.end(),[clip](const auto& c){return c.id==clip;});
    if(owner==identities.clips.end())throw std::invalid_argument("Unknown clip");
    return *std::find_if(identities.tracks.begin(),identities.tracks.end(),[&](const auto& t){return t.id==owner->track;});
}
QString idText(std::uint64_t id){return QString::number(static_cast<qulonglong>(id));}
std::uint64_t idValue(const QJsonValue& value) {
    bool ok=false;const auto text=value.toString();const auto id=text.toULongLong(&ok);
    if(!value.isString() || !ok || !id || idText(id)!=text)throw std::invalid_argument("Invalid content identity");
    return id;
}
int integer(const QJsonValue& value,int low,int high) {
    if(!value.isDouble())throw std::invalid_argument("Project requires numeric content");
    const double n=value.toDouble();
    if(!std::isfinite(n) || n<low || n>high || std::floor(n)!=n)throw std::invalid_argument("Project content is out of range or not an integer");
    return static_cast<int>(n);
}
QJsonObject exact(const QJsonValue& value,std::initializer_list<const char*> names) {
    if(!value.isObject())throw std::invalid_argument("Project content requires an object");
    const auto object=value.toObject();
    if(object.size()!=static_cast<qsizetype>(names.size()))throw std::invalid_argument("Unsupported project content fields");
    for(const auto* name:names)if(!object.contains(QLatin1String(name)))throw std::invalid_argument("Missing project content field");
    return object;
}
QJsonObject encodePattern(const StepPattern& pattern) {
    QJsonArray steps;
    for(const auto& step:pattern.steps)
        steps.append(QJsonObject{{"enabled",step.enabled},{"pitch",step.pitch},{"velocity",step.velocity},{"gate",step.gate}});
    return QJsonObject{{"length",pattern.step_count},{"steps",steps}};
}
StepPattern decodePattern(const QJsonValue& value) {
    const auto object=exact(value,{"length","steps"});
    StepPattern pattern;
    pattern.step_count=integer(object["length"],1,16);
    const auto steps=object["steps"].toArray();
    if(!object["steps"].isArray() || steps.size()!=16)throw std::invalid_argument("Pattern requires 16 stored steps");
    for(int i=0;i<16;++i) {
        const auto step=exact(steps[i],{"enabled","pitch","velocity","gate"});
        if(!step["enabled"].isBool())throw std::invalid_argument("Invalid step object");
        pattern.steps[static_cast<std::size_t>(i)]={step["enabled"].toBool(),integer(step["pitch"],0,127),
            integer(step["velocity"],0,100),integer(step["gate"],1,100)};
    }
    validatePattern(pattern);
    return pattern;
}
AuthoredProject decodeTemplate(const QByteArray& bytes, const QJsonObject& root) {
    if(root["version"].isDouble() && (root["version"].toDouble()==1 || root["version"].toDouble()==2)) {
        const auto settings=decodeProjectSettings(bytes);const auto identities=defaultTemplateIdentities();
        return {migrateTemplate(settings,identities),identities};
    }
    const auto features=root["required_features"].toArray();
    const bool sceneSlots=features.contains("template-scene-slots-v1");
    const bool multipleScenes=features.contains("template-scenes-v1");
    QJsonArray expected{"template-identities-v1"};
    if(sceneSlots)expected.append("template-scene-slots-v1");
    if(multipleScenes)expected.append("template-scenes-v1");
    if(root.size()!=5 || !root["version"].isDouble() || root["version"].toDouble()!=3 ||
       !root["settings"].isObject() || !root["session"].isObject() || !root["required_features"].isArray() ||
       features!=expected)
        throw std::invalid_argument("Unsupported authored project fields/version/features");
    const auto settings=decodeProjectSettings(QJsonDocument(root["settings"].toObject()).toJson(QJsonDocument::Compact));
    const auto identities=fl_live::decodeDocumentMetadata(QJsonDocument(root["session"].toObject()).toJson(QJsonDocument::Compact));
    validateTemplateIdentities(identities);
    if(!multipleScenes && identities.scenes.size()!=1)
        throw std::invalid_argument("Multiple scenes require template-scenes-v1");
    if(!sceneSlots && std::any_of(identities.scenes.begin(),identities.scenes.end(),[](const auto& scene){return scene.entries.size()!=2;}))
        throw std::invalid_argument("Unfilled scene slots require template-scene-slots-v1");
    return {migrateTemplate(settings,identities),identities};
}
}
fl_live::DocumentSnapshot defaultTemplateIdentities() {
    fl_live::SessionDocument document;
    const auto a=document.createTrack("Pulse / Audio",fl_live::TrackKind::audio);
    const auto m=document.createTrack("Steps / MIDI",fl_live::TrackKind::midi);
    const auto ca=document.createClip(a,"Low pulse"),cm=document.createClip(m,"Step pattern");
    const auto scene=document.createScene("Scene 1");
    document.setSlot(scene,a,ca);document.setSlot(scene,m,cm);
    return document.snapshot();
}
AuthoredProject defaultProject() {
    const auto identities=defaultTemplateIdentities();
    return {migrateTemplate(ProjectSettings{},identities),identities};
}
void validateTemplateIdentities(const fl_live::DocumentSnapshot& identities) {
    fl_live::SessionDocument::validate(identities);
    if(identities.tracks.size()!=2 || identities.clips.size()!=2 || identities.scenes.empty() ||
       identities.tracks[0].kind!=fl_live::TrackKind::audio || identities.tracks[1].kind!=fl_live::TrackKind::midi)
        throw std::invalid_argument("Unsupported authored template structure");
    for(const auto& track:identities.tracks)
        if(std::count_if(identities.clips.begin(),identities.clips.end(),[&](const auto& clip){return clip.track==track.id;})!=1)
            throw std::invalid_argument("Authored template requires one clip per track");
}
std::vector<fl_live::DocumentClipSource> templateSources(const ProjectSettings& settings,const fl_live::DocumentSnapshot& identities) {
    validateTemplateIdentities(identities);
    auto plan=prepareProject(settings);
    std::vector<fl_live::DocumentClipSource> result;
    for(std::size_t i=0;i<2;++i) result.push_back({clipFor(identities,identities.tracks[i].id),std::move(plan.tracks[i].clips[0])});
    return result;
}
std::vector<fl_live::DocumentTrackGain> templateGains(const ProjectSettings& settings,const fl_live::DocumentSnapshot& identities) {
    validateTemplateIdentities(identities);
    (void)encodeProjectSettings(settings);
    return {{identities.tracks[0].id,static_cast<float>(settings.levels[0])/100},
            {identities.tracks[1].id,static_cast<float>(settings.levels[1])/100}};
}
SessionContent migrateTemplate(const ProjectSettings& settings,const fl_live::DocumentSnapshot& identities) {
    validateTemplateIdentities(identities);
    (void)encodeProjectSettings(settings);
    SessionContent content{settings.tempo,{},{}};
    for(std::size_t i=0;i<2;++i) {
        const auto track=identities.tracks[i].id;
        content.tracks.push_back({track,settings.levels[i]});
        ClipContent clip{clipFor(identities,track)};
        if(i==1)clip.pattern={settings.step_count,settings.steps};
        content.clips.push_back(std::move(clip));
    }
    validateContent(content,identities);
    return content;
}
ProjectSettings templateView(const SessionContent& content,const fl_live::DocumentSnapshot& identities) {
    ProjectSettings settings;
    settings.tempo=content.tempo;
    const auto audio=std::find_if(identities.tracks.begin(),identities.tracks.end(),[](const auto& t){return t.kind==fl_live::TrackKind::audio;});
    const auto midi=std::find_if(identities.tracks.begin(),identities.tracks.end(),[](const auto& t){return t.kind==fl_live::TrackKind::midi;});
    if(audio!=identities.tracks.end())settings.levels[0]=trackContent(content,audio->id).level;
    if(midi!=identities.tracks.end()) {
        settings.levels[1]=trackContent(content,midi->id).level;
        const auto& pattern=clipContent(content,clipFor(identities,midi->id)).pattern;
        settings.step_count=pattern.step_count;settings.steps=pattern.steps;
    }
    return settings;
}
std::uint64_t mediaFrames(const ClipContent& clip) {
    return clip.media && clip.media->channels ? clip.media->samples.size()/clip.media->channels : 0;
}
const ClipContent& clipContent(const SessionContent& content,fl_live::ClipId clip) {
    const auto found=std::find_if(content.clips.begin(),content.clips.end(),[clip](const auto& c){return c.clip==clip;});
    if(found==content.clips.end())throw std::invalid_argument("Missing clip content");
    return *found;
}
const TrackContent& trackContent(const SessionContent& content,fl_live::TrackId track) {
    const auto found=std::find_if(content.tracks.begin(),content.tracks.end(),[track](const auto& t){return t.track==track;});
    if(found==content.tracks.end())throw std::invalid_argument("Missing track content");
    return *found;
}
namespace {
PortType portOf(DeviceKind kind){return kind==DeviceKind::gain ? PortType::audio : PortType::midi;}
}
DeviceChain deviceChain(const TrackDevices& entry,fl_live::TrackKind kind) {
    // Validation guarantees one chain; walk it from the source.
    DeviceChain chain;bool afterInstrument=kind==fl_live::TrackKind::audio;
    DeviceEndpoint at{DeviceEndpoint::Kind::source};
    for(std::size_t steps=0;steps<=entry.devices.size()+2;++steps) {
        const auto next=std::find_if(entry.connections.begin(),entry.connections.end(),[&](const auto& c){return c.from==at;});
        if(next==entry.connections.end())break;
        at=next->to;
        if(at.kind==DeviceEndpoint::Kind::instrument)afterInstrument=true;
        if(at.kind==DeviceEndpoint::Kind::device) {
            const auto& device=*std::find_if(entry.devices.begin(),entry.devices.end(),[&](const auto& d){return d.id==at.device;});
            (afterInstrument ? chain.audio : chain.midi).push_back(device);
        }
    }
    return chain;
}
std::vector<DeviceConnection> chainConnections(const std::vector<Device>& devices,fl_live::TrackKind kind) {
    std::vector<DeviceEndpoint> nodes{{DeviceEndpoint::Kind::source}};
    for(const auto& d:devices)if(d.kind==DeviceKind::transpose)nodes.push_back({DeviceEndpoint::Kind::device,d.id});
    if(kind==fl_live::TrackKind::midi)nodes.push_back({DeviceEndpoint::Kind::instrument});
    for(const auto& d:devices)if(d.kind==DeviceKind::gain)nodes.push_back({DeviceEndpoint::Kind::device,d.id});
    nodes.push_back({DeviceEndpoint::Kind::output});
    std::vector<DeviceConnection> result;
    for(std::size_t i=0;i+1<nodes.size();++i)result.push_back({nodes[i],nodes[i+1]});
    return result;
}
fl_live::MidiClip transposed(fl_live::MidiClip clip,const std::vector<Device>& midi) {
    std::vector<fl_live::Note> kept;
    for(auto note:clip.notes) {
        int pitch=note.pitch;bool inside=true;
        for(const auto& device:midi){pitch+=static_cast<int>(device.value);if(pitch<0 || pitch>127){inside=false;break;}}
        if(!inside)continue;
        note.pitch=static_cast<std::uint8_t>(pitch);kept.push_back(note);
    }
    clip.notes=std::move(kept);
    return clip;
}
namespace {
// Typed chain validation of one track's devices.
void validateDevices(const TrackDevices& entry,fl_live::TrackKind kind,std::uint64_t next_id) {
    if(entry.devices.empty() || entry.devices.size()>devices_per_track_limit)throw std::invalid_argument("A track holds 1 to 8 devices");
    for(const auto& d:entry.devices) {
        if(d.id<1 || d.id>=next_id)throw std::invalid_argument("Device IDs must come from the project's device counter");
        if(std::count_if(entry.devices.begin(),entry.devices.end(),[&](const auto& o){return o.id==d.id;})!=1)throw std::invalid_argument("Duplicate device ID");
        if(d.kind==DeviceKind::gain && (!std::isfinite(d.value) || d.value<-60 || d.value>12))throw std::invalid_argument("Gain must be -60 to +12 dB");
        if(d.kind==DeviceKind::transpose && (!std::isfinite(d.value) || d.value<-48 || d.value>48 || std::floor(d.value)!=d.value))
            throw std::invalid_argument("Transpose must be whole semitones from -48 to +48");
        if(d.kind!=DeviceKind::gain && d.kind!=DeviceKind::transpose)throw std::invalid_argument("Unknown device kind");
    }
    using K=DeviceEndpoint::Kind;
    const bool midi=kind==fl_live::TrackKind::midi;
    // Port types: what an endpoint emits and what it accepts.
    const auto out=[&](const DeviceEndpoint& e)->std::optional<PortType> {
        if(e.kind==K::source)return midi ? PortType::midi : PortType::audio;
        if(e.kind==K::instrument && midi)return PortType::audio;
        if(e.kind==K::device)for(const auto& d:entry.devices)if(d.id==e.device)return portOf(d.kind);
        return std::nullopt;
    };
    const auto in=[&](const DeviceEndpoint& e)->std::optional<PortType> {
        if(e.kind==K::output)return PortType::audio;
        if(e.kind==K::instrument && midi)return PortType::midi;
        if(e.kind==K::device)for(const auto& d:entry.devices)if(d.id==e.device)return portOf(d.kind);
        return std::nullopt;
    };
    for(const auto& c:entry.connections) {
        const auto from=out(c.from),to=in(c.to);
        if(!from || !to)throw std::invalid_argument("A device connection names an unknown or unusable endpoint");
        if(*from!=*to)throw std::invalid_argument("A device connection joins a MIDI port to an audio port");
    }
    // One chain: every node has one way in and one way out, and walking from
    // the source reaches the output through every node without repeating.
    const std::size_t nodes=entry.devices.size()+(midi ? 3 : 2);
    if(entry.connections.size()!=nodes-1)throw std::invalid_argument("Devices must form one chain from source to output (no branches, merges or loose devices)");
    for(const auto& c:entry.connections)
        if(std::count_if(entry.connections.begin(),entry.connections.end(),[&](const auto& o){return o.from==c.from;})!=1 ||
           std::count_if(entry.connections.begin(),entry.connections.end(),[&](const auto& o){return o.to==c.to;})!=1)
            throw std::invalid_argument("Devices must form one chain from source to output (no branches, merges or loose devices)");
    std::vector<DeviceEndpoint> seen{{K::source}};DeviceEndpoint at{K::source};
    while(at.kind!=K::output) {
        const auto next=std::find_if(entry.connections.begin(),entry.connections.end(),[&](const auto& c){return c.from==at;});
        if(next==entry.connections.end())throw std::invalid_argument("The device chain does not reach the output");
        at=next->to;
        if(std::find(seen.begin(),seen.end(),at)!=seen.end())throw std::invalid_argument("The device chain contains a cycle");
        seen.push_back(at);
    }
    if(seen.size()!=nodes) {
        // Unreached nodes either loop among themselves or hang loose.
        for(const auto& c:entry.connections) {
            if(std::find(seen.begin(),seen.end(),c.from)!=seen.end())continue;
            DeviceEndpoint walk=c.from;
            for(std::size_t step=0;step<nodes;++step) {
                const auto next=std::find_if(entry.connections.begin(),entry.connections.end(),[&](const auto& o){return o.from==walk;});
                if(next==entry.connections.end())break;
                walk=next->to;
                if(walk==c.from)throw std::invalid_argument("The device chain contains a cycle");
            }
        }
        throw std::invalid_argument("Devices must form one chain from source to output (no branches, merges or loose devices)");
    }
    double db=0;for(const auto& d:entry.devices)if(d.kind==DeviceKind::gain)db+=d.value;
    if(db>combined_device_gain_limit_db)throw std::invalid_argument("Combined device gain exceeds +24 dB");
}
}
void validateContent(const SessionContent& content,const fl_live::DocumentSnapshot& identities) {
    fl_live::SessionDocument::validate(identities);
    if(content.tempo<20 || content.tempo>300)throw std::invalid_argument("Tempo must be 20 to 300 BPM");
    if(identities.scenes.empty())throw std::invalid_argument("A desktop project keeps at least one scene");
    if(content.tracks.size()!=identities.tracks.size() || content.clips.size()!=identities.clips.size())
        throw std::invalid_argument("Every track and clip requires exactly one content entry");
    for(const auto& track:identities.tracks) {
        const auto& value=trackContent(content,track.id);
        if(value.level<0 || value.level>100)throw std::invalid_argument("Track level must be 0 to 100 percent");
        if(std::count_if(content.tracks.begin(),content.tracks.end(),[&](const auto& t){return t.track==track.id;})!=1)
            throw std::invalid_argument("Duplicate track content");
        const auto clips=static_cast<std::size_t>(std::count_if(identities.clips.begin(),identities.clips.end(),[&](const auto& c){return c.track==track.id;}));
        if(clips<1 || clips>desktop_clips_per_track)throw std::invalid_argument("Each desktop track holds 1 to 8 clips");
    }
    for(std::size_t i=0;i<content.automation.size();++i) {
        const auto& lane=content.automation[i];
        if(std::none_of(identities.tracks.begin(),identities.tracks.end(),[&](const auto& t){return t.id==lane.track;}))
            throw std::invalid_argument("Automation lane for an unknown track");
        if(lane.parameter!=AutomatedParameter::level && lane.parameter!=AutomatedParameter::pan && lane.parameter!=AutomatedParameter::device)
            throw std::invalid_argument("Unknown automated parameter");
        for(std::size_t j=0;j<i;++j)if(content.automation[j].track==lane.track && content.automation[j].parameter==lane.parameter && content.automation[j].device==lane.device)
            throw std::invalid_argument("A parameter has at most one automation lane");
        if(lane.points.empty() || lane.points.size()>fl_live::Engine::automation_points_capacity)
            throw std::invalid_argument("An automation lane holds 1 to 4096 points");
        double low=lane.parameter==AutomatedParameter::level ? 0 : -100,high=100;
        if(lane.parameter!=AutomatedParameter::device && lane.device)throw std::invalid_argument("Only device lanes name a device");
        if(lane.parameter==AutomatedParameter::device) {
            const auto entry=std::find_if(content.devices.begin(),content.devices.end(),[&](const auto& e){return e.track==lane.track;});
            const Device* device=nullptr;
            if(entry!=content.devices.end())for(const auto& d:entry->devices)if(d.id==lane.device)device=&d;
            if(!device)throw std::invalid_argument("A device lane must name a device on its track");
            // The engine holds one lane of each kind per track.
            for(std::size_t j=0;j<content.automation.size();++j) {
                const auto& other=content.automation[j];
                if(j==i || other.track!=lane.track || other.parameter!=AutomatedParameter::device)continue;
                for(const auto& d:entry->devices)if(d.id==other.device && d.kind==device->kind)
                    throw std::invalid_argument("A track can automate at most one gain device and one transpose device");
            }
            if(device->kind==DeviceKind::gain) {
                low=-60;high=12;
                double others=0;for(const auto& d:entry->devices)if(d.kind==DeviceKind::gain && d.id!=device->id)others+=d.value;
                for(const auto& point:lane.points)if(point.value+others>combined_device_gain_limit_db)throw std::invalid_argument("Combined device gain exceeds +24 dB");
            } else {
                low=-48;high=48;
                for(const auto& point:lane.points)if(point.value!=std::floor(point.value))throw std::invalid_argument("Transpose automation uses whole semitones");
            }
        }
        for(std::size_t k=0;k<lane.points.size();++k) {
            const auto& point=lane.points[k];
            if(point.at<0 || point.at>automation_position_limit || (k && point.at<=lane.points[k-1].at))
                throw std::invalid_argument("Automation points must strictly increase within 256 bars");
            if(!std::isfinite(point.value) || point.value<low || point.value>high)
                throw std::invalid_argument("Automation value is outside the parameter's range");
        }
    }
    if(content.next_device_id<1)throw std::invalid_argument("Invalid device counter");
    for(const auto& entry:content.devices) {
        const auto track=std::find_if(identities.tracks.begin(),identities.tracks.end(),[&](const auto& t){return t.id==entry.track;});
        if(track==identities.tracks.end())throw std::invalid_argument("Devices for an unknown track");
        if(std::count_if(content.devices.begin(),content.devices.end(),[&](const auto& e){return e.track==entry.track;})!=1)
            throw std::invalid_argument("A track has at most one device chain");
        validateDevices(entry,track->kind,content.next_device_id);
        for(const auto& d:entry.devices)
            for(const auto& other:content.devices)if(&other!=&entry)
                if(std::any_of(other.devices.begin(),other.devices.end(),[&](const auto& o){return o.id==d.id;}))throw std::invalid_argument("Duplicate device ID");
    }
    for(const auto& clip:identities.clips) {
        const auto& value=clipContent(content,clip.id);
        if(std::count_if(content.clips.begin(),content.clips.end(),[&](const auto& c){return c.clip==clip.id;})!=1)
            throw std::invalid_argument("Duplicate clip content");
        validatePattern(value.pattern);
        validateSynth(value.synth);
        if(value.piano)validatePiano(*value.piano);
        if(value.piano && trackOf(identities,clip.id).kind!=fl_live::TrackKind::midi)throw std::invalid_argument("Only MIDI clips have piano-roll notes");
        if(trackOf(identities,clip.id).kind!=fl_live::TrackKind::midi && value.synth!=SynthSettings{})
            throw std::invalid_argument("Only MIDI clips have synth settings");
        const bool wav=value.audio==AudioSource::wav;
        if(value.audio!=AudioSource::pulse && value.audio!=AudioSource::empty && !wav)throw std::invalid_argument("Unknown audio source");
        if(wav!=!value.asset.isEmpty() || (!value.asset.isEmpty() && (!QDir::isAbsolutePath(value.asset) || value.asset.size()>asset_path_limit)))
            throw std::invalid_argument("Imported audio requires one absolute asset path");
        if(value.media && (!wav || value.media->samples.empty()))throw std::invalid_argument("Decoded media requires an imported WAV clip");
        if(value.hasRange() && !wav)throw std::invalid_argument("Only imported audio clips have a source range");
        if(value.range_start>fl_live::PreparationBudget::audio_frame_limit || value.range_length>fl_live::PreparationBudget::audio_frame_limit)
            throw std::invalid_argument("Source range exceeds the import frame limit");
        if(const auto frames=mediaFrames(value); frames && (value.range_start>=frames || value.range_length>frames-value.range_start))
            throw std::invalid_argument("Source range must be a nonempty part of the imported audio");
    }
}
std::vector<fl_live::DocumentClipSource> contentSources(const SessionContent& content,const fl_live::DocumentSnapshot& identities) {
    validateContent(content,identities);
    std::vector<fl_live::DocumentClipSource> result;
    for(const auto& clip:identities.clips) {
        const auto& value=clipContent(content,clip.id);
        const auto kind=std::find_if(identities.tracks.begin(),identities.tracks.end(),[&](const auto& t){return t.id==clip.track;})->kind;
        if(kind==fl_live::TrackKind::midi) {
            auto midi=value.piano ? pianoClip(*value.piano,content.tempo,value.synth) : patternClip(value.pattern,content.tempo,value.synth);
            const auto entry=std::find_if(content.devices.begin(),content.devices.end(),[&](const auto& e){return e.track==clip.track;});
            if(entry!=content.devices.end()) {
                // An automated transpose is applied at note-on instead.
                auto stage=deviceChain(*entry,kind).midi;
                std::erase_if(stage,[&](const auto& d){return std::any_of(content.automation.begin(),content.automation.end(),[&](const auto& l){
                    return l.parameter==AutomatedParameter::device && l.device==d.id;});});
                midi=transposed(std::move(midi),stage);
            }
            result.push_back({clip.id,std::move(midi)});
        }
        else if(value.audio==AudioSource::pulse)result.push_back({clip.id,pulseClip(content.tempo)});
        else if(value.media) {
            // Copy only the selected source frames; the engine loops or stops
            // at the end of this copy.
            const auto channels=value.media->channels;
            const auto frames=mediaFrames(value);
            const auto length=value.range_length ? value.range_length : frames-value.range_start;
            const auto first=value.media->samples.begin()+static_cast<std::ptrdiff_t>(value.range_start*channels);
            result.push_back({clip.id,fl_live::AudioClip{{first,first+static_cast<std::ptrdiff_t>(length*channels)},value.loop,channels}});
        }
        else result.push_back({clip.id,fl_live::AudioClip{{0.0F},true}}); // empty or missing: silence
    }
    return result;
}
std::vector<fl_live::DocumentAutomation> contentAutomation(const SessionContent& content,const fl_live::DocumentSnapshot& identities) {
    validateContent(content,identities);
    std::vector<fl_live::DocumentAutomation> result;
    for(const auto& lane:content.automation) {
        fl_live::DocumentAutomation prepared{lane.track,lane.parameter==AutomatedParameter::level ? fl_live::AutomationTarget::gain : fl_live::AutomationTarget::pan,{}};
        // A gain-device lane becomes a factor lane scaled by the track's other
        // gain devices; a transpose lane stays in semitones.
        std::optional<DeviceKind> device;double others=1;
        if(lane.parameter==AutomatedParameter::device) {
            const auto& entry=*std::find_if(content.devices.begin(),content.devices.end(),[&](const auto& e){return e.track==lane.track;});
            for(const auto& d:entry.devices) {
                if(d.id==lane.device)device=d.kind;
                else if(d.kind==DeviceKind::gain)others*=static_cast<double>(static_cast<float>(std::pow(10.0,d.value/20)));
            }
            prepared.target=*device==DeviceKind::gain ? fl_live::AutomationTarget::device_gain : fl_live::AutomationTarget::transpose;
        }
        for(const auto& point:lane.points) {
            // Same upward rounding as every other musical boundary.
            const auto numerator=48000ULL*60*static_cast<std::uint64_t>(point.at);
            const auto denominator=static_cast<std::uint64_t>(content.tempo)*100;
            const float value=!device ? static_cast<float>(point.value/100.0)
                : *device==DeviceKind::gain ? static_cast<float>(others*std::pow(10.0,point.value/20)) : static_cast<float>(point.value);
            prepared.points.push_back({(numerator+denominator-1)/denominator,value});
        }
        result.push_back(std::move(prepared));
    }
    return result;
}
std::vector<fl_live::DocumentTrackGain> contentGains(const SessionContent& content,const fl_live::DocumentSnapshot& identities) {
    validateContent(content,identities);
    std::vector<fl_live::DocumentTrackGain> result;
    for(const auto& track:identities.tracks) {
        float device=1;
        const auto entry=std::find_if(content.devices.begin(),content.devices.end(),[&](const auto& e){return e.track==track.id;});
        if(entry!=content.devices.end())
            for(const auto& d:deviceChain(*entry,track.kind).audio)device*=static_cast<float>(std::pow(10.0,d.value/20));
        result.push_back({track.id,static_cast<float>(trackContent(content,track.id).level)/100,device});
    }
    return result;
}
QString assetRelativePath(const QString& asset,const QString& baseDirectory) {
    if(baseDirectory.isEmpty() || asset.isEmpty())return {};
    const auto relative=QDir(baseDirectory).relativeFilePath(asset);
    if(relative.isEmpty() || QDir::isAbsolutePath(relative) || relative==".." || relative.startsWith("../"))return {};
    return relative;
}
QByteArray encodeAuthoredProject(const AuthoredProject& project,const QString& baseDirectory) {
    validateContent(project.content,project.identities);
    QJsonArray tracks,clips;
    // Ranges are written only when used, behind their own required feature.
    const bool ranges=std::any_of(project.content.clips.begin(),project.content.clips.end(),[](const auto& c){return c.hasRange();});
    const bool relative=std::any_of(project.content.clips.begin(),project.content.clips.end(),[&](const auto& c){
        return c.audio==AudioSource::wav && !assetRelativePath(c.asset,baseDirectory).isEmpty();});
    for(const auto& track:project.identities.tracks)
        tracks.append(QJsonObject{{"id",idText(track.id.value)},{"level",trackContent(project.content,track.id).level}});
    for(const auto& clip:project.identities.clips) {
        const auto& value=clipContent(project.content,clip.id);
        if(trackOf(project.identities,clip.id).kind==fl_live::TrackKind::midi)
        {
            QJsonObject object{{"id",idText(clip.id.value)},{"pattern",encodePattern(value.pattern)}};
            static const char* waves[]={"sine","triangle","square","saw"};
            if(value.synth!=SynthSettings{})
                object.insert("synth",QJsonObject{{"waveform",waves[value.synth.waveform]},{"attack_ms",value.synth.attack_ms},
                    {"decay_ms",value.synth.decay_ms},{"sustain",value.synth.sustain},{"release_ms",value.synth.release_ms}});
            if(value.piano) {
                QJsonArray notes;
                for(const auto& note:value.piano->notes)
                    notes.append(QJsonObject{{"id",idText(note.id)},{"start",note.start},{"length",note.length},{"pitch",note.pitch},{"velocity",note.velocity}});
                QJsonObject piano{{"length",value.piano->length},{"next_id",idText(value.piano->next_id)},{"notes",notes}};
                if(!value.piano->loop)piano.insert("loop",false); // written only for one-shot clips
                object.insert("piano",piano);
            }
            clips.append(object);
        }
        else if(value.audio==AudioSource::wav) {
            QJsonObject object{{"id",idText(clip.id.value)},{"audio","wav"},{"path",value.asset}};
            if(ranges)object.insert("range",QJsonObject{{"start",static_cast<double>(value.range_start)},
                {"length",static_cast<double>(value.range_length)},{"loop",value.loop}});
            if(const auto inside=assetRelativePath(value.asset,baseDirectory);!inside.isEmpty())object.insert("relative",inside);
            clips.append(object);
        }
        else clips.append(QJsonObject{{"id",idText(clip.id.value)},{"audio",value.audio==AudioSource::pulse ? "pulse" : "empty"}});
    }
    const auto session=QJsonDocument::fromJson(fl_live::encodeDocumentMetadata(project.identities)).object();
    const bool automated=!project.content.automation.empty();
    const bool synths=std::any_of(project.content.clips.begin(),project.content.clips.end(),[](const auto& c){return c.synth!=SynthSettings{};});
    const bool pianos=std::any_of(project.content.clips.begin(),project.content.clips.end(),[](const auto& c){return c.piano.has_value();});
    const bool devices=!project.content.devices.empty();
    const bool deviceLanes=std::any_of(project.content.automation.begin(),project.content.automation.end(),[](const auto& l){return l.parameter==AutomatedParameter::device;});
    QJsonObject root{{"format","fl-live-project"},{"version",4},
        {"required_features",[&]{QJsonArray f{"general-session-v1"};if(ranges)f.append("clip-ranges-v1");if(relative)f.append("asset-relative-v1");
            if(automated)f.append("automation-v1");if(synths)f.append("synth-v1");if(pianos)f.append("piano-roll-v1");if(devices)f.append("devices-v1");if(deviceLanes)f.append("device-automation-v1");return f;}()},
        {"tempo",project.content.tempo},
        {"session",session},{"tracks",tracks},{"clips",clips}};
    if(automated) {
        QJsonArray lanes;
        for(const auto& lane:project.content.automation) {
            QJsonArray points;
            for(const auto& point:lane.points)points.append(QJsonObject{{"at",static_cast<double>(point.at)},{"value",point.value}});
            QJsonObject object{{"track",idText(lane.track.value)},{"parameter",lane.parameter==AutomatedParameter::level ? "level" : lane.parameter==AutomatedParameter::pan ? "pan" : "device"},{"points",points}};
            if(lane.parameter==AutomatedParameter::device)object.insert("device",idText(lane.device));
            lanes.append(object);
        }
        root.insert("automation",lanes);
    }
    if(devices) {
        const auto endpoint=[](const DeviceEndpoint& e)->QJsonValue{
            switch(e.kind){case DeviceEndpoint::Kind::source:return "source";case DeviceEndpoint::Kind::instrument:return "instrument";
            case DeviceEndpoint::Kind::output:return "output";default:return idText(e.device);}};
        QJsonArray entries;
        for(const auto& entry:project.content.devices) {
            QJsonArray list,connections;
            for(const auto& d:entry.devices)list.append(QJsonObject{{"id",idText(d.id)},{"kind",d.kind==DeviceKind::gain ? "gain" : "transpose"},{"value",d.value}});
            for(const auto& c:entry.connections)connections.append(QJsonArray{endpoint(c.from),endpoint(c.to)});
            entries.append(QJsonObject{{"track",idText(entry.track.value)},{"devices",list},{"connections",connections}});
        }
        root.insert("devices",QJsonObject{{"next_id",idText(project.content.next_device_id)},{"tracks",entries}});
    }
    const auto bytes=QJsonDocument(root).toJson();
    if(bytes.size()>limit)throw std::invalid_argument("Authored project exceeds size limit");
    return bytes;
}
AuthoredProject decodeAuthoredProject(const QByteArray& bytes) {
    if(bytes.size()>limit)throw std::invalid_argument("Authored project exceeds size limit");
    QJsonParseError error;const auto json=QJsonDocument::fromJson(bytes,&error);
    if(error.error!=QJsonParseError::NoError || !json.isObject())throw std::invalid_argument("Project is not a valid JSON object");
    const auto root=json.object();
    if(root["format"]!="fl-live-project")throw std::invalid_argument("Unsupported project format");
    if(!root["version"].isDouble() || root["version"].toDouble()!=4)return decodeTemplate(bytes,root);
    const auto features=root["required_features"].toArray();
    const bool ranges=features.contains("clip-ranges-v1"),relativePaths=features.contains("asset-relative-v1"),automated=features.contains("automation-v1");
    const bool synths=features.contains("synth-v1"),pianos=features.contains("piano-roll-v1"),devices=features.contains("devices-v1");
    const auto v4=automated && devices ? exact(root,{"format","version","required_features","tempo","session","tracks","clips","automation","devices"})
                : automated ? exact(root,{"format","version","required_features","tempo","session","tracks","clips","automation"})
                : devices ? exact(root,{"format","version","required_features","tempo","session","tracks","clips","devices"})
                : exact(root,{"format","version","required_features","tempo","session","tracks","clips"});
    QJsonArray canonical{"general-session-v1"};
    if(ranges)canonical.append("clip-ranges-v1");
    if(relativePaths)canonical.append("asset-relative-v1");
    if(automated)canonical.append("automation-v1");
    if(synths)canonical.append("synth-v1");
    if(pianos)canonical.append("piano-roll-v1");
    if(devices)canonical.append("devices-v1");
    const bool deviceLanes=features.contains("device-automation-v1");
    if(deviceLanes)canonical.append("device-automation-v1");
    if(features!=canonical)throw std::invalid_argument("Unsupported required project feature");
    if(!v4["session"].isObject())throw std::invalid_argument("Project requires a session object");
    AuthoredProject project;
    project.identities=fl_live::decodeDocumentMetadata(QJsonDocument(v4["session"].toObject()).toJson(QJsonDocument::Compact));
    project.content.tempo=integer(v4["tempo"],20,300);
    const auto tracks=v4["tracks"].toArray(),clips=v4["clips"].toArray();
    if(!v4["tracks"].isArray() || !v4["clips"].isArray() ||
       tracks.size()>static_cast<qsizetype>(fl_live::SessionDocument::track_limit) ||
       clips.size()>static_cast<qsizetype>(fl_live::SessionDocument::clip_limit))
        throw std::invalid_argument("Invalid or oversized project content arrays");
    for(const auto& value:tracks) {
        const auto track=exact(value,{"id","level"});
        project.content.tracks.push_back({fl_live::TrackId{idValue(track["id"])},integer(track["level"],0,100)});
    }
    for(const auto& value:clips) {
        if(!value.isObject())throw std::invalid_argument("Clip content requires an object");
        const auto object=value.toObject();
        ClipContent clip;
        if(object.contains("pattern")) {
            const bool hasSynth=synths && object.contains("synth"),hasPiano=pianos && object.contains("piano");
            const auto fields=hasSynth && hasPiano ? exact(value,{"id","pattern","synth","piano"}) : hasSynth ? exact(value,{"id","pattern","synth"})
                            : hasPiano ? exact(value,{"id","pattern","piano"}) : exact(value,{"id","pattern"});
            clip.clip=fl_live::ClipId{idValue(fields["id"])};clip.pattern=decodePattern(fields["pattern"]);
            if(hasSynth) {
                const auto synth=exact(fields["synth"],{"waveform","attack_ms","decay_ms","sustain","release_ms"});
                const QStringList waves{"sine","triangle","square","saw"};
                if(!synth["waveform"].isString() || !waves.contains(synth["waveform"].toString()))throw std::invalid_argument("Unsupported waveform");
                clip.synth={static_cast<int>(waves.indexOf(synth["waveform"].toString())),integer(synth["attack_ms"],0,10000),
                    integer(synth["decay_ms"],0,10000),integer(synth["sustain"],0,100),integer(synth["release_ms"],0,10000)};
            }
            if(hasPiano) {
                const bool oneShot=fields["piano"].toObject().contains("loop");
                const auto piano=oneShot ? exact(fields["piano"],{"length","next_id","notes","loop"}) : exact(fields["piano"],{"length","next_id","notes"});
                if(oneShot && piano["loop"]!=QJsonValue(false))throw std::invalid_argument("A saved piano-roll loop flag must be false");
                PianoRoll roll{integer(piano["length"],1,piano_length_limit),!oneShot,idValue(piano["next_id"]),{}};
                if(!piano["notes"].isArray() || piano["notes"].toArray().size()>static_cast<qsizetype>(fl_live::Engine::notes_per_clip_capacity))
                    throw std::invalid_argument("Invalid piano-roll notes");
                for(const auto& n:piano["notes"].toArray()) {
                    const auto note=exact(n,{"id","start","length","pitch","velocity"});
                    roll.notes.push_back({idValue(note["id"]),integer(note["start"],0,piano_length_limit),integer(note["length"],1,piano_length_limit),
                        integer(note["pitch"],0,127),integer(note["velocity"],1,100)});
                }
                validatePiano(roll); // order, uniqueness and overlap exactly as saved
                clip.piano=std::move(roll);
            }
            if(trackOf(project.identities,clip.clip).kind!=fl_live::TrackKind::midi)throw std::invalid_argument("Pattern on an audio clip");
        } else {
            const auto source=object["audio"].toString();
            const bool hasRelative=source=="wav" && relativePaths && object.contains("relative");
            QJsonObject fields;
            if(source!="wav")fields=exact(value,{"id","audio"});
            else if(ranges && hasRelative)fields=exact(value,{"id","audio","path","range","relative"});
            else if(ranges)fields=exact(value,{"id","audio","path","range"});
            else if(hasRelative)fields=exact(value,{"id","audio","path","relative"});
            else fields=exact(value,{"id","audio","path"});
            clip.clip=fl_live::ClipId{idValue(fields["id"])};
            if(trackOf(project.identities,clip.clip).kind!=fl_live::TrackKind::audio)throw std::invalid_argument("Audio source on a MIDI clip");
            if(source=="pulse")clip.audio=AudioSource::pulse;
            else if(source=="empty")clip.audio=AudioSource::empty;
            else if(source=="wav") {
                if(!fields["path"].isString())throw std::invalid_argument("Imported audio requires a path string");
                clip.audio=AudioSource::wav;clip.asset=fields["path"].toString();
                if(hasRelative) {
                    const auto inside=fields["relative"].toString();
                    if(!fields["relative"].isString() || inside.isEmpty() || inside.size()>asset_path_limit || QDir::isAbsolutePath(inside) ||
                       QDir::cleanPath(inside)!=inside || inside==".." || inside.startsWith("../"))
                        throw std::invalid_argument("Relative asset path must stay inside the project folder");
                    project.relative.push_back({fl_live::ClipId{idValue(fields["id"])},inside});
                }
                if(ranges) {
                    const auto range=exact(fields["range"],{"start","length","loop"});
                    const int limit=static_cast<int>(fl_live::PreparationBudget::audio_frame_limit);
                    clip.range_start=static_cast<std::uint64_t>(integer(range["start"],0,limit));
                    clip.range_length=static_cast<std::uint64_t>(integer(range["length"],0,limit));
                    if(!range["loop"].isBool())throw std::invalid_argument("Range loop must be true or false");
                    clip.loop=range["loop"].toBool();
                }
            } else throw std::invalid_argument("Unsupported audio source");
        }
        project.content.clips.push_back(std::move(clip));
    }
    if(automated) {
        if(!v4["automation"].isArray() || v4["automation"].toArray().size()>2*static_cast<qsizetype>(fl_live::SessionDocument::track_limit))
            throw std::invalid_argument("Invalid automation array");
        for(const auto& value:v4["automation"].toArray()) {
            const bool deviceLane=deviceLanes && value.toObject().value("parameter")=="device";
            const auto lane=deviceLane ? exact(value,{"track","parameter","device","points"}) : exact(value,{"track","parameter","points"});
            AutomationLane decoded{fl_live::TrackId{idValue(lane["track"])}};
            if(lane["parameter"]=="level")decoded.parameter=AutomatedParameter::level;
            else if(lane["parameter"]=="pan")decoded.parameter=AutomatedParameter::pan;
            else if(deviceLane){decoded.parameter=AutomatedParameter::device;decoded.device=idValue(lane["device"]);}
            else throw std::invalid_argument("Unsupported automated parameter");
            if(!lane["points"].isArray() || lane["points"].toArray().size()>static_cast<qsizetype>(fl_live::Engine::automation_points_capacity))
                throw std::invalid_argument("Invalid automation points");
            for(const auto& p:lane["points"].toArray()) {
                const auto point=exact(p,{"at","value"});
                if(!point["value"].isDouble())throw std::invalid_argument("Automation values must be numbers");
                decoded.points.push_back({integer(point["at"],0,static_cast<int>(automation_position_limit)),point["value"].toDouble()});
            }
            project.content.automation.push_back(std::move(decoded));
        }
    }
    if(devices) {
        const auto section=exact(v4["devices"],{"next_id","tracks"});
        project.content.next_device_id=idValue(section["next_id"]);
        if(!section["tracks"].isArray() || section["tracks"].toArray().isEmpty() ||
           section["tracks"].toArray().size()>static_cast<qsizetype>(fl_live::SessionDocument::track_limit))
            throw std::invalid_argument("Invalid device tracks");
        const auto endpoint=[](const QJsonValue& v)->DeviceEndpoint{
            if(v=="source")return {DeviceEndpoint::Kind::source};
            if(v=="instrument")return {DeviceEndpoint::Kind::instrument};
            if(v=="output")return {DeviceEndpoint::Kind::output};
            return {DeviceEndpoint::Kind::device,idValue(v)};};
        for(const auto& value:section["tracks"].toArray()) {
            const auto entry=exact(value,{"track","devices","connections"});
            TrackDevices decoded{fl_live::TrackId{idValue(entry["track"])},{},{}};
            if(!entry["devices"].isArray() || entry["devices"].toArray().size()>static_cast<qsizetype>(devices_per_track_limit) || !entry["connections"].isArray() ||
               entry["connections"].toArray().size()>static_cast<qsizetype>(devices_per_track_limit+2))
                throw std::invalid_argument("Invalid device chain");
            for(const auto& d:entry["devices"].toArray()) {
                const auto device=exact(d,{"id","kind","value"});
                if(!device["value"].isDouble())throw std::invalid_argument("Device values must be numbers");
                Device parsed{idValue(device["id"]),DeviceKind::gain,device["value"].toDouble()};
                if(device["kind"]=="gain")parsed.kind=DeviceKind::gain;
                else if(device["kind"]=="transpose")parsed.kind=DeviceKind::transpose;
                else throw std::invalid_argument("Unsupported device kind");
                decoded.devices.push_back(parsed);
            }
            for(const auto& c:entry["connections"].toArray()) {
                if(!c.isArray() || c.toArray().size()!=2)throw std::invalid_argument("A device connection is a pair of endpoints");
                decoded.connections.push_back({endpoint(c.toArray()[0]),endpoint(c.toArray()[1])});
            }
            project.content.devices.push_back(std::move(decoded));
        }
    }
    validateContent(project.content,project.identities);
    return project;
}
AuthoredProject readAuthoredProject(const QString& path) {
    QFile file(path);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("Cannot open project for reading");
    if(file.size()>limit)throw std::invalid_argument("Authored project exceeds size limit");
    const auto bytes=file.read(limit+1);
    if(file.error()!=QFileDevice::NoError || bytes.size()>limit)throw std::runtime_error("Project read failed or exceeded size limit");
    return decodeAuthoredProject(bytes);
}
void writeAuthoredProject(const QString& path,const AuthoredProject& project) {
    const auto bytes=encodeAuthoredProject(project,QFileInfo(path).absolutePath());QSaveFile file(path);file.setDirectWriteFallback(false);
    if(!file.open(QIODevice::WriteOnly) || !fl_live::commitDurably(file,bytes))
        throw std::runtime_error("Cannot save authored project atomically; previous file was preserved");
}
