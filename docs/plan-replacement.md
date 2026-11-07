# Plan replacement and retirement

This is the engine and controller contract for #53: a prepared plan replaces the one being rendered without stopping the transport, and the old plan is destroyed only after the render owner acknowledges it. Playback edits in the desktop are #54; this page covers the groundwork they build on.

## Preparing a replacement

A `PlanReplacement` shares ownership of a complete `SessionPlan` and holds a `PlanMapping` and an activation frame `at`. An aliasing `shared_ptr` can point at the plan inside a prepared `DocumentRenderPlan` and keep that document alive; the render thread never releases a reference. Build it on a worker or the control thread, never in the callback. The mapping indexes the plan it replaces: `track_from_old[t]` is the new index of old track `t` or `-1`, and `clip_from_old[t][c]` is the new index of that track's clip `c` within the new track, or `-1`. `planMapping(from, to)` builds one from two `DocumentRenderPlan`s by track and clip ID, so a renamed or reordered track keeps its state and a deleted one maps to `-1`.

`RenderController::offer` validates on the calling thread and returns:

- `accepted`: the replacement is handed to the render owner.
- `busy`: an earlier offer has not activated yet, its retirement has not been collected, or the control queue is full. One replacement is in flight at a time. The refused replacement is discarded.
- `invalid`: the engine refuses the plan, the mapping does not cover every old track and clip, two old tracks map to one new track, an index is out of range, or the sample rate or tempo differs. Nothing changes.

The mapping is checked against the latest accepted plan, so offers apply in the order they were accepted.

## When it activates

`offer` publishes the plan and then queues an internal marker behind the controls already submitted, so a replacement activates in control order. Controls submitted before the offer use the old plan's track and clip indexes; controls submitted after it use the new plan's. Indexes therefore never need a version tag.

When the render owner reaches the marker:

- While the transport runs, the replacement activates exactly before transport frame `at`. The controller splits the block at that frame, so output is the same for every block size. When `at` has already passed it activates immediately. Controls behind the marker wait until it has activated and apply from the following block. A far-future `at` therefore also delays later controls, including Stop; panic is not delayed.
- While stopped, it activates at once.
- Panic activates a held or queued replacement at once, since nothing is sounding, and discards the other queued controls as before.

The desktop uses `at = 0`, so each playback edit activates at the next block boundary.

## What carries over

`Engine::replace` moves runtime state from each old track to its mapped new track:

- A playing clip that maps to a clip of the same kind keeps its launch frame. Audio continues at the same position in the new content, and a different length takes effect at once.
- A playing MIDI clip keeps its sounding voices, with their pitch and remaining duration, and renders them with the new instrument. A voice already releasing past the new release length ends. The note cursor moves to the first note at or after the current clip position, so passed notes do not retrigger and later ones still start.
- A playing clip mapped to `-1`, or to a clip of the other kind, stops along with its voices.
- Queued commands keep their order; their track and clip indexes are remapped, and those referring to removed tracks or clips are dropped and counted in `RenderStatus::dropped_commands`.
- Automation cursors restart against the new lanes.

## Status

`RenderStatus::generation` counts activated replacements and `RenderStatus::dropped_commands` counts queued commands they removed. `track_count` and the per-track status follow the active plan.

## Saturation and panic

The control queue holds 64 controls, and a replacement marker takes one of them. When it is full, `submit` returns false and `offer` returns `busy`; nothing is dropped silently, and the caller retries. Panic does not use the queue. It is a separate flag that the render owner checks before draining. It stops the transport, discards queued controls, activates a held or queued replacement, and renders silence in that block, even when the queue is full or a replacement with a future frame is holding later controls. Per block, the render owner drains at most 64 controls and performs at most one replacement.

`edit-bursts` checks:

- a saturated queue refuses both controls and an offer, and panic still silences in the next block;
- recovery: a replacement and fresh controls are accepted afterward;
- panic passes a replacement that holds a Stop behind a far-future frame.

A threaded run floods bursts of up to 72 controls and a replacement offer every round, for 3,000 rounds, with a panic every 50. All 60 panics produced a silent, stopped block within two render blocks of the request: one in flight plus the panic block. The render thread intercepted no allocation or release. The test prints the longest 64-frame block under bursts and the time of a worst-case replacement (64 MIDI tracks × 1,024 notes with sounding voices and a full command queue); these are host measurements, not a deadline qualification. The [receipt](../validation/edit-bursts.json) records one run.

## Retirement

On activation the render owner stores the plan it stopped using in a retirement slot and then counts the swap. `collect()` on the control thread returns an acknowledged `Retirement` holding that plan, which the render thread no longer references; the caller destroys it there. The first retirement holds no plan, because the constructor's plan belongs to its caller. Destroying the controller after the render thread is joined frees the active replacement and any offer that never activated.

`Engine::replace` and the activation path do not allocate, free, lock, log or perform I/O. The engine sizes its runtime storage for 64 tracks at construction so replacement can reorder tracks into a preallocated spare.

## Evidence and limits

`plan-replacement` checks:

- activation at frame 1000 with identical output for blocks 1, 64, 257, 512 and 3000, with one queued command dropped and one remapped;
- refusals for each mapping fault, an invalid plan, and a changed rate or tempo;
- control ordering: a launch submitted before the offer uses the old plan, one submitted after waits behind it and uses the new plan; a full queue refuses the offer; a stopped transport activates at once;
- MIDI voice continuity under a new waveform, cursor placement and a shortened release;
- 2,000 threaded replacements with 400 refused offers. Each retired plan is overwritten with NaN before it is destroyed, so a stale render-side read would show in the output. The run ends with an unactivated offer left for the destructor.

The render thread intercepts C++ allocation and deallocation and observes none. `document-render-preparation` checks `planMapping` across a track and clip deletion. The test runs under AddressSanitizer in CI; a local ThreadSanitizer run is recorded in the [receipt](../validation/plan-replacement.json).

Tempo and sample-rate changes are refused rather than replaced. Replacement keeps the earlier allocation-interception limits of the [engine contract](engine-contract.md#ownership-and-rendering). It has not been measured against a real-time deadline or on physical audio.
