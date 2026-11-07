# Piano roll

A MIDI clip can use a piano roll instead of its step pattern (#35). The notes sit at free positions on a sixteenth-note grid and keep stable IDs. Move, resize and per-note pitch/velocity editing are #36; clip length and loop controls are #38.

## Model

Positions are ticks of 1/96 beat from the clip start. A clip's piano roll holds:

| Field | Range |
|---|---|
| Length | 1 tick to 16 bars (6,144 ticks); converted clips start with the pattern's length |
| Note start, length | Inside the clip: start ≥ 0, length ≥ 1 tick, start + length ≤ clip length |
| Pitch | MIDI 0–127 |
| Velocity | 1–100 percent |
| Notes | At most 1,024, and at most 16 sounding at any note's start, matching the synth's 16 voices |

Notes are kept in start order, then by ID. Each note's ID comes from the clip's counter, which is saved with the notes. A later edit never reissues a deleted ID, while Undo restores the counter together with the notes it removes. Selection is view state: it is not saved or undone, survives unrelated edits by ID, and clears when the step editor switches to another clip.

At preparation each tick position becomes a frame with the same upward rounding as other musical boundaries, `frame = ceil(ticks × 48000 × 60 / (tempo × 96))`. Note starts and ends round independently. The clip loops at its length and plays through the clip's [built-in synth](instrument.md).

## Editing

Tick **Piano roll** in the pattern header. This converts the step pattern once: each sounding step becomes a note at its quarter-note position with length gate × 96 / 100 ticks (at least one), keeping its pitch and velocity (at least 1 %). The converted notes sound exactly like the pattern; a test compares float exports. The pattern itself is retained, and unticking **Piano roll** plays it again. Each switch is one Undo step.

The grid shows two octaves; **Octave down** and **Octave up** move the view. Its height follows the space the window leaves.

- Click an empty cell to add a sixteenth note at that pitch with 80 % velocity, snapped down to the sixteenth. The new note is selected.
- Click a note to select it; Shift-click adds it to or removes it from the selection.
- **Delete** (with the grid focused) or **Delete selected** removes the selected notes. **Escape** clears the selection.
- For exact notes from the keyboard, set Start and Length in sixteenths, Pitch and Velocity, then press **Add note**.

Adding and deleting work while playing, like other [playback edits](general-session.md#playback-edits). An out-of-range or overcrowded note is refused with a message and leaves the notes and history unchanged ("At most 16 notes can sound at once").

## Persistence

Clips with notes add a `piano` object beside `pattern` and the project declares the required feature `piano-roll-v1`:

```json
{"id": "4", "pattern": {...}, "piano": {"length": 384, "next_id": "6",
  "notes": [{"id": "1", "start": 0, "length": 72, "pitch": 60, "velocity": 70}]}}
```

IDs are decimal strings like other identities. Decoding requires exactly these fields and the canonical order, and refuses out-of-range values, unissued or duplicate IDs, notes outside the clip, notes on audio clips and notes without the feature.

## Verification

`authored-project-io` covers validation refusals, tick-to-frame rendering at 137 BPM, round trip and malformed files. In `desktop-session`, `pianoRollEditing` covers conversion (sample-identical export), add, select, delete, refusals including the 16-note limit, ID stability, Undo/Redo, switching back and Save/Open. `qmlPianoRollInput` adds, selects, Shift-selects and deletes with the pointer and the Delete key, undoes with the button, clears with Escape and adds a note from the keyboard fields. The [receipt](../validation/piano-roll-notes.json) records the run.
