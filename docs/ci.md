# Linux CI

Gitea Actions runs `.gitea/workflows/ci.yml` for pull requests targeting main, pushes to any branch and manual dispatch. The `linux-contracts` job uses the existing `ubuntu-latest` Docker runner label; no new runner or host configuration is required.

`ci/Dockerfile` pins the amd64 Arch base image by digest and package repositories to the 2026-10-20 archive. This supplies the project's Qt 6.12 / PipeWire 1.6.9 / libsndfile 1.2.2 baseline. Keep the digest, repository date and qualified dependency requirements in sync when upgrading. Installed package versions are recorded in the results. The image is a verification environment, not a distributable product or release.

After building the image, CI runs it as an unprivileged user with networking disabled. It mounts no host audio devices, desktop sockets or source directories. Source is copied into the image. The private PipeWire test starts only its own software graph inside that container; its Unix-domain sockets do not require network access. Both server and client module configurations belong to the fixture; desktop RTKit/portal scheduling is excluded. ASan/UBSan and LeakSanitizer remain enabled without leak suppressions.

`ci/verify.sh` runs these presets in order, stopping on the first failure:

| Preset | Coverage |
|---|---|
| desktop | Engine, control handoff, WAV round trip, private PipeWire graph, offscreen QML editing/save/open checks and stopped desktop backend replacements |
| release | Optimized engine/controller and WAV checks with assertions active |
| engine-only | Core build without Qt, PipeWire, libsndfile or Python as linked dependencies |
| pipewire-sanitize | Engine/controller/WAV/private audio checks with ASan and UBSan |
| document-io-sanitize | Engine/document and Qt Core metadata file round trips/admission with ASan and UBSan; no Qt Quick, display or PipeWire |

The scene-control revision runs 45 CTest executions across these five cohorts, including repeated tests across configurations. Each cohort includes the Qt-free document-to-render and atomic scene-scheduling checks. That is not a count of distinct tests. The Qt Core file codecs and authored-project migration/render checks have their own sanitizer cohort; the PipeWire sanitizer cohort still excludes Qt. ThreadSanitizer, native Wayland input, QML-to-device output, physical audio, performance and packaging are separate qualification work.

Configure/build/test output, CTest logs, JUnit reports, installed toolchain versions and the offscreen desktop/active-clip/step-editor/history/identity-renaming/scene-controls PNGs are collected under `build/ci-results`. The workflow retains them as the `linux-verification` artifact for 14 days, including available diagnostics after a failed test. The Gitea-compatible v3 artifact action and checkout action are pinned to commits. No deployment, publication or automatic merge is configured.

To reproduce locally with Docker:

```bash
rtk proxy docker build --file ci/Dockerfile --tag fl-live-ci:local .
rtk proxy docker run --rm --network none fl-live-ci:local
```

To retain local results, create a named container, start it attached, copy `/src/build/ci-results` with `docker cp`, then remove only that container. The workflow performs those steps automatically. `bash ci/verify.sh` also works with the required toolchain installed locally.

During initial container admission, the default system client module configuration produced a 652-byte LeakSanitizer report. Pairing the backend's `pw_init()` with `pw_deinit()` corrected its lifecycle but did not alone remove that report. Owning a minimal software-client configuration removed the desktop-module dependency and passed leak detection. This qualifies the private graph; it does not establish leak-free behavior for every system client module configuration. Desktop scheduling-module lifecycle remains a separate backend qualification item.
