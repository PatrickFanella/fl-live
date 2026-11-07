#include "authored_project.hpp"
#include "fl_live/document_io.hpp"
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("Failed: " #condition); } while (false)
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}CHECK(failed);}
std::vector<float> render(const fl_live::SessionPlan& plan,std::size_t block){
    fl_live::Engine engine(plan);engine.start();
    for(std::size_t t=0;t<plan.tracks.size();++t)CHECK(engine.schedule({fl_live::CommandKind::launch,0,t,0})==fl_live::ScheduleResult::accepted);
    std::vector<float> result(100003);
    for(std::size_t i=0;i<result.size();i+=block)engine.process(std::span(result).subspan(i,std::min(block,result.size()-i)));
    return result;
}
// Version 3 files are no longer written; build them as the previous release did.
QByteArray v3(const ProjectSettings& settings,const fl_live::DocumentSnapshot& identities,QJsonArray features={"template-identities-v1"}) {
    return QJsonDocument(QJsonObject{{"format","fl-live-project"},{"version",3},{"required_features",features},
        {"settings",QJsonDocument::fromJson(encodeProjectSettings(settings)).object()},
        {"session",QJsonDocument::fromJson(fl_live::encodeDocumentMetadata(identities)).object()}}).toJson();
}
void legacyMigration() {
    const QByteArray legacy=R"({"format":"fl-live-project","version":1,"template":"pulse-steps-v1","tempo":137,"levels":[35,60],"pitches":[48,55,59,62]})";
    auto project=decodeAuthoredProject(legacy);
    CHECK(project.content.tempo==137 && templateView(project.content,project.identities).steps[0].pitch==48);
    CHECK(templateView(project.content,project.identities).levels==(std::array<int,2>{35,60}));
    CHECK(project.identities==defaultTemplateIdentities());
    const auto settings=templateView(project.content,project.identities);
    CHECK(decodeAuthoredProject(encodeProjectSettings(settings)).content==project.content);
    // Version 3: renamed identities above 2^53, reversed clip order.
    fl_live::SessionDocument document(project.identities);
    document.renameTrack(project.identities.tracks[0].id,"Drums");
    document.renameClip(project.identities.clips[1].id,"Melody");
    auto identities=document.snapshot();
    constexpr std::uint64_t delta=9007199254740992ULL;
    for(auto& t:identities.tracks)t.id.value+=delta;
    for(auto& c:identities.clips){c.id.value+=delta;c.track.value+=delta;}
    for(auto& s:identities.scenes){s.id.value+=delta;for(auto& e:s.entries){e.track.value+=delta;e.clip.value+=delta;}}
    identities.next_id+=delta;
    std::reverse(identities.clips.begin(),identities.clips.end());
    const auto migrated=decodeAuthoredProject(v3(settings,identities));
    CHECK(migrated.identities==identities && templateView(migrated.content,migrated.identities)==settings);
    // Migrated content renders the same samples as the template plan did.
    const fl_live::DocumentRenderPlan before(identities,templateSources(settings,identities),48000,137000,{},templateGains(settings,identities));
    const fl_live::DocumentRenderPlan after(migrated.identities,contentSources(migrated.content,migrated.identities),48000,137000,{},
        contentGains(migrated.content,migrated.identities));
    CHECK(render(before.plan(),1)==render(after.plan(),257));
    CHECK(render(prepareProject(settings),64)==render(after.plan(),257));
    // Version 3 feature gates still hold for old files.
    auto sparse=identities;sparse.scenes[0].entries.resize(1);
    CHECK(decodeAuthoredProject(v3(settings,sparse,{"template-identities-v1","template-scene-slots-v1"})).identities==sparse);
    rejects([&]{decodeAuthoredProject(v3(settings,sparse));});
    fl_live::SessionDocument scenes(identities);scenes.createScene("Break");
    CHECK(decodeAuthoredProject(v3(settings,scenes.snapshot(),{"template-identities-v1","template-scene-slots-v1","template-scenes-v1"})).identities==scenes.snapshot());
    rejects([&]{decodeAuthoredProject(v3(settings,scenes.snapshot(),{"template-identities-v1","template-scene-slots-v1"}));});
    auto root=QJsonDocument::fromJson(v3(settings,identities)).object();
    const auto bad=[&](auto edit){auto value=root;edit(value);rejects([&]{decodeAuthoredProject(QJsonDocument(value).toJson());});};
    bad([](auto& r){r["version"]=5;});bad([](auto& r){r["version"]=true;});
    bad([](auto& r){r["required_features"]=QJsonArray{"future"};});
    bad([](auto& r){r["extra"]=0;});bad([](auto& r){r.remove("session");});
    bad([](auto& r){auto s=r["settings"].toObject();s["tempo"]=0;r["settings"]=s;});
    rejects([&]{decodeAuthoredProject(legacy+QByteArray(65536,' '));});
}
AuthoredProject general() {
    // Two audio and two MIDI tracks covering every source kind.
    auto project=defaultProject();
    fl_live::SessionDocument document(project.identities);
    const auto audio=document.createTrack("Imported",fl_live::TrackKind::audio);
    const auto midi=document.createTrack("Bass",fl_live::TrackKind::midi);
    const auto wav=document.createClip(audio,"Loop"),notes=document.createClip(midi,"Riff");
    document.setSlot(project.identities.scenes[0].id,midi,notes);
    document.setTrackMix(audio,fl_live::TrackMix{-30,false,true});
    project.identities=document.snapshot();
    project.content.tracks.push_back({audio,55});project.content.tracks.push_back({midi,90});
    ClipContent sample{wav};sample.audio=AudioSource::wav;sample.asset="/media/loops/break 120.wav";
    ClipContent riff{notes};riff.pattern.step_count=7;riff.pattern.steps[6]={true,36,100,1};
    project.content.clips.push_back(sample);project.content.clips.push_back(riff);
    project.content.tempo=133;
    return project;
}
void generalRoundTrip() {
    auto project=general();
    validateContent(project.content,project.identities);
    const auto bytes=encodeAuthoredProject(project);
    const auto root=QJsonDocument::fromJson(bytes).object();
    CHECK(root["version"]==4 && root["required_features"].toArray()==QJsonArray{"general-session-v1"});
    const auto decoded=decodeAuthoredProject(bytes);
    CHECK(decoded.identities==project.identities && decoded.content==project.content);
    QTemporaryDir directory;CHECK(directory.isValid());const auto path=directory.filePath("general.json");
    writeAuthoredProject(path,project);CHECK(readAuthoredProject(path).content==project.content);
    // Media is never decoded by the codec; the imported clip renders silence
    // until resolved, and resolving it makes it play.
    const fl_live::DocumentRenderPlan silent(decoded.identities,contentSources(decoded.content,decoded.identities));
    CHECK(std::get<fl_live::AudioClip>(silent.plan().tracks[2].clips[0]).samples==std::vector<float>{0.0F});
    auto resolved=decoded.content;
    std::find_if(resolved.clips.begin(),resolved.clips.end(),[](const auto& c){return c.audio==AudioSource::wav;})->media=
        std::make_shared<const fl_live::AudioClip>(fl_live::AudioClip{{0.25F,-0.25F},true,2});
    const fl_live::DocumentRenderPlan loud(decoded.identities,contentSources(resolved,decoded.identities),48000,133000,{},contentGains(resolved,decoded.identities));
    CHECK(std::get<fl_live::AudioClip>(loud.plan().tracks[2].clips[0]).channels==2);
    CHECK(std::get<fl_live::MidiClip>(loud.plan().tracks[3].clips[0]).notes.size()==5); // template steps 1-4 plus step 7
    // The imported track is soloed (pan -30): every other track prepares silent.
    CHECK(loud.plan().tracks[2].gain==0.55F && loud.plan().tracks[2].pan==-0.3F);
    CHECK(loud.plan().tracks[0].gain==0 && loud.plan().tracks[3].gain==0);
    // Empty audio tracks and templates without any MIDI track.
    auto empty=project;
    std::find_if(empty.content.clips.begin(),empty.content.clips.end(),[](const auto& c){return c.audio==AudioSource::wav;})->audio=AudioSource::empty;
    std::find_if(empty.content.clips.begin(),empty.content.clips.end(),[](const auto& c){return c.audio==AudioSource::empty;})->asset.clear();
    CHECK(decodeAuthoredProject(encodeAuthoredProject(empty)).content==empty.content);
    const auto view=templateView(project.content,project.identities);
    CHECK(view.tempo==133 && view.levels==(std::array<int,2>{70,25}));
}
void generalRejection() {
    const auto project=general();
    const auto good=QJsonDocument::fromJson(encodeAuthoredProject(project)).object();
    const auto bad=[&](auto edit){auto r=good;edit(r);rejects([&]{decodeAuthoredProject(QJsonDocument(r).toJson());});};
    const auto clip=[&](int i,auto edit){bad([&](auto& r){auto a=r["clips"].toArray();auto c=a[i].toObject();edit(c);a[i]=c;r["clips"]=a;});};
    bad([](auto& r){r["version"]=5;});
    bad([](auto& r){r["required_features"]=QJsonArray{"general-session-v1","future"};});
    bad([](auto& r){r["extra"]=true;});
    bad([](auto& r){r.remove("tracks");});
    bad([](auto& r){r["tempo"]=301;});
    bad([](auto& r){r["session"]=QJsonArray{};});
    bad([](auto& r){auto a=r["tracks"].toArray();auto t=a[0].toObject();t["level"]=101;a[0]=t;r["tracks"]=a;});
    bad([](auto& r){auto a=r["tracks"].toArray();a.removeLast();r["tracks"]=a;});
    bad([](auto& r){auto a=r["clips"].toArray();a.append(a[0]);r["clips"]=a;});
    bad([](auto& r){auto a=r["clips"].toArray();a.removeLast();r["clips"]=a;});
    clip(0,[](auto& c){c["audio"]="future";});
    clip(0,[](auto& c){c.remove("audio");c["pattern"]=QJsonObject{};});           // pattern on an audio clip
    clip(1,[](auto& c){c.remove("pattern");c["audio"]="pulse";});                 // audio source on a MIDI clip
    clip(2,[](auto& c){c["path"]="relative.wav";});
    clip(2,[](auto& c){c["path"]="";});
    clip(2,[](auto& c){c.remove("path");});
    clip(0,[](auto& c){c["path"]="/a.wav";});                                      // only wav carries a path
    clip(3,[](auto& c){auto p=c["pattern"].toObject();p["length"]=17;c["pattern"]=p;});
    clip(3,[](auto& c){auto p=c["pattern"].toObject();auto s=p["steps"].toArray();s.removeLast();p["steps"]=s;c["pattern"]=p;});
    clip(3,[](auto& c){c["id"]="99";});
    // A desktop track holds 1 to 8 clips; a project keeps at least one scene.
    auto twoClips=project;fl_live::SessionDocument document(twoClips.identities);
    const auto extra=document.createClip(twoClips.identities.tracks[0].id,"Second");twoClips.identities=document.snapshot();
    twoClips.content.clips.push_back({extra});
    CHECK(decodeAuthoredProject(encodeAuthoredProject(twoClips)).content==twoClips.content);
    auto nine=twoClips;fl_live::SessionDocument many(nine.identities);
    for(int n=0;n<7;++n){const auto more=many.createClip(nine.identities.tracks[0].id,"More");nine.content.clips.push_back({more});}
    nine.identities=many.snapshot();
    rejects([&]{encodeAuthoredProject(nine);});
    auto none=project;fl_live::SessionDocument empty(none.identities);
    const auto lone=none.identities.clips[0].id;empty.deleteClip(lone);none.identities=empty.snapshot();
    std::erase_if(none.content.clips,[&](const auto& c){return c.clip==lone;});
    rejects([&]{encodeAuthoredProject(none);});
    auto noScenes=project;noScenes.identities.scenes.clear();
    rejects([&]{encodeAuthoredProject(noScenes);});
    auto badLevel=project;badLevel.content.tracks[0].level=-1;rejects([&]{encodeAuthoredProject(badLevel);});
    QTemporaryDir directory;CHECK(directory.isValid());const auto path=directory.filePath("keep.json");
    writeAuthoredProject(path,project);
    QFile file(path);CHECK(file.open(QIODevice::ReadOnly));const auto saved=file.readAll();file.close();
    rejects([&]{writeAuthoredProject(path,badLevel);});
    CHECK(file.open(QIODevice::ReadOnly));CHECK(file.readAll()==saved);
    rejects([&]{decodeAuthoredProject(QByteArray(4*1024*1024+1024*1024+1,' '));});
}
void clipRanges() {
    auto project=general();
    {   // Center the imported track so each channel is the raw source.
        fl_live::SessionDocument document(project.identities);
        document.setTrackMix(project.identities.tracks[2].id,fl_live::TrackMix{});project.identities=document.snapshot();
    }
    auto& wav=*std::find_if(project.content.clips.begin(),project.content.clips.end(),[](const auto& c){return c.audio==AudioSource::wav;});
    const auto wavId=wav.clip;
    CHECK(!QJsonDocument::fromJson(encodeAuthoredProject(project)).object()["required_features"].toArray().contains("clip-ranges-v1"));
    // Ten stereo frames: left n/16, right -n/16.
    fl_live::AudioClip source{{},true,2};
    for(int n=0;n<10;++n){source.samples.push_back(static_cast<float>(n)/16);source.samples.push_back(-static_cast<float>(n)/16);}
    wav.media=std::make_shared<const fl_live::AudioClip>(source);
    wav.range_start=2;wav.range_length=5;wav.loop=false;
    validateContent(project.content,project.identities);
    const auto bytes=encodeAuthoredProject(project);
    const auto root=QJsonDocument::fromJson(bytes).object();
    CHECK((root["required_features"].toArray()==QJsonArray{"general-session-v1","clip-ranges-v1"}));
    auto decoded=decodeAuthoredProject(bytes);
    auto expected=project.content;
    for(auto& c:expected.clips)c.media.reset();
    CHECK(decoded.content==expected);
    // Source-rate slice: one-shot plays frames 2..6 once from the launch frame.
    const auto render=[&](const SessionContent& content,std::size_t block) {
        const fl_live::DocumentRenderPlan plan(project.identities,contentSources(content,project.identities));
        fl_live::Engine engine(plan.plan());
        CHECK(engine.schedule(*plan.launch(wavId,13))==fl_live::ScheduleResult::accepted);
        engine.start();std::vector<float> out(64);
        for(std::size_t at=0;at<out.size();at+=2*block)CHECK(engine.process(std::span(out).subspan(at,std::min(2*block,out.size()-at)),2));
        return out;
    };
    for(auto block:{1U,7U,257U}) {
        const auto once=render(project.content,block);
        for(std::size_t f=0;f<32;++f) {
            const bool playing=f>=13 && f<18;
            const float value=playing ? static_cast<float>(f-13+2)/16 : 0;
            CHECK(once[2*f]==value && once[2*f+1]==-value);
        }
        auto looped=project.content;
        std::find_if(looped.clips.begin(),looped.clips.end(),[&](const auto& c){return c.clip==wavId;})->loop=true;
        const auto loop=render(looped,block);
        for(std::size_t f=13;f<32;++f)CHECK(loop[2*f]==static_cast<float>((f-13)%5+2)/16);
    }
    // Length 0 means to the end of the source; ranges must stay inside it.
    const auto fails=[&](std::uint64_t start,std::uint64_t length,bool keepWav) {
        auto bad=project.content;auto& c=*std::find_if(bad.clips.begin(),bad.clips.end(),[&](const auto& x){return x.clip==wavId;});
        c.range_start=start;c.range_length=length;
        if(!keepWav){c.audio=AudioSource::empty;c.asset.clear();c.media.reset();}
        rejects([&]{validateContent(bad,project.identities);});
    };
    fails(10,0,true);fails(9,2,true);fails(0,11,true);fails(1,0,false);
    auto toEnd=project.content;std::find_if(toEnd.clips.begin(),toEnd.clips.end(),[&](const auto& x){return x.clip==wavId;})->range_length=0;
    validateContent(toEnd,project.identities);
    const fl_live::DocumentRenderPlan whole(project.identities,contentSources(toEnd,project.identities));
    CHECK(std::get<fl_live::AudioClip>(whole.plan().tracks[2].clips[0]).samples.size()==16);
    // An unresolved clip keeps its saved range and renders silence.
    CHECK(std::get<fl_live::AudioClip>(fl_live::DocumentRenderPlan(decoded.identities,contentSources(decoded.content,decoded.identities))
        .plan().tracks[2].clips[0]).samples==std::vector<float>{0.0F});
    const auto bad=[&](auto edit){auto r=root;edit(r);rejects([&]{decodeAuthoredProject(QJsonDocument(r).toJson());});};
    const auto range=[&](auto edit){bad([&](auto& r){auto a=r["clips"].toArray();auto c=a[2].toObject();auto g=c["range"].toObject();edit(g);c["range"]=g;a[2]=c;r["clips"]=a;});};
    bad([](auto& r){r["required_features"]=QJsonArray{"general-session-v1"};});
    bad([](auto& r){auto a=r["clips"].toArray();auto c=a[2].toObject();c.remove("range");a[2]=c;r["clips"]=a;});
    bad([](auto& r){auto a=r["clips"].toArray();auto c=a[0].toObject();c["range"]=QJsonObject{};a[0]=c;r["clips"]=a;});
    range([](auto& g){g["start"]=-1;});range([](auto& g){g["length"]=1.5;});range([](auto& g){g["loop"]=0;});range([](auto& g){g["end"]=3;});
    range([](auto& g){g["start"]=16777217.0;});
}
void relativeAssets() {
    auto project=general();
    auto& wav=*std::find_if(project.content.clips.begin(),project.content.clips.end(),[](const auto& c){return c.audio==AudioSource::wav;});
    const auto wavId=wav.clip;
    wav.asset="/projects/song/audio/loop.wav";
    CHECK(assetRelativePath(wav.asset,"/projects/song")=="audio/loop.wav");
    CHECK(assetRelativePath(wav.asset,"/projects/other").isEmpty() && assetRelativePath(wav.asset,"").isEmpty());
    const auto outside=QJsonDocument::fromJson(encodeAuthoredProject(project,"/elsewhere")).object();
    CHECK(!outside["required_features"].toArray().contains("asset-relative-v1"));
    const auto bytes=encodeAuthoredProject(project,"/projects/song");
    const auto root=QJsonDocument::fromJson(bytes).object();
    CHECK((root["required_features"].toArray()==QJsonArray{"general-session-v1","asset-relative-v1"}));
    const auto decoded=decodeAuthoredProject(bytes);
    CHECK(decoded.content==project.content && decoded.relative==(std::vector<RelativeAsset>{{wavId,"audio/loop.wav"}}));
    const auto bad=[&](auto edit){auto r=root;edit(r);rejects([&]{decodeAuthoredProject(QJsonDocument(r).toJson());});};
    const auto relative=[&](QJsonValue v){bad([&](auto& r){auto a=r["clips"].toArray();auto c=a[2].toObject();c["relative"]=v;a[2]=c;r["clips"]=a;});};
    relative("../loop.wav");relative("/abs/loop.wav");relative("a/../../b.wav");relative("");relative(3);relative("./audio/loop.wav");
    bad([](auto& r){r["required_features"]=QJsonArray{"general-session-v1"};});
    bad([](auto& r){r["required_features"]=QJsonArray{"asset-relative-v1","general-session-v1"};});
    bad([](auto& r){auto a=r["clips"].toArray();auto c=a[0].toObject();c["relative"]="x.wav";a[0]=c;r["clips"]=a;}); // pulse clip
}
void automationLanes() {
    auto project=general();
    const auto pulse=project.identities.tracks[0].id,bass=project.identities.tracks[3].id;
    project.content.automation={{pulse,AutomatedParameter::level,{{0,0},{400,100}}},{bass,AutomatedParameter::pan,{{0,-100},{800,100}}}};
    validateContent(project.content,project.identities);
    const auto bytes=encodeAuthoredProject(project);
    const auto root=QJsonDocument::fromJson(bytes).object();
    CHECK(root["required_features"].toArray().contains("automation-v1") && root["automation"].toArray().size()==2);
    auto decoded=decodeAuthoredProject(bytes);
    CHECK(decoded.content.automation==project.content.automation);
    // Positions map to frames with the shared upward rounding at 133 BPM.
    const auto lanes=contentAutomation(project.content,project.identities);
    CHECK(lanes.size()==2 && lanes[0].target==fl_live::AutomationTarget::gain && lanes[1].target==fl_live::AutomationTarget::pan);
    CHECK((lanes[0].points==std::vector<fl_live::AutomationPoint>{{0,0.0F},{86617,1.0F}})); // ceil(48000*60*4/133)
    CHECK(lanes[1].points[1].at==173234 && lanes[1].points[0].value==-1.0F); // ceil(173233.08)
    // Rendering: the pulse track's gain ramps from 0 to 1 over the first bar.
    auto unsolo=project; {fl_live::SessionDocument d(unsolo.identities);d.setTrackMix(unsolo.identities.tracks[2].id,{});unsolo.identities=d.snapshot();}
    const fl_live::DocumentRenderPlan plan(unsolo.identities,contentSources(unsolo.content,unsolo.identities),48000,133000,{},
        contentGains(unsolo.content,unsolo.identities),contentAutomation(unsolo.content,unsolo.identities));
    CHECK(plan.plan().tracks[0].gain_automation.size()==2 && plan.plan().tracks[3].pan_automation.size()==2);
    fl_live::Engine engine(plan.plan());
    CHECK(engine.schedule(*plan.launch(project.identities.clips[0].id,0))==fl_live::ScheduleResult::accepted);
    engine.start();std::vector<float> out(2*50000);CHECK(engine.process(out,2));
    const auto source=pulseClip(133);
    for(std::size_t f=1;f<50000;f+=997) {
        const float gain=static_cast<float>(static_cast<double>(f)/86617.0);
        CHECK(out[2*f]==gain*source.samples[f%source.samples.size()]);
    }
    // A muted track stays silent even when automated.
    auto muted=unsolo;{fl_live::SessionDocument d(muted.identities);d.setTrackMix(pulse,{0,true,false});muted.identities=d.snapshot();}
    const fl_live::DocumentRenderPlan silent(muted.identities,contentSources(muted.content,muted.identities),48000,133000,{},
        contentGains(muted.content,muted.identities),contentAutomation(muted.content,muted.identities));
    CHECK(silent.plan().tracks[0].gain==0 && silent.plan().tracks[0].gain_automation.empty());
    // Validation.
    const auto invalid=[&](auto edit){auto c=project.content;edit(c);rejects([&]{validateContent(c,project.identities);});};
    invalid([](auto& c){c.automation[0].track=fl_live::TrackId{999};});
    invalid([](auto& c){c.automation.push_back(c.automation[0]);});
    invalid([](auto& c){c.automation[0].points.clear();});
    invalid([](auto& c){c.automation[0].points={{10,5},{10,6}};});
    invalid([](auto& c){c.automation[0].points={{0,101}};});
    invalid([](auto& c){c.automation[1].points={{0,-101}};});
    invalid([](auto& c){c.automation[0].points={{-1,5}};});
    invalid([](auto& c){c.automation[0].points={{automation_position_limit+1,5}};});
    invalid([](auto& c){c.automation[0].points={{0,std::nan("")}};});
    rejects([&]{fl_live::DocumentRenderPlan p(project.identities,contentSources(project.content,project.identities),48000,133000,{},{},
        std::vector<fl_live::DocumentAutomation>{{fl_live::TrackId{999},fl_live::AutomationTarget::gain,{{0,1}}}});});
    const auto bad=[&](auto edit){auto r=root;edit(r);rejects([&]{decodeAuthoredProject(QJsonDocument(r).toJson());});};
    bad([](auto& r){r.remove("automation");});
    bad([](auto& r){r["required_features"]=QJsonArray{"general-session-v1"};}); // automation present without its feature
    bad([](auto& r){auto a=r["automation"].toArray();auto l=a[0].toObject();l["parameter"]="volume";a[0]=l;r["automation"]=a;});
    bad([](auto& r){auto a=r["automation"].toArray();auto l=a[0].toObject();l["extra"]=1;a[0]=l;r["automation"]=a;});
    bad([](auto& r){auto a=r["automation"].toArray();auto l=a[0].toObject();auto pts=l["points"].toArray();auto pt=pts[0].toObject();pt["value"]="0";pts[0]=pt;l["points"]=pts;a[0]=l;r["automation"]=a;});
    project.content.automation.clear();
    CHECK(!QJsonDocument::fromJson(encodeAuthoredProject(project)).object().contains("automation"));
}
void synthSettings() {
    auto project=general();
    auto& riff=*std::find_if(project.content.clips.begin(),project.content.clips.end(),[&](const auto& c){return c.clip==project.identities.clips[3].id;});
    riff.synth={3,5,120,40,800};
    const auto bytes=encodeAuthoredProject(project);
    const auto root=QJsonDocument::fromJson(bytes).object();
    CHECK(root["required_features"].toArray().contains("synth-v1"));
    CHECK(decodeAuthoredProject(bytes).content==project.content);
    const auto sources=contentSources(project.content,project.identities);
    const auto& clip=std::get<fl_live::MidiClip>(std::find_if(sources.begin(),sources.end(),[&](const auto& s){return s.id==riff.clip;})->content);
    CHECK((clip.instrument==fl_live::Instrument{fl_live::Waveform::saw,240,5760,0.4F,38400}));
    const auto bad=[&](auto edit){auto r=root;edit(r);rejects([&]{decodeAuthoredProject(QJsonDocument(r).toJson());});};
    const auto synth=[&](const char* key,QJsonValue v){bad([&](auto& r){auto a=r["clips"].toArray();auto c=a[3].toObject();auto s=c["synth"].toObject();s[key]=v;c["synth"]=s;a[3]=c;r["clips"]=a;});};
    synth("waveform","noise");synth("sustain",101);synth("attack_ms",-1);synth("release_ms",10001);synth("extra",1);
    bad([](auto& r){r["required_features"]=QJsonArray{"general-session-v1"};});
    auto audioSynth=project.content;audioSynth.clips[0].synth.attack_ms=5;rejects([&]{validateContent(audioSynth,project.identities);});
}
// Piano-roll notes: validation, tick-to-frame rendering, persistence and
// malformed files (#35).
void pianoRoll() {
    // Validation refuses each malformed case without needing a project.
    const PianoRoll good{384,true,4,{{3,0,24,64,50},{1,0,72,60,70},{2,96,72,64,70}}};
    rejects([&]{validatePiano(good);}); // the order is (start, ID): ID 3 may not precede ID 1 at the same start
    auto ok=good;sortPiano(ok);validatePiano(ok);
    CHECK(ok.notes[0].id==1 && ok.notes[1].id==3 && ok.notes[2].id==2);
    const auto bad=[&](auto edit){auto r=ok;edit(r);rejects([&]{validatePiano(r);});};
    bad([](auto& r){r.length=0;});bad([](auto& r){r.length=piano_length_limit+1;});
    bad([](auto& r){r.notes[0].pitch=128;});bad([](auto& r){r.notes[0].velocity=0;});bad([](auto& r){r.notes[0].velocity=101;});
    bad([](auto& r){r.notes[0].length=0;});bad([](auto& r){r.notes[2].start=piano_length_limit;});bad([](auto& r){r.notes[2].length=piano_length_limit;});
    bad([](auto& r){r.notes[1].id=1;});bad([](auto& r){r.notes[2].id=4;}); // duplicate, unissued
    // Past the clip length is allowed (silent or cut at the end when rendered), up to 16 bars.
    auto past=ok;past.notes[2].length=289;validatePiano(past);
    const auto cut=pianoClip(past,120,{});CHECK(cut.notes.size()==3 && cut.notes[2].start+cut.notes[2].duration==cut.length);
    past.notes[2].start=384;past.notes[2].length=24;sortPiano(past);validatePiano(past);CHECK(pianoClip(past,120,{}).notes.size()==2);
    bad([](auto& r){r.next_id=1;});
    auto crowded=ok;for(int n=0;n<14;++n)crowded.notes.push_back({crowded.next_id++,0,24,40+n,80});
    sortPiano(crowded);validatePiano(crowded); // 16 sounding at tick 0
    crowded.notes.push_back({crowded.next_id++,10,24,90,80});sortPiano(crowded);rejects([&]{validatePiano(crowded);});
    PianoRoll many{piano_length_limit,true,1,{}};
    for(int n=0;n<1025;++n)many.notes.push_back({many.next_id++,n*6,6,60,80});
    rejects([&]{validatePiano(many);});many.notes.pop_back();validatePiano(many);
    // Rendering at 137 BPM: frame = ceil(ticks * 30000 / 137), rounded independently.
    const auto midi=pianoClip(ok,137,{});
    const auto frame=[](std::uint64_t t){return (t*30000+136)/137;};
    CHECK(midi.length==frame(384) && midi.notes.size()==3);
    CHECK(midi.notes[0].start==0 && midi.notes[0].duration==frame(72) && midi.notes[0].pitch==60 && midi.notes[0].velocity==0.7F);
    CHECK(midi.notes[2].start==frame(96) && midi.notes[2].duration==frame(168)-frame(96));
    // A MIDI clip with notes renders them instead of its pattern, and persists behind piano-roll-v1.
    auto project=general();
    auto& riff=*std::find_if(project.content.clips.begin(),project.content.clips.end(),[&](const auto& c){return c.clip==project.identities.clips[3].id;});
    riff.piano=ok;
    const auto sources=contentSources(project.content,project.identities);
    const auto& rendered=std::get<fl_live::MidiClip>(std::find_if(sources.begin(),sources.end(),[&](const auto& x){return x.id==riff.clip;})->content);
    CHECK(rendered.notes.size()==3 && rendered.length==pianoClip(ok,project.content.tempo,{}).length);
    const auto bytes=encodeAuthoredProject(project);
    const auto root=QJsonDocument::fromJson(bytes).object();
    CHECK(root["required_features"].toArray().contains("piano-roll-v1"));
    const auto piano=root["clips"].toArray()[3].toObject()["piano"].toObject();
    CHECK(piano["next_id"]=="4" && piano["notes"].toArray()[0].toObject()["id"]=="1" && piano["length"]==384);
    CHECK(decodeAuthoredProject(bytes).content==project.content);
    const auto broken=[&](auto edit){auto r=root;edit(r);rejects([&]{decodeAuthoredProject(QJsonDocument(r).toJson());});};
    const auto note=[&](const char* key,QJsonValue v){broken([&](auto& r){auto a=r["clips"].toArray();auto c=a[3].toObject();auto p=c["piano"].toObject();
        auto n=p["notes"].toArray();auto o=n[0].toObject();o[key]=v;n[0]=o;p["notes"]=n;c["piano"]=p;a[3]=c;r["clips"]=a;});};
    note("pitch",128);note("velocity",0);note("start",-1);note("id",7);note("id","01");note("extra",1);note("length",0);
    broken([](auto& r){auto f=r["required_features"].toArray();f.removeLast();r["required_features"]=f;}); // notes without the feature
    broken([](auto& r){auto a=r["clips"].toArray();auto c=a[3].toObject();auto p=c["piano"].toObject();
        const auto n=p["notes"].toArray();p["notes"]=QJsonArray{n[2],n[0],n[1]};c["piano"]=p;a[3]=c;r["clips"]=a;}); // out of order
    auto onAudio=project.content;onAudio.clips[0].piano=ok;rejects([&]{validateContent(onAudio,project.identities);});
}
// A note moved while it sounds: its voice finishes the original duration and
// the moved note starts at its new position; nothing is left sounding (#36).
void movedNoteVoices() {
    const PianoRoll before{384,true,2,{{1,0,48,60,80}}};
    PianoRoll after=before;after.notes[0].start=192;
    const fl_live::SessionPlan a{48000,120000,{{1,{pianoClip(before,120,{})}}}},b{48000,120000,{{1,{pianoClip(after,120,{})}}}};
    fl_live::Engine engine(a);
    std::vector<float> out;
    const auto step=[&](int at,std::size_t voices){if(!engine.process(out,2) || engine.active_voices()!=voices)throw std::runtime_error("moved note voices, step "+std::to_string(at)+": "+std::to_string(engine.active_voices()));};
    CHECK(engine.schedule({fl_live::CommandKind::launch,0,0,0})==fl_live::ScheduleResult::accepted);
    engine.start();
    out.resize(2*6000);
    step(1,1); // 6000 of 12000 frames sounded
    (void)engine.replace(b,{{0},{{0}}});
    out.resize(2*5999);step(2,1); // finishing its own duration
    out.resize(2*1);step(3,0);  // ended at frame 12000
    out.resize(2*(48000-12000));step(4,0); // silent until the moved note
    out.resize(2*1);step(5,1);             // starts at frame 48000
    out.resize(2*11998);step(6,1);
    out.resize(2*1);step(7,0);             // and ends after frame 59999
}
// Loop timing of a 3-beat-and-a-sixteenth clip at 137 BPM: every repetition
// is the rounded clip length, at any block size; a one-shot stops (#38).
void pianoLoopTiming() {
    const auto frame=[](std::uint64_t t){return (t*30000+136)/137;};
    const fl_live::Frame length=frame(312),onset=frame(37),release=frame(61);
    CHECK(length==68322 && onset==8103 && release==13358);
    const auto render=[&](bool loop,std::size_t block){
        const PianoRoll roll{312,loop,2,{{1,37,24,69,100}}};
        const fl_live::SessionPlan plan{48000,137000,{{1,{pianoClip(roll,137,{2,0,0,100,0})}}}};
        fl_live::Engine engine(plan);
        CHECK(engine.schedule({fl_live::CommandKind::launch,0,0,0})==fl_live::ScheduleResult::accepted);
        engine.start();
        std::vector<float> out(2*5*length);
        for(std::size_t at=0;at<5*length;at+=block){const auto n=std::min<std::size_t>(block,5*length-at);CHECK(engine.process(std::span(out).subspan(2*at,2*n),2));}
        return std::pair{out,engine.track_statuses()[0].active_clip};
    };
    const auto [looped,active]=render(true,64);
    CHECK(active==0);
    for(fl_live::Frame k=0;k<5;++k) {
        const auto base=k*length;
        CHECK(looped[2*(base+onset-1)]==0 && looped[2*(base+onset)]!=0 && looped[2*(base+release-1)]!=0 && looped[2*(base+release)]==0);
    }
    for(const std::size_t block:{1U,127U,257U,1000U})CHECK(render(true,block).first==looped);
    const auto [once,stopped]=render(false,257);
    CHECK(stopped<0 && std::equal(once.begin(),once.begin()+2*length,looped.begin()));
    CHECK(std::all_of(once.begin()+2*length,once.end(),[](float v){return v==0;}));
    // The one-shot flag is saved only when false; a true flag in a file is refused.
    auto project=general();
    auto& riff=*std::find_if(project.content.clips.begin(),project.content.clips.end(),[&](const auto& c){return c.clip==project.identities.clips[3].id;});
    riff.piano=PianoRoll{312,false,2,{{1,37,24,69,100}}};
    const auto bytes=encodeAuthoredProject(project);
    auto root=QJsonDocument::fromJson(bytes).object();
    CHECK(root["clips"].toArray()[3].toObject()["piano"].toObject()["loop"]==QJsonValue(false));
    CHECK(decodeAuthoredProject(bytes).content==project.content);
    auto clips=root["clips"].toArray();auto clip=clips[3].toObject();auto piano=clip["piano"].toObject();piano["loop"]=true;clip["piano"]=piano;clips[3]=clip;root["clips"]=clips;
    rejects([&]{decodeAuthoredProject(QJsonDocument(root).toJson());});
}
// Typed device chains (#73, #74): validation, compilation and persistence.
void deviceGraph() {
    auto project=general();
    const auto midiTrack=project.identities.tracks[3].id,audioTrack=project.identities.tracks[0].id;
    CHECK(project.identities.tracks[3].kind==fl_live::TrackKind::midi && project.identities.tracks[0].kind==fl_live::TrackKind::audio);
    const std::vector<Device> chain{{1,DeviceKind::transpose,5},{2,DeviceKind::gain,-6},{3,DeviceKind::transpose,-12}};
    project.content.devices={{midiTrack,chain,chainConnections(chain,fl_live::TrackKind::midi)}};
    project.content.next_device_id=4;
    validateContent(project.content,project.identities);
    using K=DeviceEndpoint::Kind;
    CHECK((project.content.devices[0].connections==std::vector<DeviceConnection>{{{K::source},{K::device,1}},{{K::device,1},{K::device,3}},
        {{K::device,3},{K::instrument}},{{K::instrument},{K::device,2}},{{K::device,2},{K::output}}}));
    // Compiled: the MIDI stage transposes notes (+5 then -12), the audio stage scales the track.
    auto plain=project;plain.content.devices.clear();
    const auto find=[](const auto& sources,fl_live::ClipId id){return std::get<fl_live::MidiClip>(std::find_if(sources.begin(),sources.end(),[&](const auto& x){return x.id==id;})->content);};
    const auto clip=project.identities.clips[3].id;
    const auto before=find(contentSources(plain.content,plain.identities),clip),after=find(contentSources(project.content,project.identities),clip);
    CHECK(before.notes.size()==after.notes.size() && !after.notes.empty());
    for(std::size_t i=0;i<after.notes.size();++i)CHECK(after.notes[i].pitch==before.notes[i].pitch-7 && after.notes[i].start==before.notes[i].start);
    const auto gains=contentGains(project.content,project.identities);
    CHECK(gains[3].device_gain==static_cast<float>(std::pow(10.0,-6.0/20)) && gains[0].device_gain==1.0F);
    // A note pushed out of range by any device is dropped, not clamped.
    fl_live::MidiClip edge{100,{{0,10,125,1},{10,10,3,1},{20,10,60,1}},true};
    const auto moved=transposed(edge,{{1,DeviceKind::transpose,5},{2,DeviceKind::transpose,-5}});
    CHECK(moved.notes.size()==2 && moved.notes[0].pitch==3 && moved.notes[1].pitch==60); // 125+5 left the range at the first device
    // Persistence behind devices-v1, round trip and malformed files.
    const auto bytes=encodeAuthoredProject(project);
    const auto root=QJsonDocument::fromJson(bytes).object();
    CHECK(root["required_features"].toArray().contains("devices-v1") && root["devices"].toObject()["next_id"]=="4");
    const auto entry=root["devices"].toObject()["tracks"].toArray()[0].toObject();
    CHECK(entry["connections"].toArray()[0]==QJsonValue(QJsonArray{"source","1"}) && entry["devices"].toArray()[1].toObject()["kind"]=="gain");
    CHECK(decodeAuthoredProject(bytes).content==project.content);
    const auto broken=[&](auto edit){auto r=root;edit(r);rejects([&]{decodeAuthoredProject(QJsonDocument(r).toJson());});};
    const auto onEntry=[&](auto change){broken([&](QJsonObject& r){auto d=r["devices"].toObject();auto t=d["tracks"].toArray();auto e=t[0].toObject();change(e);t[0]=e;d["tracks"]=t;r["devices"]=d;});};
    onEntry([](QJsonObject& e){auto l=e["devices"].toArray();auto o=l[0].toObject();o["kind"]="reverb";l[0]=o;e["devices"]=l;});
    onEntry([](QJsonObject& e){auto l=e["devices"].toArray();auto o=l[0].toObject();o["extra"]=1;l[0]=o;e["devices"]=l;});
    onEntry([](QJsonObject& e){auto c=e["connections"].toArray();c[0]=QJsonArray{"source","nowhere"};e["connections"]=c;});
    onEntry([](QJsonObject& e){auto c=e["connections"].toArray();c[0]=QJsonArray{"source","1","2"};e["connections"]=c;});
    broken([](QJsonObject& r){auto f=r["required_features"].toArray();f.removeLast();r["required_features"]=f;});
    broken([](QJsonObject& r){auto d=r["devices"].toObject();d["next_id"]="3";r["devices"]=d;});
    // Validation refuses each unsupported graph before any plan exists.
    const auto refused=[&](auto edit){auto c=project.content;edit(c);rejects([&]{validateContent(c,project.identities);});};
    const auto connect=[&](std::vector<DeviceConnection> connections,const char* reason){
        auto c=project.content;c.devices[0].connections=std::move(connections);
        std::string message;try{validateContent(c,project.identities);}catch(const std::exception& e){message=e.what();}
        if(message.find(reason)==std::string::npos)throw std::runtime_error("expected '"+std::string(reason)+"', got '"+message+"'");};
    const DeviceEndpoint source{K::source},instrument{K::instrument},output{K::output},one{K::device,1},two{K::device,2},three{K::device,3};
    connect({{source,two},{two,one},{one,three},{three,instrument},{instrument,output}},"MIDI port to an audio port");
    connect({{source,one},{one,three},{three,instrument},{instrument,two},{two,output},{source,instrument}},"no branches");
    connect({{source,one},{one,three},{three,instrument},{instrument,two},{two,output},{{K::output},two}},"unknown or unusable endpoint"); // out of the output
    connect({{source,instrument},{instrument,two},{two,output},{one,three},{three,one}},"cycle");
    connect({{source,one},{one,three},{three,instrument},{instrument,output}},"loose devices");
    connect({{source,one},{one,three},{three,instrument},{instrument,{K::device,9}},{{K::device,9},output}},"unknown or unusable endpoint");
    connect({{source,one},{one,three},{three,output},{instrument,two},{two,instrument}},"MIDI port to an audio port");
    refused([](SessionContent& c){c.devices[0].devices[0].value=0.5;});refused([](SessionContent& c){c.devices[0].devices[0].value=49;});
    refused([](SessionContent& c){c.devices[0].devices[1].value=13;});refused([](SessionContent& c){c.devices[0].devices[1].value=std::nan("");});
    refused([](SessionContent& c){c.next_device_id=3;});refused([](SessionContent& c){c.devices[0].track=fl_live::TrackId{999};});
    refused([](SessionContent& c){c.devices.push_back(c.devices[0]);});refused([](SessionContent& c){c.devices[0].devices.clear();c.devices[0].connections.clear();});
    refused([&](SessionContent& c){const std::vector<Device> t{{1,DeviceKind::transpose,2}};c.devices[0]={audioTrack,t,chainConnections(t,fl_live::TrackKind::audio)};}); // MIDI device on audio
    refused([](SessionContent& c){std::vector<Device> g;for(std::uint64_t i=1;i<=3;++i)g.push_back({i,DeviceKind::gain,12});c.devices[0].devices=g;
        c.devices[0].connections=chainConnections(g,fl_live::TrackKind::midi);}); // +36 dB combined
    refused([](SessionContent& c){std::vector<Device> g;for(std::uint64_t i=1;i<=9;++i)g.push_back({i,DeviceKind::gain,0});c.next_device_id=10;c.devices[0].devices=g;
        c.devices[0].connections=chainConnections(g,fl_live::TrackKind::midi);}); // nine devices
    // The render plan bounds the compiled gain; the engine applies it with automation.
    auto loud=contentGains(project.content,project.identities);loud[3].device_gain=17;
    rejects([&]{fl_live::DocumentRenderPlan(project.identities,contentSources(project.content,project.identities),48000,120000,{},loud);});
    fl_live::SessionPlan plan{48000,120000,{{1,{fl_live::AudioClip{{0.5F},true}},0,{{0,1.0F},{4,0.5F}},{},0.25F}}};
    fl_live::Engine engine(plan);CHECK(engine.schedule({fl_live::CommandKind::launch,0,0,0})==fl_live::ScheduleResult::accepted);engine.start();
    std::vector<float> out(5);CHECK(engine.process(out,1));
    CHECK(out[0]==0.5F*0.25F && out[4]==0.5F*(0.5F*0.25F));
}
// Device automation lanes (#77): validation, compilation and persistence.
void deviceAutomation() {
    auto project=general();
    const auto midiTrack=project.identities.tracks[3].id;
    const std::vector<Device> chain{{1,DeviceKind::transpose,5},{2,DeviceKind::gain,-6},{3,DeviceKind::gain,3}};
    project.content.devices={{midiTrack,chain,chainConnections(chain,fl_live::TrackKind::midi)}};project.content.next_device_id=4;
    project.content.automation.push_back({midiTrack,AutomatedParameter::device,{{0,-60},{400,0}},2});
    project.content.automation.push_back({midiTrack,AutomatedParameter::device,{{0,0},{400,12}},1});
    validateContent(project.content,project.identities);
    const auto lanes=contentAutomation(project.content,project.identities);
    const auto gainLane=std::find_if(lanes.begin(),lanes.end(),[](const auto& l){return l.target==fl_live::AutomationTarget::device_gain;});
    const auto transposeLane=std::find_if(lanes.begin(),lanes.end(),[](const auto& l){return l.target==fl_live::AutomationTarget::transpose;});
    CHECK(gainLane!=lanes.end() && transposeLane!=lanes.end() && gainLane->track==midiTrack);
    const double others=static_cast<double>(static_cast<float>(std::pow(10.0,3.0/20)));
    CHECK(gainLane->points[0].value==static_cast<float>(others*std::pow(10.0,-60.0/20)) && gainLane->points[1].value==static_cast<float>(others));
    const auto tempo=static_cast<std::uint64_t>(project.content.tempo)*100;
    CHECK(gainLane->points[1].at==(2880000ULL*400+tempo-1)/tempo && transposeLane->points[1].value==12.0F);
    // The automated transpose leaves the static note compile.
    auto plain=project;plain.content.devices.clear();plain.content.automation.clear();
    const auto clip=project.identities.clips[3].id;
    const auto find=[&](const auto& sources){return std::get<fl_live::MidiClip>(std::find_if(sources.begin(),sources.end(),[&](const auto& x){return x.id==clip;})->content);};
    const auto automated=find(contentSources(project.content,project.identities)),untouched=find(contentSources(plain.content,plain.identities));
    CHECK(automated.notes.size()==untouched.notes.size());
    for(std::size_t i=0;i<automated.notes.size();++i)CHECK(automated.notes[i].pitch==untouched.notes[i].pitch && automated.notes[i].start==untouched.notes[i].start);
    const fl_live::DocumentRenderPlan prepared(project.identities,contentSources(project.content,project.identities),48000,120000,{},
        contentGains(project.content,project.identities),lanes);
    CHECK(prepared.plan().tracks[3].device_gain_automation.size()==2 && prepared.plan().tracks[3].transpose_automation.size()==2);
    // Persistence behind device-automation-v1.
    const auto bytes=encodeAuthoredProject(project);
    const auto root=QJsonDocument::fromJson(bytes).object();
    CHECK(root["required_features"].toArray().last()=="device-automation-v1");
    CHECK(root["automation"].toArray()[0].toObject()["device"]=="2" && root["automation"].toArray()[0].toObject()["parameter"]=="device");
    CHECK(decodeAuthoredProject(bytes).content==project.content);
    auto noFeature=root;{auto f=noFeature["required_features"].toArray();f.removeLast();noFeature["required_features"]=f;}
    rejects([&]{decodeAuthoredProject(QJsonDocument(noFeature).toJson());});
    auto levelDevice=root;{auto a=levelDevice["automation"].toArray();auto l=a[0].toObject();l["parameter"]="level";a[0]=l;levelDevice["automation"]=a;}
    rejects([&]{decodeAuthoredProject(QJsonDocument(levelDevice).toJson());});
    // Refusals.
    const auto refused=[&](auto edit){auto c=project.content;edit(c);rejects([&]{validateContent(c,project.identities);});};
    refused([](SessionContent& c){c.automation[0].device=9;});                                   // no such device
    refused([](SessionContent& c){c.automation.push_back({c.automation[0].track,AutomatedParameter::device,{{0,0}},3});}); // a second automated gain
    refused([](SessionContent& c){c.automation[1].points[1].value=0.5;});                        // whole semitones
    refused([](SessionContent& c){c.automation[1].points[1].value=49;});
    refused([](SessionContent& c){c.automation[0].points[1].value=13;});
    // The combined limit counts the lane against the other gain devices: +1 dB with two others at +12 is +25.
    const auto twoLoud=[](SessionContent& c,double point){c.devices[0].devices[2].value=12;c.devices[0].devices.push_back({4,DeviceKind::gain,12});c.next_device_id=5;
        c.devices[0].connections=chainConnections(c.devices[0].devices,fl_live::TrackKind::midi);c.automation[0].points[1].value=point;};
    refused([&](SessionContent& c){twoLoud(c,1);});
    {auto c=project.content;twoLoud(c,0);validateContent(c,project.identities);} // exactly +24
    refused([](SessionContent& c){c.automation.push_back({c.automation[0].track,AutomatedParameter::level,{{0,50}},2});}); // a device ID on a level lane
    auto ok=project.content;ok.devices[0].devices[2].value=12;ok.automation[0].points[1].value=12;validateContent(ok,project.identities); // exactly +24
}
// Retained fixtures (#56): template versions 1-3 as files, and every shipped
// example, which uses each saved feature between them. Version 4 files
// re-encode to the same bytes; unknown features, versions and fields fail.
void retainedFixtures() {
    const QDir fixtures(QStringLiteral(FL_LIVE_FIXTURES)),examples(QStringLiteral(FL_LIVE_EXAMPLES));
    const auto read=[](const QString& path){QFile f(path);if(!f.open(QIODevice::ReadOnly))throw std::runtime_error("missing fixture "+path.toStdString());return f.readAll();};
    if(qEnvironmentVariableIsSet("FL_LIVE_WRITE_FIXTURES")) { // regenerate deliberately, never in CI
        const QByteArray v1=R"({"format":"fl-live-project","version":1,"template":"pulse-steps-v1","tempo":137,"levels":[35,60],"pitches":[48,55,59,62]})";
        const auto project=decodeAuthoredProject(v1);const auto settings=templateView(project.content,project.identities);
        fl_live::SessionDocument document(project.identities);
        document.renameTrack(project.identities.tracks[0].id,"Drums");document.renameClip(project.identities.clips[1].id,"Melody");
        const auto write=[&](const QString& name,const QByteArray& bytes){QFile f(fixtures.filePath(name));if(!f.open(QIODevice::WriteOnly))throw std::runtime_error("write");f.write(bytes);};
        write("template-v1.json",v1);write("template-v2.json",encodeProjectSettings(settings));write("template-v3.json",v3(settings,document.snapshot()));
    }
    std::vector<AuthoredProject> templates;
    for(const auto* name:{"template-v1.json","template-v2.json","template-v3.json"})templates.push_back(decodeAuthoredProject(read(fixtures.filePath(name))));
    for(const auto& t:templates) {
        const auto view=templateView(t.content,t.identities);
        CHECK(t.content.tempo==137 && view.levels==(std::array<int,2>{35,60}) && view.steps[0].pitch==48 && view.steps[3].pitch==62);
        CHECK(t.content==templates[0].content);
    }
    CHECK(templates[2].identities.tracks[0].name=="Drums" && templates[2].identities.clips[1].name=="Melody" && templates[0].identities==defaultTemplateIdentities());
    const auto files=examples.entryList({"*.json"},QDir::Files);
    QStringList features;
    for(const auto& name:files) {
        const auto bytes=read(examples.filePath(name));
        const auto project=decodeAuthoredProject(bytes);
        CHECK(encodeAuthoredProject(project,examples.path())==bytes); // deterministic, lossless
        for(const auto& f:QJsonDocument::fromJson(bytes).object()["required_features"].toArray())if(!features.contains(f.toString()))features.append(f.toString());
        // Identity references resolve: devices and lanes on existing tracks, lane devices on their track, note IDs below the counter.
        for(const auto& lane:project.content.automation) {
            CHECK(std::any_of(project.identities.tracks.begin(),project.identities.tracks.end(),[&](const auto& t){return t.id==lane.track;}));
            if(lane.parameter==AutomatedParameter::device)
                CHECK(std::any_of(project.content.devices.begin(),project.content.devices.end(),[&](const auto& e){return e.track==lane.track &&
                    std::any_of(e.devices.begin(),e.devices.end(),[&](const auto& d){return d.id==lane.device;});}));
        }
        for(const auto& clip:project.content.clips)if(clip.piano)for(const auto& note:clip.piano->notes)CHECK(note.id<clip.piano->next_id);
        // Unknown required features, versions and fields fail safely.
        const auto broken=[&](auto edit){auto root=QJsonDocument::fromJson(bytes).object();edit(root);rejects([&]{decodeAuthoredProject(QJsonDocument(root).toJson());});};
        broken([](QJsonObject& r){auto f=r["required_features"].toArray();f.append("future-feature-v1");r["required_features"]=f;});
        broken([](QJsonObject& r){r["version"]=5;});
        broken([](QJsonObject& r){r["unexpected"]=true;});
    }
    // Between them the examples exercise every saved feature except imported audio.
    for(const auto* feature:{"general-session-v1","automation-v1","synth-v1","piano-roll-v1","devices-v1","device-automation-v1"})CHECK(features.contains(feature));
}
int main(){try{
    retainedFixtures();
    deviceAutomation();
    deviceGraph();
    pianoLoopTiming();
    movedNoteVoices();
    pianoRoll();
    synthSettings();
    automationLanes();
    relativeAssets();
    legacyMigration();generalRoundTrip();generalRejection();clipRanges();
    std::cout<<"PASS: v1-v3 migration with sample equivalence, v4 general content round trip, sources, clip ranges, piano-roll notes, device chains and rejection\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
