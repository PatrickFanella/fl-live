# fl-live

fl-live is an open-source music workstation for Linux, built in C++20 with Qt 6
and QML and playing through PipeWire. It is organized around a session view:
audio and MIDI tracks, clips launched in scenes, a piano roll, a track mixer,
device chains, automation and recording.

fl-live is in development. There is no release yet. You can build it and a
development package from source. Nothing has been checked on physical audio
hardware or with native Wayland input; tests run offline, against the QML on
Qt's offscreen platform, and against a private software-only PipeWire graph.
See [support and limitations](docs/limitations.md) for what is checked and what
is not.

## Features

- **Session of tracks and clips**: up to 64 audio or MIDI tracks with up to
  eight clips each. Add, duplicate, delete and rename tracks and clips.
- **Scenes**: create, copy, reorder and delete scenes, choose which clips each
  scene includes, and launch or stop a scene immediately or on the next bar.
- **MIDI editing**: a 1–16 step editor with pitch, velocity and gate, or a piano
  roll with add, select, move, resize and velocity editing on a grid, plus clip
  length and loop.
- **Built-in synth**: four waveforms, an ADSR envelope and 16 voices.
- **Audio clips**: import WAV files (mono or stereo, 48 kHz) in the background,
  view the waveform and choose start, end and loop ranges. Relink moved files or
  collect them into the project folder.
- **Track mixer**: level, balance-law pan, mute and solo, mixed to stereo with an
  optional mono downmix.
- **Devices**: per-track chains of gain and transpose devices.
- **Automation**: level, pan and device-parameter lanes with linear
  interpolation.
- **Recording**: arm audio and MIDI tracks, record takes into the project's
  `recordings/` folder, place them on bar lines, and monitor audio and MIDI
  input. Audio takes interrupted by a crash can be recovered.
- **Edits while playing**: most track, mixer, scene, pattern and automation
  edits apply during playback. Undo and Redo cover committed edits.
- **Projects**: versioned JSON with atomic saves, migration from older versions,
  autosave every 30 seconds and a restore prompt after a crash.
- **Export**: render the mix to WAV (start bar, length, tail, stereo or mono,
  16-bit, 24-bit or 32-bit float) in the background with progress and cancel.
- **Keyboard access**: shortcuts, focus restoration and accessible names
  ([keyboard and accessibility](docs/keyboard.md)).
- **Command-line tools**: `fl-live-render` renders a demo to WAV offline, and
  `fl-live-play` is a headless PipeWire test player.

Plugin hosting is planned, starting with CLAP ([plugin format decision](docs/plugins.md)).
It is not implemented.

## Quick start

Requirements: Linux on x86-64, a C++20 compiler, CMake 3.25 or newer and Ninja.
The desktop app also needs Qt 6.12 (Core, Gui, Qml, Quick, Quick Controls 2 and
Qt Test), the PipeWire 1.6.9 client library and libsndfile 1.2.2. Some tests use
Python 3.11 or newer and the PipeWire command-line tools (`pipewire`, `pw-cat`,
`pw-cli`, `pw-dump`, `pw-link`).

Build, test and run the desktop app:

```bash
git clone https://github.com/subculture-collective/fl-live.git
cd fl-live
cmake --preset desktop
cmake --build --preset desktop
ctest --preset desktop
./build/desktop/fl-live
```

The app starts in silent preview: it advances a simulated clock and discards
the audio. To play through PipeWire, name the server and the output node:

```bash
./build/desktop/fl-live --remote pipewire-0 --target YOUR_OUTPUT_NODE
```

The app never picks an output device on its own. Open one of the
[example projects](examples/README.md) to start.

### Presets

Each preset has matching configure, build and test presets.

| Preset | What it builds |
| --- | --- |
| `desktop` | Everything, including the Qt app and PipeWire tests |
| `debug`, `release` | Engine, controller and offline renderer (no Qt) |
| `sanitize` | Debug build with AddressSanitizer and UBSan |
| `engine-only` | Core engine with no libsndfile, PipeWire, Qt or Python |
| `pipewire`, `pipewire-sanitize` | PipeWire backend and its private-graph tests |
| `document-io-sanitize` | Project file round trips with sanitizers |

The PipeWire tests start their own software-only server in a temporary
directory. They load no hardware modules and do not touch your desktop audio.

### Offline demo render

```bash
cmake --preset debug && cmake --build --preset debug
./build/debug/fl-live-render ./build/debug/my-demo.wav 257
```

This writes an eight-second mono WAV without using a sound device. The last
argument is the render block size. Existing files are never overwritten.

## Configuration

fl-live has no configuration file. Output is chosen per run with `--remote`
and `--target`; audio and MIDI inputs are chosen in the app. Projects are JSON
files you save where you like. Recovery snapshots live in
`~/.local/share/fl-live/recovery/`.

## Install a development package

[docs/install.md](docs/install.md) explains how to build a tarball with CPack or
a byte-reproducible archive with `packaging/reproducible_archive.py`, and how to
install it into a prefix such as `~/.local`. The package holds `fl-live`,
`fl-live-play`, `fl-live-render`, a desktop entry, the example projects and the
docs. It is a development build, not a release.

## Project layout

| Path | Contents |
| --- | --- |
| `include/fl_live/`, `src/` | Engine, capture, project documents, media import and PipeWire backend |
| `apps/desktop/` | Qt/QML desktop application |
| `apps/*.cpp` | `fl-live-play` and `fl-live-render` |
| `tests/` | C++ and Python tests |
| `examples/` | Example projects |
| `docs/` | Design and behavior notes for each area |
| `ci/` | Pinned build image and the CI script |
| `packaging/` | Desktop entry, reproducible archive script and third-party notices |

## Contributing

- Bugs and feature requests: [GitHub issues](https://github.com/subculture-collective/fl-live/issues).
- Questions and ideas: [GitHub Discussions](https://github.com/subculture-collective/fl-live/discussions).
- Code: open a pull request on GitHub against `main`.

CI builds a pinned Arch Linux image (`ci/Dockerfile`) and runs `ci/verify.sh`
inside it on every pull request: the desktop, release, engine-only,
PipeWire-sanitizer and document-sanitizer presets. To reproduce it locally:

```bash
docker build --file ci/Dockerfile --tag fl-live-ci:local .
docker run --rm --network none fl-live-ci:local
```

## License

GNU General Public License, version 3 or (at your option) any later version
(`GPL-3.0-or-later`). See [LICENSE](LICENSE) and the
[third-party notices](packaging/THIRD-PARTY-NOTICES.md).
