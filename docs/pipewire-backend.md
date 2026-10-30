# PipeWire output and control handoff

This is an optional Linux backend for the prepared mono engine. The initial qualification baseline is PipeWire 1.6.9. It is not a complete audio-device manager or a usable DAW interface.

## Ownership and bounded commands

RenderController owns Engine. Exactly one control producer submits start/stop/rewind/schedule messages through a 64-entry SPSC queue. Exactly one rendering consumer applies at most 64 messages per block, then calls Engine::process. Submission failure is explicit. Engine timestamp/order/ID failures increase the rejection counter, and status returns the last handled serial. Serials are caller-supplied; the caller should make them monotonic.

Panic uses a separate atomic flag, so queue saturation cannot prevent requesting silence. At the next callback it stops the engine, discards at most 64 pending controls and emits silence. A newly submitted command may run on a later block; panic is not a persistent mute latch. Handled serial includes discarded controls, so it does not mean every command was applied. Playing state comes from the running field.

Status uses atomic fields and a versioned snapshot, including each track’s active clip and earliest scheduled action. The control-side reader may retry for a coherent block-completion snapshot; the rendering writer never waits for it. Queue indices/status atomics must be lock-free on the target. The immutable plan still outlives all processing.

The backend owns the PipeWire control loop, context/core and stream. Shutdown stops the control loop and destroys the stream/context before transferring engine ownership back to the caller. `finish()` stops the engine, clears pending controls and publishes stopped state. Device/server failure can prevent a final callback acknowledgement; teardown still silences engine state after processing resources are gone.

## Stream and errors

The remote name is explicit. There is no silent fallback to the desktop server. A named target enables autoconnection; otherwise port setup/linking belongs to a session manager or explicit host/test configuration. The optional group property is used by the isolated test driver, not imposed on all production streams.

The stream requests mono native-endian F32 at the prepared plan's sample rate. A mismatched configured/negotiated rate or format fails rather than changing playback speed. The backend does not implement transparent live sample-rate adaptation. Quantum changes up to 8,192 frames are accepted by the engine API; larger buffers are rejected with an empty chunk/error rather than an unbounded buffer clear. This maximum is a policy, not evidence that every smaller quantum/rate transition has been measured.

The data callback dequeues mapped buffers, invokes the controller, sets chunk size/stride and returns the buffer. Error counters, callback/frame counts, maximum measured process duration and duration-over-budget count are exposed. Those duration counts are not PipeWire graph xrun counters and do not establish end-to-end latency. No user logging, file I/O, asset destruction or unbounded command draining occurs there.

Backend errors now stay latched until teardown/reconnect. Oversized callbacks return an empty chunk and request panic; later buffers within the size limit are filled with silence without consuming controls or advancing the engine. Buffer-return errors also request panic. Teardown still performs the final stopped-state publication if no further controller callback runs. The headless client's JSON includes `min_render_frames` and `max_render_frames`: observed nonzero buffer sizes passed to the controller, with zero meaning no such call. These counters distinguish an observed callback size from a requested server setting; `frames` includes submitted silence and is not a count of engine-rendered samples.

## Qualification

The private integration test starts its own server with a dummy driver and no hardware/session-manager modules. It explicitly configures and links only owned nodes. An independent pw-cat capture receives 24,000 frames and checks every sample of the authored repeating waveform after a measured startup-silence prefix. The test requires at least 20,000 non-silent samples. The prefix is retained as a finding, not treated as measured physical latency.

A second client processes actual samples before that owned server is terminated. The app reports server loss, exits nonzero and publishes stopped state. Missing-server startup also fails without readiness. Every owned process is reaped; no pre-existing server is stopped or reconfigured.

CTest also runs fixed-quantum private servers at 257 and 1,024 frames. Each case requires both observed render-size bounds to equal the requested quantum and compares captured waveform samples before exercising server loss. The existing 128-frame case keeps the complete desktop recovery journey. A separate 16,384-frame case raises the private server's allocation limit beyond its usual 8,192-frame ceiling so the backend's rejection path actually runs. It requires `-EMSGSIZE`, no controller rendering or position advance, and a stopped final state with pending controls drained. The offscreen desktop test also checks that a configured 44,100 Hz backend against the 48,000 Hz template fails clearly, preserves savable project settings and cannot start playback.

These cases qualify separate fixed software-server configurations. They do not qualify live quantum transitions, negotiated rate/format changes, physical device changes or preservation of a playing document across such transitions. Issue #23 remains open for those checks.

## Live-quantum continuity finding

The frame-coded control adds a stronger audio oracle than the eight-frame demo loop. A Qt-free test client prepares a 65,521-frame ramp before connecting. Each value encodes its source-frame index using an exactly representable binary fraction. The parent records 72,000 frames and checks the captured sequence independently. The default `pipewire-frame-coded-control` test holds the quantum at 257 frames and requires exact continuity after the retained startup prefix.

The same client can acknowledge live callback-size changes from 257 to 1,024 to 128 frames. The parent waits for actual source samples before changing the owned server's `clock.force-quantum` setting, then requires both transitions to lie within the captured source-frame span. It changes only the private server; no desktop service, physical device or session-manager setting is touched.

On Kvant on 2026-10-30, the fixed control passed, but the live-transition diagnostic failed continuity: it captured an unexpected silent interval followed by a source-frame offset. Successful size acknowledgements and stopped cleanup did not establish continuous audio. The retained receipt distinguishes sample mismatches from unexpected zero frames; a persistent offset can make every later sample differ without every later frame being silent or lost. The responsible layer remains undetermined: this experiment spans the engine/backend, PipeWire adapters/driver and capture client. It does not identify graph xruns or physical latency.

The live case remains an opt-in, failing diagnostic while issue #23 is open. No previous gate is disabled or relaxed. To reproduce it:

```bash
rtk proxy cmake --preset desktop -DFL_LIVE_LIVE_QUANTUM_DIAGNOSTIC=ON
rtk proxy cmake --build --preset desktop
rtk proxy ctest --preset desktop -R '^pipewire-live-quantums$' --output-on-failure
```

It emits a JSON result with `passed: false`, callback-size acknowledgements, capture hash and sample findings, then exits nonzero. Restore the ordinary gate selection with `rtk proxy cmake --preset desktop -DFL_LIVE_LIVE_QUANTUM_DIAGNOSTIC=OFF`. The diagnostic also works with `pipewire-sanitize`; the fixed frame-coded control is included in both presets' ordinary test suites. See [the dated receipt](../validation/live-quantum-continuity.json).

## Reference producer comparison

The follow-up fixture can replace the captured producer with PipeWire's own `pw-cat` playing independently generated raw frame-coded PCM. Only `fixture-reference` is linked to the capture input. The fl-live probe stays unlinked and acknowledges quantum changes on the same private driver. Its frame counts describe the observer, not the reference producer's source indices or actual callback sizes. The result records the linked port names and identifies that distinction. Capture must remain active after the last change; observer counters are not substituted for reference-source time.

The reference's fixed-257 control passes. Its live 257 → 1,024 → 128 diagnostic reproduces the unexpected silent interval and source offset with no fl-live output linked into capture. Continuously draining capture stdout during the metadata commands did not eliminate the failure: both producers still recorded 1,153 unexpected zero frames in the retained runs. This comparison shows that fl-live's linked audio output is not required to reproduce the symptom. It does not distinguish PipeWire graph/adapter behavior from the capture path, identify a graph xrun, or prove that fl-live has no separate fault.

`pipewire-reference-control` joins the ordinary software gates. With the existing diagnostic option enabled, reproduce the failing reference case using:

```bash
rtk proxy ctest --preset desktop -R '^pipewire-reference-live-quantums$' --output-on-failure
```

Both live diagnostics retain strict sample comparison and exit nonzero on discontinuity. The fixture now drains capture stdout concurrently during metadata changes to avoid introducing bounded-pipe backpressure. For the fl-live path alone, `--pause-capture-during-changes` retains the serialized-reader comparison; the reference live case requires continuous draining so capture activity can be checked independently of observer counters. See [reference-producer observations](../validation/reference-quantum-continuity.json). Live continuity and issue #23 remain open; this is a reproducer/control change, not a product repair.

PipeWire 1.6.9 pw-cat returns exit code 1 on the finite capture-limit path because that path quits without setting its drained flag. The test records the code, requires no stderr, requires the exact full payload and compares the samples. This exception is narrowly tied to the selected helper behavior, not a blanket acceptance of failing capture tools. [Upstream count and exit paths](https://github.com/PipeWire/pipewire/blob/1.6.9/src/tools/pw-cat.c).

Debug/release and backend ASan/UBSan checks pass locally. The concurrent queue/status tests also pass a separately compiled ThreadSanitizer run. These are finite tests, not proof of all races or all library internals. The instrumented engine/controller paths observe zero C++ allocations. Actual physical output, native input recording, hardware hotplug, broader format/rate changes, stereo routing, long-duration xruns and UI operation remain unqualified.

The backend now balances each successful `pw_init()` call with `pw_deinit()` after stream/context/loop teardown, including partial startup failure. The software integration fixture owns a minimal client configuration as well as its server configuration; it excludes desktop scheduling modules. See [CI qualification limits](ci.md) for the default-client LeakSanitizer observation.
