#include "fl_live/document_io.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <QFileInfo>
#include <fcntl.h>
#include <unistd.h>
#include <charconv>
#include <cmath>
#include <stdexcept>

namespace fl_live {
namespace {
QJsonObject fields(const QJsonValue& value, std::initializer_list<const char*> names) {
    if (!value.isObject()) throw std::invalid_argument("Metadata requires an object");
    const auto object = value.toObject();
    if (object.size() != static_cast<qsizetype>(names.size())) throw std::invalid_argument("Unsupported metadata fields");
    for (const auto* name : names) if (!object.contains(QLatin1String(name)))
        throw std::invalid_argument("Missing metadata field");
    return object;
}
QJsonArray array(const QJsonValue& value, std::size_t limit) {
    if (!value.isArray() || value.toArray().size() > static_cast<qsizetype>(limit))
        throw std::invalid_argument("Invalid or oversized metadata array");
    return value.toArray();
}
QString identity(std::uint64_t id) { return QString::fromStdString(std::to_string(id)); }
std::uint64_t identity(const QJsonValue& value) {
    if (!value.isString()) throw std::invalid_argument("Metadata IDs require decimal strings");
    const auto bytes = value.toString().toLatin1();
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(bytes.constData(), bytes.constData() + bytes.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != bytes.constData() + bytes.size() || identity(result) != value.toString())
        throw std::invalid_argument("Invalid or noncanonical metadata ID");
    return result;
}
QString name(const std::string& bytes) {
    const auto text = QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size()));
    if (text.toUtf8().toStdString() != bytes) throw std::invalid_argument("Metadata names require valid UTF-8");
    return text;
}
std::string name(const QJsonValue& value) {
    if (!value.isString()) throw std::invalid_argument("Metadata names require strings");
    const auto text = value.toString();
    const auto bytes = text.toUtf8();
    if (QString::fromUtf8(bytes) != text) throw std::invalid_argument("Invalid Unicode metadata name");
    return bytes.toStdString();
}
}
QByteArray encodeDocumentMetadata(const DocumentSnapshot& state) {
    SessionDocument::validate(state);
    QJsonArray tracks, clips, scenes;
    // Mixer fields are written only when used, so identity-only files keep
    // their original bytes and readers that predate the mixer still open them.
    const bool mixer = std::any_of(state.tracks.begin(), state.tracks.end(), [](const auto& t) { return t.mix != TrackMix{}; });
    for (const auto& track : state.tracks) {
        QJsonObject object{{"id", identity(track.id.value)}, {"name", name(track.name)},
            {"kind", track.kind == TrackKind::audio ? "audio" : "midi"}};
        if (mixer) object.insert("mix", QJsonObject{{"pan", track.mix.pan}, {"mute", track.mix.mute}, {"solo", track.mix.solo}});
        tracks.append(object);
    }
    for (const auto& clip : state.clips)
        clips.append(QJsonObject{{"id", identity(clip.id.value)}, {"track", identity(clip.track.value)}, {"name", name(clip.name)}});
    for (const auto& scene : state.scenes) {
        QJsonArray entries;
        for (const auto& slot : scene.entries)
            entries.append(QJsonObject{{"track", identity(slot.track.value)}, {"clip", identity(slot.clip.value)}});
        scenes.append(QJsonObject{{"id", identity(scene.id.value)}, {"name", name(scene.name)}, {"slots", entries}});
    }
    const auto bytes = QJsonDocument(QJsonObject{{"format", "fl-live-session-metadata"}, {"version", 1},
        {"required_features", mixer ? QJsonArray{"session-identities-v1", "track-mixer-v1"} : QJsonArray{"session-identities-v1"}},
        {"next_id", identity(state.next_id)},
        {"tracks", tracks}, {"clips", clips}, {"scenes", scenes}}).toJson(QJsonDocument::Compact);
    if (bytes.size() > document_metadata_byte_limit) throw std::invalid_argument("Metadata exceeds the 4 MiB limit");
    return bytes;
}
DocumentSnapshot decodeDocumentMetadata(const QByteArray& bytes) {
    if (bytes.size() > document_metadata_byte_limit) throw std::invalid_argument("Metadata exceeds the 4 MiB limit");
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !json.isObject()) throw std::invalid_argument("Invalid metadata JSON object");
    const auto root = fields(json.object(), {"format", "version", "required_features", "next_id", "tracks", "clips", "scenes"});
    if (root["format"] != "fl-live-session-metadata" || !root["version"].isDouble() || root["version"].toDouble() != 1)
        throw std::invalid_argument("Unsupported metadata format/version");
    const auto features = array(root["required_features"], 2);
    const bool mixer = features == QJsonArray{"session-identities-v1", "track-mixer-v1"};
    if (!mixer && features != QJsonArray{"session-identities-v1"}) throw std::invalid_argument("Unsupported required metadata feature");
    DocumentSnapshot state;
    state.next_id = identity(root["next_id"]);
    for (const auto& value : array(root["tracks"], SessionDocument::track_limit)) {
        const auto track = mixer ? fields(value, {"id", "name", "kind", "mix"}) : fields(value, {"id", "name", "kind"});
        const auto kind = track["kind"];
        if (kind != "audio" && kind != "midi") throw std::invalid_argument("Unsupported metadata track kind");
        TrackMix mix;
        if (mixer) {
            const auto values = fields(track["mix"], {"pan", "mute", "solo"});
            const auto pan = values["pan"].toDouble();
            if (!values["pan"].isDouble() || pan != std::floor(pan) || pan < -TrackMix::pan_limit || pan > TrackMix::pan_limit ||
                !values["mute"].isBool() || !values["solo"].isBool())
                throw std::invalid_argument("Invalid metadata track mix");
            mix = {static_cast<int>(pan), values["mute"].toBool(), values["solo"].toBool()};
        }
        state.tracks.push_back({TrackId{identity(track["id"])}, name(track["name"]), kind == "audio" ? TrackKind::audio : TrackKind::midi, mix});
    }
    for (const auto& value : array(root["clips"], SessionDocument::clip_limit)) {
        const auto clip = fields(value, {"id", "track", "name"});
        state.clips.push_back({ClipId{identity(clip["id"])}, TrackId{identity(clip["track"])}, name(clip["name"])});
    }
    for (const auto& value : array(root["scenes"], SessionDocument::scene_limit)) {
        const auto scene = fields(value, {"id", "name", "slots"});
        DocumentScene decoded{SceneId{identity(scene["id"])}, name(scene["name"]), {}};
        for (const auto& entry : array(scene["slots"], SessionDocument::track_limit)) {
            const auto slot = fields(entry, {"track", "clip"});
            decoded.entries.push_back({TrackId{identity(slot["track"])}, ClipId{identity(slot["clip"])}});
        }
        state.scenes.push_back(std::move(decoded));
    }
    SessionDocument::validate(state);
    return state;
}
DocumentSnapshot readDocumentMetadata(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot open metadata file");
    if (file.size() > document_metadata_byte_limit) throw std::invalid_argument("Metadata exceeds the 4 MiB limit");
    const auto bytes = file.read(document_metadata_byte_limit + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > document_metadata_byte_limit)
        throw std::runtime_error("Metadata read failed or exceeded size limit");
    return decodeDocumentMetadata(bytes);
}
void writeDocumentMetadata(const QString& path, const DocumentSnapshot& state) {
    const auto bytes = encodeDocumentMetadata(state);
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || !commitDurably(file, bytes))
        throw std::runtime_error("Cannot save metadata atomically; previous file was preserved");
}
bool commitDurably(QSaveFile& file, const QByteArray& bytes) {
    if (file.write(bytes) != bytes.size() || !file.flush() || ::fdatasync(file.handle()) != 0) {
        file.cancelWriting();
        file.commit(); // discards the temporary; the destination is untouched
        return false;
    }
    const auto directory = QFileInfo(file.fileName()).absolutePath().toLocal8Bit();
    if (!file.commit()) return false;
    // Make the rename itself durable. A failure here leaves the new, complete
    // file in place but is still reported so the caller does not mark it saved.
    const int fd = ::open(directory.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    return synced;
}
void loadDocumentMetadata(const QString& path, SessionDocument& document) {
    auto candidate = readDocumentMetadata(path);
    document.replace(std::move(candidate));
}
} // namespace fl_live
