# Built-in synth

Every MIDI clip plays through a small built-in synth with one declared policy. It is deliberately simple; plugins (#66–#72) are the path to richer instruments.

| Setting | Range | Meaning |
|---|---|---|
| Waveform | Sine, Triangle, Square, Saw | Naive oscillators: square and saw alias at high pitches |
| A (attack) | 0–10,000 ms | Linear rise from 0 to full level |
| D (decay) | 0–10,000 ms | Linear fall from full level to sustain |
| S (sustain) | 0–100 % | Level held until the note ends |
| R (release) | 0–10,000 ms | Linear fall from the level at note end to 0 |

Times convert to frames at 48 kHz (1 ms = 48 frames). The defaults (sine, 0/0/100 %/0) reproduce the original fixed instrument sample for sample. A note's peak is its velocity × 0.1, then the track level and pan apply.

## Voices and overload

Each track has 16 voices. A clip still may not hold more than 16 overlapping notes; releases can outlast notes, though. A new note takes a free voice. Failing that, it takes the releasing voice that is currently quietest, and failing that the oldest voice; ties take the lowest-numbered voice. Releases continue across the clip's loop boundary. Stopping a track, launching another clip on it, scene stop and panic still silence every voice at once, so a release never outlives its clip.

## Editing and persistence

The pattern header shows the synth of the MIDI clip in the step editor. Each change is a stopped edit and one Undo step ("Undo synth release"). Duplicating a track copies its synth. Projects store non-default synths per MIDI clip behind the required feature `synth-v1`, as `"synth": {"waveform": "saw", "attack_ms": 5, "decay_ms": 120, "sustain": 40, "release_ms": 800}`.

## Verification

- `engine-contracts`: an A4 note with attack, decay, sustain and release is compared with an independent oscillator × envelope formula (within 2×10⁻⁶) at blocks 1/64/257/512. It also checks that velocity scales linearly, the four waveform shapes at known phases, a release crossing the loop boundary, 16 voices under overload, stop clearing releasing voices, refused settings, and zero intercepted allocations.
- `authored-project-io`: synth persistence and frame conversion; malformed waveform, sustain, times and extra fields are refused, and synth settings on an audio clip are refused.
- `qmlSynthEditing`: combo and typed A/S/R through the QML, refusals and Undo/Redo. A float export's first step is checked against an independent triangle × envelope render. Also covers Save/Open, a default synth on a new MIDI track and copying on duplicate.
