# PipeWire output and control handoff

This is an optional Linux backend for the prepared mono engine. The initial qualification baseline is PipeWire 1.6.9. It is not a complete audio-device manager or a usable DAW interface.

## Ownership and bounded commands

RenderController owns Engine. Exactly one control producer submits start/stop/rewind/schedule messages through a 64-entry SPSC queue. Exactly one rendering consumer applies at most 64 messages per block, then calls Engine::process. Submission failure is explicit. Engine timestamp/order/ID failures increase the rejection counter, and status returns the last handled serial. Serials are caller-supplied; the caller should make them monotonic.

Panic uses a separate atomic flag, so queue saturation cannot prevent requesting silence. At the next callback it stops the engine, discards at most 64 pending controls and emits silence. A newly submitted command may run on a later block; panic is not a persistent mute latch. Handled serial includes discarded controls, so it does not mean every command was applied. Playing state comes from the running field.

Status uses atomic fields and a versioned snapshot. The control-side reader may retry for a coherent block-completion snapshot; the rendering writer never waits for it. Queue indices/status atomics must be lock-free on the target. The immutable plan still outlives all processing.

The backend owns the PipeWire control loop, context/core and stream. Shutdown stops the control loop and destroys the stream/context before transferring engine ownership back to the caller. `finish()` stops the engine, clears pending controls and publishes stopped state. Device/server failure can prevent a final callback acknowledgement; teardown still silences engine state after processing resources are gone.

## Stream and errors

The remote name is explicit. There is no silent fallback to the desktop server. A named target enables autoconnection; otherwise port setup/linking belongs to a session manager or explicit host/test configuration. The optional group property is used by the isolated test driver, not imposed on all production streams.

The stream requests mono native-endian F32 at the prepared plan's sample rate. A mismatched configured/negotiated rate or format fails rather than changing playback speed. The backend does not implement transparent live sample-rate adaptation. Quantum changes up to 8,192 frames are accepted by the engine API; larger buffers are rejected with an empty chunk/error rather than an unbounded buffer clear. This maximum is a policy, not evidence that every smaller quantum/rate transition has been measured.

The data callback dequeues mapped buffers, invokes the controller, sets chunk size/stride and returns the buffer. Error counters, callback/frame counts, maximum measured process duration and duration-over-budget count are exposed. Those duration counts are not PipeWire graph xrun counters and do not establish end-to-end latency. No user logging, file I/O, asset destruction or unbounded command draining occurs there.

## Qualification

The private integration test starts its own server with a dummy driver and no hardware/session-manager modules. It explicitly configures and links only owned nodes. An independent pw-cat capture receives 24,000 frames and checks every sample of the authored repeating waveform after a measured startup-silence prefix. The test requires at least 20,000 non-silent samples. The prefix is retained as a finding, not treated as measured physical latency.

A second client processes actual samples before that owned server is terminated. The app reports server loss, exits nonzero and publishes stopped state. Missing-server startup also fails without readiness. Every owned process is reaped; no pre-existing server is stopped or reconfigured.

PipeWire 1.6.9 pw-cat returns exit code 1 on the finite capture-limit path because that path quits without setting its drained flag. The test records the code, requires no stderr, requires the exact full payload and compares the samples. This exception is narrowly tied to the selected helper behavior, not a blanket acceptance of failing capture tools. [Upstream count and exit paths](https://github.com/PipeWire/pipewire/blob/1.6.9/src/tools/pw-cat.c).

Debug/release and backend ASan/UBSan checks pass locally. The concurrent queue/status tests also pass a separately compiled ThreadSanitizer run. These are finite tests, not proof of all races or all library internals. The instrumented engine/controller paths observe zero C++ allocations. Actual physical output, native input recording, hardware hotplug, broader format/rate changes, stereo routing, long-duration xruns and UI operation remain unqualified.
