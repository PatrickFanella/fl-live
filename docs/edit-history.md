# Editing history

Undo and Redo cover track/clip names, tempo, track levels, MIDI pattern length and each step's note/rest, pitch, velocity and gate. They require acknowledged stopped transport with no unhandled control messages. Like a normal edit, each successful history operation prepares the session at position zero and clears active clips and queued launches. Clip launches, transport, editor selection and file paths are outside document history.

The header buttons expose the next operation through tooltips. Standard Undo/Redo key bindings are enabled while the window is active, except when an editable text input owns text undo or the unsaved-close or rename dialog is open. On Linux the tested bindings are Ctrl+Z and Ctrl+Shift+Z. Clicking a history button explicitly operates on the musical document. Step selection remains independent of history; undoing to a shorter pattern clamps the current selection to its last valid step.

## Transaction and memory boundary

History belongs to the GUI thread. It contains 129 fixed-capacity entries: an initial state plus at most 128 successful edits. Each entry stores scalar source settings, a shared immutable identity snapshot and an edit kind/index for labels. A static assertion requires copy assignment not to throw. Candidate metadata allocation happens before graph commit; history storage does not grow dynamically or reach the render callback. Evicted metadata is destroyed on the GUI owner.

A normal edit prepares and validates its candidate through the existing immutable graph replacement path, then appends a snapshot. No-op changes and failed edits add nothing. Undo/Redo prepare the target snapshot first and move the history cursor only on success. Allocation/validation failures preserve the document and cursor. A backend replacement failure stops audio and disables further submissions while preserving the old document/history; explicit Retry audio preserves the document/history and reconnects stopped.

A new edit after Undo removes that redo branch. When the history reaches 128 edits, another edit evicts the oldest retained state and keeps 128 changes available. Each committed control value is an edit; key-repeat changes are not coalesced into one gesture. History is not serialized or restored after app restart.

## Save checkpoints and document boundaries

Dirty state compares the entire current source-settings and identity snapshot with the last successfully saved or opened content. It does not compare a history index. Undoing back to saved content makes the project clean; Redo away from it makes it dirty. Manually restoring the same saved content is also clean, even if the corresponding history entry has been evicted.

Save and Save As preserve Undo/Redo and update the saved-content checkpoint only after a successful atomic write. History operations do not write files or change the destination path. A failed save leaves the checkpoint unchanged. Saving while positioned in the middle of history keeps its redo branch.

Open still refuses unsaved edits. A successful Open prepares the loaded settings, sets the clean checkpoint, selects step 1 and resets history to a single state. A malformed or otherwise failed Open preserves the existing history. The desktop reads v1/v2 sources and writes the version 3 authored wrapper with identities and names; see [desktop identity integration](desktop-identities.md).

## Verification

Session tests cover all edit kinds, complete undo/redo, redo branching, no-ops and invalid edits, saved-content equality, Save As and failed saves, failed/successful Open, pending/running restrictions, queued-launch cleanup, selection clamping and the 128-edit bound. Qt tests click the header buttons, activate the standard keyboard shortcuts and confirm that Ctrl+Z in the project path undoes text without changing music. The private PipeWire fixture performs five stopped project rebuilds, including pitch Undo/Redo, on its owned software server.

These are local/offscreen/software-graph checks. They do not qualify live edits, physical audio presentation, crash recovery or persisted history. The headless ASan/UBSan cohort remains separate from Qt editor instrumentation.

Project-path notifications are separate from render/status notifications. A path being typed remains intact across render ticks; Save/Open explicitly update the model path. The QML test covers persistence of both the chosen path and an appended character across polling before text Undo.

Rename dialogs reset their text-undo baseline when opened, including repeated edits of the same name. Offscreen input tests cover modal text Undo, invalid names, Cancel, return focus, step selection, saved-name equality and v3 Save/Open/playback with full-width string IDs.
