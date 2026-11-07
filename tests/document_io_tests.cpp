#include <QFile>
#include "fl_live/document_io.hpp"
#include "fl_live/document_render.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <algorithm>

using namespace fl_live;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("Failed: " #condition); } while (false)
template<class Action> void rejects(Action action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    CHECK(rejected);
}
QByteArray read(const QString& path) {
    QFile file(path); CHECK(file.open(QIODevice::ReadOnly)); return file.readAll();
}
void write(const QString& path, const QByteArray& bytes) {
    QFile file(path); CHECK(file.open(QIODevice::WriteOnly)); CHECK(file.write(bytes) == bytes.size());
}
DocumentSnapshot fixture() {
    const QByteArray json = R"({"format":"fl-live-session-metadata","version":1,
        "required_features":["session-identities-v1"],"next_id":"18446744073709551615",
        "tracks":[{"id":"9007199254740993","name":"Audio","kind":"audio"}],
        "clips":[{"id":"9007199254740995","track":"9007199254740993","name":"Loop"}],
        "scenes":[{"id":"9007199254740997","name":"Intro","slots":[
            {"track":"9007199254740993","clip":"9007199254740995"}]}]})";
    const auto state = decodeDocumentMetadata(json);
    CHECK(state.next_id == std::numeric_limits<std::uint64_t>::max());
    CHECK(state.tracks[0].id.value == 9007199254740993ULL);
    CHECK(state.clips[0].id.value == 9007199254740995ULL);
    CHECK(state.scenes[0].entries[0].clip == state.clips[0].id);
    return state;
}
void roundTrips() {
    QTemporaryDir directory; CHECK(directory.isValid());
    const auto state = fixture();
    const auto encoded = encodeDocumentMetadata(state);
    const auto root = QJsonDocument::fromJson(encoded).object();
    CHECK(root["next_id"].isString());
    CHECK(root["next_id"].toString() == "18446744073709551615");
    CHECK(decodeDocumentMetadata(encoded) == state);
    const auto path = directory.filePath("identities.json");
    writeDocumentMetadata(path, state); CHECK(readDocumentMetadata(path) == state);
    SessionDocument loaded; loadDocumentMetadata(path, loaded); CHECK(loaded.snapshot() == state);
    rejects([&] { loaded.createScene("Exhausted"); });
    SessionDocument source;
    const auto track = source.createTrack("Bäss 🎹", TrackKind::midi);
    const auto clip = source.createClip(track, "Notes");
    const auto scene = source.createScene("Verse"); source.setSlot(scene, track, clip);
    source.renameClip(clip, "Renamed");
    writeDocumentMetadata(path, source.snapshot());
    SessionDocument restored(readDocumentMetadata(path));
    CHECK(restored.snapshot() == source.snapshot());
    CHECK(restored.snapshot().clips[0].id == clip);
    const auto copy = restored.duplicateScene(scene);
    CHECK(copy != scene && restored.snapshot().scenes.back().entries[0].clip != clip);
    const auto cursor = restored.snapshot().next_id;
    loadDocumentMetadata(path, restored);
    CHECK(restored.snapshot().next_id == cursor);
    CHECK(restored.snapshot().scenes[0].id == scene);
    CHECK(restored.createScene("After restore").value >= cursor);
    CHECK(decodeDocumentMetadata(encodeDocumentMetadata({})) == DocumentSnapshot{});
}
void persistedIdsRender() {
    QTemporaryDir directory; CHECK(directory.isValid());
    const auto state=fixture(); const auto id=state.clips[0].id;
    const std::vector<DocumentClipSource> sources{{id,AudioClip{{0.125F,-0.25F,0.375F},true}}};
    const DocumentRenderPlan before(state,sources,48000,137125);
    const auto path=directory.filePath("render-identities.json");
    writeDocumentMetadata(path,state);
    const DocumentRenderPlan after(readDocumentMetadata(path),sources,48000,137125);
    const auto render=[&](const DocumentRenderPlan& prepared,std::size_t block) {
        Engine engine(prepared.plan());
        const auto command=prepared.launch(id,17);CHECK(command);
        CHECK(engine.schedule(*command)==ScheduleResult::accepted);
        engine.start();std::vector<float> samples(1027);
        for(std::size_t i=0;i<samples.size();i+=block)
            engine.process(std::span(samples).subspan(i,std::min(block,samples.size()-i)));
        CHECK(prepared.clipId(command->track,engine.track_statuses()[command->track].active_clip)==id);
        return samples;
    };
    const auto original=render(before,1);CHECK(original==render(after,257));
    const float wave[]={0.125F,-0.25F,0.375F};
    for(std::size_t i=0;i<original.size();++i) CHECK(original[i]==(i<17?0:wave[(i-17)%3]));
}
void rejectedInputs() {
    QTemporaryDir directory; CHECK(directory.isValid());
    const auto path = directory.filePath("input.json");
    const auto good = QJsonDocument::fromJson(encodeDocumentMetadata(fixture())).object();
    SessionDocument current;
    current.createTrack("Keep unsaved work", TrackKind::midi);
    const auto before = current.snapshot();
    const auto invalid = [&](const QByteArray& bytes) {
        rejects([&] { decodeDocumentMetadata(bytes); });
        write(path, bytes);
        rejects([&] { loadDocumentMetadata(path, current); });
        CHECK(current.snapshot() == before);
        CHECK(read(path) == bytes);
    };
    const auto mutate = [&](auto edit) { auto root = good; edit(root); invalid(QJsonDocument(root).toJson()); };
    for (const auto& bytes : {QByteArray(), QByteArray("[1]"), QByteArray("{partial"), QByteArray("true"),
                             QByteArray("{\"format\":\"fl-live-project\",\"version\":2}")}) invalid(bytes);
    invalid(QByteArray(document_metadata_byte_limit + 1, ' '));
    auto malformedUtf8 = QJsonDocument(good).toJson();
    malformedUtf8.replace("\"Audio\"", QByteArray("\"\xff\"", 3));
    invalid(malformedUtf8);
    mutate([](auto& r) { r["version"] = 2; });
    mutate([](auto& r) { r["version"] = true; });
    mutate([](auto& r) { r["version"] = "1"; });
    mutate([](auto& r) { r["format"] = "fl-live-project"; });
    mutate([](auto& r) { r["required_features"] = QJsonArray{"future-audio"}; });
    mutate([](auto& r) { r["required_features"] = QJsonArray{}; });
    mutate([](auto& r) { r["required_features"] = QJsonArray{"session-identities-v1", "session-identities-v1"}; });
    mutate([](auto& r) { r["notes"] = QJsonArray{}; });
    mutate([](auto& r) { r.remove("clips"); });
    mutate([](auto& r) { r["tracks"] = QJsonObject{}; });
    mutate([](auto& r) { r["next_id"] = 9007199254740992.0; });
    for (const auto* id : {"0", "01", "+1", "-1", " 1", "1 ", "1e2", "1.5", "18446744073709551616", ""})
        mutate([&](auto& r) { r["next_id"] = id; });
    mutate([](auto& r) { auto a=r["tracks"].toArray();auto t=a[0].toObject();t["id"]="0";a[0]=t;r["tracks"]=a; });
    mutate([](auto& r) { auto a=r["tracks"].toArray();a.append(a[0]);r["tracks"]=a; });
    mutate([](auto& r) { auto a=r["tracks"].toArray();auto t=a[0].toObject();t["kind"]="future";a[0]=t;r["tracks"]=a; });
    mutate([](auto& r) { auto a=r["clips"].toArray();auto c=a[0].toObject();c["track"]="999";a[0]=c;r["clips"]=a; });
    mutate([](auto& r) { auto a=r["clips"].toArray();auto c=a[0].toObject();c["id"]="9007199254740993";a[0]=c;r["clips"]=a; });
    mutate([](auto& r) { auto a=r["scenes"].toArray();auto s=a[0].toObject();auto entries=s["slots"].toArray();entries.append(entries[0]);s["slots"]=entries;a[0]=s;r["scenes"]=a; });
    mutate([](auto& r) { auto a=r["tracks"].toArray();auto t=a[0].toObject();t["extra"]=true;a[0]=t;r["tracks"]=a; });
    mutate([](auto& r) { auto a=r["tracks"].toArray();auto t=a[0].toObject();t["name"]=QString(QChar(0xd800));a[0]=t;r["tracks"]=a; });
    mutate([](auto& r) { auto a=r["tracks"].toArray();auto t=a[0].toObject();t["name"]=QString(129,'x');a[0]=t;r["tracks"]=a; });
    mutate([](auto& r) { auto a=r["tracks"].toArray();while(a.size()<=64)a.append(a[0]);r["tracks"]=a; });
    rejects([&] { loadDocumentMetadata(directory.filePath("missing.json"), current); });
    CHECK(current.snapshot() == before);
}
void mixerRoundTrip() {
    auto state = fixture();
    const auto plain = encodeDocumentMetadata(state);
    CHECK(!plain.contains("track-mixer-v1") && !plain.contains("\"mix\""));
    state.tracks[0].mix = TrackMix{-100, true, true};
    const auto encoded = encodeDocumentMetadata(state);
    const auto root = QJsonDocument::fromJson(encoded).object();
    CHECK((root["required_features"].toArray() == QJsonArray{"session-identities-v1", "track-mixer-v1"}));
    CHECK(decodeDocumentMetadata(encoded) == state);
    state.tracks[0].mix = TrackMix{37, false, false};
    CHECK(decodeDocumentMetadata(encodeDocumentMetadata(state)) == state);
    // Feature and fields must agree; values must be in range and typed.
    const auto good = QJsonDocument::fromJson(encodeDocumentMetadata(state)).object();
    const auto bad = [&](auto edit) { auto r = good; edit(r); rejects([&] { decodeDocumentMetadata(QJsonDocument(r).toJson()); }); };
    const auto mix = [&](const char* key, QJsonValue value) {
        bad([&](auto& r) { auto a=r["tracks"].toArray();auto t=a[0].toObject();auto m=t["mix"].toObject();m[key]=value;t["mix"]=m;a[0]=t;r["tracks"]=a; });
    };
    bad([](auto& r) { r["required_features"] = QJsonArray{"session-identities-v1"}; });
    bad([](auto& r) { r["required_features"] = QJsonArray{"track-mixer-v1", "session-identities-v1"}; });
    bad([](auto& r) { auto a=r["tracks"].toArray();auto t=a[0].toObject();t.remove("mix");a[0]=t;r["tracks"]=a; });
    mix("pan", 101); mix("pan", -101); mix("pan", 1.5); mix("pan", "0"); mix("mute", 1); mix("solo", QJsonValue());
    bad([](auto& r) { auto a=r["tracks"].toArray();auto t=a[0].toObject();auto m=t["mix"].toObject();m["gain"]=1;t["mix"]=m;a[0]=t;r["tracks"]=a; });
    auto identityOnly = QJsonDocument::fromJson(plain).object();
    auto tracks = identityOnly["tracks"].toArray(); auto track = tracks[0].toObject();
    track["mix"] = QJsonObject{{"pan", 0}, {"mute", false}, {"solo", false}}; tracks[0] = track; identityOnly["tracks"] = tracks;
    rejects([&] { decodeDocumentMetadata(QJsonDocument(identityOnly).toJson()); });
}
void savePreservationAndMaximum() {
    QTemporaryDir directory; CHECK(directory.isValid());
    const auto path = directory.filePath("saved.json");
    const auto state = fixture(); writeDocumentMetadata(path, state);
    const auto original = read(path);
    auto invalid = state; invalid.next_id = 0;
    rejects([&] { writeDocumentMetadata(path, invalid); }); CHECK(read(path) == original);
    invalid = state; invalid.tracks[0].name = std::string("\xff", 1);
    rejects([&] { writeDocumentMetadata(path, invalid); }); CHECK(read(path) == original);
    rejects([&] { writeDocumentMetadata(directory.filePath("missing/child.json"), state); });
    CHECK(read(path) == original);
    DocumentSnapshot maximum;
    maximum.next_id = std::numeric_limits<std::uint64_t>::max() - 10000;
    const std::string name(128, '"'); // worst-case JSON escaping with full-width IDs
    for (std::size_t t=0;t<64;++t) {
        const auto track = TrackId{maximum.next_id++};
        maximum.tracks.push_back({track,name,TrackKind::audio,TrackMix{-100,true,true}});
        for (std::size_t c=0;c<64;++c) maximum.clips.push_back({ClipId{maximum.next_id++},track,name});
    }
    for (std::size_t s=0;s<256;++s) {
        DocumentScene scene{SceneId{maximum.next_id++},name,{}};
        for(std::size_t t=0;t<64;++t) scene.entries.push_back({maximum.tracks[t].id,maximum.clips[t*64].id});
        maximum.scenes.push_back(std::move(scene));
    }
    const auto bytes=encodeDocumentMetadata(maximum);
    CHECK(bytes.size() <= document_metadata_byte_limit);
    CHECK(decodeDocumentMetadata(bytes) == maximum);
}
int main() {
    try {
        roundTrips(); persistedIdsRender(); rejectedInputs(); mixerRoundTrip(); savePreservationAndMaximum();
        std::cout << "PASS: metadata file identity round trips, strict admission and preservation\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
