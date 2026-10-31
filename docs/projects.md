# Project settings and step patterns

The desktop app edits and persists the Pulse + Steps session: tempo from 20–300 integer BPM, two track levels from 0–100 percent and a MIDI pattern of 1–16 quarter-note steps. Each step stores an enabled flag, MIDI pitch 0–127, velocity 0–100 percent and gate 1–100 percent. Selection is UI state and does not mark the document dirty. [Undo/Redo](edit-history.md) restores up to 128 committed document edits while stopped; saved-content equality determines dirty state. Edits require acknowledged stopped transport with no pending control messages, then rebuild at position zero and clear launches.

Select a numbered step in the grid. Note enabled turns it into a note or rest; its pitch, velocity and gate controls retain their values while disabled. Gate is the note duration as a percentage of a quarter note. Velocity scales the engine's normalized velocity, rather than storing a MIDI 7-bit velocity byte. A zero-velocity enabled note is silent. Steps beyond the selected pattern length do not render, but retain their settings when the pattern is shortened and extended. An all-rest pattern is valid.

## Version 2 source-settings format

Desktop saves now use the [version 3 authored wrapper](desktop-identities.md), embedding this version 2 source-settings object and stable identity metadata/names. The source-only writer still emits version 2 with template `pulse-step-sequencer-v1` for retained fixtures. The 48 kHz mono pulse remains the authored decaying 110 Hz source; the MIDI track uses the original fixed-voice sine instrument. Every document stores exactly 16 step objects, even when the audible pattern is shorter. This is still an authored-template format: it does not store arbitrary tracks, imported media, free-position notes, plugins or an arrangement.

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

The reader still accepts version 1 with template `pulse-steps-v1` and its four-element `pitches` array. It maps those pitches to four enabled quarter-note steps, with velocity 70 percent and gate 75 percent. The remaining twelve steps start disabled. Tempo and levels are preserved. Desktop imports of v1/v2 receive deterministic initial track/clip/scene IDs and save as v3; earlier readers reject that new version. Use a new path to retain a legacy original.

The pulse length is one beat rounded up to a sample. MIDI starts, note ends and the pattern length round independently from rational musical positions to samples. A gate of 100 percent ends exactly at the next rounded step boundary. Repeated fixed-length clips may gradually drift from the rational launch grid at fractional sample periods. Pattern length is independent of the 4/4 launch grid: a five-step pattern loops over five quarter notes. General musical-time scheduling, swing and ties remain future work.

## Save and open

Enter a local file path in the project field. Save uses `QSaveFile` with direct-write fallback disabled; failed atomic writes preserve the prior file. A new destination refuses an existing file unless that exact normalized path was previously opened or saved by this session. Use a new path for Save As. Concurrent external modification detection is not implemented.

Open requires a regular file. Legacy sources retain their 64 KiB limit; the v3 wrapper permits 4 MiB plus 64 KiB and validates embedded source settings, metadata and required features separately. The parser checks the version/template pair, field counts, array sizes, boolean types and numeric ranges, including stored steps outside the active pattern. Malformed projects preserve current settings and the prepared graph. Unsaved edits must be saved before opening another project. Closing with edits presents Save and close, Discard and close, and Cancel.

Save can run during playback because it serializes GUI-owned source settings and immutable identity metadata. Position, active clips, queued launches, Undo/Redo history and selected editor step are not persisted. Open starts stopped at bar 1, beat 1 and selects step 1; launch clips again before playing.

## Ownership and verification

Settings, JSON, sample generation and graph allocation stay on the GUI thread. A replacement validates a new immutable plan/controller before joining the old backend, then attaches a replacement backend and swaps owners. Old graph destruction occurs only after its callbacks have joined. Validation failures leave the previous graph intact. Backend replacement failure preserves the old document/history with audio stopped; saving and explicit Retry audio remain available.

Tests cover v1 migration, v2 settings and rendered-sample round trips, rests/all-rest patterns, gate timing, velocity scaling, every pattern length at tempo extremes/fractional sample periods, retained hidden steps, selection, malformed files, stopped/pending restrictions and atomic-file preservation. Qt tests click the actual step grid/enable control, change pitch/velocity/gate with keyboard input, save/reopen, and inspect the reloaded controls. The private PipeWire fixture still checks stopped graph replacement. These checks do not qualify edited-session audio on physical devices or a general piano-roll workflow.

## Export the current template mix

Stop transport and wait for acknowledgement, enter a new WAV path, choose 1–16 bars, and select **Export both tracks**. Export uses the current settings, including unsaved edits. Both template clips launch at frame zero in a separate offline engine; transport position, active clips, project path, saved checkpoint and Undo/Redo are unchanged.

The result is mono, 48 kHz, little-endian 16-bit PCM WAV. Duration is the selected number of 4/4 bars at the current tempo, rounded upward once to a sample boundary. Both clips loop under the existing engine policies. Output ends at that frame without tails or a fade. Samples are clamped to ±1 and rounded to signed PCM using a 32767 scale. This exports the template mix, not a recording of the launch performance or a selection of active clips.

The destination must be new. Exclusive creation refuses existing files and links, including a project JSON path. Ordinary write/flush errors attempt to remove only the newly created output; existing destinations are preserved. Export runs synchronously on the GUI thread and has no cancellation or progress control. A process crash or power loss can leave a partial WAV, and flush does not promise durable storage. Project saving retains its separate atomic-write policy. At the slowest allowed tempo, the 16-bar limit is 192 seconds of audio (18.4 MB of PCM data).

The desktop tests compare decoded PCM samples against both edited tracks rendered independently with a different block size, check duration at 137 BPM, silent mixing, exclusive creation, length limits, pending/running refusal, document/history preservation and the QML export button. Physical playback and native Wayland remain separate qualification work.
