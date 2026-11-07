# Project settings and step patterns

The desktop app edits and persists a Session of audio and MIDI tracks; see [general Session tracks](general-session.md) for track management, WAV import and the version 4 file. New projects start as the Pulse + Steps template. Settings are tempo from 20–300 integer BPM, a level from 0–100 percent per track and, per MIDI clip, a pattern of 1–16 quarter-note steps. Each step stores an enabled flag, MIDI pitch 0–127, velocity 0–100 percent and gate 1–100 percent. Selection is UI state and does not mark the document dirty. [Undo/Redo](edit-history.md) restores up to 128 committed document edits while stopped; saved-content equality determines dirty state. Edits require acknowledged stopped transport with no pending control messages, then rebuild at position zero and clear launches.

Select a numbered step in the grid. Note enabled turns it into a note or rest; its pitch, velocity and gate controls retain their values while disabled. Gate is the note duration as a percentage of a quarter note. Velocity scales the engine's normalized velocity, rather than storing a MIDI 7-bit velocity byte. A zero-velocity enabled note is silent. Steps beyond the selected pattern length do not render, but retain their settings when the pattern is shortened and extended. An all-rest pattern is valid.

## Version 2 source-settings format

Desktop saves now use the [version 4 general format](general-session.md#project-files-version-4); the [version 3 authored wrapper](desktop-identities.md), which embedded this version 2 source-settings object, still opens. The source-only writer still emits version 2 with template `pulse-step-sequencer-v1` for retained fixtures. The 48 kHz mono pulse remains the authored decaying 110 Hz source; the MIDI track uses the original fixed-voice sine instrument. Every document stores exactly 16 step objects, even when the audible pattern is shorter. This is still an authored-template format: it does not store arbitrary tracks, imported media, free-position notes, plugins or an arrangement.

A complete four-step default project is:

```json
{
  "format": "fl-live-project",
  "version": 2,
  "template": "pulse-step-sequencer-v1",
  "tempo": 120,
  "levels": [70, 25],
  "pattern": {
    "length": 4,
    "steps": [
      {"enabled": true, "pitch": 60, "velocity": 70, "gate": 75},
      {"enabled": true, "pitch": 64, "velocity": 70, "gate": 75},
      {"enabled": true, "pitch": 67, "velocity": 70, "gate": 75},
      {"enabled": true, "pitch": 72, "velocity": 70, "gate": 75},
      {"enabled": false, "pitch": 60, "velocity": 70, "gate": 75},
      {"enabled": false, "pitch": 60, "velocity": 70, "gate": 75},
      {"enabled": false, "pitch": 60, "velocity": 70, "gate": 75},
      {"enabled": false, "pitch": 60, "velocity": 70, "gate": 75},
      {"enabled": false, "pitch": 60, "velocity": 70, "gate": 75},
      {"enabled": false, "pitch": 60, "velocity": 70, "gate": 75},
      {"enabled": false, "pitch": 60, "velocity": 70, "gate": 75},
      {"enabled": false, "pitch": 60, "velocity": 70, "gate": 75},
      {"enabled": false, "pitch": 60, "velocity": 70, "gate": 75},
      {"enabled": false, "pitch": 60, "velocity": 70, "gate": 75},
      {"enabled": false, "pitch": 60, "velocity": 70, "gate": 75},
      {"enabled": false, "pitch": 60, "velocity": 70, "gate": 75}
    ]
  }
}
```

## Version 1 migration

The reader still accepts version 1 with template `pulse-steps-v1` and its four-element `pitches` array. It maps those pitches to four enabled quarter-note steps, with velocity 70 percent and gate 75 percent. The remaining twelve steps start disabled. Tempo and levels are preserved. Desktop imports of v1/v2 receive deterministic initial track/clip/scene IDs and save as v4; earlier readers reject that new version. Use a new path to retain a legacy original.

The pulse length is one beat rounded up to a sample. MIDI starts, note ends and the pattern length round independently from rational musical positions to samples. A gate of 100 percent ends exactly at the next rounded step boundary. Repeated fixed-length clips may gradually drift from the rational launch grid at fractional sample periods. Pattern length is independent of the 4/4 launch grid: a five-step pattern loops over five quarter notes. General musical-time scheduling, swing and ties remain future work.

## Save and open

Enter a local file path in the project field. Save uses `QSaveFile` with direct-write fallback disabled; failed atomic writes preserve the prior file. See [interrupted and failed saves](#interrupted-and-failed-saves). A new destination refuses an existing file unless that exact normalized path was previously opened or saved by this session. Use a new path for Save As. Concurrent external modification detection is not implemented.

Open requires a regular file. Legacy sources retain their 64 KiB limit; the v3 wrapper permits 4 MiB plus 64 KiB and validates embedded source settings, metadata and required features separately. The parser checks the version/template pair, field counts, array sizes, boolean types and numeric ranges, including stored steps outside the active pattern. Malformed projects preserve current settings and the prepared graph. Unsaved edits must be saved before opening another project. Closing with edits presents Save and close, Discard and close, and Cancel.

### Interrupted and failed saves

Project, legacy settings and metadata saves go through `commitDurably()`: write the encoded bytes to Qt's temporary file, flush, `fdatasync` it, publish it over the destination, then `fsync` the directory. `QSaveFile::commit()` alone does not sync, so before this change a crash or power loss right after the rename could leave the destination without its new contents. Any failed step discards the temporary and leaves the previous destination untouched; the error says the previous file was preserved. If only the final directory sync fails, the complete new file is already in place, but the save is still reported as failed and the session stays marked unsaved.

On Linux, Qt writes to an unnamed `O_TMPFILE` and publishes it with `linkat` (new destination) or a briefly named `NAME.XXXXXX` link plus `rename` (existing destination). A killed process can therefore leave, at most, one `NAME.XXXXXX` file beside the project, only if it dies between those two calls. fl-live never opens such files; they hold a complete copy of the interrupted save and can be inspected or deleted by hand. The destination itself is always the last complete save.

`atomic-save-faults` checks this with a test-only `LD_PRELOAD` injector that fails `write` (after 512 bytes, `ENOSPC`), `fsync`/`fdatasync` (`EIO`) or publishing (`rename`/`renameat`/`renameat2`/`linkat`, `EIO`) for files in one directory. Real `Session::saveProject` calls, both overwrite and Save As, must fail and keep the destination bytes, an unrelated neighboring file, the directory listing, tempo/pitch edits, the project path, the unsaved state and Undo/Redo; Undo, Redo and a later save then work. A second stage kills a process with SIGKILL at 40 random points while it alternately saves two 129 KB projects; every time the destination equals one of the two complete saves, and the project reopens through `Session::loadProject`. Power loss, full-disk behavior of a real filesystem, network filesystems and concurrent writers are not covered.

Save can run during playback because it serializes GUI-owned source settings and immutable identity metadata. Position, active clips, queued launches, Undo/Redo history and selected editor step are not persisted. Open starts stopped at bar 1, beat 1 and selects step 1; launch clips again before playing.

## Ownership and verification

Settings, JSON, sample generation and graph allocation stay on the GUI thread. A replacement validates a new immutable plan/controller before joining the old backend, then attaches a replacement backend and swaps owners. Old graph destruction occurs only after its callbacks have joined. Validation failures leave the previous graph intact. Backend replacement failure preserves the old document/history with audio stopped; saving and explicit Retry audio remain available.

Tests cover v1 migration, v2 settings and rendered-sample round trips, rests/all-rest patterns, gate timing, velocity scaling, every pattern length at tempo extremes/fractional sample periods, retained hidden steps, selection, malformed files, stopped/pending restrictions and atomic-file preservation. Qt tests click the actual step grid/enable control, change pitch/velocity/gate with keyboard input, save/reopen, and inspect the reloaded controls. The private PipeWire fixture still checks stopped graph replacement. These checks do not qualify edited-session audio on physical devices or a general piano-roll workflow.

## Export the current template mix

Stop transport and wait for acknowledgement, enter a new WAV path, choose 1–16 bars, and select **Export all tracks**. Export uses the current settings, including unsaved edits. Every track's clip launches at frame zero in a separate offline engine prepared through the same document plan as playback, so track level, pan, mute and solo apply; transport position, active clips, project path, saved checkpoint and Undo/Redo are unchanged.

The result is stereo (interleaved left/right), 48 kHz, little-endian 16-bit PCM WAV. Duration is the selected number of 4/4 bars at the current tempo, rounded upward once to a sample boundary. Both clips loop under the existing engine policies. Output ends at that frame without tails or a fade. Samples are clamped to ±1 and rounded to signed PCM using a 32767 scale. This exports the template mix, not a recording of the launch performance or a selection of active clips.

The destination must be new. Export runs on a worker thread from a copy of the document taken when it starts, so later edits, Undo/Redo, Save and transport do not change the file being written; a second export is refused until the first finishes. The header row shows a progress bar and **Cancel export** while it runs. Samples go to `NAME.part`, created exclusively next to the destination. When rendering finishes, the partial file is flushed and `fdatasync`ed, then published with `link()`, which fails instead of replacing anything if a file appeared at the destination meanwhile; the partial file is then removed and the directory synced. Cancellation, write errors and a refused publish all remove `NAME.part` and leave the destination absent (or untouched, if someone else created it). Only a crash or kill can leave `NAME.part` behind: fl-live never reads it, and a later export to the same path refuses to start and asks you to delete it or choose another path. Closing the session cancels a running export and waits for the worker. At the slowest allowed tempo, the 16-bar limit is 192 seconds of audio (36.9 MB of PCM data).

`exportCancellationAndFailures` clicks Export and Cancel in the actual QML while a 192-second export is running, then checks that no destination or partial file remains and that edits, Undo/Redo, path, unsaved state and transport are unchanged. It also forces a mid-stream write failure with a 1 MiB `RLIMIT_FSIZE`, plants a stale `NAME.part`, creates a file at the destination during export, compares an export started before further edits with one rendered from the restored state, and destroys a session during export.

The desktop tests compare each decoded channel against the edited tracks rendered independently and mixed with the declared pan law and mute/solo policy, check duration at 137 BPM, silent mixing, exclusive creation, length limits, pending/running refusal, document/history preservation and the QML export button. Physical playback and native Wayland remain separate qualification work.

## Track mixer

Each track has level (template settings), pan, mute and solo. Pan, mute and solo are stored on the track in the session metadata (`TrackMix`) and follow the [balance pan law](engine-contract.md#channels-and-pan): pan is −100 (left) to 100 (right) in whole percent, center keeps both channels at full level. While any track is soloed only soloed tracks play; mute wins over solo. Mixer edits are stopped edits like the rest of the template: each is one Undo step and rebuilds the prepared plan. Duplicating a track copies its mixer settings; deleting the last soloed track releases solo for the others.

Pan, mute and solo persist through the v3 wrapper's session object. When any track uses them, that object declares the required feature `track-mixer-v1` and every track carries a `mix` object; otherwise the file keeps its earlier identity-only form. Readers that predate the mixer refuse files that use it instead of dropping the settings. The desktop now renders and outputs stereo (PipeWire `FL`/`FR`); silent preview advances the same stereo clock.
