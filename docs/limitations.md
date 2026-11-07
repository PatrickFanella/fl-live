# Support and limitations

This page separates what has been measured from what is planned or unqualified. "Checked" means an automated test in this repository exercises the behavior; the linked receipt records the source revision, environment and limits. Nothing here has been qualified on physical audio hardware, native Wayland input or a packaged release.

## Kinds of evidence

| Kind | Meaning |
|---|---|
| Offline | Engine or document code run directly in a test process, no audio server |
| Offscreen UI | The production QML driven by synthetic pointer and keyboard events on Qt's offscreen platform |
| Private graph | A private, software-only PipeWire server started by the test; no devices, no desktop session |
| Hosted CI | The same suites in the pinned container on the shared Gitea runners |

## Matrix

| Area | Supported and checked | Evidence | Planned, partial or unqualified |
|---|---|---|---|
| Timing | Constant-tempo 4/4, 20–300 BPM, rational bar boundaries; launch, stop and scene timing at non-dividing block sizes | Offline: [engine](../validation/engine-foundation.json), [scene controls](../validation/scene-controls.json), [general regressions](../validation/general-session-regressions.json) | Tempo changes, other meters, swing |
| Mix | Stereo mix, balance-law pan, mute/solo, mono downmix | Offline and private graph: [stereo output](../validation/stereo-output.json), [track mixer](../validation/track-mixer.json) | Sends, buses, effects, latency compensation (#92) |
| Tracks | Up to 64 audio/MIDI tracks, one clip each; add, duplicate, delete, rename with Undo | Offscreen UI: [general tracks](../validation/general-session-tracks.json), [D2 journey](../validation/d2-session-journey.json) | More than one clip per track; Arrangement view (#91) |
| Scenes | Up to 256 scenes; create, copy, order, delete; Immediate and One bar launch/stop | Offscreen UI: [scene management](../validation/scene-management.json), [copy and order](../validation/scene-copy-order.json) | Follow actions, scene tempo |
| MIDI | 1–16 quarter-note steps per MIDI clip with pitch, velocity and gate, or a [piano roll](piano-roll.md) of up to 1,024 notes with pointer and keyboard add, select, delete, move, resize and velocity on a chosen grid; a [built-in synth](instrument.md) with four waveforms, ADSR and a 16-voice overload policy | Offline and offscreen UI: [step editor](../validation/midi-step-editor.json), [synth](../validation/builtin-synth.json), [piano roll](../validation/piano-roll-notes.json), [move and resize](../validation/piano-roll-editing.json) | Clip length/loop (#38), dense-drawing measurement (#41), filters, band-limited oscillators, a sampler, MIDI input |
| Audio files | WAV only: 8/16/24/32-bit PCM or 32/64-bit float, mono or stereo, 48 kHz; source/loop ranges; waveform view | Offline and offscreen UI: [WAV import](../validation/wav-import.json), [clip ranges](../validation/clip-ranges.json) | Resampling, warping, other formats, other session rates |
| Asset references | Absolute path plus project-relative recovery; Relink; Collect into the project folder; never substitutes or overwrites | Offscreen UI: [asset policy](../validation/asset-relink-collect.json), [recovery and export journey](../validation/recovery-export-journey.json) | Search for moved files by content |
| Projects | Version 4 JSON; versions 1–3 migrate; synced atomic saves; fault-injected and killed saves keep a complete file | Offline: [atomic saves](../validation/atomic-save-faults.json) | Power-loss and network-filesystem behavior |
| Recovery | Autosave every 30 s while unsaved; Restore/Discard banner after a crash | Offscreen UI with a real abort: [autosave](../validation/autosave-recovery.json) | Edits after the last snapshot are lost |
| Export | WAV, start bar 1–256, 1–64 bars, silent tail, stereo or mono, PCM16/PCM24/float32; background with progress and cancel; never replaces a file | Offline and offscreen UI: [render policy](../validation/export-render-policy.json), [progress and cancel](../validation/export-progress-cancel.json) | Effect tails, compressed formats, stems |
| Audio output | PipeWire, mono or FL/FR, explicit remote and target; recovery after server loss | Private graph: [PipeWire foundation](../validation/pipewire-foundation.json), [backend recovery](../validation/audio-backend-recovery.json) | Physical devices, latency, xrun measurement (#19–#21), device reconnection (#25) |
| Real-time continuity | Fixed-quantum private-graph controls when the host is idle | Private graph: [reference quantum continuity](../validation/reference-quantum-continuity.json) | Live quantum changes (#23); under host load the private graph drops or slips quanta, which the content checks report rather than fail |
| Input | Offscreen keyboard and pointer events on the production QML; [shortcuts, focus restoration and unique accessible names](keyboard.md) | Offscreen UI: [keyboard and accessibility](../validation/keyboard-accessibility.json) | Native Wayland input (#22), keyboard-only qualification journey (#83), screen-reader checks (#80) |
| Recording | None | — | Audio/MIDI recording (#43–#50) |
| Automation | [Level and pan lanes](automation.md) with stable IDs, linear interpolation, persistence and a pointer/keyboard lane editor | Offline and offscreen UI: [automation](../validation/parameter-automation.json), [output qualification](../validation/automation-qualification.json) | Dragging points, clip-relative lanes, other parameters |
| Playback edits | Track, mixer, rename, scene, range, pattern, synth and automation edits and their Undo [apply while playing](general-session.md#playback-edits) through [plan replacement](plan-replacement.md); tempo and file operations are refused | Offline and offscreen UI: [plan replacement](../validation/plan-replacement.json), [playback edits](../validation/playback-edits.json), [edit bursts and panic](../validation/edit-bursts.json) | Desktop audio capture of a live edit; real-time deadlines under load |
| Devices, plugins | None | — | Typed devices (#73–#78), plugin hosting (#66–#72) |
| Platforms | Linux with Qt 6.12 and PipeWire 1.6.9 in the pinned container; a [development package](install.md) installs and launches in a clean runtime-only container | Hosted CI; [packaging](../validation/linux-packaging.json) | A licensed release (#84), other distributions, macOS (#96) |

## Known gaps that affect everyday use

- Tempo, Open, New, import, relink and export need stopped transport; other edits [apply while playing](general-session.md#playback-edits). A full control queue refuses controls visibly; Silence now always works.
- The built-in synth is basic: naive oscillators and a linear envelope, no filter.
- Imported audio must already be at 48 kHz.
- No license has been chosen, so nothing here is a release.
