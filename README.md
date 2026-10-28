# fl-live — Linux session workstation

Original source for a Linux-first open-source music workstation, working toward a useful MVP and ultimately a close Live/Max replacement. C++20 and Qt 6/QML are the selected product stack. The implemented slices are a headless engine, offline renderer, bounded control handoff, optional PipeWire output backend and a Qt/QML session application with editable template settings and project save/reload.

The private reference-product research remains in a separate workspace. No captured executable code, factory assets or research binaries are part of this source tree. Project licensing is still undecided; no release has been published. The canonical product repository is [subculture-collective/fl-live](https://git.subcult.tv/subculture-collective/fl-live), initially private.

## Build and test

Requires a C++20 compiler, CMake 3.25 or newer and Ninja. The WAV example additionally requires pkg-config and the initial libsndfile 1.2.2 baseline. Tests for its output use Python 3.11 or newer. Qt is not required by these targets.

```bash
rtk proxy cmake --preset debug
rtk proxy cmake --build --preset debug
rtk proxy ctest --preset debug
```

Other presets are `release`, `sanitize` (AddressSanitizer and UBSan) and `engine-only`. The last disables the renderer and needs no libsndfile or Python. Each has matching configure, build and test presets.

## Render the authored demo

```bash
rtk proxy ./build/debug/fl-live-render ./build/debug/my-demo.wav 257
```

Use a new output path; existing files are refused. The eight-second, mono PCM16 WAV contains two seconds of silence, four seconds of a simple pulse/bass pattern, then two seconds of silence. Its audio and MIDI clips launch at frame 96,000 and stop at frame 288,000. Nothing is sent to a sound device by this command.

## Implemented

- Constant-tempo 4/4 sample clock and exact rational bar-boundary calculation.
- Source-rate mono audio clips, looping/one-shot playback and track gain.
- MIDI clip note scheduling with a minimal fixed-capacity sine instrument.
- Sample-timed clip launch/replacement/stop, bounded command storage and explicit overflow results.
- Transport stop/panic, frozen stopped position and stopped rewind.
- Offline WAV rendering from the same engine API intended for a later audio adapter.

The engine is single-owner. Its prepared plan must remain alive and immutable. RenderController provides the bounded handoff for one control producer and one rendering consumer; it does not make arbitrary live project edits safe. [Engine contract](docs/engine-contract.md) records timing/ownership, and [backend contract](docs/pipewire-backend.md) records the handoff and stream limits.

## Test PipeWire without touching desktop audio

The optional backend is pinned to the initial PipeWire 1.6.9 baseline. Its integration test requires `pipewire`, `pw-cat`, `pw-cli`, `pw-dump` and `pw-link`.

```bash
rtk proxy cmake --preset pipewire
rtk proxy cmake --build --preset pipewire
rtk proxy ctest --preset pipewire
```

The test creates a private software-only server in a temporary directory, links only its authored playback/capture clients, validates samples and tests server loss. It loads no hardware modules and changes no running desktop service. `pipewire-sanitize` adds ASan/UBSan. For manual playback, `fl-live-play` requires an explicit `--remote`; `--target` opts into autoconnection to a named node. Without a target, port configuration/linking needs a session manager or explicit setup as in the test.

## Continuous integration

[Gitea Actions](https://git.subcult.tv/subculture-collective/fl-live/actions) runs desktop, release, engine-only and PipeWire sanitizer checks in a pinned Linux container. Test logs, JUnit reports and the offscreen desktop capture are retained as artifacts. See [CI setup and local reproduction](docs/ci.md).

## Current verification and next work

Debug, release, sanitizer and engine-only checks passed on Kvant. Tests cover integral and fractional tempo boundaries, sample-exact launch/loop/stop, note cleanup, queue bounds and output preservation. The decoded demo PCM matches across block sizes 1/64/128/257/512 on the qualification machine. Instrumented C++ allocation checks report zero allocations during tested render paths. This does not qualify hardware deadlines, all allocators, concurrent access or all platforms.

See [status](docs/STATUS.md), the original [engine validation](validation/engine-foundation.json) and [backend validation](validation/pipewire-foundation.json). Backend processing and server loss pass in the private graph; hardware audibility/latency and long-duration xrun qualification remain open. The initial Qt/QML session interface is checked offscreen with mouse input and engine acknowledgements. Native Wayland interaction and physical audio remain unqualified. Template project save/reload and stopped tempo/level/pitch editing are implemented. General note positioning, recording, plugins and programmable-device graphs remain product milestones.

## Run the desktop prototype

Requires Qt 6.12 Quick/QuickControls2 (and Qt Test for tests) plus the PipeWire baseline above.

```bash
rtk proxy cmake --preset desktop
rtk proxy cmake --build --preset desktop
rtk proxy ctest --preset desktop
rtk proxy ./build/desktop/fl-live
```

The default is **silent preview**: launch either authored clip, then press Play. The preview discards rendered samples and advances 480 frames per 10 ms timer tick; it is a simulated clock, not a latency or timing benchmark. Stop clears clips and queued launches; relaunch them to resume. Return to start works only while stopped. The interface shows render-owned active clips and the earliest queued track action. A requested launch becomes queued after render acknowledgement and highlights only after it is rendered. While stopped, edit tempo, levels and a 1–16-step MIDI pattern with rests, pitch, velocity and gate, then save a new JSON path or reopen a saved project. Edits return to the start and clear launches. Undo/Redo in the header restore the most recent 128 committed changes while stopped; returning to saved content clears the unsaved indicator. See [editing history](docs/edit-history.md) and [project format and ownership](docs/projects.md).

For output through an existing session manager, explicitly choose a server and target:

```bash
rtk proxy ./build/desktop/fl-live --remote pipewire-0 --target YOUR_OUTPUT_NODE
```

Replace the target with the intended PipeWire node name. Without `--target`, manual port setup/linking is required. The app does not select an output device automatically. This hardware path has not been qualified yet. See [desktop contract](docs/desktop-session.md) and [validation receipt](validation/desktop-session.json).

The desktop can export its current two-track template mix while stopped: enter a new WAV path, select 1–16 bars, then choose **Export both tracks**. Output is mono 48 kHz / 16-bit PCM; both clips start at frame zero. Existing destinations are refused. See [project export policy](docs/projects.md#export-the-current-template-mix).
