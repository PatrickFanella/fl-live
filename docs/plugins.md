# External plugins: format decision

Decision record for #66, made by the owner on 2026-11-07. Nothing here is implemented yet; plugin support is claimed only when the qualification issues (#67–#72) link their evidence.

## Formats and order

1. **CLAP first.** It is the only format the first useful release has to qualify (epic #14).
2. **LV2 second** (#164): many open-source Linux plugins use it.
3. **VST3 third** (#165).

VST2 is out of scope because Steinberg no longer licenses its SDK. All three formats sit behind one format-neutral host layer: scanning, processing, parameters, saved state, latency and missing-plugin behaviour are written once, with a backend per format.

## Pinned CLAP version

| | |
|---|---|
| Upstream | https://github.com/free-audio/clap |
| Release | 1.2.10 (`CLAP_VERSION` 1.2.10 in `include/clap/version.h`) |
| Commit | `195b42a004144fab0b3cf95e9c067187d15365b7` (tag `1.2.10`, checked 2026-11-07) |
| License | MIT, copyright 2021 Alexandre Bique, per `LICENSE` at that commit |

A host needs only the headers: CLAP is a C ABI with per-method thread rules. CI builds without network access, so the integration (#67) vendors the headers of that commit into the source tree instead of fetching them. A later version is a separate, reviewed update.

## License obligations

fl-live is GPL-3.0-or-later (#84); MIT code may be combined with it. When the headers are vendored, their MIT notice (copyright line and permission text) goes into `packaging/THIRD-PARTY-NOTICES.md` and ships with every package. Third-party plugins are separate programs loaded at run time under their own licenses; fl-live distributes none.

## Initial scope

- One CLAP instrument and one CLAP effect: load, process, play, save and reopen.
- Stereo audio ports and one note port; parameters with automation; plugin state saved in the project; reported latency.
- Scanning in a helper process with a timeout, so a crashing or hanging plugin cannot take down the editor (#67).
- Search paths from `include/clap/entry.h`: `~/.clap`, `/usr/lib/clap` and the directories listed in `CLAP_PATH`.
- No plugin windows. Linux plugin GUIs mostly assume X11 embedding and native Wayland is not qualified (#22); parameters get a generic panel in the QML instead. Plugin windows are #70.

## Not decided here

- Process isolation while a plugin runs (runtime isolation), as distinct from scan isolation (#72, #92).
- Which real plugins qualify the journey. The candidates are open-source plugins that ship CLAP on Linux, chosen when #68 starts.
