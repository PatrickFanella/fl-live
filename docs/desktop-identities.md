# Desktop identity and rename integration

The authored two-track desktop now uses `DocumentRenderPlan` for playback. Its audio and MIDI clips come from the existing Pulse + Steps generator, with the original track levels passed as complete ID-keyed gain bindings. The prepared plan owns the source samples and metadata; no GUI object or history snapshot reaches the callback. Tests compare its rendered samples exactly with the previous source plan at different block sizes.

Track/clip IDs cross QML as canonical decimal strings. Launch and track-stop controls resolve those IDs against the current prepared plan; render-owned active indices map back through that same plan. Compatibility index methods remain for existing callers, but the production QML launch/stop handlers use IDs. Names are display data and do not identify commands. Files with IDs above 2^53 are covered through actual offscreen pointer/key input, Save/Open and playback feedback.

## Rename workflow

While stopped with no pending control acknowledgements, choose Rename beside a track title, clip or scene. The modal selects its current name; Enter or Apply commits, and Cancel/Escape leaves the document unchanged. Invalid names leave the dialog open with an error. Closing restores focus to the invoking control and preserves the selected MIDI step. Display labels render names as plain text.

Each committed rename is one document-history entry. Undo/Redo restore names and IDs alongside tempo/levels/pattern settings. Renaming to the existing name adds no history entry. The dialog starts a fresh text-undo boundary on every opening, including when the previous text equals the current name. Standard document shortcuts are disabled while it is open so text Undo cannot change the project beneath it.

The grid remains the declared authored template: one audio track, one MIDI track, one clip per track and one scene with independently included/excluded clips. Scene launch/stop now offers explicit Immediate/One bar timing; see [scene controls](scene-controls.md). Add, duplicate, delete and arbitrary media remain work under issue #27 and its dependent units. Metadata documents with other valid shapes are rejected by this template's file/preparation adapter rather than partially opened.

## Version 3 authored project files

Desktop Save now writes `fl-live-project` version 3 with exactly these fields:

| Field | Contract |
|---|---|
| `format` | `fl-live-project` |
| `version` | Numeric integer 3 |
| `required_features` | `["template-identities-v1"]`, plus `"template-scene-slots-v1"` when the scene has empty slots |
| `settings` | The complete source-settings object, canonically written in existing v2 form |
| `session` | The complete version-1 Session metadata object, including IDs and allocation cursor |

The Qt Core `fl_live_authored_project` adapter reads legacy v1/v2 projects through the retained source parser and supplies deterministic initial identities. A subsequent desktop Save writes v3. Old source-only readers reject v3 safely. The source-only `readProject`/`writeProject` functions remain for legacy source fixtures; desktop persistence uses `readAuthoredProject`/`writeAuthoredProject` so names are not silently discarded.

Input/output is capped at 4 MiB plus 64 KiB. Source settings and metadata retain their own validation and byte limits. Unknown root fields, future versions/features, malformed source/metadata and unsupported template shapes fail before graph/document replacement. Saving validates and encodes before atomic `QSaveFile` replacement with direct-write fallback disabled. The existing normalized-path ownership guard still controls overwrite/Save As. A standalone metadata file is not an authored musical project and both readers reject the other format.

Version 3 preserves all supported source settings, names and identities. It does not introduce imported assets, arbitrary note content, device state or a general Session schema. Export remains the same authored two-track mix; names do not change its PCM. Musical history, transport/queued state and editor selection are not serialized.

## Verification boundary

Qt Core tests under ASan/UBSan cover v1/v2 migration, v3 save/reopen, full-width string IDs, unsupported-feature/version/shape rejection and unchanged source audio with preserved gains. Offscreen Qt tests drive rename dialogs with mouse/keyboard input, Undo/Redo and modal text Undo, invalid/cancel paths, focus/step preservation, Save/Open and stable-ID playback. The private PipeWire fixture renames through QML before captured A4 playback and retains names/IDs through server loss, v3 saving and explicit stopped reconnect.

These checks do not qualify native Wayland input, physical output, live editing, general track management or crash/power-loss recovery. History remains bounded to 128 edits on the GUI owner; metadata snapshots are immutable shared values and history assignment is statically required not to throw. Candidate allocation/preparation happens before graph commit; backend teardown still joins processing before old plan destruction. See [the dated receipt](../validation/desktop-identity-renaming.json).
