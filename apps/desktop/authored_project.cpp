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
        if(static_cast<std::size_t>(std::count_if(identities.clips.begin(),identities.clips.end(),[&](const auto& c){return c.track==track.id;}))!=desktop_clips_per_track)
            throw std::invalid_argument("Each desktop track holds exactly one clip");
    }
    for(std::size_t i=0;i<content.automation.size();++i) {
        const auto& lane=content.automation[i];
        if(std::none_of(identities.tracks.begin(),identities.tracks.end(),[&](const auto& t){return t.id==lane.track;}))
            throw std::invalid_argument("Automation lane for an unknown track");
        if(lane.parameter!=AutomatedParameter::level && lane.parameter!=AutomatedParameter::pan)throw std::invalid_argument("Unknown automated parameter");
        for(std::size_t j=0;j<i;++j)if(content.automation[j].track==lane.track && content.automation[j].parameter==lane.parameter)
            throw std::invalid_argument("A parameter has at most one automation lane");
        if(lane.points.empty() || lane.points.size()>fl_live::Engine::automation_points_capacity)
            throw std::invalid_argument("An automation lane holds 1 to 4096 points");
        const double low=lane.parameter==AutomatedParameter::level ? 0 : -100;
        for(std::size_t k=0;k<lane.points.size();++k) {
            const auto& point=lane.points[k];
            if(point.at<0 || point.at>automation_position_limit || (k && point.at<=lane.points[k-1].at))
                throw std::invalid_argument("Automation points must strictly increase within 256 bars");
            if(!std::isfinite(point.value) || point.value<low || point.value>100)
                throw std::invalid_argument("Automation value is outside the parameter's range");
        }
    }
    for(const auto& clip:identities.clips) {
        const auto& value=clipContent(content,clip.id);
        if(std::count_if(content.clips.begin(),content.clips.end(),[&](const auto& c){return c.clip==clip.id;})!=1)
            throw std::invalid_argument("Duplicate clip content");
        validatePattern(value.pattern);
        validateSynth(value.synth);
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
        if(kind==fl_live::TrackKind::midi)result.push_back({clip.id,patternClip(value.pattern,content.tempo,value.synth)});
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
        for(const auto& point:lane.points) {
            // Same upward rounding as every other musical boundary.
            const auto numerator=48000ULL*60*static_cast<std::uint64_t>(point.at);
            const auto denominator=static_cast<std::uint64_t>(content.tempo)*100;
            prepared.points.push_back({(numerator+denominator-1)/denominator,static_cast<float>(point.value/100.0)});
        }
        result.push_back(std::move(prepared));
    }
    return result;
}
std::vector<fl_live::DocumentTrackGain> contentGains(const SessionContent& content,const fl_live::DocumentSnapshot& identities) {
    validateContent(content,identities);
    std::vector<fl_live::DocumentTrackGain> result;
    for(const auto& track:identities.tracks)result.push_back({track.id,static_cast<float>(trackContent(content,track.id).level)/100});
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
    QJsonObject root{{"format","fl-live-project"},{"version",4},
        {"required_features",[&]{QJsonArray f{"general-session-v1"};if(ranges)f.append("clip-ranges-v1");if(relative)f.append("asset-relative-v1");
            if(automated)f.append("automation-v1");if(synths)f.append("synth-v1");return f;}()},
        {"tempo",project.content.tempo},
        {"session",session},{"tracks",tracks},{"clips",clips}};
    if(automated) {
        QJsonArray lanes;
        for(const auto& lane:project.content.automation) {
            QJsonArray points;
            for(const auto& point:lane.points)points.append(QJsonObject{{"at",static_cast<double>(point.at)},{"value",point.value}});
            lanes.append(QJsonObject{{"track",idText(lane.track.value)},{"parameter",lane.parameter==AutomatedParameter::level ? "level" : "pan"},{"points",points}});
        }
        root.insert("automation",lanes);
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
    const bool synths=features.contains("synth-v1");
    const auto v4=automated ? exact(root,{"format","version","required_features","tempo","session","tracks","clips","automation"})
                            : exact(root,{"format","version","required_features","tempo","session","tracks","clips"});
    QJsonArray canonical{"general-session-v1"};
    if(ranges)canonical.append("clip-ranges-v1");
    if(relativePaths)canonical.append("asset-relative-v1");
    if(automated)canonical.append("automation-v1");
    if(synths)canonical.append("synth-v1");
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
            const bool hasSynth=synths && object.contains("synth");
            const auto fields=hasSynth ? exact(value,{"id","pattern","synth"}) : exact(value,{"id","pattern"});
            clip.clip=fl_live::ClipId{idValue(fields["id"])};clip.pattern=decodePattern(fields["pattern"]);
            if(hasSynth) {
                const auto synth=exact(fields["synth"],{"waveform","attack_ms","decay_ms","sustain","release_ms"});
                const QStringList waves{"sine","triangle","square","saw"};
                if(!synth["waveform"].isString() || !waves.contains(synth["waveform"].toString()))throw std::invalid_argument("Unsupported waveform");
                clip.synth={static_cast<int>(waves.indexOf(synth["waveform"].toString())),integer(synth["attack_ms"],0,10000),
                    integer(synth["decay_ms"],0,10000),integer(synth["sustain"],0,100),integer(synth["release_ms"],0,10000)};
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
            const auto lane=exact(value,{"track","parameter","points"});
            AutomationLane decoded{fl_live::TrackId{idValue(lane["track"])}};
            if(lane["parameter"]=="level")decoded.parameter=AutomatedParameter::level;
            else if(lane["parameter"]=="pan")decoded.parameter=AutomatedParameter::pan;
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
