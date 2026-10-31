#include "authored_project.hpp"
#include "fl_live/document_io.hpp"
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <algorithm>
#include <stdexcept>

namespace {
constexpr qsizetype limit = fl_live::document_metadata_byte_limit + 65536;
fl_live::ClipId clipFor(const fl_live::DocumentSnapshot& identities, fl_live::TrackId track) {
    const auto found=std::find_if(identities.clips.begin(),identities.clips.end(),[track](const auto& clip){return clip.track==track;});
    if(found==identities.clips.end()) throw std::invalid_argument("Missing authored clip");
    return found->id;
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
QByteArray encodeAuthoredProject(const AuthoredProject& project) {
    validateTemplateIdentities(project.identities);
    const auto settings=QJsonDocument::fromJson(encodeProjectSettings(project.settings)).object();
    const auto identities=QJsonDocument::fromJson(fl_live::encodeDocumentMetadata(project.identities)).object();
    QJsonArray features{"template-identities-v1"};
    if(std::any_of(project.identities.scenes.begin(),project.identities.scenes.end(),[](const auto& scene){return scene.entries.size()!=2;}))
        features.append("template-scene-slots-v1");
    if(project.identities.scenes.size()!=1)features.append("template-scenes-v1");
    const auto bytes=QJsonDocument(QJsonObject{{"format","fl-live-project"},{"version",3},
        {"required_features",features}, {"settings",settings},{"session",identities}}).toJson();
    if(bytes.size()>limit)throw std::invalid_argument("Authored project exceeds size limit");
    return bytes;
}
AuthoredProject decodeAuthoredProject(const QByteArray& bytes) {
    if(bytes.size()>limit)throw std::invalid_argument("Authored project exceeds size limit");
    QJsonParseError error;const auto json=QJsonDocument::fromJson(bytes,&error);
    if(error.error!=QJsonParseError::NoError || !json.isObject())throw std::invalid_argument("Project is not a valid JSON object");
    const auto root=json.object();
    if(root["format"]!="fl-live-project")throw std::invalid_argument("Unsupported project format");
    if(root["version"].isDouble() && (root["version"].toDouble()==1 || root["version"].toDouble()==2))
        return {decodeProjectSettings(bytes),defaultTemplateIdentities()};
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
    AuthoredProject project{decodeProjectSettings(QJsonDocument(root["settings"].toObject()).toJson(QJsonDocument::Compact)),
        fl_live::decodeDocumentMetadata(QJsonDocument(root["session"].toObject()).toJson(QJsonDocument::Compact))};
    validateTemplateIdentities(project.identities);
    if(!multipleScenes && project.identities.scenes.size()!=1)
        throw std::invalid_argument("Multiple scenes require template-scenes-v1");
    if(!sceneSlots && std::any_of(project.identities.scenes.begin(),project.identities.scenes.end(),[](const auto& scene){return scene.entries.size()!=2;}))
        throw std::invalid_argument("Unfilled scene slots require template-scene-slots-v1");
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
    const auto bytes=encodeAuthoredProject(project);QSaveFile file(path);file.setDirectWriteFallback(false);
    if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit())
        throw std::runtime_error("Cannot save authored project atomically; previous file was preserved");
}
