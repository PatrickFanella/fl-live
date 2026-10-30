#include "fl_live/session_document.hpp"
#include "fl_live/engine.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

using namespace fl_live;
static_assert(!std::is_convertible_v<ClipId, TrackId>);
static_assert(!std::is_convertible_v<SceneId, ClipId>);
static_assert(SessionDocument::track_limit == Engine::track_capacity);
static_assert(std::is_nothrow_swappable_v<DocumentSnapshot>);
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("Failed: " #condition); } while (false)

template<class Action> void rejected(SessionDocument& document, Action action) {
    const auto before = document.snapshot();
    bool failed = false;
    try { action(); }
    catch (const std::invalid_argument&) { failed = true; }
    catch (const std::overflow_error&) { failed = true; }
    CHECK(failed);
    CHECK(document.snapshot() == before);
}
void identitiesAndCopies() {
    SessionDocument document;
    const auto audio = document.createTrack("Audio", TrackKind::audio);
    const auto midi = document.createTrack("MIDI", TrackKind::midi);
    const auto clip = document.createClip(audio, "Loop");
    const auto note = document.createClip(midi, "Notes");
    const auto scene = document.createScene("Verse");
    document.setSlot(scene, audio, clip); document.setSlot(scene, midi, note);
    document.renameTrack(audio, "Drums"); document.renameClip(clip, "Beat"); document.renameScene(scene, "Intro");
    CHECK(document.snapshot().tracks[0].id == audio);
    CHECK(document.snapshot().clips[0].id == clip);
    CHECK(document.snapshot().scenes[0].id == scene);
    CHECK(document.snapshot().scenes[0].slots[0] == (SceneSlot{audio, clip}));
    const auto saved = document.snapshot();
    SessionDocument restored(saved);
    CHECK(restored.snapshot() == saved); // validated snapshot boundary, not a file codec
    const auto duplicate = document.duplicateClip(clip);
    CHECK(duplicate != clip);
    CHECK(document.snapshot().clips.back().track == audio);
    CHECK(document.snapshot().clips.back().name == "Beat");
    CHECK(document.snapshot().scenes[0].slots[0].clip == clip); // standalone copy is unassigned
    const auto trackCopy = document.duplicateTrack(audio);
    CHECK(trackCopy != audio);
    CHECK(document.snapshot().tracks.back().name == "Drums");
    CHECK(document.snapshot().scenes[0].slots.size() == 3);
    const auto newSlot = document.snapshot().scenes[0].slots.back();
    CHECK(newSlot.track == trackCopy && newSlot.clip != clip && newSlot.clip != duplicate);
    const auto sceneCopy = document.duplicateScene(scene);
    CHECK(sceneCopy != scene);
    const auto copied = document.snapshot().scenes.back();
    CHECK(copied.name == "Intro" && copied.slots.size() == 3);
    for (std::size_t i = 0; i < copied.slots.size(); ++i) {
        CHECK(copied.slots[i].track == document.snapshot().scenes[0].slots[i].track);
        CHECK(copied.slots[i].clip != document.snapshot().scenes[0].slots[i].clip);
    }
    const auto highWater = document.snapshot().next_id;
    document.replace(saved);
    CHECK(document.snapshot().next_id == highWater);
    const auto later = document.createScene("Later");
    CHECK(later.value >= highWater); // restoring old content cannot recycle recent IDs
    document.deleteScene(later);
    CHECK(document.createScene("Another").value > later.value);
}
void invalidEditsAndDeletion() {
    SessionDocument document;
    const auto a = document.createTrack("A", TrackKind::audio);
    const auto b = document.createTrack("B", TrackKind::midi);
    const auto clip = document.createClip(a, "Clip");
    const auto scene = document.createScene("Scene");
    document.setSlot(scene, a, clip);
    rejected(document, [&] { document.setSlot(scene, b, clip); });
    rejected(document, [&] { document.setSlot(scene, a, ClipId{999}); });
    rejected(document, [&] { document.createClip(TrackId{999}, "Missing"); });
    rejected(document, [&] { document.createTrack("Bad", static_cast<TrackKind>(99)); });
    rejected(document, [&] { document.renameTrack(a, ""); });
    rejected(document, [&] { document.renameClip(clip, "bad\nname"); });
    rejected(document, [&] { document.renameScene(scene, std::string(129, 'x')); });
    rejected(document, [&] { document.deleteTrack(TrackId{999}); });
    rejected(document, [&] { document.deleteClip(ClipId{999}); });
    rejected(document, [&] { document.deleteScene(SceneId{999}); });
    rejected(document, [&] { document.duplicateTrack(TrackId{999}); });
    rejected(document, [&] { document.duplicateClip(ClipId{999}); });
    rejected(document, [&] { document.duplicateScene(SceneId{999}); });
    rejected(document, [&] { document.clearSlot(scene, TrackId{999}); });
    document.clearSlot(scene, a); CHECK(document.snapshot().scenes[0].slots.empty());
    document.setSlot(scene, a, clip);
    const auto replacement = document.createClip(a, "Replacement");
    document.setSlot(scene, a, replacement);
    CHECK(document.snapshot().scenes[0].slots.size() == 1);
    CHECK(document.snapshot().scenes[0].slots[0].clip == replacement);
    document.deleteClip(clip); // unassigned deletion preserves the other slot
    CHECK(document.snapshot().scenes[0].slots.size() == 1);
    document.deleteClip(replacement); CHECK(document.snapshot().scenes[0].slots.empty());
    const auto remaining = document.createClip(a, "Remaining");
    document.setSlot(scene, a, remaining);
    document.deleteTrack(a);
    CHECK(document.snapshot().clips.empty() && document.snapshot().scenes[0].slots.empty());
    CHECK(document.snapshot().tracks.size() == 1 && document.snapshot().tracks[0].id == b);
    document.deleteScene(scene); CHECK(document.snapshot().scenes.empty());
}
void invalidSnapshots() {
    SessionDocument document;
    const auto track = document.createTrack("Track", TrackKind::audio);
    const auto clip = document.createClip(track, "Clip");
    const auto scene = document.createScene("Scene"); document.setSlot(scene, track, clip);
    const auto valid = document.snapshot();
    const auto bad = [&](auto edit) {
        auto candidate = valid; edit(candidate);
        rejected(document, [&] { document.replace(candidate); });
        bool constructorRejected = false;
        try { SessionDocument invalid(candidate); }
        catch (const std::invalid_argument&) { constructorRejected = true; }
        CHECK(constructorRejected);
    };
    bad([](auto& s) { s.next_id = 0; });
    bad([](auto& s) { s.next_id = s.scenes[0].id.value; });
    bad([](auto& s) { s.tracks[0].id.value = 0; });
    bad([](auto& s) { s.clips[0].id.value = s.tracks[0].id.value; });
    bad([](auto& s) { s.tracks.push_back(s.tracks[0]); });
    bad([](auto& s) { s.clips[0].track.value = 999; });
    bad([](auto& s) { s.scenes[0].slots[0].clip.value = 999; });
    bad([](auto& s) { s.scenes[0].slots[0].track.value = 999; });
    bad([](auto& s) { s.scenes[0].slots.push_back(s.scenes[0].slots[0]); });
    bad([](auto& s) { s.scenes[0].slots.resize(SessionDocument::track_limit + 1, s.scenes[0].slots[0]); });
    bad([](auto& s) { s.clips[0].name = std::string("bad\0name", 8); });
    auto almostExhausted = valid; almostExhausted.next_id = std::numeric_limits<std::uint64_t>::max() - 1;
    document.replace(almostExhausted);
    rejected(document, [&] { document.duplicateScene(scene); }); // fails after candidate scene ID allocation
    auto exhausted = valid; exhausted.next_id = std::numeric_limits<std::uint64_t>::max();
    document.replace(exhausted);
    rejected(document, [&] { document.createTrack("Overflow", TrackKind::audio); });
    rejected(document, [&] { document.duplicateTrack(track); });
}
void limits() {
    DocumentSnapshot state;
    for (std::size_t i = 0; i < SessionDocument::track_limit; ++i)
        state.tracks.push_back({TrackId{state.next_id++}, "Track", TrackKind::audio});
    for (std::size_t i = 0; i < SessionDocument::scene_limit; ++i)
        state.scenes.push_back({SceneId{state.next_id++}, "Scene", {}});
    SessionDocument full(state);
    rejected(full, [&] { full.createTrack("Extra", TrackKind::audio); });
    rejected(full, [&] { full.duplicateTrack(state.tracks[0].id); });
    rejected(full, [&] { full.createScene("Extra"); });
    rejected(full, [&] { full.duplicateScene(state.scenes[0].id); });
    state = {};
    for (std::size_t t = 0; t < 16; ++t) {
        const auto id = TrackId{state.next_id++};
        state.tracks.push_back({id, "Track", TrackKind::midi});
        for (std::size_t i = 0; i < SessionDocument::clips_per_track_limit; ++i)
            state.clips.push_back({ClipId{state.next_id++}, id, "Clip"});
    }
    state.scenes.push_back({SceneId{state.next_id++}, "Scene", {{state.tracks[0].id, state.clips[0].id}}});
    SessionDocument clips(state);
    rejected(clips, [&] { clips.createClip(state.tracks[0].id, "Extra"); });
    rejected(clips, [&] { clips.duplicateClip(state.clips[0].id); });
    rejected(clips, [&] { clips.duplicateScene(state.scenes[0].id); });
    rejected(clips, [&] { clips.duplicateTrack(state.tracks[0].id); });
    const auto emptyTrack = clips.createTrack("Empty", TrackKind::audio);
    rejected(clips, [&] { clips.createClip(emptyTrack, "Total limit"); });
    // Check the per-track boundary independently of the total clip ceiling.
    state.tracks.resize(1); state.clips.resize(256);
    SessionDocument perTrack(state);
    rejected(perTrack, [&] { perTrack.createClip(state.tracks[0].id, "Extra"); });
    rejected(perTrack, [&] { perTrack.duplicateScene(state.scenes[0].id); });
    const auto second = perTrack.createTrack("Room", TrackKind::midi);
    CHECK(perTrack.createClip(second, "Allowed").value > 0);
}
int main() {
    try {
        identitiesAndCopies(); invalidEditsAndDeletion(); invalidSnapshots(); limits();
        std::cout << "PASS: document identities, transactional edits, snapshots, references and limits\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
