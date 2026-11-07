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

Every object has an exact field set. MIDI clips carry `pattern`; audio clips carry `audio` (`pulse`, `empty` or `wav`) and, for `wav` only, an absolute `path`. Unknown versions, features, fields, sources, relative paths, kind mismatches, missing or duplicate entries and out-of-range values fail before the current project is replaced. Versions 1–3 still open and migrate to the same content; their rendered samples are unchanged. Older releases refuse version 4.

Opening a project decodes its imported WAVs on the control thread before preparing playback. A missing or refused file does not block opening: the clip plays silence, the track shows "Missing: NAME" and the status line lists each missing file with the reason. Saving keeps the original path, so restoring the file and reopening repairs it. Relinking to a moved file is not offered yet (#60).

## Verification

`authored-project-io` checks v1/v2/v3 migration with sample-identical renders, a four-track v4 round trip covering every source kind with IDs above 2^53, render sources for resolved and unresolved media, and rejection of malformed content.

`desktop-session` adds:

- `generalTrackManagement`: add tracks, per-track patterns, worker import of a stereo WAV, refused imports, an import held until Stop, a four-track stereo export compared channel by channel with independent per-track renders, Save/Open with byte-identical re-export, a missing asset, Duplicate with new IDs and copied scene slot, Delete with editor fallback, Undo/Redo of both, and an empty project.
- `trackLimit`: 64 tracks, refused 65th add and duplicate without history change, Save/Open.
- `qmlTrackManagement`: actual pointer input on Add audio/MIDI track, Import, Edit pattern, step editing, Duplicate, Delete, Undo, clip launch and Play.

These are offline and offscreen checks. Native Wayland input, physical audio and the D2 save/reopen/export journey qualification (#37) remain separate.
