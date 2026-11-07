# General Session tracks

The desktop no longer stops at the two-track Pulse + Steps template. A project holds up to 64 tracks, each audio or MIDI, each with exactly one clip in this slice. New projects still open as the familiar template: one audio track with the built-in pulse and one MIDI track with the step pattern.

## Model and limits

`SessionContent` sits beside the identity document (`SessionDocument`) and is keyed by the same track and clip IDs:

| Entity | Content |
|---|---|
| Project | Tempo, 20–300 integer BPM |
| Track | Level 0–100 percent. Name, kind, pan, mute and solo stay in the identity document |
| MIDI clip | One 1–16 step pattern (enabled, pitch, velocity, gate per step; hidden steps retained) |
| Audio clip | `pulse` (built-in decaying 110 Hz pulse), `empty` (silence until a WAV is imported) or `wav` (absolute path to an imported file) |

`validateContent()` requires one content entry per track and clip, matching kinds, one clip per track and at least one scene. The engine and document limits still apply: 64 tracks, 256 scenes, 128-byte names. One clip per track is a limit of this desktop slice, not of the document model; additional clip slots per track remain open under #27/#31.

Audio media is never embedded. A `wav` clip stores the absolute path it was imported from. Decoded samples are shared, immutable `AudioClip` copies, so history entries reference rather than copy them. All imported clips together must fit the preparation budget of 2^24 stored samples (about 2.9 minutes of stereo, or 5.8 minutes of mono, at 48 kHz); one import is limited to half of that.

## Track management

**Add audio track** creates an empty audio track; **Add MIDI track** creates a MIDI track whose four steps start as rests. Both get level 70 percent and are not added to any scene. **Duplicate** copies the track's level, mixer settings, clip content and scene slots under fresh IDs, names it "NAME copy" and shares any imported audio. **Delete** removes the track, its clip and its scene slots. Each is a stopped edit and one Undo step; Undo restores the original IDs, and the ID high-water mark keeps retired IDs from being reused. Deleting every track leaves a valid empty project that can be saved, reopened and launched.

Every track column shows its kind, clip source, level, pan, mute, solo, scene inclusion, launch/stop, and Duplicate/Delete. Columns sit in a horizontal strip that scrolls when they do not fit. The step editor below edits one MIDI clip at a time: **Edit pattern** on a MIDI track selects it. Choosing a pattern is view state, not an edit. When the edited track is deleted, the editor moves to the first remaining MIDI track, or shows a hint when there is none.

## WAV import

Enter a path under an audio track and choose **Import**. Decoding runs on a `WavImportWorker` thread under the [WAV admission policy](media-import.md): one or two channels, 48 kHz only, no resampling. The track shows "Importing NAME…" meanwhile; one import runs at a time. A refused file reports the importer's message ("Import failed: …") and changes nothing. A successful import replaces that track's clip source as one Undo step. If transport is running when decoding finishes, the result waits and is applied after Stop.

## Waveform and source range

An audio track with an imported WAV shows its waveform: per-pixel minimum/maximum peaks of the whole source across all channels. Peaks are computed on the GUI thread when the view needs them and cached per decoded clip and width; the render callback never prepares view data. The canvas repaints only when the source, range or width changes.

**Start ms**, **End** and **Loop** choose which source frames play. The range is in source frames at the session rate; the controls step in milliseconds (48 frames), and the last End step always reaches the true end of the file. Playback is source-rate with no warping: the selected frames are copied into the prepared clip, which loops or, with Loop off, plays once and then stops the track until it is launched again. A range must be nonempty and inside the file; invalid ranges, ranges on non-imported clips and edits while playing are refused without changing history. Each range edit is one Undo step. Importing a new file resets the range to the whole file. The shaded part of the waveform is the selected range (green looped, amber once).

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

Opening a project decodes its imported WAVs on the control thread before preparing playback. If a file became shorter than its saved range, it opens playing the whole file and the status line says the range was reset. A missing or refused file does not block opening: the clip plays silence, the track shows "Missing: NAME" and the status line lists each missing file with the reason. Saving keeps the original path, so restoring the file and reopening repairs it. Relinking to a moved file is not offered yet (#60).

## Verification

`authored-project-io` checks clip-range persistence, feature gating and bounds, and one-shot and looped range rendering at blocks 1/7/257 frames. It also checks v1/v2/v3 migration with sample-identical renders, a four-track v4 round trip covering every source kind with IDs above 2^53, render sources for resolved and unresolved media, and rejection of malformed content.

`desktop-session` adds:

- `generalTrackManagement`: add tracks, per-track patterns, worker import of a stereo WAV, refused imports, an import held until Stop, a four-track stereo export compared channel by channel with independent per-track renders, Save/Open with byte-identical re-export, a missing asset, Duplicate with new IDs and copied scene slot, Delete with editor fallback, Undo/Redo of both, and an empty project.
- `clipRangeEditing`: waveform peaks of a known ramp, range edits and refusals, an exported one-shot range and a looped range compared sample by sample, Save/Open of ranges, a file that became shorter, and re-import resetting the range.
- `qmlClipRange`: keyboard edits of Start and End, a pointer click on Loop, Undo, and a check that every row of an audio column stays inside its column.
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
