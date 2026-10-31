# Versioned Session metadata files

`fl_live_document_io` is an optional Qt Core adapter for `SessionDocument`. It writes names, track kinds, clip ownership, scene entries, stable IDs and the allocation high-water mark. The model and engine libraries remain independent of Qt and file I/O. All encoding, validation and file operations belong on a control/worker owner, outside rendering.

The format is `fl-live-session-metadata`, version 1, with required feature `session-identities-v1`. It is a metadata transport, not the desktop's `fl-live-project` musical format. Notes, media, tempo/mixer state, parameters and devices are not supported fields. Both formats reject each other; template versions 1 and 2 continue through their existing reader and migration tests. A later general musical schema must define its own integration and migration rules.

```json
{
  "format": "fl-live-session-metadata",
  "version": 1,
  "required_features": ["session-identities-v1"],
  "next_id": "4",
  "tracks": [{"id": "1", "name": "Audio", "kind": "audio"}],
  "clips": [{"id": "2", "track": "1", "name": "Loop"}],
  "scenes": [{"id": "3", "name": "Intro", "slots": [{"track": "1", "clip": "2"}]}]
}
```

IDs and `next_id` use canonical unsigned decimal strings, including values above 2^53. Numeric JSON IDs, signs, leading zeros, whitespace and overflow are rejected. This avoids rounding the document's full-width IDs through JSON numeric consumers. Zero remains invalid under model validation. The exhausted cursor value 18446744073709551615 is representable and reopens without allowing further allocation.

The adapter requires the exact declared field set at each object level, expected array/string types, version 1 and the single supported required feature. Unknown fields/features, future versions and malformed or oversized input fail before document replacement. Model validation then checks identity uniqueness, limits, clip owners and scene references. Names must preserve valid Unicode/UTF-8 through conversion as well as satisfy the model's byte/control-character bounds.

Input and encoded output are limited to 4 MiB. Array counts are checked before constructing the corresponding entity lists. Tests exercise the maximum entity/scene-entry workload with full-width IDs and escaped 128-byte names within that byte budget. They also use a hand-authored fixture with IDs above 2^53 and the maximum cursor, independently of the encoder.

`readDocumentMetadata()` returns a fully validated candidate. `loadDocumentMetadata()` commits it through `SessionDocument::replace()` only after decoding succeeds; the current document and cursor survive a rejected load. Restoring older content retains the current higher cursor. `writeDocumentMetadata()` validates and encodes before opening the destination, then uses `QSaveFile` with direct-write fallback disabled. The caller supplies a destination it is authorized to replace. Tests verify overwrite/reopen, preservation on invalid state and failed opens, and rejected-load preservation. Process-kill, disk-full, short-write and power-loss behavior remain unqualified.

The desktop preset builds the adapter and its file tests. `document-io-sanitize` tests the Qt Core codec/model under ASan/UBSan without Qt Quick, a display or PipeWire. CI runs that cohort alongside its existing four cohorts and collects its logs/JUnit results. Engine-only builds continue without Qt.

This supplies metadata file round-trip evidence for issues #26 and #56. The authored desktop embeds this codec inside its v3 musical wrapper for the supported two-track shape. Standalone metadata files do not supply a playable general project, general musical-state migration, asset recovery or the general save/reopen/export journey. Those integration criteria remain open. See [the validation receipt](../validation/session-metadata-persistence.json).
