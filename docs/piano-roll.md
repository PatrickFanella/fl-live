# Piano roll

A MIDI clip can use a piano roll instead of its step pattern. Notes sit at free tick positions, keep stable IDs, and can be added, selected, deleted (#35), moved, resized and given a velocity (#36). The clip has its own length and loop switch (#38).

## Model

Positions are ticks of 1/96 beat from the clip start. A clip's piano roll holds:

| Field | Range |
|---|---|
| Length | 1 tick to 16 bars (6,144 ticks); converted clips start with the pattern's length |
| Loop | On (default) repeats the clip; off plays it once and stops the track |
| Note start, length | Within 16 bars: start ≥ 0, length ≥ 1 tick. Edits keep notes inside the clip length; see [Length and loop](#length-and-loop) |
| Pitch | MIDI 0–127 |
| Velocity | 1–100 percent |
| Notes | At most 1,024, and at most 16 sounding at any note's start, matching the synth's 16 voices |

Notes are kept in start order, then by ID. Each note's ID comes from the clip's counter, which is saved with the notes. A later edit never reissues a deleted ID, while Undo restores the counter together with the notes it removes. Selection is view state: it is not saved or undone, survives unrelated edits by ID, and clears when the step editor switches to another clip.

At preparation each tick position becomes a frame with the same upward rounding as other musical boundaries, `frame = ceil(ticks × 48000 × 60 / (tempo × 96))`. Note starts and ends round independently. The clip loops at its length and plays through the clip's [built-in synth](instrument.md).

## Editing

Tick **Piano roll** in the pattern header. This converts the step pattern once: each sounding step becomes a note at its quarter-note position with length gate × 96 / 100 ticks (at least one), keeping its pitch and velocity (at least 1 %). The converted notes sound exactly like the pattern; a test compares float exports. The pattern itself is retained, and unticking **Piano roll** plays it again. Each switch is one Undo step.

The grid shows two octaves; **Octave down** and **Octave up** move the view. Its height follows the space the window leaves.

**Snap** chooses the grid: 1/16 (24 ticks, the default), 1/8, 1/4 or Off (one tick).

- Click an empty cell to add a note one grid step long (a sixteenth with Snap off) at that pitch with 80 % velocity. Its start snaps down to the grid. The new note is selected.
- Click a note to select it; Shift-click adds it to or removes it from the selection.
- Drag a note to move the selection. The horizontal change snaps to the grid and the vertical change moves whole semitones. Drag the last five pixels of a note to resize the selection. Selected notes show the snapped change while you drag. The change is relative, so a note that starts off the grid stays off it by the same amount.
- With the grid focused: Left and Right move the selection by one grid step, and Shift+Left and Shift+Right resize it. Up and Down move it a semitone, and Shift+Up and Shift+Down an octave. **Delete** or **Backspace** removes the selection, and **Escape** clears it. **Delete selected** does the same as Delete.
- **Velocity %** in the header sets the velocity of every selected note.
- For exact notes from the keyboard, set Start and Length in sixteenths, Pitch and Velocity, then press **Add note**.

Edits to several notes are all or nothing. If any selected note would start before the clip, end after it, leave MIDI pitches 0–127 or become shorter than one tick, the whole edit is refused with a message ("nothing moved" or "nothing changed"), and the notes and history stay unchanged. Overlapping notes are allowed, even on the same pitch. Each sounds on its own voice, within the 16-voice limit, which a move can also exceed and be refused for. Each move, resize or velocity change is one Undo step.

A note moved or deleted while it sounds keeps its voice for the rest of its original duration and then ends; the [plan replacement](plan-replacement.md) carries voices but never retriggers passed notes. The moved note then sounds at its new position. No voice is left sounding.

Adding and deleting work while playing, like other [playback edits](general-session.md#playback-edits). An out-of-range or overcrowded note is refused with a message and leaves the notes and history unchanged ("At most 16 notes can sound at once").

## Length and loop

**Length (16ths)** sets the clip length from one sixteenth to 16 bars, and **Loop** switches repetition. Each change is one Undo step and also works while playing.

Shortening is non-destructive. Notes that start at or after the new end stay in the clip, keep their IDs and are saved, but are silent and not drawn (`pianoNotes` marks them `sounding: false`). A note that crosses the end is cut there when rendered. Lengthening the clip plays them again unchanged. Adding, moving and resizing notes still require the result to lie inside the current length, so silent notes cannot be moved until the clip is long enough to show them. The 16-voice limit counts silent notes as well.

The clip length becomes frames once, with the same upward rounding, and each repetition is exactly that many frames. Repetitions can therefore drift from the musical grid by up to one frame per loop, as step patterns already did. With Loop off the clip plays once, the track then stops, and a sounding release ends with it.

## Persistence

Clips with notes add a `piano` object beside `pattern` and the project declares the required feature `piano-roll-v1`. A one-shot clip adds `"loop": false`; a looping clip omits the field, and `"loop": true` is refused so each state has one spelling:

```json
{"id": "4", "pattern": {...}, "piano": {"length": 384, "next_id": "6",
  "notes": [{"id": "1", "start": 0, "length": 72, "pitch": 60, "velocity": 70}]}}
```

IDs are decimal strings like other identities. Decoding requires exactly these fields and the canonical order, and refuses out-of-range values, unissued or duplicate IDs, notes outside the clip, notes on audio clips and notes without the feature.

## Verification

`authored-project-io` covers validation refusals, tick-to-frame rendering at 137 BPM, round trip and malformed files. In `desktop-session`, `pianoRollEditing` covers conversion (sample-identical export), add, select, delete, refusals including the 16-note limit, ID stability, Undo/Redo, switching back and Save/Open. `qmlPianoRollInput` adds, selects, Shift-selects and deletes with the pointer and the Delete key, undoes with the button, clears with Escape and adds a note from the keyboard fields.

For #36:

- `pianoRollMoveResize` covers moves, resizes and velocity, each refusal of the all-or-nothing policy, same-pitch overlap and the voice limit through a move.
- In the same test, a square note moved 37 ticks off the grid at 137 BPM starts at frame 8,103 and ends at frame 23,869 in the float export, matching the independently rounded positions.
- `qmlPianoRollDragAndKeys` drags to move and to resize, refuses a drag past the clip end, and uses the arrow keys, Snap Off and the velocity box.
- `authored-project-io` checks that a note moved while sounding finishes its voice and then sounds at its new position.

The [move and resize receipt](../validation/piano-roll-editing.json) records the run.

For #38:

- `authored-project-io` renders a 3¼-beat clip at 137 BPM for five loops. Each loop's note starts at k × 68,322 + 8,103 frames and stops at k × 68,322 + 13,358, independently rounded, with identical output at blocks 1, 64, 127, 257 and 1,000. A one-shot matches the first loop and is then silent with the track stopped. The `loop` flag round trips and `true` is refused.
- `pianoRollLengthLoop` shows that a clip shortened to two beats exports exactly like a two-beat clip whose later notes were deleted. Lengthening restores the original export. Edits outside the length are refused, Loop off plays one bar then silence, and Save/Open keeps the length, flag and silent notes.
- `qmlPianoRollLength` drives the length box, the Loop box and Undo.

The [length and loop receipt](../validation/piano-roll-length.json) records the run.

For #39, `pianoRollHistoryAndPersistence` applies every operation in turn: conversion, add, a two-note move, resize, velocity, delete, length, loop off and an add at pitch 127 and velocity 1. After each one it saves and reopens in a fresh session, checking notes, IDs, length, loop flag and mode. It then undoes back to the start and redoes forward, matching the recorded state, including the Undo label, at every step. Six refused files change nothing: the open document, its redo branch, path and saved state stay as they were. The files are an out-of-range pitch, an ID above the counter, an unknown note field, `"loop": true`, notes without `piano-roll-v1`, and an unknown `piano-roll-v2` feature. The [history receipt](../validation/piano-roll-history.json) records the run.

For #42, `qmlPianoRollPlaybackJourney` builds a clip in the QML. It deletes the converted notes with Shift-click and Delete, picks the square wave, types three notes (one at beat 2, two overlapping from beat 3 on one pitch), drags the first one a sixteenth later and exports.

- Every onset, end and amplitude in the float export matches the edited tick, velocity and voice sum: frames are ticks × 250 at 120 BPM, and the amplitude is 0.25 × velocity × 0.1.
- Zero-crossing counts match 440 Hz and 220 Hz within two crossings.
- Undo moves the onset back to frame 24,000 and Redo restores it. After Save and reopening in a fresh window, the export is sample-identical.
- Each termination path leaves no voice sounding, with `RenderStatus::active_voices` read from render acknowledgements on the preview clock:
  - Silence now;
  - deleting every note while playing (the sounding notes finish, then two and a half seconds stay silent);
  - Undo, which plays them again;
  - shortening the clip while playing;
  - Loop off, after which the clip ends and the track stops;
  - Stop.

The [playback receipt](../validation/piano-roll-playback.json) records the runs. The [receipt](../validation/piano-roll-notes.json) records the run.
