# Recording

Recording (epic #11) is being built from the inside out. This page covers what exists now: the capture pipeline that takes audio from the audio callback to a WAV file (#44), the PipeWire audio and MIDI input streams, recording in the desktop: arming, audio takes and MIDI takes as clips (parts of #43, #44 and #47), timestamps and placement on the transport's bar lines (#45), finishing takes when the server or input is lost (#46), and recovery of audio takes interrupted by a crash (#48). Not built yet: input monitoring, choosing an input inside the app, crash recovery for MIDI takes, and latency compensation for physical devices.

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

- **Arm.** Every track has an **Arm** box. Arming the first audio track opens the audio input stream, and arming the first MIDI track opens the MIDI input, so recording starts without setting up a stream. Disarming the last track of a kind closes its stream.
- **Input.** `fl-live --remote NAME --input NODE` links the audio input to `NODE`, and `--midi-input NODE` links the MIDI input. Without them the inputs wait for an external link, as the output does without `--target`.
- **Record.** **Record** needs at least one armed track, and a saved project when an audio track is armed. It creates one take per armed track in the project's `recordings/` folder, named `TRACK take N.wav`. From a stopped transport it returns to the start and plays, so takes begin on bar 1; while the transport runs, takes are placed from the next bar line (see Placement). N never reuses an existing file or `.part` file. While recording, arming, deleting a track, a second Record, opening a project and starting a new project are refused.
- **Stop.** Stop finishes every take, reads it back and adds it to its track as a new clip named `Take N`, which becomes the track's selected clip. All the takes of one recording are one Undo step ("recorded take"). An empty take is deleted. A take that fails to write, cannot be read back, belongs to a track removed meanwhile, or would exceed the eight clips a track holds keeps its file, and the status line says where it is. Dropped frames are reported with the file name.
- **Media retention.** Undo and clip deletion never delete a take's file. The project refers to it by path, like imported audio, so Redo and reopening the project find it.
- **Journal.** Before audio starts, each take gets a journal, `TRACK take N.wav.json`, naming its track ID, the project file, the channel count and the rate. It is removed when the take finishes, and kept while a `.part` file remains to recover.
- **Preview.** With no remote, the silent preview records silence at the render clock (480 frames per tick), which lets the tests check takes without audio hardware.

## Placement and timestamps

**Timestamps.** All streams of one fl-live session share the graph clock: the output, the audio input and the MIDI input are in the same driver group. Each output cycle publishes a transport anchor, which pairs the graph-clock frame of the cycle with the transport frame rendered first in it. A take records the graph-clock frame of its first cycle. MIDI events carry their cycle's frame plus their offset in it. While recording, the desktop converts a take's first frame to a transport frame with the latest anchor, read within one tick. The silent preview uses its simulated clock the same way.

**Placement policy.** A recorded clip starts at the first bar line at or after the take's first frame. With 4/4 at the session tempo, that is the transport's bar grid.
- Audio: the clip's range starts that many frames into the take, and the file keeps everything. A take that ends before reaching a bar line adds no clip and keeps its file.
- MIDI: notes count from the bar line. Notes that start before it are left out, and the message counts them.
- Record from a stopped transport returns to the start, so the take starts on bar 1 and nothing is cut.
- A take whose first frame could not be placed, because the transport never ran, is added unaligned and the message says so.

**Compensation policy.** No input or output latency is compensated. Placement uses the graph clock alone. On the private software graph the measured offset is zero (see Verification). Physical devices add converter and buffer latency that is not measured here (#19) and not subtracted.

## MIDI takes

`PipeWireMidiCapture` connects as node `fl-live-midi-input` with one control port and accepts raw MIDI and UMP; UMP is converted to MIDI 1.0 bytes on the data thread. Each message is stamped with the graph clock position of its cycle plus its offset in the cycle. `MidiTake` (`include/fl_live/capture.hpp`) keeps note-on and note-off messages on any channel in a fixed ring without allocating, and ignores other messages. While recording, the desktop drains the ring every tick. A full ring refuses events and counts them.

When the take finishes, notes are paired. A note-on with velocity 0 is a note-off, a repeated note-on closes the held note first, and a note still held at the end closes there, so a take never leaves a note stuck. The desktop turns the take into a new piano-roll clip on its track:

- Frames become ticks at the session tempo (96 per beat), rounded to the nearest tick. Lengths are at least one tick.
- MIDI velocity 1–127 becomes 1–100 percent: `(v × 100 + 63) / 127`, rounded down.
- The clip is the take rounded up to whole bars, from 1 to 16, and loops.
- Notes starting past 16 bars, beyond 16 sounding at once or beyond 1,024 are left out, and notes crossing 16 bars are cut there. The message counts each case, and notes still held at the end.
- A take with no notes adds no clip, and the message says so.

Audio and MIDI takes from one recording are one Undo step. A MIDI take has no file, so it cannot be recovered after a crash.

## Loss while recording

- **Server or output lost.** When the output stream reports an error, the desktop stops audio as before. It also detaches and finishes every take and closes the input stream. Edits are refused while audio is stopped, so the finished takes are held, and the message says how many will become clips after **Retry audio**. When Retry audio succeeds, they are added as selected clips in one Undo step, and the input reopens if a track is still armed. Opening or starting another project instead drops the held takes from memory and says so; their files stay in `recordings/`.
- **Input lost.** When only the input stream reports an error, for example because its node was removed, playback continues. The takes are finished and added as clips right away, the input is closed and the tracks are disarmed, and the message asks to arm again.
- **Disk full or write failure.** The take keeps its `.part` file and journal (see the capture pipeline above), so **Recover takes** can finish it when the project is next opened.

## Route changes, overflow and panic

- **Route change.** Unlinking the input while recording does not stop the take: it records silence until the input is linked again. A MIDI note whose note-off was missed while unlinked is closed at the end of the take.
- **Overflow.** When the disk falls behind, the take ring (2 s) refuses whole blocks; the capture side never waits for the disk. Stop reports the dropped frames with the file name, and the take still becomes a clip holding the blocks that fit. Stop then waits for the worker to write what the ring holds, so it takes as long as the disk needs for up to 2 s of audio.
- **Panic.** **Silence now** silences playback in the next block but keeps recording. The takes finish on Stop as usual, and notes held through the panic are closed with the take.
- **Edits.** Clips on an armed track, including the playing one, can be edited and deleted while recording. Track deletion and arming changes are refused until Stop.

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

For MIDI, `capture-pipeline` checks pairing, a velocity-0 note-off, a repeated note-on, notes closed at the end, ignored controller messages, overflow counting and an empty take, with no allocation or file call in the callback. `desktop-session` (`midiRecording`) arms the MIDI track and records in preview, feeding note messages with `previewMidi()` at the simulated clock. The new clip holds the notes at the expected ticks and percentages, survives Undo/Redo and save/reopen, and plays when launched. A take without notes adds nothing, and an audio-plus-MIDI recording is one Undo step.

`pipewire-midi-capture` plays an independently authored Standard MIDI File with `pw-midiplay` into `fl-live-midi-input` on the private graph. At 120 BPM and 96 ticks per quarter note one tick is 250 frames, so the expected timeline follows from the file. The file has a chord, overlapping and adjacent repeated notes, and velocities 1 and 127. Every note must arrive with its pitch, velocity and exact frame timing relative to the first note. `pipewire-desktop-midi-recording` plays the same file into the desktop `Session` through the probe's `--record-midi` mode, and the new piano-roll clip must hold every note with its pitch and mapped velocity, at its tick and length within one tick. Quantizing can flip one tick when a note falls within a frame of a half-tick boundary, which CI showed once. In eight local runs each, every note matched with no offset. The [MIDI receipt](../validation/midi-recording.json) records them.

For #49, `recording-overflow` runs `save-fault-probe record-overflow` with the fault injector making every write into `recordings/` sleep 100 ms. 400 preview ticks (192,000 frames) took about 4 ms. The status line reported the dropped frames, all whole 480-frame blocks, and frames on disk plus dropped frames equal 192,000. The take still became a clip. `desktop-session` (`recordingThroughPanicAndEdits`) deletes the playing clip on the armed track while running and recording. A panic then stops playback with no voice left while the audio and MIDI takes continue, and Stop adds both takes with the held note closed. Playing the recorded note and panicking leaves no voice, and Undo removes both takes in one step. `pipewire-recording-route-change` unlinks the live input 0.6 s into a take and relinks it 0.3 s later. Every non-silent frame is still a reference frame in order, the silence and the jump past it are counted, and the take becomes the selected clip. Each passed eight local runs in a row. The [cleanup receipt](../validation/recording-cleanup.json) records them.

For placement (#45), `desktop-session` (`recordingPlacement`) records while the transport runs. The take starts at transport frame 14,400, in bar 1 at 120 BPM. The audio clip's range starts 81,600 frames into the file, at bar 2, which survives save. MIDI notes played on bar 2 beats 1 and 2 land on ticks 0 and 96, and a note played before the bar line is left out and reported. A Record from a stopped transport away from the start returns to bar 1 and cuts nothing.

Two private-graph tests measure placement at 48 kHz, quantum 256, 120 BPM:

- `pipewire-recording-placement` uses `graph-source`, a fixture program that emits clicks and MIDI notes at exact graph-clock frames. The frames are chosen from the desktop's transport anchor so that the events fall on bar 2–4 downbeats and on four beat positions.
- `pipewire-recording-loopback` does not depend on the anchor for scheduling. The audio track plays a one-bar clip with a click on frame 0, so fl-live's own output clicks on every transport bar line, and that output is linked back into `fl-live-input`. Any anchor error or graph delay would show as an offset.

The tolerance is one quantum for audio and none for MIDI. In eight runs of each, clicks landed 0 frames from the recorded clip's bar lines in both tests, and every MIDI note landed on its exact tick. The [placement receipt](../validation/recording-placement.json) records the runs, rate, quantum and policy.

For the UI journey (#50), `desktop-session` (`qmlRecordingJourney`) uses synthetic pointer and keyboard input on the production QML, offscreen. It saves through the project field, clicks Arm on the audio and MIDI tracks, then Record, feeds two MIDI notes and clicks Stop. It then edits the MIDI take in the piano roll (click a note, Right moves it a sixteenth, Up raises it a semitone) and sets the audio take's range start to 25 ms, and saves with Ctrl+S. A fresh window opens the project through the project field and Open button. The edited notes and the 1,200-frame range are intact, and clicking both clip buttons and Play makes the takes the active clips with the moved note sounding. The [journey receipt](../validation/recording-journey.json) links this run to the receipts for stop, device loss, interrupted capture, recovery and placement. Native Wayland input and a physical audio interface or MIDI controller remain to be qualified (#22, #19).

