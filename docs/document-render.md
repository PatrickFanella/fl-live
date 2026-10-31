# Preparing a Session document for rendering

`fl_live_document_render` connects the Qt-free metadata model to the existing engine. `DocumentRenderPlan` owns an immutable metadata snapshot and independent copies of supplied clip content. Prepare it on a worker/control owner, then keep it alive until every engine/controller borrowing `plan()` has stopped and been destroyed. The object cannot be copied or moved, so its plan address stays stable.

Each `DocumentClipSource` supplies one stable clip ID and an engine `AudioClip` or `MidiClip`. Source order is independent of document order. Every document clip needs exactly one source; duplicates, missing content and orphan replacements are rejected. Audio tracks require audio content and MIDI tracks require MIDI content. Audio is mono PCM already at the selected session rate; MIDI positions and durations are frames. The adapter does not decode, resample or infer source rates.

Preparation validates the document, rate/tempo, source identity/count, track kind and content budgets before copying media. It then applies the engine's sample/note/voice validation outside rendering. The initial engine ceiling is 64 clips per track and 1,024 notes per clip; these are exposed engine constants with unchanged behavior. Metadata can store up to 256 clips per track, so a valid metadata document is not necessarily renderable by this engine slice. Such input fails explicitly at preparation.

The default copied-content budget is 16,777,216 mono float frames and 65,536 MIDI notes across the plan. Callers may lower either budget, including to zero, but cannot raise it above those ceilings. Subtraction checks prevent counter overflow. The budget limits the new plan's copied content, not total process memory: callers can also own source arrays, candidates and retiring plans. An omitted gain list uses unity. An optional complete ID-keyed gain list supports finite values in [0,4]; duplicates, missing tracks and count mismatches are rejected. Gains are immutable prepared values, not live automation.

## Stable IDs and render indices

The engine continues to use compact track/clip indices. `launch(ClipId, frame)` and `stop(TrackId, frame)` resolve IDs against this exact prepared snapshot and return an optional engine command. Missing IDs return no command. Engine scheduling still validates timestamps, order and queue capacity; resolving an ID is not an acknowledgement that a command was accepted or rendered.

`trackId(index)` and `clipId(trackIndex, clipIndex)` map feedback from the matching plan back to document IDs. A negative inactive/pending sentinel or invalid index returns no identity. Track order and per-track clip order may change on the next preparation, while the IDs remain stable. Resolve again against the current plan rather than reusing raw indices or interpreting an old snapshot through a new plan.

Preparation does not submit controls, commit a document or replace a live graph. A future UI must stage both the candidate document and its prepared plan before committing them. Old plans/assets remain alive until their render owner has stopped or acknowledged retirement. Metadata deletion alone does not stop a currently playing note. Scene-batch scheduling, live replacement and acknowledged retirement remain separate work.

## Verification boundary

The Qt-free tests prepare two audio and two MIDI tracks, launch by IDs above 2^53, replace/stop clips and compare output with independently calculated samples across block sizes 1, 64 and 257. Reordering tracks and clips changes render indices while preserving the intended content and ID feedback. A quantized-controller test verifies the rational next-bar boundary at 137.125 BPM and maps render-owned active feedback back to the requested ID.

Rejection cases cover missing/duplicate/orphan content, kind mismatch, non-finite/empty audio, invalid notes/voice overlap, invalid metadata, unsupported rate/tempo, renderer clip capacity and exact content budgets. Ownership tests clear/mutate source inputs after preparation and keep an old playing plan valid while preparing a deletion candidate. Explicitly stopping the old engine clears held voices; the rebuilt plan cannot resolve the deleted clip and renders silence on that empty track. This is stopped replacement evidence, not live-edit note-cleanup qualification.

The Qt Core file test saves/reopens metadata with full-width IDs, supplies the same external content to both preparations, and checks identical output across different block sizes plus an independent waveform oracle. The metadata file does not store that content, so this is ID-to-content resolution evidence, not general musical-project or asset-recovery qualification.

This boundary advances issues #26/#27/#33/#56. The desktop now uses this adapter for the authored two-track template; see [identity and rename integration](desktop-identities.md). Management controls, general media/note persistence, scene launch, mixer controls, native input and physical audio remain open. See [the validation receipt](../validation/document-render-preparation.json).
