# Scene launch and stop

The desktop scene row launches or stops both authored tracks together. Choose **Immediate** or **One bar**, then **Launch scene** or **Stop scene**. Launching while stopped queues the scene; press Play to render it. Scene stop clears clips and MIDI voices while transport continues. Transport Stop and Silence now still stop transport and clear pending work.

The desktop retains one audio track and one MIDI track, with 1–256 scenes sharing their authored clips. Each clip has an **Include in scene** checkbox, editable while stopped. An excluded track stops when this scene launches; its clip remains available for individual launch. An empty scene stops both tracks while transport continues. Scene Rename uses the existing stopped modal, focus restoration, document Undo/Redo and v3 Save/Open path. It preserves the selected MIDI step and full-width string ID. The Scenes selector and New scene/Delete scene controls manage arrangements. Track/clip create/duplicate/delete and scene duplication with independent clip content remain open under #27. Launch/stop timing is a view choice, initially One bar; it does not change the document or persist in project files. Individual clip/track buttons retain their one-bar timing.

## Render-owned timing and ordering

Immediate means the current engine position when the render owner consumes the control, before that callback's first rendered sample. It does not mean the GUI click's wall-clock time or hardware presentation time. One bar uses the first rational 4/4 boundary at or after that same position. The grid is inclusive: position zero and an exact boundary apply there. Fractional bar lengths round upward independently rather than accumulating rounded lengths.

`DocumentRenderPlan::scene(SceneId, stop, frame)` resolves a scene against its immutable document and returns one command per prepared track. Launch uses the scene's referenced clip; an unfilled slot stops that track. Scene stop stops every prepared track. Unknown IDs return no batch. A valid scene in a zero-track document produces an empty batch, which the controller rejects as a no-op. The desktop's validated template always has two tracks; its scene can have zero, one or two filled slots.

The GUI copies the resolved commands into one bounded `ControlKind::scene` queue value. The render owner chooses one time for the entire group, ignoring incoming command timestamps. `Engine::scheduleTogether` validates every command and available capacity before inserting anything. A failed group leaves the existing schedule and activity intact. One rejection increments the aggregate rejected count once, and the control serial is acknowledged even on rejection. A full producer queue rejects the entire message before submission.

Scene groups can be inserted before already queued later actions. Existing events at the same sample run first, followed by the new group's commands in their supplied order. Multiple scene controls consumed at the same position therefore retain arrival order. Immediate actions keep future queued actions; they do not cancel them. An immediate stop can silence a scene now while an earlier queued one-bar launch still starts it later. Transport Stop or Silence now clears that future work. The older single-command `schedule` API retains its ordering rejection for callers that submit an earlier timestamp after a later one.

## Feedback, ownership and limits

Scene feedback distinguishes an unhandled Launch/Stop requested serial from render-owned activity. After acknowledgement, Active means all included clips match the active indices and excluded tracks are stopped in the coherent snapshot. Stopped means every track is inactive; Partial means the tracks differ from the full scene. An “actions queued” suffix reflects pending track actions, including requests from individual clip buttons. Feedback is published after each callback; a queued action is not predicted active before its sample is rendered. The aggregate status does not expose a separate outcome for every historical scene request.

The GUI remains the sole producer. The backend remains the sole render owner; silent preview renders on the GUI owner. The queue value contains fixed arrays and indices, with no Qt objects, strings, media ownership or history pointers. Admission and rendering use fixed-capacity storage without allocation, locks, file access or logging. The immutable prepared plan still outlives its engine/controller. Backend teardown joins processing before plan destruction. A scene replacement resets the target tracks' runtime before rendering the boundary sample, including held MIDI voices.

The producer queue holds 64 controls. The engine schedule holds 64 individual actions, so one 64-track scene consumes the whole schedule when it is otherwise empty. Pending groups can exhaust that budget sooner than the control queue. Admission never schedules a subset to make room. Silence now remains an out-of-band recovery action even when the control queue is full. These bounds establish finite work and storage; they do not qualify hardware callback deadlines.

## Verification

`scene-scheduling` uses two audio and two MIDI tracks with IDs above 2^53. Independent sample checks cover Immediate and One bar at 137.125 BPM with blocks 1, 64 and 257, including exact launch/stop samples and independently calculated first/second rational boundaries. Other cases cover reordered IDs and scene entries, empty-slot note cleanup, immediate insertion ahead of a queued scene, same-frame launch/stop/replacement order, ring wrap, invalid final commands, both queue-capacity failures, panic/retry and the full 64-track engine limit. Instrumented allocation checks also exercise both scene policies.

Offscreen Qt input renames a scene, saves/reopens its wide ID, chooses timing through the ComboBox, launches by mouse and stops by keyboard. Session tests cover requested/queued/active feedback, history, invalid IDs/policies, stopped queueing and full-queue recovery. The default window keeps the step controls visible; smaller windows retain the scrolling editor.

The private PipeWire workflow invokes the production QML scene controls, excludes the audio track, requires only MIDI active, and isolates the MIDI track for an independent 440 Hz/0.02-peak capture. It acknowledges scene stop, then keeps transport running for 350 ms; the recording must end in silence. Server loss and explicit stopped reconnect retain the scene name, ID, slot choices and document history without resuming playback. This is software-graph evidence, separate from offscreen pointer/key input. Native Wayland, physical output, latency, lossless live-quantum delivery, performance and release qualification remain open. See [the validation receipt](../validation/scene-controls.json).

## Editing scene slots

The Include in scene checkboxes use stable track IDs and the existing document set/clear-slot operations. Each changed checkbox is one Undo/Redo entry; no-ops and invalid IDs add nothing. Editing requires stopped transport and handled control messages. Replacement validates and prepares the complete candidate before commit, clears old launches and returns to frame zero. Selection and clip/source content survive. Slot edits never reach a live callback or delete an asset.

Save/Open retain the choices. An incomplete scene writes version 3 with required features `["template-identities-v1", "template-scene-slots-v1"]`. A full scene keeps the original single feature. The reader accepts those exact feature lists and rejects an incomplete scene that omits its feature declaration. Legacy v1/v2 and original full-scene v3 files retain their existing meaning. Older v3 readers reject the new feature safely.

An empty scene reports Stopped after rendering, while a nonempty scene can report Active with excluded tracks stopped. An individual launch on an excluded track makes the scene Partial; launching the scene again stops that track. Export both tracks continues to export the authored source mix regardless of scene membership. General track/clip management and independent clip duplication remain open. See [the slot-editing receipt](../validation/scene-slots.json).

## Creating, selecting and deleting scenes

New scene adds an empty scene with a fresh stable ID and selects it. Rename and Include in scene edit the selected scene. The Scenes selector changes only view selection; it works while playing without submitting a launch or changing dirty state. Launch scene resolves the selected ID, and feedback compares the current render state with that scene. Unhandled request labels belong to the scene that submitted them. The selector model updates only on document replacement, so render polling does not reset its popup or keyboard position. Names render as plain text.

Delete scene removes only that scene and its slot references; clips and source content remain available. At least one scene must remain. Deleting the selected scene chooses its previous neighbor, or the next scene when deleting the first. Deleting another scene preserves selection. Creation/deletion require stopped transport with handled controls and use the same staged preparation as other edits. Their Undo/Redo entries restore both document content and selection context. Undoing creation returns to its prior selection; undoing deletion selects the restored scene. Redo restores the selection chosen by the original edit. Slot/name/scalar edit history also restores its scene context; selecting a scene alone creates no history entry.

A GUI-owned allocation high-water mark never decreases during history traversal or a redo fork. Save writes that cursor even after undoing creation, so Save/Open cannot recycle an issued ID. Dirty state compares musical settings, tracks, clips and scenes, excluding that allocator bookkeeping. Returning to saved content is clean even if the cursor advanced. Open starts a new document namespace, resets history and selects the first stored scene; view selection is not serialized. The high-water mark is never used or modified by rendering.

Multiple-scene files add required feature `template-scenes-v1`; scenes with unfilled slots also require `template-scene-slots-v1`. Features are written in canonical order after `template-identities-v1`. Readers reject missing declarations, duplicates and unknown features before document replacement. A one-scene file retains its older feature set. The two source clips and their settings remain shared across scenes; this feature creates arrangements, not independent clip copies.

Qt tests cover scene creation/selection/deletion, neighbor selection, Undo/Redo, redo forks and non-recycled IDs through Save/Open, the 256-scene limit, invalid IDs, pending/running edit refusal and selected-scene playback. Actual mouse/key input uses the selector, New/Delete, history buttons and Save/Open. The private QML/PipeWire workflow creates a second scene and captures its MIDI-only arrangement; server-loss recovery retains multiple scenes and source/slot history. The Qt Core sanitizer cohort covers feature combinations and persistence. See [the scene-management receipt](../validation/scene-management.json). Native/physical and general track-management qualification remain open.

## Copying and ordering arrangements

**Copy arrangement** creates and selects a fresh scene named Scene copy. It copies the source scene's slot choices into a separate scene, so including or excluding a clip in the copy leaves the original unchanged. Both scenes reference the existing authored source clips: MIDI/source settings remain shared. The button tooltip declares that sharing. This differs from the general document's `duplicateScene` operation, which also creates fresh clip identities. General independent clip authoring remains open.

Copy uses the same 256-scene limit, stopped preparation, history and ID high-water mark as New scene. Undo removes the copy and restores the previous selection; Redo restores the same copy ID and selects it. Copying after Undo, including after Save/Open, allocates a new ID rather than reusing a retired one. Invalid/missing IDs, pending/running edits and capacity failures preserve the document/history.

**Move up** and **Move down** swap a scene with its neighbor in the stored scene list. They preserve scene IDs, names, slot choices and clip/source IDs. The moved scene stays selected, and Undo/Redo restore order and selection context. End-of-list moves and directions other than -1/+1 are refused without an edit. Save/Open preserve the order; Open selects its first scene as before. These operations use the existing multiple-scene feature declarations, with no format or engine change.

Desktop tests copy a MIDI-only scene, change only the copy's slot choices, reorder and reopen it, then launch both arrangements and check the intended active clips. They also cover history, limits and ID preservation through a redo fork and Save/Open. Actual QML input clicks Copy arrangement, reorders by pointer and keyboard, and exercises Undo, deletion and persistence. The private PipeWire workflow copies and reorders its MIDI-only scene before independently captured A4 playback, and the server-loss fixture retains an arrangement copy. See [the copy/order receipt](../validation/scene-copy-order.json). These are offscreen/private-software checks; native input and physical output remain unqualified.

## General sessions

The scene policy, timing and feedback above apply unchanged to general sessions with any number of tracks and scenes. `general-session-regressions` checks sample timing with four tracks at 137 BPM, where bar boundaries are not integral, at blocks 1, 64, 128, 257 and 512. It also covers an Immediate sparse replacement that releases MIDI voices, a scene stop, and controller saturation with panic.

`generalSceneFeedback` (`desktop-session`) follows the feedback on four tracks and three scenes:

- A One bar launch reads "Launch requested" until the render owner acknowledges it, then "Stopped · actions queued" with every track "Launch queued", then "Active" at the boundary.
- An Immediate replacement keeps two tracks and stops the other two in one block.
- With the control queue full, the request is refused with "Control queue full" and no scene reads "requested".
- With the engine's 64 action slots already taken by queued launches, a four-track scene is rejected as a whole. The request clears, the message says it was rejected, and no track shows a partial or false active state.
- After panic, the same scene launches.
- Unknown scenes and policies are refused without any requested state.

The [receipt](../validation/general-scene-feedback.json) records the run.
