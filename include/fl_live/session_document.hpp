#pragma once
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fl_live {
template<class Tag> struct DocumentId {
    std::uint64_t value = 0;
    auto operator<=>(const DocumentId&) const = default;
};
using TrackId = DocumentId<struct TrackIdTag>;
using ClipId = DocumentId<struct ClipIdTag>;
using SceneId = DocumentId<struct SceneIdTag>;
enum class TrackKind { audio, midi };
// Mixer switches and pan, in whole percent so files round-trip exactly.
// Pan -100..100 maps to the engine's balance law. A track is audible when it
// is not muted and either no track is soloed or it is soloed itself; mute
// wins over solo. Gain stays with the musical content owner.
struct TrackMix {
    static constexpr int pan_limit = 100;
    int pan = 0;
    bool mute = false;
    bool solo = false;
    bool operator==(const TrackMix&) const = default;
};
struct DocumentTrack {
    TrackId id;
    std::string name;
    TrackKind kind;
    TrackMix mix{};
    bool operator==(const DocumentTrack&) const = default;
};
// Identity/ownership metadata only. Media and note content are separate work.
struct DocumentClip {
    ClipId id;
    TrackId track;
    std::string name;
    bool operator==(const DocumentClip&) const = default;
};
struct SceneSlot {
    TrackId track;
    ClipId clip;
    bool operator==(const SceneSlot&) const = default;
};
struct DocumentScene {
    SceneId id;
    std::string name;
    std::vector<SceneSlot> entries;
    bool operator==(const DocumentScene&) const = default;
};
struct DocumentSnapshot {
    std::uint64_t next_id = 1;
    std::vector<DocumentTrack> tracks;
    std::vector<DocumentClip> clips;
    std::vector<DocumentScene> scenes;
    bool operator==(const DocumentSnapshot&) const = default;
};

// Single control/worker owner, never the render callback. Operations prepare a
// candidate and commit only after validation; they can allocate and throw.
// IDs are document-local and independent of vector positions/backend handles.
class SessionDocument {
public:
    static constexpr std::size_t track_limit = 64, scene_limit = 256;
    static constexpr std::size_t clip_limit = 4096, clips_per_track_limit = 256;
    static constexpr std::size_t name_byte_limit = 128;
    explicit SessionDocument(DocumentSnapshot snapshot = {});
    const DocumentSnapshot& snapshot() const noexcept { return state_; }
    static void validate(const DocumentSnapshot& snapshot);
    void replace(DocumentSnapshot snapshot);
    TrackId createTrack(std::string name, TrackKind kind);
    ClipId createClip(TrackId track, std::string name);
    SceneId createScene(std::string name);
    void renameTrack(TrackId id, std::string name);
    void renameClip(ClipId id, std::string name);
    void renameScene(SceneId id, std::string name);
    TrackId duplicateTrack(TrackId id);
    ClipId duplicateClip(ClipId id);
    SceneId duplicateScene(SceneId id);
    void deleteTrack(TrackId id);
    void deleteClip(ClipId id);
    void deleteScene(SceneId id);
    void setTrackMix(TrackId id, TrackMix mix);
    // Whether a track contributes to the mix under the mute/solo policy.
    static bool audible(const DocumentSnapshot& snapshot, TrackId id);
    void setSlot(SceneId scene, TrackId track, ClipId clip);
    void clearSlot(SceneId scene, TrackId track);
private:
    DocumentSnapshot state_;
    template<class Edit> void mutate(Edit edit) {
        auto candidate = state_;
        edit(candidate);
        validate(candidate);
        using std::swap;
        swap(state_, candidate);
    }
};
} // namespace fl_live
