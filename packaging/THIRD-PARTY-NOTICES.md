# Notices

## fl-live

fl-live is free software under the GNU General Public License, version 3 or (at your option) any later version (GPL-3.0-or-later). The full text is in `LICENSE`. The owner chose this license on 2026-11-07 (issue #84).

The repository is still private and no release has been approved (issue #90). This package is a development build for the maintainer's own systems.

Anyone who distributes fl-live in binary form must offer its complete corresponding source under the same license. The libraries below are linked dynamically, so a user can replace them, which is what their LGPL licenses require; their notices are shipped by the distribution packages that provide them. Each license below is compatible with GPL-3.0-or-later.

## Runtime dependencies

The binaries link dynamically against these system libraries; none of their code is bundled. Versions are the qualification baseline of the pinned build image (Arch Linux package snapshot of 2026-10-20). Each library's own license applies and is shipped by its distribution package.

| Library | Baseline | License (per upstream) | Used by |
|---|---|---|---|
| Qt 6 Core, Gui, Qml, Quick, Quick Controls 2 | 6.12.0 | LGPL-3.0-only (or GPL / commercial) | `fl-live` |
| PipeWire (libpipewire-0.3) | 1.6.9 | MIT | `fl-live`, `fl-live-play` |
| libsndfile | 1.2.2 | LGPL-2.1-or-later | `fl-live`, `fl-live-render` |
| GNU C++ runtime (libstdc++), glibc | toolchain of the build image | GPL-3.0 with GCC Runtime Library Exception; LGPL-2.1-or-later | all binaries |

The bundled example projects in `share/fl-live/examples` were authored for fl-live with its built-in sounds and contain no third-party material.
