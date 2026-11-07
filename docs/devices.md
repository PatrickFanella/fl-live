# Devices

A track can run its signal through a small chain of typed devices (#73, #74). This page defines the model, the connection rules and how a chain is compiled into the render plan. Gain and transpose are the first two devices (#75, #76); editing devices in the desktop and automating their parameters are #77.

## Ports and chains

Ports carry either MIDI notes or stereo audio. Every track has fixed endpoints:

| Endpoint | Accepts | Emits | Tracks |
|---|---|---|---|
| source | — | MIDI (MIDI track) or audio (audio track) | all |
| instrument | MIDI | audio | MIDI tracks: the clip's [built-in synth](instrument.md) |
| output | audio | — | all; feeds level, pan, mute and solo |

| Device | In | Out | Parameter |
|---|---|---|---|
| transpose | MIDI | MIDI | semitones, whole numbers from −48 to +48 |
| gain | audio | audio | decibels from −60 to +12 |

A track's devices and connections must form **one chain** from the source to the output that passes through every node once. On a MIDI track that means source → MIDI devices → instrument → audio devices → output; an audio track has no instrument. Validation refuses each of these, with a message, before any plan is prepared:

- a connection between a MIDI port and an audio port, for example a gain device before the instrument or a transpose on an audio track;
- an endpoint that does not exist on the track, such as an unknown device ID or an instrument on an audio track, or a connection out of the output or into the source;
- branches, merges and devices left out of the chain;
- cycles, including a loop of devices beside the chain;
- more than 8 devices on a track, or gain devices adding up to more than +24 dB.

A track without devices keeps the default chain and stores nothing.

## Identity and binding

Each device has an ID that is unique in the project. IDs come from a project-wide counter saved with the devices, so later edits never reissue a removed ID. A parameter binding is the pair (device ID, device kind): each device has exactly one parameter. Duplicating a track copies its chain under fresh IDs, and deleting a track removes its chain.

## Compilation

Chains are compiled when a session is prepared, on the control side, never in the audio callback:

- **Transpose** shifts every note of the track's MIDI clip before the synth sees it, one device after another. A note whose pitch leaves 0–127 at any device is dropped, not clamped. Because each note keeps its own start and duration, note-off always matches note-on.
- **Gain** devices multiply into one per-track factor, `device_gain` = ∏ 10^(dB/20). The engine applies it with the track level, including a level automation lane, which does not replace it. The engine accepts factors from 0 to 16; the +24 dB chain limit stays below that.

A refused chain never reaches the engine. Loading a file with one, or making an edit that would create one, leaves the open document, its history and the playing plan unchanged.

## Persistence

Projects with devices declare the required feature `devices-v1` and carry a top-level object:

```json
"devices": {"next_id": "4", "tracks": [{"track": "2",
  "devices": [{"id": "1", "kind": "transpose", "value": 5}, {"id": "2", "kind": "gain", "value": -6}],
  "connections": [["source", "1"], ["1", "instrument"], ["instrument", "2"], ["2", "output"]]}]}
```

Endpoints are `"source"`, `"instrument"`, `"output"` or a device ID string. Decoding requires exactly these fields, a counter above every ID, and a chain that validates. Unknown kinds, unknown endpoints, connections that are not pairs and devices without the feature are refused.

## Verification

`authored-project-io` checks the declared chain order, compiled pitches and gain, dropped out-of-range notes, round trip, malformed files, and every refused graph by its message: type mismatch, branch, cycle, loose device, unknown endpoint, a connection out of the output, range errors, too many devices and too much gain. It also checks the plan's gain bound and that the engine applies `device_gain` together with a level lane. In `desktop-session`, `deviceChainsInSession` covers loading, duplicate (fresh IDs) and delete, and shows that a cyclic and a mistyped chain in a file are refused with the open document, path and history unchanged. The [receipt](../validation/device-graph.json) records the run.
