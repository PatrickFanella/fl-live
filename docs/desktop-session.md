# Desktop session contract

The optional `desktop` preset builds `fl-live`, an original Qt 6.12/QML session prototype. Two immutable authored tracks provide a decaying audio pulse and a four-note MIDI loop. This is a development slice; it cannot yet edit or persist projects.

`Session` owns the immutable plan before constructing its controller. The GUI thread is the sole command producer. In silent preview, its 10 ms timer renders and discards 480 mono frames per tick on that same thread. Timer delays slow the simulated clock. In PipeWire mode, the backend exclusively owns rendering; the GUI timer only polls status. Backend destruction joins processing before controller/plan destruction. No Qt code runs in the audio callback.

Play/Stop/Return to start use the bounded control queue. Transport labels derive from the coherent render snapshot, not from a button press. Pending means a submitted serial has not been handled. Handled does not mean a scheduled clip has become active. Rejections are reported as an aggregate change; the current engine has no per-command outcome or active-clip snapshot. The interface therefore makes no active-clip claim.

Launch and track stop use new controller commands resolved against the current position on the rendering owner. The bar grid is inclusive: a request consumed exactly on a boundary applies there. Otherwise it applies at the next boundary. `TempoGrid::try_next_bar` returns an empty optional on overflow without constructing exceptions or allocating. The original throwing `next_bar` remains available to non-rendering callers. Mixing arbitrary future scheduled commands with these quantized controls can still trigger the engine's global ordering rejection.

Stop freezes transport and clears all clips and scheduled commands. Return to start is enabled only while acknowledged stopped with no pending controls. Launch while stopped schedules a clip but does not start transport. Silence now uses out-of-band panic; it clears queued controls and stops on the next callback, without persistent muting. New controls can subsequently restart playback.

No remote argument selects silent preview. `--remote NAME` explicitly connects the audio backend; `--target NODE` additionally opts into autoconnection through the user's session manager. A target requires a remote. Backend failure joins/tears down processing, displays stopped transport and disables further submissions until app restart. Device selection, reconnect and rate adaptation remain future work.

## Verification boundary

`desktop-session` uses Qt's offscreen platform and software scene graph. It loads the actual QML resource, sends mouse clicks to the clip/Play/Stop controls and checks transport labels. A deterministic bridge test verifies acknowledgement, rewind, invalid clip and panic. Sample checks verify launch at frame 96,000 and track stop at 192,000 across 257-frame blocks; clock overflow returns no boundary. A local PNG from the actual offscreen window is retained under `build/desktop/desktop-session.png` and was inspected for layout.

The desktop preset also runs engine/controller/offline WAV/private PipeWire tests. The private audio integration still uses the headless playback client, so it does not qualify the QML-to-physical-device journey. Wayland desktop input, hardware audibility, device changes, accessibility and extended playback remain unverified. Licensing, hosted CI, packaging and a useful editable musical workflow remain incomplete.
