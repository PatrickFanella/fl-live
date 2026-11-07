# Recording

Recording (epic #11) is being built from the inside out. This page covers what exists now: the capture pipeline that takes audio from the audio callback to a WAV file (#44), the PipeWire input stream that feeds it, and audio recording in the desktop: arming, takes and recorded clips (parts of #43, #46 and #47). Not built yet: MIDI recording, input monitoring, choosing an input inside the app, timestamps and latency compensation (#45), finishing takes on device loss (#46) and recovery after a crash (#48).

## Capture pipeline

`CaptureTake` (`include/fl_live/capture.hpp`) records one take of one or two channels at the session rate:

- **Callback side.** `capture()` copies one block of interleaved samples into a preallocated single-producer/single-consumer ring. It does not allocate, lock, wait or touch files. A block is accepted or refused whole.
- **Worker side.** A worker thread empties the ring every few milliseconds and appends 32-bit float WAV frames to `NAME.part`. `finish()` stops accepting, writes everything the ring still holds, closes the file and renames it to `NAME`.
- **Overflow.** If the worker falls behind and a block does not fit, the block is dropped whole and counted (`frames_dropped`, `blocks_dropped`), and the take is marked overflowed. The file holds every block that fit, in order. Dropped blocks leave no gap marker, so the file's timeline is shorter than the take by exactly the dropped frames.
- **Write failure.** If a write fails (for example a full disk), writing stops, the error names the `.part` file, and the frames written before the failure stay in it. `finish()` closes that file with a valid header and does not rename it. Capture keeps returning immediately.
- **Never replaces.** A take refuses to start when `NAME` or `NAME.part` already exists, and keeps the `.part` name if `NAME` appears while recording.

## PipeWire input stream

`PipeWireCapture` (`include/fl_live/pipewire_backend.hpp`) connects to the explicit remote as node `fl-live-input` with one (MONO) or two (FL, FR) 32-bit float ports at the session rate. Like the output, it links only where it is told: a target, autoconnect, or an external link. On PipeWire's data thread each buffer goes to the current `CaptureSink`, such as a `CaptureTake`. Without a sink, input is read and discarded.

`setSink()` changes the sink from the control side. It returns only once the data thread can no longer be inside the previous sink, because the data thread counts itself in before reading the pointer and out after the call. A take can therefore be finished or destroyed right after it is detached. Buffers with a wrong layout, an offset past their memory or a format other than the negotiated one are counted as bad and not passed on.

## Recording in the desktop

- **Arm.** Each audio track has an **Arm** box. MIDI tracks have none; arming one through the API is refused because MIDI recording is not built. Arming the first track opens the input stream, so recording starts without setting up a stream. Disarming the last track closes it.
- **Input.** `fl-live --remote NAME --input NODE` links the input stream to `NODE`. Without `--input` it waits for an external link, as the output does without `--target`.
- **Record.** **Record** needs at least one armed track and a saved project. It creates one take per armed track in the project's `recordings/` folder, named `TRACK take N.wav`, and starts the transport if it is stopped. N never reuses an existing file or `.part` file. While recording, arming, deleting a track, a second Record, opening a project and starting a new project are refused.
- **Stop.** Stop finishes every take, reads it back and adds it to its track as a new clip named `Take N`, which becomes the track's selected clip. All the takes of one recording are one Undo step ("recorded take"). An empty take is deleted. A take that fails to write, cannot be read back, belongs to a track removed meanwhile, or would exceed the eight clips a track holds keeps its file, and the status line says where it is. Dropped frames are reported with the file name.
- **Media retention.** Undo and clip deletion never delete a take's file. The project refers to it by path, like imported audio, so Redo and reopening the project find it.
- **Preview.** With no remote, the silent preview records silence at the render clock (480 frames per tick), which lets the tests check takes without audio hardware.

## Verification

`capture-pipeline` (`tests/capture_tests.cpp`) interposes the C library's `write`, `pwrite`, `open` and `fsync`, and counts allocations, so it can see which thread does what.

- The ring is all or nothing, keeps order and wraps.
- 400 stereo blocks of 257 frames pass through a real take from a separate capture thread. The finished file is the blocks bit for bit, and the capture thread made no allocation and no file call.
- Pushing 200 blocks into a two-block ring overflows. Written plus dropped frames equal the captured frames, and the file is whole blocks in increasing order.
- An injected `ENOSPC` after 20 blocks reports the failure with the kept file name. `finish()` leaves a readable `.part` file whose frames equal the written count, with the first 20 blocks intact.

The test also passes under ThreadSanitizer and under AddressSanitizer with UndefinedBehaviorSanitizer in local builds. The [receipt](../validation/capture-pipeline.json) records the run.

`pipewire-capture` (`tests/pipewire_capture.py`) starts an owned private PipeWire server at quantum 256 with no devices or session manager. `pw-cat` plays an independently authored stereo reference, in which left and right follow different sequences. It is linked by `pw-link` into `fl-live-input`, and `capture-probe` records 1.5 s into a `CaptureTake`. Every captured frame must equal the next reference frame with its channels in place; a forward jump, where the graph dropped quanta under load, is counted and limited to a tenth of the take. In six local runs, including one under AddressSanitizer, all of about 72,000 frames matched with no jumps, no bad buffers and no dropped blocks. The [stream receipt](../validation/pipewire-capture.json) records them.

`desktop-session` (`recordingTakes` in `tests/desktop_tests.cpp`) drives the real `Session` in preview and the production QML. It checks the refusals listed above. A 50-tick take is exactly 24,000 silent frames, is added and selected as `Take 2`, and Undo restores the clip list with the file kept. After Redo, the project is saved and reopened with the clip intact. An immediate stop leaves no clip and no file, and the next take is `take 2`. In the QML, Arm appears only on the audio track, Record is disabled without an armed track, and Record and Stop through the controls add another clip.

`pipewire-desktop-recording` runs the same private-graph check with `desktop-backend-probe --record` in place of `capture-probe`. The probe saves a project and arms its audio track, which opens `fl-live-input`. The fixture links the reference, then the probe records 1.5 s through `Session::record()` and stops. The take must be in `recordings/`, be the track's new selected clip, and match the reference frame by frame under the same rule. In twelve local runs every captured frame matched with no jumps. The [desktop receipt](../validation/desktop-recording.json) records the runs.
