# Project settings and step patterns

The desktop app edits and persists the Pulse + Steps session: tempo from 20–300 integer BPM, two track levels from 0–100 percent and a MIDI pattern of 1–16 quarter-note steps. Each step stores an enabled flag, MIDI pitch 0–127, velocity 0–100 percent and gate 1–100 percent. Selection is UI state and does not mark the document dirty. [Undo/Redo](edit-history.md) restores up to 128 committed document edits while stopped; saved-content equality determines dirty state. Edits require acknowledged stopped transport with no pending control messages, then rebuild at position zero and clear launches.

Select a numbered step in the grid. Note enabled turns it into a note or rest; its pitch, velocity and gate controls retain their values while disabled. Gate is the note duration as a percentage of a quarter note. Velocity scales the engine's normalized velocity, rather than storing a MIDI 7-bit velocity byte. A zero-velocity enabled note is silent. Steps beyond the selected pattern length do not render, but retain their settings when the pattern is shortened and extended. An all-rest pattern is valid.

## Version 2 format

All saves now write version 2 with template `pulse-step-sequencer-v1`. The 48 kHz mono pulse remains the authored decaying 110 Hz source; the MIDI track uses the original fixed-voice sine instrument. Every document stores exactly 16 step objects, even when the audible pattern is shorter. This is still an authored-template format: it does not store arbitrary tracks, imported media, free-position notes, plugins or an arrangement.

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

The reader still accepts version 1 with template `pulse-steps-v1` and its four-element `pitches` array. It maps those pitches to four enabled quarter-note steps, with velocity 70 percent and gate 75 percent. The remaining twelve steps start disabled. Tempo and levels are preserved. Save writes v2; earlier v1-only app builds cannot read that saved file. Use a new path to retain a v1 original.

The pulse length is one beat rounded up to a sample. MIDI starts, note ends and the pattern length round independently from rational musical positions to samples. A gate of 100 percent ends exactly at the next rounded step boundary. Repeated fixed-length clips may gradually drift from the rational launch grid at fractional sample periods. Pattern length is independent of the 4/4 launch grid: a five-step pattern loops over five quarter notes. General musical-time scheduling, swing and ties remain future work.

## Save and open

Enter a local file path in the project field. Save uses `QSaveFile` with direct-write fallback disabled; failed atomic writes preserve the prior file. A new destination refuses an existing file unless that exact normalized path was previously opened or saved by this session. Use a new path for Save As. Concurrent external modification detection is not implemented.

Open requires a regular file no larger than 64 KiB. The parser checks the version/template pair, field counts, array sizes, boolean types and numeric ranges, including stored steps outside the active pattern. Malformed projects preserve current settings and the prepared graph. Unsaved edits must be saved before opening another project. Closing with edits presents Save and close, Discard and close, and Cancel.

Save can run during playback because it serializes GUI-owned settings. Position, active clips, queued launches, Undo/Redo history and selected editor step are not persisted. Open starts stopped at bar 1, beat 1 and selects step 1; launch clips again before playing.

## Ownership and verification

Settings, JSON, sample generation and graph allocation stay on the GUI thread. A replacement validates a new immutable plan/controller before joining the old backend, then attaches a replacement backend and swaps owners. Old graph destruction occurs only after its callbacks have joined. Validation failures leave the previous graph intact. Backend replacement failure preserves the old document with audio stopped and controls disabled until app restart; saving remains available.

Tests cover v1 migration, v2 settings and rendered-sample round trips, rests/all-rest patterns, gate timing, velocity scaling, every pattern length at tempo extremes/fractional sample periods, retained hidden steps, selection, malformed files, stopped/pending restrictions and atomic-file preservation. Qt tests click the actual step grid/enable control, change pitch/velocity/gate with keyboard input, save/reopen, and inspect the reloaded controls. The private PipeWire fixture still checks stopped graph replacement. These checks do not qualify edited-session audio on physical devices or a general piano-roll workflow.
