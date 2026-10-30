#include "fl_live/session_document.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace fl_live {
namespace {
template<class Entities, class Id> auto& find(Entities& entities, Id id) {
    const auto it = std::find_if(entities.begin(), entities.end(), [id](const auto& entity) { return entity.id == id; });
    if (it == entities.end()) throw std::invalid_argument("Unknown document entity");
    return *it;
}
template<class Id> Id allocate(DocumentSnapshot& state) {
    if (state.next_id == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Document identity space exhausted");
    return Id{state.next_id++};
}
void nameValid(const std::string& name) {
    if (name.empty() || name.size() > SessionDocument::name_byte_limit ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw std::invalid_argument("Entity name must be 1..128 bytes without control characters");
}
}

SessionDocument::SessionDocument(DocumentSnapshot snapshot) : state_(std::move(snapshot)) { validate(state_); }
void SessionDocument::validate(const DocumentSnapshot& state) {
    if (!state.next_id || state.tracks.size() > track_limit || state.clips.size() > clip_limit || state.scenes.size() > scene_limit)
        throw std::invalid_argument("Document limits exceeded or invalid identity cursor");
    std::unordered_set<std::uint64_t> ids;
    const auto identity = [&](auto id) {
        if (!id.value || id.value >= state.next_id || !ids.insert(id.value).second)
            throw std::invalid_argument("Zero, duplicate or unallocated document identity");
    };
    std::unordered_map<std::uint64_t, std::size_t> tracks;
    std::unordered_map<std::uint64_t, TrackId> clips;
    for (const auto& track : state.tracks) {
        identity(track.id); nameValid(track.name);
        if (track.kind != TrackKind::audio && track.kind != TrackKind::midi)
            throw std::invalid_argument("Unknown track kind");
        tracks.emplace(track.id.value, 0);
    }
    for (const auto& clip : state.clips) {
        identity(clip.id); nameValid(clip.name);
        const auto owner = tracks.find(clip.track.value);
        if (owner == tracks.end() || ++owner->second > clips_per_track_limit)
            throw std::invalid_argument("Missing clip owner or track clip limit exceeded");
        clips.emplace(clip.id.value, clip.track);
    }
    for (const auto& scene : state.scenes) {
        identity(scene.id); nameValid(scene.name);
        if (scene.entries.size() > track_limit) throw std::invalid_argument("Scene slot limit exceeded");
        std::unordered_set<std::uint64_t> entries;
        for (const auto& slot : scene.entries) {
            const auto clip = clips.find(slot.clip.value);
            if (clip == clips.end() || clip->second != slot.track || !entries.insert(slot.track.value).second)
                throw std::invalid_argument("Invalid or duplicate scene slot");
        }
    }
}
void SessionDocument::replace(DocumentSnapshot snapshot) {
    validate(snapshot);
    // Restoring an older snapshot must not reuse identities allocated later in
    // this document's lifetime. File persistence must retain this high-water mark.
    snapshot.next_id = std::max(snapshot.next_id, state_.next_id);
    using std::swap;
    swap(state_, snapshot);
}
TrackId SessionDocument::createTrack(std::string name, TrackKind kind) {
    TrackId result;
    mutate([&](auto& state) { result = allocate<TrackId>(state); state.tracks.push_back({result, std::move(name), kind}); });
    return result;
}
ClipId SessionDocument::createClip(TrackId track, std::string name) {
    ClipId result;
    mutate([&](auto& state) { result = allocate<ClipId>(state); state.clips.push_back({result, track, std::move(name)}); });
    return result;
}
SceneId SessionDocument::createScene(std::string name) {
    SceneId result;
    mutate([&](auto& state) { result = allocate<SceneId>(state); state.scenes.push_back({result, std::move(name), {}}); });
    return result;
}
void SessionDocument::renameTrack(TrackId id, std::string name) {
    mutate([&](auto& state) { find(state.tracks, id).name = std::move(name); });
}
void SessionDocument::renameClip(ClipId id, std::string name) {
    mutate([&](auto& state) { find(state.clips, id).name = std::move(name); });
}
void SessionDocument::renameScene(SceneId id, std::string name) {
    mutate([&](auto& state) { find(state.scenes, id).name = std::move(name); });
}
void SessionDocument::setSlot(SceneId scene, TrackId track, ClipId clip) {
    mutate([&](auto& state) {
        auto& entries = find(state.scenes, scene).entries;
        std::erase_if(entries, [track](auto slot) { return slot.track == track; });
        entries.push_back({track, clip});
    });
}
void SessionDocument::clearSlot(SceneId scene, TrackId track) {
    mutate([&](auto& state) {
        find(state.tracks, track);
        auto& entries = find(state.scenes, scene).entries;
        std::erase_if(entries, [track](auto slot) { return slot.track == track; });
    });
}
ClipId SessionDocument::duplicateClip(ClipId id) {
    ClipId result;
    mutate([&](auto& state) {
        auto clip = find(state.clips, id);
        result = clip.id = allocate<ClipId>(state);
        state.clips.push_back(std::move(clip));
    });
    return result;
}
TrackId SessionDocument::duplicateTrack(TrackId id) {
    TrackId result;
    mutate([&](auto& state) {
        auto track = find(state.tracks, id);
        result = track.id = allocate<TrackId>(state);
        state.tracks.push_back(std::move(track));
        std::unordered_map<std::uint64_t, ClipId> copies;
        const auto count = state.clips.size();
        for (std::size_t i = 0; i < count; ++i) if (state.clips[i].track == id) {
            auto clip = state.clips[i];
            const auto old = clip.id;
            clip.id = allocate<ClipId>(state); clip.track = result;
            copies.emplace(old.value, clip.id);
            state.clips.push_back(std::move(clip));
        }
        for (auto& scene : state.scenes) {
            const auto slot = std::find_if(scene.entries.begin(), scene.entries.end(), [id](auto entry) { return entry.track == id; });
            if (slot != scene.entries.end()) {
                const auto copy = copies.at(slot->clip.value);
                scene.entries.push_back({result, copy});
            }
        }
    });
    return result;
}
SceneId SessionDocument::duplicateScene(SceneId id) {
    SceneId result;
    mutate([&](auto& state) {
        auto scene = find(state.scenes, id);
        result = scene.id = allocate<SceneId>(state);
        for (auto& slot : scene.entries) {
            auto clip = find(state.clips, slot.clip);
            slot.clip = clip.id = allocate<ClipId>(state);
            state.clips.push_back(std::move(clip));
        }
        state.scenes.push_back(std::move(scene));
    });
    return result;
}
void SessionDocument::deleteClip(ClipId id) {
    mutate([&](auto& state) {
        find(state.clips, id);
        std::erase_if(state.clips, [id](const auto& clip) { return clip.id == id; });
        for (auto& scene : state.scenes) std::erase_if(scene.entries, [id](auto slot) { return slot.clip == id; });
    });
}
void SessionDocument::deleteTrack(TrackId id) {
    mutate([&](auto& state) {
        find(state.tracks, id);
        std::erase_if(state.tracks, [id](const auto& track) { return track.id == id; });
        std::erase_if(state.clips, [id](const auto& clip) { return clip.track == id; });
        for (auto& scene : state.scenes) std::erase_if(scene.entries, [id](auto slot) { return slot.track == id; });
    });
}
void SessionDocument::deleteScene(SceneId id) {
    mutate([&](auto& state) {
        find(state.scenes, id);
        std::erase_if(state.scenes, [id](const auto& scene) { return scene.id == id; });
    });
}
} // namespace fl_live
