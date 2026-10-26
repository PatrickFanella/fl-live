# Delivery status

Updated 2026-10-26. The near-term goal is a useful Linux MVP, with a close Live/Max replacement as the long-term destination. Exhaustive reverse engineering is not an implementation gate.

| Area | Current evidence | Remaining |
|---|---|---|
| Source/build foundation | Separate registered product project; C++20/CMake/Ninja builds | Product license, hosted CI, packaging |
| Deterministic engine | Offline audio/MIDI clips, gain, launch/stop and rational clock checked | Additional policies, broader workloads, multi-channel engine |
| Offline export | Actual demo WAV round trip and block-size invariance | General project export, tails/format choices and cancellation |
| Real-time backend | None implemented | PipeWire adapter, control handoff, device changes and deadline measurements |
| Qt/QML application | Stack selected; no app target yet | Grid/mixer/editors, actual native Wayland operation and accessibility |
| Project editing/state | Immutable authored C++ plan | Command history, save/reload, media management and recovery |
| Recording/plugins/graphs | Planned | Implementation and integrated qualification |

The first D1 engine slice is implemented. D0/D1 as a whole remain partial. D2–D5 are not complete. A generated WAV is not a usable workstation.

Next implement an isolated PipeWire adapter with bounded control ownership and explicit state/error reporting. Qualify actual processing separately from hardware audibility. Then connect the same engine to the Qt UI; UI playback state must come from backend/engine acknowledgements.

The product repository is hosted privately at https://git.subcult.tv/subculture-collective/fl-live while licensing is undecided. The validated engine foundation is the initial main-branch baseline. Hosted CI and release qualification remain pending. The reference research archive was not moved into this repository. Its preservation requirements still govern native Soyuz experiments, not this local engine build.
