# Product source

This is the original C++20/Qt 6 product implementation. The research archive is a separate workspace at `/home/onnwee/Work/Lab/fl-live`; do not copy its binaries, captured code, private fixtures or chronological archive into this repository.

Use `rtk` for shell commands. Read the inherited global instructions and required writing skill. Preserve unrelated work. No agent delegation is requested.

Keep the engine independent of Qt and platform audio APIs. Audio rendering must not allocate, lock, access files, log or destroy owned graphs. Document thread ownership and asset lifetime. Test scheduling across non-dividing block sizes. Distinguish offline, backend, UI and release qualification.

Current build: `cmake --preset debug`, `cmake --build --preset debug`, `ctest --preset debug`, each prefixed with `rtk proxy`. The product license is not selected yet; do not publish a release or apply an unapproved license grant.
