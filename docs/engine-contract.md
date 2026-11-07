# Engine slice contract

This is an original product policy for the first implementation, not an assertion of exact Ableton behavior.

## Scope and validation

The prepared SessionPlan contains mono or interleaved stereo audio at the session sample rate and/or MIDI note clips. Construction validates the plan before rendering. Supported sample rates are 8,000–384,000 Hz and tempos 20–300 BPM in thousandths of a BPM. The meter is 4/4. There are at most 64 tracks and 64 clips per track in this slice. These are explicit implementation limits, not a fixed final product scope.

Audio clips have one or two channels and contain nonempty arrays of whole frames of finite samples; stereo samples are interleaved left/right. MIDI clips have positive length, at most 1,024 sorted notes and no more than 16 simultaneous notes. Durations are positive and half-open, contained within the clip. Velocity is finite in [0,1]. Gain is finite in [0,4] and pan finite in [-1,1]. Track mixing is floating-point and unclipped in the core. The PCM16 example enables clipping in its file writer.

There is no decoding, resampling, warping, surround or arbitrary channel routing, dynamic tempo change, plugin, external MIDI output or envelope smoothing in this slice. The sine instrument is a testable authored sound source, not a production synth. It has no anti-aliasing or amplitude envelope.

## Channels and pan

The engine mixes two channels internally. Each track uses a balance pan law: the far channel is attenuated linearly with `|pan|` and the near channel stays at unity, so center pan is 0 dB on both channels and pan ±1 silences only the far channel. A mono source and the MIDI instrument feed both channels before pan. A stereo source keeps its left and right channels. This is a declared product policy, not a constant-power law and not a claim about any reference product.

`process(output, 2)` writes interleaved left/right frames. `process(output, 1)` and the original `process(output)` write the explicit `(left + right) / 2` downmix, which is identical to the previous mono output for centered mono sources. Any other channel count, or a buffer that is not a whole number of frames, is filled with silence, applies no commands, does not advance the playhead and returns false. RenderController applies the same refusal before draining controls.

## Timing

The playhead is an unsigned 64-bit frame count. A constant-tempo bar has exact rational duration `sample_rate * 240000 / bpm_milli`. Each boundary is independently rounded upward to a sample; rounded bar lengths are not repeatedly added. `next_bar(at)` includes a boundary exactly at `at`. Frame zero is a boundary. Unrepresentable boundaries throw outside the render callback.

The caller computes quantization with TempoGrid and submits absolute frame commands. A command at frame t is applied before rendering that sample. Commands must be submitted in nondecreasing timestamp order; equal-frame commands apply in submission order, so the last operation on a track wins. Past commands and invalid track/clip IDs are rejected.

The queue holds 64 commands. A full queue returns `full` and leaves existing commands intact. The current explicit recovery is `stop()`, which clears queue and voices. A future concurrent control adapter must provide reliable stop/panic delivery rather than assuming this queue can accept a critical command when full.

Audio loops repeat the complete source array with no join crossfade. MIDI loop boundaries reset note cursors and voices. One-shots stop at their exclusive end. Clip replacement clears old voices before starting the new clip. Stop clears active clips and voices and freezes the playhead. Start resumes that position; rewind changes it to zero only when stopped.

## Ownership and rendering

The caller owns the immutable plan. The engine holds a reference; the plan must outlive it and must not be edited after validation. Construction allocates runtime state and calculates pitch increments before playback. Render uses preallocated runtime/command/voice storage.

All engine operations belong to one rendering owner and occur at block boundaries or inside that owner's processing loop. The engine itself is not a concurrent API. RenderController now adds a single-producer/single-consumer handoff around it; the PipeWire data callback owns processing until backend shutdown completes. UI code must use that handoff and must not access Engine directly. This does not support concurrent plan mutation or graph replacement.

`process()` performs no explicit allocation, locking, file I/O, logging or graph destruction. Tests intercept C++ new/new[] (including aligned allocation) over selected render paths and observe zero allocations. They do not detect every C-library allocator, prove all library internals lock-free or establish callback deadlines. Real-time qualification remains separate.

## Acceptance evidence

`engine-contracts` checks 120,000 rational bar-boundary examples with an independent closed-form oracle, audio scheduling with block sizes 1/64/128/257/512/4096, stereo source routing, the balance pan law, MIDI pan and the mono downmix at frame blocks 1/64/128/257/512, refused channel layouts, MIDI duration/loop/replacement/stop, queue overflow/order/wrap and invalid plans. It remains active in optimized builds; it does not depend on disabled C assert macros.

`render-roundtrip` invokes the real command, opens each WAV independently with Python's standard wave reader, checks format/frame count/silent boundaries, compares decoded PCM across five block sizes and verifies existing output is preserved. This qualifies a finite offline output journey on the recorded machine, not live audio or GUI operation.

`track_statuses()` is a single-render-owner query returning fixed-capacity activity and earliest queued-action state. Its one-shot end is exclusive; multiple same-frame commands retain insertion order. Cross-thread readers use `RenderController::status()`, which publishes these fields atomically with transport. See [clip feedback](desktop-session.md#clip-feedback) for block-boundary semantics and verification.
