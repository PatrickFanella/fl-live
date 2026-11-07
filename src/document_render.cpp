#include "fl_live/document_render.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace fl_live {
namespace {
SessionPlan prepare(const DocumentSnapshot& document, std::span<const DocumentClipSource> sources,
    std::uint32_t rate, std::uint32_t tempo, PreparationBudget budget, std::span<const DocumentTrackGain> gains) {
    SessionDocument::validate(document);
    (void)TempoGrid(rate, tempo);
    if (sources.size() != document.clips.size()) throw std::invalid_argument("Every document clip requires exactly one source");
    if (budget.audio_frames > PreparationBudget::audio_frame_limit || budget.midi_notes > PreparationBudget::midi_note_limit)
        throw std::invalid_argument("Preparation budget exceeds supported limit");
    std::unordered_map<std::uint64_t, float> levels;
    if (!gains.empty() && gains.size() != document.tracks.size()) throw std::invalid_argument("Every track requires one gain binding");
    for (const auto& gain : gains)
        if (!std::isfinite(gain.gain) || gain.gain < 0 || gain.gain > 4 || !levels.emplace(gain.id.value, gain.gain).second)
            throw std::invalid_argument("Invalid or duplicate track gain");
    std::unordered_map<std::uint64_t, const Clip*> content;
    std::size_t audio_frames = 0, midi_notes = 0;
    for (const auto& source : sources) {
        if (!source.id.value || !content.emplace(source.id.value, &source.content).second)
            throw std::invalid_argument("Invalid or duplicate clip source ID");
        if (const auto* audio = std::get_if<AudioClip>(&source.content)) {
            if (audio->samples.size() > budget.audio_frames - audio_frames) throw std::invalid_argument("Audio preparation budget exceeded");
            audio_frames += audio->samples.size();
        } else {
            const auto count = std::get<MidiClip>(source.content).notes.size();
            if (count > Engine::notes_per_clip_capacity || count > budget.midi_notes - midi_notes)
                throw std::invalid_argument("MIDI preparation budget exceeded");
            midi_notes += count;
        }
    }
    // Check all ownership/kind/count references before copying any media.
    for (const auto& track : document.tracks) {
        if (!gains.empty() && !levels.contains(track.id.value)) throw std::invalid_argument("Missing track gain binding");
        std::size_t count = 0;
        for (const auto& clip : document.clips) if (clip.track == track.id) {
            if (++count > Engine::clips_per_track_capacity) throw std::invalid_argument("Track exceeds render clip capacity");
            const auto source = content.find(clip.id.value);
            if (source == content.end()) throw std::invalid_argument("Missing clip content");
            if (std::holds_alternative<AudioClip>(*source->second) != (track.kind == TrackKind::audio))
                throw std::invalid_argument("Clip content does not match track kind");
        }
    }
    SessionPlan plan{rate, tempo, {}};
    plan.tracks.reserve(document.tracks.size());
    for (const auto& track : document.tracks) {
        Track prepared;
        if (!gains.empty()) prepared.gain = levels.at(track.id.value);
        // Mute/solo resolve to zero gain at preparation; pan maps percent to
        // the engine's balance law. Edits rebuild the plan.
        if (!SessionDocument::audible(document, track.id)) prepared.gain = 0;
        prepared.pan = static_cast<float>(track.mix.pan) / static_cast<float>(TrackMix::pan_limit);
        for (const auto& clip : document.clips) if (clip.track == track.id)
            prepared.clips.push_back(*content.at(clip.id.value));
        plan.tracks.push_back(std::move(prepared));
    }
    // Reuse the engine's sample/note/voice validation on the control side.
    const Engine validated(plan);
    return plan;
}
}
DocumentRenderPlan::DocumentRenderPlan(DocumentSnapshot document, std::span<const DocumentClipSource> sources,
    std::uint32_t sample_rate, std::uint32_t bpm_milli, PreparationBudget budget, std::span<const DocumentTrackGain> gains)
    : document_(std::move(document)), plan_(prepare(document_, sources, sample_rate, bpm_milli, budget, gains)) {}

std::optional<std::size_t> DocumentRenderPlan::trackIndex(TrackId id) const noexcept {
    for (std::size_t i = 0; i < document_.tracks.size(); ++i) if (document_.tracks[i].id == id) return i;
    return std::nullopt;
}
std::optional<TrackId> DocumentRenderPlan::trackId(std::size_t index) const noexcept {
    if (index >= document_.tracks.size()) return std::nullopt;
    return document_.tracks[index].id;
}
std::optional<ClipId> DocumentRenderPlan::clipId(std::size_t track, int clip) const noexcept {
    const auto owner = trackId(track);
    if (!owner || clip < 0) return std::nullopt;
    std::size_t index = 0;
    for (const auto& entity : document_.clips) if (entity.track == *owner) {
        if (index++ == static_cast<std::size_t>(clip)) return entity.id;
    }
    return std::nullopt;
}
std::optional<Command> DocumentRenderPlan::launch(ClipId id, Frame at) const noexcept {
    const auto target = std::find_if(document_.clips.begin(), document_.clips.end(), [id](const auto& clip) { return clip.id == id; });
    if (target == document_.clips.end()) return std::nullopt;
    const auto track = trackIndex(target->track);
    if (!track) return std::nullopt;
    const auto index = static_cast<std::size_t>(std::count_if(document_.clips.begin(), target,
        [&](const auto& clip) { return clip.track == target->track; }));
    return Command{CommandKind::launch, at, *track, index};
}
std::optional<Command> DocumentRenderPlan::stop(TrackId id, Frame at) const noexcept {
    const auto track = trackIndex(id);
    if (!track) return std::nullopt;
    return Command{CommandKind::stop_track, at, *track, 0};
}
std::optional<CommandBatch> DocumentRenderPlan::scene(SceneId id, bool stop, Frame at) const noexcept {
    const auto scene = std::find_if(document_.scenes.begin(), document_.scenes.end(), [id](const auto& item) { return item.id == id; });
    if (scene == document_.scenes.end()) return std::nullopt;
    CommandBatch result;
    for (std::size_t index = 0; index < document_.tracks.size(); ++index) {
        const auto track = document_.tracks[index].id;
        const auto entry = std::find_if(scene->entries.begin(), scene->entries.end(), [track](const auto& slot) { return slot.track == track; });
        result.commands[result.count++] = !stop && entry != scene->entries.end()
            ? *launch(entry->clip, at) : Command{CommandKind::stop_track, at, index, 0};
    }
    return result;
}
} // namespace fl_live
