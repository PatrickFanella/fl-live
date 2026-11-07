# Failures and recovery

Every failure the desktop can report names what it was working on, and says what is kept and what to do next (#82). None of them changes the open document, its Undo history or decoded audio, unless the text says so.

| Failure | Message starts with | What is kept | What to do |
|---|---|---|---|
| Open: missing, unreadable, malformed or unsupported file (including an unknown required feature or an invalid device chain) | "Could not open NAME: reason" | The open project, its path and history | Check the file or choose another |
| Open with unsaved edits | "Save your changes before opening another project." | Everything | Save or undo, then open |
| Save: missing folder, write, sync or rename failure, or another existing file | "Could not save NAME: reason" | The edits, history and the previous file on disk | Choose another path or fix the folder, then save again |
| Import: missing file or unsupported WAV (rate, format, size) | "Could not import NAME: reason" | The track's current audio | Choose a supported WAV, or relink |
| Missing audio when opening | "Project opened. Missing audio plays as silence; relink or import it: NAME (reason)" | The reference to the file | **Relink** to the moved file, or import another |
| Export: bad settings, existing file, missing folder, write failure | "Could not export NAME: reason" | The document; any existing file is left as it was, and no partial file remains | Adjust the settings or choose another path |
| Audio server missing or lost | "reason (PipeWire remote R, target T). Audio is stopped; your project and history are kept." | The document and history; Save still works | Restore the server or output, then **Retry audio** |
| Record: no armed track, unsaved project, input stream cannot open, take file cannot be created | "Could not start recording: reason. Nothing was recorded." | Everything; no file is left | Arm a track, save the project, or fix the input, then record again |
| A take that fails to write, cannot be read back, lost its track or would exceed eight clips | "… the take is kept as PATH" or "Writing the recording failed: … kept in NAME.part" | The document, and the take's file in `recordings/` | Import or relink the kept file if you want it as a clip |
| Recover takes: a `.part` file that is not a float WAV take, has an incomplete header or no complete frame | "Cannot recover NAME.part: reason" | The `.part` file, unchanged, still listed | Delete it, or keep it for other tools |
| Recover takes: no journal, track gone, eight clips, or the recovered file cannot be decoded | "… the recovered take is kept as PATH" | The recovered file; the document is unchanged for that take | Import or relink the kept file |
| Server or output lost while recording | "… Audio is stopped; your project and history are kept. … N take(s) will be added as clips after Retry audio." | The document and every finished take file | Restore the server, then **Retry audio** |
| Recording input lost (node removed) | "The recording input was lost (PipeWire remote R). Recorded N take(s) as new clips. Arm a track again to reopen it." | The takes, as clips; playback continues | Arm a track again |
| MIDI take with no notes, or notes past 16 bars, beyond 16 sounding or beyond 1,024 | "No notes were played on TRACK" or "TRACK kept the first 16 bars: …" / "left out N notes …" | Every note that fits, as a clip | Record shorter or thinner takes |
| A full control queue | "Control queue full. Try again, or use Silence now." | Everything, including an edit made at that moment | Try again; **Silence now** always works |

Plugins are not hosted yet, so there are no plugin failures to report; plugin failures belong to #72.

## Verification

- `actionableFailures` (`desktop-session`) triggers each failure kind and checks the message for the file or remote and the action. Afterwards it requires the tempo, levels, track sources (decoded audio included), Undo text, saved state and project path to be unchanged. The kinds are: a missing audio server with a named target, two Save failures, three Open failures (missing, malformed, a future feature), an unsupported import, three export failures (existing file left byte-identical, bad format, missing folder), and missing audio repaired by Relink.
- `atomic-save-faults` injects write, sync and rename failures into real saves. It now also requires the message to name the file and to say the edits are kept and to save again.

The [receipt](../validation/actionable-failures.json) records the run.
