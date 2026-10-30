# Session document identity model

`fl_live_document` provides a Qt-free, backend-free metadata model for issue #26. One control or worker owner edits `SessionDocument`. It owns track, clip and scene names, track kinds, clip ownership and scene-slot references. Media samples, notes, mixer values and playback scheduling are later integration work. The desktop still uses its template project format; this model is not yet its live document.

`TrackId`, `ClipId` and `SceneId` are distinct C++ types wrapping unsigned 64-bit values. Zero is invalid. One document-local cursor allocates values across all three entity kinds, and validation rejects duplicate values even across kinds. Names and vector positions do not determine identity. Different documents may have the same numeric IDs; cross-document import needs an explicit remapping policy.

## Operations

- Create allocates a new ID; rename retains it. Duplicate names are allowed.
- Duplicate clip creates a fresh clip ID on the same track and leaves it unassigned to scenes.
- Duplicate track creates a fresh track ID and fresh IDs for all owned clips. Each scene slot for the original track gains a corresponding slot for the new track and copied clip.
- Duplicate scene creates a fresh scene ID and fresh IDs for its referenced clips, retaining their track owners. It does not alias the original scene's clips.
- Delete clip removes its scene references. Delete track removes its clips and slots. Delete scene leaves clips available on their tracks.
- Set slot replaces that track's entry in the selected scene. A slot must reference a clip owned by that track. Clear slot accepts an existing track with no assignment as a no-op; unknown entities are errors.

Missing scene slots carry no playback meaning yet. Stop/launch policy belongs to scene scheduling work. These operations do not run against an active engine and do not establish note cleanup during playback.

## Validation and limits

The initial limits are 64 tracks, 256 scenes, 4,096 clips total and 256 clips per track. A scene has at most one slot per track. The track ceiling is checked against the engine capacity in the test suite. These are implementation limits, not the four-track/four-scene MVP qualification workload.

Names contain 1–128 bytes and reject ASCII control characters and DEL. The model does not normalize Unicode or validate UTF-8; a future codec/UI adapter must define its text admission policy. A track's kind is audio or MIDI. Clips require existing owners; slots require existing clips on the specified track. Invalid IDs, stale allocation cursors, unknown kinds, dangling references, duplicate slots and exceeded limits are rejected.

Each operation copies a candidate, performs the edit, validates it and swaps it into place. Validation, allocation or identity exhaustion can throw before commit; the previous document and its ID cursor survive. This path allocates, copies and destroys metadata and must stay outside the audio callback. Borrowed references from `snapshot()` must not be retained across edits; copy a snapshot when a stable value is needed.

## Snapshot and persistence boundary

`DocumentSnapshot` transfers the full metadata state, including IDs and `next_id`. Constructing a document from a snapshot validates it before use. `replace()` validates an incoming snapshot before commit and retains the greater of its allocation cursor and the current cursor. Restoring older content therefore cannot recycle IDs allocated later in that document's lifetime. A new document constructed from a saved snapshot only knows the high-water mark in that snapshot.

Tests cover snapshot reconstruction and identity retention, including cursor exhaustion partway through a duplicate operation. The optional [Qt Core metadata adapter](session-metadata.md) now stores this metadata boundary losslessly and validates it before document replacement. The general musical schema, template migration, Undo integration, asset admission and actual save/reopen/export journey remain work under the persistence epic.

UI operations and rendering must translate stable IDs through a validated prepared plan. Removing entities from a metadata snapshot is not authority to destroy assets still used by rendering. The existing single render owner, bounded commands, panic and acknowledged plan-retirement contracts remain the integration requirements. Issue #26 retains its integration boundary; issue #27 covers management controls. The C++ scene member is named `entries` to avoid Qt's `slots` macro; the serialized metadata key is `slots`.
