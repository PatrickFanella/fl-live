#pragma once
#include "fl_live/engine.hpp"
#include "fl_live/session_document.hpp"

namespace fl_live {
struct DocumentClipSource {
    ClipId id;
    Clip content;
};
struct DocumentTrackGain { TrackId id; float gain; };
// Automatable track parameters. A lane replaces the static gain or pan of
// its track; mute/solo still win (an inaudible track prepares silent).
enum class AutomationTarget { gain, pan };
struct DocumentAutomation {
    TrackId track;
    AutomationTarget target;
    std::vector<AutomationPoint> points; // transport frames, engine units
};
struct PreparationBudget {
    static constexpr std::size_t audio_frame_limit = 1U << 24;
    static constexpr std::size_t midi_note_limit = 65536;
    std::size_t audio_frames = audio_frame_limit;
    std::size_t midi_notes = midi_note_limit;
};

// Prepare on a worker/control owner, never in the callback. Track gain comes
// from the optional bindings (default 1); each track's TrackMix then applies
// pan and the mute/solo policy (an inaudible track prepares with zero gain).
// Owns independent
// copies of metadata and source-rate mono clip content. Must outlive every
// Engine/RenderController constructed from plan(). No live replacement API.
class DocumentRenderPlan final {
public:
    DocumentRenderPlan(DocumentSnapshot document, std::span<const DocumentClipSource> sources,
        std::uint32_t sample_rate = 48000, std::uint32_t bpm_milli = 120000,
        PreparationBudget budget = {}, std::span<const DocumentTrackGain> gains = {},
        std::span<const DocumentAutomation> automation = {});
    DocumentRenderPlan(const DocumentRenderPlan&) = delete;
    DocumentRenderPlan& operator=(const DocumentRenderPlan&) = delete;
    DocumentRenderPlan(DocumentRenderPlan&&) = delete;
    DocumentRenderPlan& operator=(DocumentRenderPlan&&) = delete;
    const SessionPlan& plan() const noexcept { return plan_; }
    const DocumentSnapshot& document() const noexcept { return document_; }
    std::optional<std::size_t> trackIndex(TrackId id) const noexcept;
    std::optional<TrackId> trackId(std::size_t index) const noexcept;
    std::optional<ClipId> clipId(std::size_t track, int clip) const noexcept;
    std::optional<Command> launch(ClipId id, Frame at) const noexcept;
    std::optional<Command> stop(TrackId id, Frame at) const noexcept;
    // Launch fills every track: an unfilled scene slot stops that track. Stop
    // acts on every track. Resolve IDs against this exact immutable plan.
    std::optional<CommandBatch> scene(SceneId id, bool stop, Frame at = 0) const noexcept;
private:
    const DocumentSnapshot document_;
    const SessionPlan plan_;
};
} // namespace fl_live
