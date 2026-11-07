# Example projects

Both projects were authored with the desktop's own Session API and saved by the app. They use only the built-in pulse and step-pattern instruments, so they contain no recordings, samples or third-party material and need no audio files. Open one with **Open** in the desktop, or `Session::loadProject`.

| File | Contents |
|---|---|
| `four-track-session.json` | 124 BPM. Two pulse tracks panned L40 and R40, the template arpeggio, and an eight-step bass pattern. Four scenes: Full, Pulses, Arpeggio, Break. |
| `single-pattern.json` | 96 BPM. A six-step pentatonic pattern on the MIDI track; the pulse track is muted. |

`desktop-session` (`bundledExamplesOpen`) opens every file in this folder in a clean session, checks that it opens without warnings or missing audio, launches each scene and exports two bars of non-silent stereo audio.

These files are version 4 projects and need a build that includes general Session tracks. They are part of the source tree and are not a release; the product license is not chosen yet.
