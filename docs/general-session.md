# General Session tracks

The desktop no longer stops at the two-track Pulse + Steps template. A project holds up to 64 tracks, each audio or MIDI, each with one to eight [clips](#clips). New projects still open as the familiar template: one audio track with the built-in pulse and one MIDI track with the step pattern.

## Model and limits

`SessionContent` sits beside the identity document (`SessionDocument`) and is keyed by the same track and clip IDs:

| Entity | Content |
|---|---|
| Project | Tempo, 20–300 integer BPM |
| Track | Level 0–100 percent. Name, kind, pan, mute and solo stay in the identity document |
| MIDI clip | One 1–16 step pattern (enabled, pitch, velocity, gate per step; hidden steps retained) |
| Audio clip | `pulse` (built-in decaying 110 Hz pulse), `empty` (silence until a WAV is imported) or `wav` (absolute path to an imported file) |

`validateContent()` requires one content entry per track and clip, matching kinds, one to eight clips per track and at least one scene. The engine and document limits still apply: 64 tracks, 256 scenes, 128-byte names.

Audio media is never embedded. A `wav` clip stores the absolute path it was imported from. Decoded samples are shared, immutable `AudioClip` copies, so history entries reference rather than copy them. All imported clips together must fit the preparation budget of 2^24 stored samples (about 2.9 minutes of stereo, or 5.8 minutes of mono, at 48 kHz); one import is limited to half of that.

## Track management

**Add audio track** creates an empty audio track; **Add MIDI track** creates a MIDI track whose four steps start as rests. Both get level 70 percent and are not added to any scene. **Duplicate** copies the track's level, mixer settings, clip content and scene slots under fresh IDs, names it "NAME copy" and shares any imported audio. **Delete** removes the track, its clip and its scene slots. Each is one Undo step and also works while playing (see [playback edits](#playback-edits)); Undo restores the original IDs, and the ID high-water mark keeps retired IDs from being reused. Deleting every track leaves a valid empty project that can be saved, reopened and launched.

Every track column shows its kind, clip source, level, pan, mute, solo, scene inclusion, launch/stop, and Duplicate/Delete. Columns sit in a horizontal strip that scrolls when they do not fit. The step editor below edits one MIDI clip at a time: **Edit pattern** on a MIDI track selects it. Choosing a pattern is view state, not an edit. When the edited track is deleted, the editor moves to the first remaining MIDI track, or shows a hint when there is none.

## Clips

A track holds one to eight clips (#27). The row under a track's clip button holds a selector, **+ Clip**, **Copy** and **Delete clip**.

- The **selected clip** is what the track's controls act on: its launch button, source line, Import, Relink, range and waveform, scene inclusion, and the step editor or piano roll when the track's pattern is being edited. Selecting is view state, so it is not saved or undone. Reopening a project selects each track's first clip.
- **+ Clip** adds a clip at the end of the track and selects it. A MIDI clip starts as rests, an audio clip empty.
- **Copy** duplicates the selected clip, content included, under a new ID named "NAME copy". Imported audio is shared.
- **Delete clip** removes the selected clip and the scene slots that used it, and selects the clip before it. A track keeps at least one clip; delete the track instead. If the deleted clip is playing, the track stops at the next block.
- Each of these is one Undo step and also works while playing. Undo puts a deleted clip back in its place with its ID.
- An import goes to the clip selected when it started, even if the selection changes before it finishes.
- **Include in scene** puts the selected clip in the scene's slot for that track. Export plays each track's selected clip.

## WAV import

Enter a path under an audio track and choose **Import**. Decoding runs on a `WavImportWorker` thread under the [WAV admission policy](media-import.md): one or two channels, 48 kHz only, no resampling. The track shows "Importing NAME…" meanwhile; one import runs at a time. A refused file reports the file and the importer's reason ("Could not import NAME: …") and changes nothing. A successful import replaces that track's clip source as one Undo step. If transport is running when decoding finishes, the result waits and is applied after Stop.

## Waveform and source range

An audio track with an imported WAV shows its waveform: per-pixel minimum/maximum peaks of the whole source across all channels. Peaks are computed on the GUI thread when the view needs them and cached per decoded clip and width; the render callback never prepares view data. The canvas repaints only when the source, range or width changes.

**Start ms**, **End** and **Loop** choose which source frames play. The range is in source frames at the session rate; the controls step in milliseconds (48 frames), and the last End step always reaches the true end of the file. Playback is source-rate with no warping: the selected frames are copied into the prepared clip, which loops or, with Loop off, plays once and then stops the track until it is launched again. A range must be nonempty and inside the file; invalid ranges and ranges on non-imported clips are refused without changing history. Each range edit is one Undo step. Importing a new file resets the range to the whole file. The shaded part of the waveform is the selected range (green looped, amber once).

## Playback edits

These edits apply while the transport runs or a control is unacknowledged: track add, duplicate and delete; level, pan, mute and solo; renames; scene create, copy, order, delete and slot inclusion; clip source ranges; MIDI steps, pattern length and synth settings; automation points; and Undo/Redo between such states. Each prepares the new document on the GUI thread and hands it to the running controller as a [plan replacement](plan-replacement.md). It activates at the next audio block without moving the playhead, and the status line reads "Edit applied while playing".

- A playing clip keeps playing when its track and clip survive the edit, at the same position, with the edited content, level or pattern. Sounding notes finish under the new synth settings; notes already passed are not replayed.
- Deleting a playing track stops its sound and its queued launches. Undo restores the track and its clip, not its playback.
- Launches and stops queued before an edit still apply, and ones made after it use the edited document. If a newer edit has not been handed over yet, Launch, Stop and scene actions are refused with "Applying the last edit to playback. Try again in a moment."
- A burst of edits keeps one replacement in flight. A newer edit supersedes one that has not been handed over yet, so the render thread sees the latest document, and every edit still gets its own Undo step.

When the control queue is full, Launch, Stop and scene actions report "Control queue full. Try again, or use Silence now." An edit made then is kept and handed over once there is room. **Silence now** never waits for the queue.

Tempo changes, Open, New, Restore, Collect audio, Export, WAV import and Relink still require stopped transport; the tempo field and those buttons are disabled while playing. A tempo change by Undo/Redo while playing is refused with "Stop transport to change the tempo. Other edits apply while playing." The document and history stay unchanged. Edits made while stopped still rebuild the session at the start, as before.

`qmlPlaybackEditJourney` walks the whole path through the QML. It launches both clips and presses Play, then, while playing, draws a level ramp, mutes a track and toggles a step. It undoes with the button, presses Ctrl+. (Silence now), saves and reopens. The float export follows the drawn ramp sample for sample. The [receipt](../validation/playback-edit-journey.json) records it.

## Project files (version 4)

Save writes `fl-live-project` version 4 with required feature `general-session-v1`:

```json
{
  "format": "fl-live-project",
  "version": 4,
  "required_features": ["general-session-v1"],
  "tempo": 150,
  "session": { "format": "fl-live-session-metadata", "version": 1, "...": "identity metadata" },
  "tracks": [{"id": "1", "level": 70}, {"id": "2", "level": 25}, {"id": "7", "level": 70}],
  "clips": [
    {"id": "3", "audio": "pulse"},
    {"id": "4", "pattern": {"length": 4, "steps": ["16 step objects"]}},
    {"id": "8", "audio": "wav", "path": "/home/me/loops/drums.wav"}
  ]
}
```

When any imported clip has a range other than "whole file, looped", the file also declares required feature `clip-ranges-v1` and every `wav` clip carries `"range": {"start": FRAME, "length": FRAMES, "loop": BOOL}`, where length 0 means to the end of the file. Files without ranges keep the plain form, so they still open in builds that predate ranges.

Every object has an exact field set. MIDI clips carry `pattern`; audio clips carry `audio` (`pulse`, `empty` or `wav`) and, for `wav` only, an absolute `path`. Unknown versions, features, fields, sources, relative paths, kind mismatches, missing or duplicate entries and out-of-range values fail before the current project is replaced. Versions 1–3 still open and migrate to the same content; their rendered samples are unchanged. Older releases refuse version 4.

### Asset locations, relinking and collecting

Imported audio is referenced, never embedded, and fl-live never writes to a source file. When an imported file lies inside the project file's folder, Save also records its path relative to that folder (required feature `asset-relative-v1`, per-clip field `relative`, which must stay inside the folder). Opening resolves each file in a fixed order:

1. The saved absolute path, whenever that file exists. A copied project therefore keeps using the original audio rather than silently switching to the copy's.
2. Only if the absolute path is gone, the recorded relative location next to the project file. A file found there is reported ("Found moved audio inside the project folder …"), and the project opens marked unsaved until Save records the new path.
3. Otherwise the clip is missing. Nothing is searched for by file name, so a different file with the same name is never picked up.

**Relink** (shown on imported or missing audio tracks) points a clip at another file and keeps its range and loop setting. It decodes on the worker, is one Undo step, and is refused when the new file is shorter than the range ("Import it instead to replace the clip"). **Collect audio** copies imported files that live outside the project folder into `NAME-assets/` beside the saved project and repoints those clips as one Undo step. It never overwrites: a different file with the same name gets a `-2`, `-3` … suffix, and a byte-identical file already there is reused. Undo restores the old paths but leaves the copies in place. The project must have been saved first.

Opening a project decodes its imported WAVs on the control thread before preparing playback. If a file became shorter than its saved range, it opens playing the whole file and the status line says the range was reset. A missing or refused file does not block opening: the clip plays silence, the track shows "Missing: NAME" and the status line lists each missing file with the reason. Saving keeps the original path, so restoring the file and reopening repairs it; so does **Relink**.

## Verification

`authored-project-io` checks clip-range persistence, feature gating and bounds, and one-shot and looped range rendering at blocks 1/7/257 frames. It also checks v1/v2/v3 migration with sample-identical renders, a four-track v4 round trip covering every source kind with IDs above 2^53, render sources for resolved and unresolved media, and rejection of malformed content.

`desktop-session` adds:

- `generalTrackManagement`: add tracks, per-track patterns, worker import of a stereo WAV, refused imports, an import held until Stop, a four-track stereo export compared channel by channel with independent per-track renders, Save/Open with byte-identical re-export, a missing asset, Duplicate with new IDs and copied scene slot, Delete with editor fallback, Undo/Redo of both, and an empty project.
- `clipRangeEditing`: waveform peaks of a known ramp, range edits and refusals, an exported one-shot range and a looped range compared sample by sample, Save/Open of ranges, a file that became shorter, and re-import resetting the range.
- `qmlClipRange`: keyboard edits of Start and End, a pointer click on Loop, Undo, and a check that every row of an audio column stays inside its column.
- `assetRelocationRelinkCollect`: a project folder moved with its audio (relative recovery, unsaved until Save), a copied folder that keeps the original absolute file, a missing file relinked with its range kept, a too-short relink refused, Undo/Redo, and Collect with a name collision, unchanged sources, reuse of identical bytes and refusal before the first Save.
- `qmlRelinkMissingAudio`: typing a path and clicking Relink on a missing track restores playback.
- `trackLimit`: 64 tracks, refused 65th add and duplicate without history change, Save/Open.
- `qmlTrackManagement`: actual pointer input on Add audio/MIDI track, Import, Edit pattern, step editing, Duplicate, Delete, Undo, clip launch and Play.

## Scheduling and failure regressions (#33)

`general-session-regressions` (desktop and `document-io-sanitize`, so also under ASan/UBSan) prepares a four-track general project through `contentSources`/`contentGains`: the pulse, the template pattern, an imported stereo clip with a looped 1–5 frame range, and a bass pattern. Tempo is 137 BPM, so a beat is 21,021.9 frames and bar boundaries round upward.

- The full scene launches at bar 1 (frame 84,088), a sparse scene replaces it mid-bar (stopping three tracks), and the scene stops at bar 3 (frame 252,263). Stereo output is identical at blocks of 1, 64, 128, 257 and 512 frames. Silence before bar 1 and after the stop, and the exact looped range after the replacement, are checked against independent values. MIDI voices are present before the replacement and zero after it.
- A full control queue reports `full` instead of dropping a control. Panic is still delivered and leaves no clip or sample playing, and fresh controls are then accepted.
- Deleting a playing MIDI track removes its clip and scene slots from the next plan. The old plan's held note continues only until its render owner stops, and a fresh engine starts silent.
- A missing WAV prepares as a launchable silent loop. Invalid content (range outside the file, `wav` without a path, a missing clip entry, level 101, a NaN inside the played range) is refused without affecting the existing plan. Samples outside the selected range are not validated by preparation; the importer already refuses non-finite files.

## D2 journey qualification (#37)

`qmlD2Journey` drives the whole D2 workload through the production QML with pointer and keyboard input only:

1. Type WAV paths and Import into the template audio track (mono) and a newly added audio track (stereo); add a MIDI track.
2. Edit both MIDI patterns: notes, pitch by arrow keys, gate, and a rest.
3. Set tempo 128 by typing, adjust levels by arrow keys, type pans L40 and R20, and type a 50 ms clip end.
4. Build four scenes with New scene, Copy arrangement and Include in scene.
5. Launch two scenes with Immediate timing while playing, then Stop.
6. Type a project path, Save and close the window.
7. In a fresh session and window, Open the project and check track IDs, kinds, sources, tempo, levels, pans, clip range, every scene's membership and both patterns.
8. Export two bars through the UI and decode the stereo WAV independently. Each channel must match per-track renders mixed with the declared levels and balance pan law within 1 LSB.

Four tracks and four scenes are the qualification workload, not a product limit (`trackLimit` covers 64 tracks; scenes allow 256). Persistence evidence for #13 is in [project files](#project-files-version-4) and the [interrupted-save checks](projects.md#interrupted-and-failed-saves).

These are offline and offscreen checks. Native Wayland input, physical audio and the D2 save/reopen/export journey qualification (#37) remain separate.
