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
| A full control queue | "Control queue full. Try again, or use Silence now." | Everything, including an edit made at that moment | Try again; **Silence now** always works |

Plugins are not hosted yet, so there are no plugin failures to report; plugin failures belong to #72.

## Verification

- `actionableFailures` (`desktop-session`) triggers each failure kind and checks the message for the file or remote and the action. Afterwards it requires the tempo, levels, track sources (decoded audio included), Undo text, saved state and project path to be unchanged. The kinds are: a missing audio server with a named target, two Save failures, three Open failures (missing, malformed, a future feature), an unsupported import, three export failures (existing file left byte-identical, bad format, missing folder), and missing audio repaired by Relink.
- `atomic-save-faults` injects write, sync and rename failures into real saves. It now also requires the message to name the file and to say the edits are kept and to save again.

The [receipt](../validation/actionable-failures.json) records the run.
