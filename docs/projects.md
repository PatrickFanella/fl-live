# Project settings, version 1

The desktop app can edit and persist the current Pulse + Steps session: integer tempo from 20–300 BPM, two track levels from 0–100 percent and four MIDI pitches from 0–127. These controls are available only while transport is acknowledged stopped and the control handoff has no pending messages. Edits rebuild the prepared session at position zero, clearing clips and scheduled launches.

This is a versioned authored-template format. It does not yet store arbitrary tracks, imported audio, note timing, plugins or a general arrangement. The `pulse-steps-v1` template identifies a 48 kHz mono decaying 110 Hz pulse and a four-step sine instrument. Each MIDI note lasts three quarters of one beat, with velocity 0.7. Future template changes require a new template identifier or explicit migration.

```json
{
  "format": "fl-live-project",
  "version": 1,
  "template": "pulse-steps-v1",
  "tempo": 137,
  "levels": [35, 60],
  "pitches": [48, 55, 59, 62]
}
```

The pulse length is one beat rounded up to a sample. MIDI starts, note ends and the four-beat clip length round independently to samples within one clip. Repeated fixed-length clips may gradually drift from the rational bar grid at fractional sample periods. The engine's launch grid remains rational; general musical-time clip scheduling remains future work.

## Save and open

Enter a local file path in the project field. Save writes JSON using `QSaveFile`, with direct-write fallback disabled. A failed atomic write leaves the existing file in place. Saving to a new path refuses an already existing file unless that exact normalized path was previously opened or saved by this session. Use a new path for Save As. Concurrent external modification detection is not implemented.

Open requires a regular file no larger than 64 KiB. The parser checks the format, version, template, field count, array sizes and every numeric setting before preparing a replacement. Unknown versions, templates, extra fields, fractional integers and out-of-range values are refused. Malformed projects preserve the current settings and graph. Unsaved edits must be saved before opening another project. Closing with edits presents Save and close, Discard and close, and Cancel.

Save can run while transport plays because it serializes GUI-owned settings, not render state. Transport position, active clips, scheduled launches and pending messages are not saved. Reopening starts stopped at bar 1, beat 1; launch clips again before playing.

## Ownership and failure behavior

All settings, JSON operations, sample generation and graph allocation run on the GUI thread. The render owner never sees a mutable settings object. A replacement prepares and validates a new immutable plan/controller before joining the old audio backend. It then attaches a backend to the candidate and swaps prepared owners. Destruction of the old graph stays on the GUI thread, after backend callbacks have joined.

If allocation or validation fails, the old prepared session survives. If creating the replacement audio backend fails, the old document survives with processing stopped and submissions disabled until app restart; it can still be saved. No automatic audio reconnection is claimed.

## Verification

The desktop tests cover JSON and rendered-sample round trips, tempo extremes, malformed/oversized projects, running and pending-control restrictions, invalid edits, unrelated-file preservation, failed save paths, dirty-state transitions and QML keyboard/mouse save/open interaction. Closing with dirty edits is checked offscreen. The private PipeWire fixture additionally constructs the desktop session and performs three stopped graph/backend replacements on its owned software server. That lifecycle check does not capture the edited session's audio or qualify physical devices.
