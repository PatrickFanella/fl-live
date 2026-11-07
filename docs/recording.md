# Recording

Recording (epic #11) is being built from the inside out. This page covers what exists now: the capture pipeline that takes audio from the audio callback to a WAV file (#44), and the PipeWire input stream that feeds it. Track arming, input choice and monitoring in the desktop (#43), timestamps (#45), shutdown on stop and device loss (#46), recorded clips (#47) and recovery after a crash (#48) are not built yet, so the desktop cannot record.

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

## Verification

`capture-pipeline` (`tests/capture_tests.cpp`) interposes the C library's `write`, `pwrite`, `open` and `fsync`, and counts allocations, so it can see which thread does what.

- The ring is all or nothing, keeps order and wraps.
- 400 stereo blocks of 257 frames pass through a real take from a separate capture thread. The finished file is the blocks bit for bit, and the capture thread made no allocation and no file call.
- Pushing 200 blocks into a two-block ring overflows. Written plus dropped frames equal the captured frames, and the file is whole blocks in increasing order.
- An injected `ENOSPC` after 20 blocks reports the failure with the kept file name. `finish()` leaves a readable `.part` file whose frames equal the written count, with the first 20 blocks intact.

The test also passes under ThreadSanitizer and under AddressSanitizer with UndefinedBehaviorSanitizer in local builds. The [receipt](../validation/capture-pipeline.json) records the run.

`pipewire-capture` (`tests/pipewire_capture.py`) starts an owned private PipeWire server at quantum 256 with no devices or session manager. `pw-cat` plays an independently authored stereo reference, in which left and right follow different sequences. It is linked by `pw-link` into `fl-live-input`, and `capture-probe` records 1.5 s into a `CaptureTake`. Every captured frame must equal the next reference frame with its channels in place; a forward jump, where the graph dropped quanta under load, is counted and limited to a tenth of the take. In six local runs, including one under AddressSanitizer, all of about 72,000 frames matched with no jumps, no bad buffers and no dropped blocks. The [stream receipt](../validation/pipewire-capture.json) records them.
