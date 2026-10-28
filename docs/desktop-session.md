# Desktop session contract

The optional `desktop` preset builds `fl-live`, an original Qt 6.12/QML session prototype. Two immutable authored tracks provide a decaying audio pulse and a configurable MIDI step loop. The authored session now supports stopped tempo/level/step-pattern edits and versioned template project save/reload; see [project contract](projects.md).

`Session` owns the immutable plan before constructing its controller. The GUI thread is the sole command producer. In silent preview, its 10 ms timer renders and discards 480 mono frames per tick on that same thread. Timer delays slow the simulated clock. In PipeWire mode, the backend exclusively owns rendering; the GUI timer only polls status. Backend destruction joins processing before controller/plan destruction. No Qt code runs in the audio callback.

Play/Stop/Return to start use the bounded control queue. Transport labels derive from the coherent render snapshot, not from a button press. Pending means a submitted serial has not been handled. Handled does not mean a scheduled clip has become active. Rejections are reported as an aggregate change; per-command outcome history is not yet available. Active clip indices and the earliest queued action for each track now come from the same coherent render snapshot as transport.

Launch and track stop use new controller commands resolved against the current position on the rendering owner. The bar grid is inclusive: a request consumed exactly on a boundary applies there. Otherwise it applies at the next boundary. `TempoGrid::try_next_bar` returns an empty optional on overflow without constructing exceptions or allocating. The original throwing `next_bar` remains available to non-rendering callers. Mixing arbitrary future scheduled commands with these quantized controls can still trigger the engine's global ordering rejection.

Stop freezes transport and clears all clips and scheduled commands. Return to start is enabled only while acknowledged stopped with no pending controls. Launch while stopped schedules a clip but does not start transport. Silence now uses out-of-band panic; it clears queued controls and stops on the next callback, without persistent muting. New controls can subsequently restart playback.

No remote argument selects silent preview. `--remote NAME` explicitly connects the audio backend; `--target NODE` additionally opts into autoconnection through the user's session manager. A target requires a remote. Backend failure joins/tears down processing, displays stopped transport and disables further submissions until app restart. Device selection, reconnect and rate adaptation remain future work.

## Verification boundary

`desktop-session` uses Qt's offscreen platform and software scene graph. It loads the actual QML resource, sends mouse clicks to the clip/Play/Stop controls and checks transport labels. A deterministic bridge test verifies acknowledgement, rewind, invalid clip and panic. Sample checks verify launch at frame 96,000 and track stop at 192,000 across 257-frame blocks; clock overflow returns no boundary. A local PNG from the actual offscreen window is retained under `build/desktop/desktop-session.png` and was inspected for layout.

The desktop preset also runs engine/controller/offline WAV/private PipeWire tests. The private audio integration still uses the headless playback client, so it does not qualify the QML-to-physical-device journey. Wayland desktop input, hardware audibility, device changes, accessibility and extended playback remain unverified. Licensing, packaging and a useful general editing workflow remain incomplete. Hosted CI is configured and passed on the preceding foundation.

The private-graph test now also performs five stopped backend replacements, including Undo/Redo through the desktop `Session` bridge. Offscreen QML checks cover tempo editing, save/open and dirty-close refusal. General clip/media editing and physical audio qualification remain open.

## Clip feedback

`Engine::track_statuses()` reads only render-owned runtime and scheduled commands. The fixed 64-track result reports an active clip index or -1, plus the earliest scheduled launch/stop, its clip index and target frame. It scans each track and queued command once. A one-shot is inactive at its exclusive end even if the next process call has not yet cleared its internal runtime entry. Muted or zero-valued clips can still be active; activity does not prove audible output.

The controller publishes these fields with transport in its existing atomic sequence protocol. All fields shared with readers are atomic, with lock-free requirements. Publishing is bounded by the 64-track and 64-command limits and allocates no memory; readers can retry but the render writer never waits. Unused track slots default to inactive and no pending action.

The GUI distinguishes an unhandled control request (Launch requested / Stop requested), an acknowledged queued action (Launch queued / Stop queued), and an active clip. Active with an upcoming action reads Active · launch queued or Active · stop queued. Buttons highlight only for active clip indices. Multiple same-sample actions remain in insertion order; the indicator exposes the earliest one, not every queued action or a prediction of the final state.

Snapshots are published after a block. A command at the next unrendered frame stays queued until that frame is processed; the UI refreshes on its polling timer. A very short clip that starts and ends within one block may never appear active in a block-completion snapshot. Stop, panic, backend finish and project replacement clear activity and queued actions. The snapshot describes rendering state, not hardware presentation time.

Tests check sample boundaries, audio/MIDI one-shot completion, clip replacement, same-frame launch/stop ordering, rejection and panic cleanup. A concurrent reader checks coherent activity/transport over 10,000 launch/stop cycles. Qt checks cover requested/queued/active states, quantized track stop and button highlights using real QML controls. `build/desktop/desktop-active-clip.png` records the rendered active state and is retained with CI artifacts.

The MIDI editor now offers 1–16 quarter-note steps with enabled/rest, pitch, velocity and gate controls. Its panel scrolls on smaller windows. Selecting a step is independent of playback and does not edit the document. The highlighted editor step is a selection, not a per-note playback cursor. Version 1 projects migrate on read; saves use version 2. See [project format](projects.md).

Undo/Redo now use the same stopped prepared-session replacement path. Save preserves history; successful Open resets it. See [editing history](edit-history.md) for the 128-edit bound, saved-content tracking and keyboard/text-input behavior.

## Desktop control-to-audio verification

The desktop variant of `pipewire-private-graph` loads the production QML with a real `Session` and PipeWire backend on the owned software server. It invokes the controls’ `valueModified` and `clicked` signals to set tempo 300, a one-step A4 pattern, velocity 50%, gate 100%, and MIDI track gain 40%, then starts transport and launches only that track. A separate `pw-cat` process records 144,000 mono float samples at 48 kHz.

The capture must contain a 440 Hz tone with peak 0.02 in the 1.5–2.0 second window, and end with 12,000 silent samples after the QML Stop action is acknowledged. The probe also requires active-clip feedback during playback, disabled editing while running, and cleared activity with editing enabled after Stop. Frequency and gain checks use captured samples rather than the engine’s own expected buffer.

This is an offscreen software integration check. It does not establish native pointer/keyboard behavior, Wayland rendering, physical audibility, hardware latency, or xrun performance. The existing offscreen input tests and backend lifecycle checks remain separate evidence.

Play and Launch may be handled in the same render block at frame zero, which is a valid bar boundary. A silent prefix is therefore permitted but not required.
