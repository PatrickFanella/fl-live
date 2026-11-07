# Recording

Recording (epic #11) is being built from the inside out. This page covers what exists now: the capture pipeline that takes audio from the audio callback to a WAV file (#44), the PipeWire input stream that feeds it, audio recording in the desktop: arming, takes and recorded clips (parts of #43, #46 and #47), finishing takes when the server or input is lost (#46), and recovery of takes interrupted by a crash (#48). Not built yet: MIDI recording, input monitoring, choosing an input inside the app, and timestamps and latency compensation (#45).

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
- **Journal.** Before audio starts, each take gets a journal, `TRACK take N.wav.json`, naming its track ID, the project file, the channel count and the rate. It is removed when the take finishes, and kept while a `.part` file remains to recover.
- **Preview.** With no remote, the silent preview records silence at the render clock (480 frames per tick), which lets the tests check takes without audio hardware.

## Loss while recording

- **Server or output lost.** When the output stream reports an error, the desktop stops audio as before. It also detaches and finishes every take and closes the input stream. Edits are refused while audio is stopped, so the finished takes are held, and the message says how many will become clips after **Retry audio**. When Retry audio succeeds, they are added as selected clips in one Undo step, and the input reopens if a track is still armed. Opening or starting another project instead drops the held takes from memory and says so; their files stay in `recordings/`.
- **Input lost.** When only the input stream reports an error, for example because its node was removed, playback continues. The takes are finished and added as clips right away, the input is closed and the tracks are disarmed, and the message asks to arm again.
- **Disk full or write failure.** The take keeps its `.part` file and journal (see the capture pipeline above), so **Recover takes** can finish it when the project is next opened.

## Interrupted takes

A process that dies while recording leaves `TRACK take N.wav.part`. Frames the worker had written are in the file, but the WAV header still says the data is empty, because only `finish()` completes it.

`recoverTake()` (`include/fl_live/capture.hpp`) turns such a file into a finished one:

- It walks the RIFF chunks and requires a one- or two-channel, 32-bit float format at a valid rate, then takes the data from the start of the `data` chunk to the end of the file. A finished header's data size is used when it is set and fits.
- It copies whole frames only and reports the bytes of a trailing partial frame.
- It writes to `DESTINATION.recovering`, reads the result back, and publishes it with a hard link, which fails instead of replacing an existing file. The `.part` file is only read.
- It refuses, with the reason, a file that is not a WAV, has an incomplete header, is not 32-bit float, or holds no complete frame.

When a project opens, the desktop lists every `.part` take in its `recordings/` folder and says how many it found. **Recover takes** recovers each one to its final name, or to `TRACK take N recovered M.wav` when that name is taken. It then adds the take as a selected `Recovered take` clip on the track its journal names, and all of them form one Undo step. Only after the recovered file has been read back in full are the `.part` file and the journal deleted. A take with no journal, whose track no longer exists, on a track that already has eight clips, or that cannot be decoded keeps its recovered file, and the message gives the path. A file that cannot be recovered is left exactly as it was and stays listed.

## Verification

`capture-pipeline` (`tests/capture_tests.cpp`) interposes the C library's `write`, `pwrite`, `open` and `fsync`, and counts allocations, so it can see which thread does what.

- The ring is all or nothing, keeps order and wraps.
- 400 stereo blocks of 257 frames pass through a real take from a separate capture thread. The finished file is the blocks bit for bit, and the capture thread made no allocation and no file call.
- Pushing 200 blocks into a two-block ring overflows. Written plus dropped frames equal the captured frames, and the file is whole blocks in increasing order.
- An injected `ENOSPC` after 20 blocks reports the failure with the kept file name. `finish()` leaves a readable `.part` file whose frames equal the written count, with the first 20 blocks intact.

The test also passes under ThreadSanitizer and under AddressSanitizer with UndefinedBehaviorSanitizer in local builds. The [receipt](../validation/capture-pipeline.json) records the run.

`pipewire-capture` (`tests/pipewire_capture.py`) starts an owned private PipeWire server at quantum 256 with no devices or session manager. `pw-cat` plays an independently authored stereo reference, in which left and right follow different sequences. It is linked by `pw-link` into `fl-live-input`, and `capture-probe` records 1.5 s into a `CaptureTake`. Every captured frame must equal the next reference frame with its channels in place; a forward jump, where the graph dropped quanta under load, and a run of silence, where it filled a late cycle with zeros, are counted and together limited to a tenth of the take. The silence case was seen once in ten runs alongside the desktop tests under host load; in serial runs neither occurs. In six local runs, including one under AddressSanitizer, all of about 72,000 frames matched with no jumps, no bad buffers and no dropped blocks. The [stream receipt](../validation/pipewire-capture.json) records them.

`desktop-session` (`recordingTakes` in `tests/desktop_tests.cpp`) drives the real `Session` in preview and the production QML. It checks the refusals listed above. A 50-tick take is exactly 24,000 silent frames, is added and selected as `Take 2`, and Undo restores the clip list with the file kept. After Redo, the project is saved and reopened with the clip intact. An immediate stop leaves no clip and no file, and the next take is `take 2`. In the QML, Arm appears only on the audio track, Record is disabled without an armed track, and Record and Stop through the controls add another clip.

`pipewire-desktop-recording` runs the same private-graph check with `desktop-backend-probe --record` in place of `capture-probe`. The probe saves a project and arms its audio track, which opens `fl-live-input`. The fixture links the reference, then the probe records 1.5 s through `Session::record()` and stops. The take must be in `recordings/`, be the track's new selected clip, and match the reference frame by frame under the same rule. In twelve local runs every captured frame matched with no jumps. The [desktop receipt](../validation/desktop-recording.json) records the runs.

For interrupted takes, `capture-pipeline` forks a process that records 100 known blocks and kills itself with `SIGKILL` once they are written. The `.part` file it leaves declares 0 data bytes for the 205,600 bytes on disk. Recovery returns all 25,700 frames bit for bit, the `.part` file is byte-identical afterwards, and a second recovery to the same name is refused with the file unchanged. A copy cut 3 bytes into a frame recovers its 1,000 whole frames and reports 3 discarded bytes. An incomplete header, garbage, a header with no frame and a 16-bit WAV are refused with their reasons and left unchanged. The finished `.part` file a write failure leaves also recovers.

`desktop-session` (`interruptedTakeRecovery`) runs `save-fault-probe record-crash`, which saves a project, records 50 preview ticks and aborts. Reopening the project reports one interrupted take with its journal and track. **Recover takes** adds a selected `Recovered take` clip in one Undo step and deletes the `.part` file and the journal. The recovered file, read by a chunk walker in the test, is float stereo at 48 kHz with exactly 24,000 silent frames. Undo keeps the file. A copy without a journal is recovered to `stray.wav` but not placed, and the message gives its path. A garbage `.part` file is refused, keeps its 512 bytes and stays listed. The QML **Recover takes** button runs the same path. The [recovery receipt](../validation/take-recovery.json) records the run.

`pipewire-recording-server-loss` and `pipewire-recording-input-loss` run the private-graph fixture with `desktop-backend-probe --record-loss`. About 1 s into the take, the fixture either terminates the server or destroys only the `fl-live-input` node with `pw-cli`.

- **Server loss.** The probe reports audio stopped, no new clip yet, one finished take file and the held-take message. The fixture restarts the server and sends Retry audio. The take is then the track's selected clip with an "Added 1 take" message, and `fl-live-input` exists again on the restarted server.
- **Input loss.** Audio stays available, the take is the selected clip at once, and the track is disarmed.

In both, the take matches the reference frame by frame up to the loss. Each passed ten local runs in a row. The [loss receipt](../validation/recording-loss.json) records them.
