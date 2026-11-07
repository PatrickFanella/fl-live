#include "authored_project.hpp"
#include "fl_live/document_io.hpp"
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <algorithm>
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
    // A desktop project keeps one clip per track and at least one scene.
    auto twoClips=project;fl_live::SessionDocument document(twoClips.identities);
    const auto extra=document.createClip(twoClips.identities.tracks[0].id,"Second");twoClips.identities=document.snapshot();
    twoClips.content.clips.push_back({extra});
    rejects([&]{encodeAuthoredProject(twoClips);});
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
int main(){try{
    legacyMigration();generalRoundTrip();generalRejection();clipRanges();
    std::cout<<"PASS: v1-v3 migration with sample equivalence, v4 general content round trip, sources, clip ranges and rejection\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
