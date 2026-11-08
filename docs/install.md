# Building a package and installing it

fl-live is licensed under GPL-3.0-or-later. No release has been approved yet (#90), so this produces a **development package for your own systems**, not a release. See [notices](../packaging/THIRD-PARTY-NOTICES.md).

## Supported environment

| | Requirement |
|---|---|
| OS | Linux, x86-64 |
| Runtime libraries | Qt 6.12 (Core, Gui, Qml, Quick, Quick Controls 2), PipeWire 1.6.9 client library, libsndfile 1.2.2, a C++20 standard library |
| Audio | A running PipeWire server for live output; silent preview needs none |
| Qualified builder | The pinned image in `ci/Dockerfile` (Arch Linux snapshot 2026-10-20) |

Other distributions and versions may work but are unqualified.

## Build the package

From a clean checkout, in the pinned image or an equivalent toolchain (CMake 3.25+, Ninja, a C++20 compiler and the development packages above):

```bash
cmake --preset desktop -DFL_LIVE_SOURCE_REVISION="$(git rev-parse HEAD)"
cmake --build --preset desktop
ctest --preset desktop
cpack --config build/desktop/CPackConfig.cmake -B build/package
```

This writes `build/package/fl-live-VERSION-REVISION-linux-x86_64.tar.gz`. In CI the revision comes from `CI_SOURCE_SHA`; a build without one records `unknown`.

### Reproducible archive

The CPack archive records file times and the build account, so two builds differ. For an archive whose bytes depend only on the source, use the packaging script instead:

```bash
SOURCE_DATE_EPOCH=$(git log -1 --format=%ct) python3 packaging/reproducible_archive.py build/desktop build/package
```

It installs the build into a temporary prefix and writes `build/package/fl-live-VERSION-REVISION-linux-x86_64.tar.gz`. Entries are in sorted order, owned by uid/gid 0 with no owner names, and dated `SOURCE_DATE_EPOCH`. The gzip stream carries no timestamp or file name, and the script prints the SHA-256. Two clean builds of the same revision, with the same toolchain image and build directory, give the same bytes. A different build directory or compiler can change the binaries. Without `SOURCE_DATE_EPOCH` the script refuses to run.

The archive holds `bin/fl-live` (desktop), `bin/fl-live-play` (headless PipeWire test player) and `bin/fl-live-render` (offline demo renderer). It also holds `share/applications/fl-live.desktop`, the example projects in `share/fl-live/examples/`, and the README, limitations matrix, keyboard reference, this page and the notices in `share/doc/fl_live/`. The programs link the runtime libraries dynamically; nothing else is bundled.

## Install on a clean system

1. Install the runtime libraries from your distribution. On Arch Linux: `pacman -S qt6-base qt6-declarative pipewire libsndfile`.
2. Unpack the archive into a prefix you own, for example `tar -xzf fl-live-*.tar.gz -C ~/.local --strip-components=1`.
3. Check the build: `fl-live --version` prints the version and source revision.
4. Start it: `fl-live` (silent preview), or `fl-live --remote pipewire-0 --target YOUR_OUTPUT_NODE` for output.
5. Open an example from `~/.local/share/fl-live/examples/`.

To remove it, delete the files the archive listed (`tar -tzf`). Recovery snapshots live in `~/.local/share/fl-live/recovery/`.

## Verification

`staged-install` (desktop preset) installs the build into a fresh prefix. It checks the file list and that no installed binary references the build tree, runs `fl-live --version` against the configured revision, renders the demo with the installed `fl-live-render`, and starts the installed desktop offscreen. A clean runtime-only container check is recorded in [the receipt](../validation/linux-packaging.json). Physical audio and native Wayland on a clean install remain unqualified.
