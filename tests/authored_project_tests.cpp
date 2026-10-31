#include "authored_project.hpp"
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
    for(std::size_t t=0;t<2;++t)CHECK(engine.schedule({fl_live::CommandKind::launch,0,t,0})==fl_live::ScheduleResult::accepted);
    std::vector<float> result(100003);
    for(std::size_t i=0;i<result.size();i+=block)engine.process(std::span(result).subspan(i,std::min(block,result.size()-i)));
    return result;
}
int main(){try{
    const QByteArray legacy=R"({"format":"fl-live-project","version":1,"template":"pulse-steps-v1","tempo":137,"levels":[35,60],"pitches":[48,55,59,62]})";
    auto project=decodeAuthoredProject(legacy);
    CHECK(project.settings.tempo==137 && project.settings.steps[0].pitch==48);
    CHECK(project.identities==defaultTemplateIdentities());
    CHECK(decodeAuthoredProject(encodeProjectSettings(project.settings)).identities==project.identities);
    fl_live::SessionDocument document(project.identities);
    document.renameTrack(project.identities.tracks[0].id,"Drums");
    document.renameClip(project.identities.clips[1].id,"Melody");
    project.identities=document.snapshot();
    // Identity strings above 2^53 remain exact through the actual desktop format.
    constexpr std::uint64_t delta=9007199254740992ULL;
    for(auto& t:project.identities.tracks)t.id.value+=delta;
    for(auto& c:project.identities.clips){c.id.value+=delta;c.track.value+=delta;}
    for(auto& s:project.identities.scenes){s.id.value+=delta;for(auto& e:s.entries){e.track.value+=delta;e.clip.value+=delta;}}
    project.identities.next_id+=delta;
    std::reverse(project.identities.clips.begin(),project.identities.clips.end());
    const auto bytes=encodeAuthoredProject(project);
    CHECK(QJsonDocument::fromJson(bytes).object()["version"]==3);
    const auto reopened=decodeAuthoredProject(bytes);
    CHECK(reopened.settings==project.settings && reopened.identities==project.identities);
    rejects([&]{decodeProjectSettings(bytes);}); // old source-only API fails safely on v3
    const fl_live::DocumentRenderPlan mapped(reopened.identities,templateSources(reopened.settings,reopened.identities),48000,
        137000,{},templateGains(reopened.settings,reopened.identities));
    const auto original=prepareProject(project.settings);
    CHECK(render(original,1)==render(mapped.plan(),257));
    QTemporaryDir directory;CHECK(directory.isValid());const auto path=directory.filePath("authored.json");
    writeAuthoredProject(path,project);CHECK(readAuthoredProject(path).identities==project.identities);
    QFile file(path);CHECK(file.open(QIODevice::ReadOnly));const auto saved=file.readAll();file.close();
    auto invalid=project;invalid.settings.tempo=-1;rejects([&]{writeAuthoredProject(path,invalid);});
    CHECK(file.open(QIODevice::ReadOnly));CHECK(file.readAll()==saved);file.close();
    const auto root=QJsonDocument::fromJson(bytes).object();
    const auto bad=[&](auto edit){auto value=root;edit(value);rejects([&]{decodeAuthoredProject(QJsonDocument(value).toJson());});};
    bad([](auto& r){r["version"]=4;});bad([](auto& r){r["version"]=true;});
    bad([](auto& r){r["required_features"]=QJsonArray{"future"};});
    bad([](auto& r){r["extra"]=0;});bad([](auto& r){r.remove("session");});
    bad([](auto& r){auto s=r["settings"].toObject();s["tempo"]=0;r["settings"]=s;});
    bad([](auto& r){auto d=r["session"].toObject();d["next_id"]="1";r["session"]=d;});
    auto unsupported=project;unsupported.identities.tracks[0].kind=fl_live::TrackKind::midi;
    rejects([&]{encodeAuthoredProject(unsupported);});
    rejects([&]{decodeAuthoredProject(QByteArray(4*1024*1024+65537,' '));});
    rejects([&]{decodeAuthoredProject(legacy+QByteArray(65536,' '));});
    std::cout<<"PASS: v1/v2 migration, v3 identity persistence, source equivalence and rejection\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
