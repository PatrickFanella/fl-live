# Devices

A track can run its signal through a small chain of typed devices (#73, #74). This page defines the model, the connection rules and how a chain is compiled into the render plan. Gain and transpose are the first two devices (#75, #76). Their parameters can be automated (#77); full qualification is #78.

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

## Editing in the desktop

Each track column has a device row. It shows the count, **+ Gain**, **+ Transpose** (MIDI tracks only) and **Show**, which lists the devices in chain order. Each listed device has a value box (gain in whole decibels, transpose in semitones), **↑** and **↓** to move it within its stage, and **Remove**. A new transpose goes at the end of the MIDI stage, before the instrument, and a new gain at the end of the chain. A device moves only within its stage, so a transpose can never cross the instrument. Removing the last device returns the track to the default chain.

Every add, remove, move and value change is one Undo step and also works while playing, through a [plan replacement](plan-replacement.md). A change that would break a rule, such as a ninth device, more than +24 dB in total, a transpose on an audio track, a value out of range or a move across the instrument, is refused with a message, and the document stays as it was.

## Gain and transpose behaviour

**Gain** (#75) scales the track by 10^(dB/20) before the level and pan, and several gain devices multiply. A level lane replaces the static level but not the device gain.

**Transpose** (#76) moves every note by whole semitones. Each note keeps its start, duration and velocity, so note-on and note-off frames do not change. A note that any device would push outside 0–127 is dropped: it is silent, not clamped to the edge. Changing a transpose while a note sounds lets that voice finish at its old pitch; later notes play at the new pitch.

## Automating device parameters

A device's parameter can have an [automation lane](automation.md) like level and pan. In the lane editor's parameter box, the track's devices follow **Level** and **Pan** in chain order ("Gain 1", "Transpose 2"). The lane's range follows the device: −60 to +12 dB for gain, −48 to +48 whole semitones for transpose. In the Session API the parameter is named `device:<id>`.

- **Gain lanes** replace that device's static value. Each point's dB becomes a factor, 10^(dB/20), multiplied by the track's other gain devices; between points the factor is linear in frames. The +24 dB chain limit applies at every point.
- **Transpose lanes** replace that device's static value. The engine reads the lane at each note-on, rounds it to the nearest semitone and adds it to the note's pitch after the static transposes. Between points the value is linear, so a ramp steps through the semitones. A note the lane pushes outside 0–127 gets no voice. A sounding note keeps its pitch to the end, so note-off always matches note-on.
- A track can automate at most one gain device and one transpose device, because the engine holds one lane of each per track.
- Removing a device removes its lane. Duplicating a track copies the lanes onto the copy's new device IDs. Each point edit is one Undo step and works while playing.

Lanes for devices are saved with `"parameter": "device"` and a `"device"` ID, behind the required feature `device-automation-v1`.

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

For the desktop, gain and transpose:

- `deviceEditing` covers chain order, moves within a stage, each refusal, the 8-device and +24 dB limits, removing back to the default chain, Undo with the same IDs, and IDs that are never reissued.
- `gainDeviceOutput` (#75) shows two gain devices (−6 and +3 dB) scaling the pulse track's float export sample for sample. With a level ramp lane the product still holds, and Save/reopen keeps the binding and the samples.
- `transposeDeviceOutput` (#76) shows an octave transpose doubling the zero crossings of a square note (220 → 440 Hz) with the same onset, end and amplitude. Two +48 transposes drop the note entirely, and Undo restores it. Save/reopen exports identical audio, and a live change while the note sounds leaves no voice sounding after Stop.
- `qmlDeviceEditing` adds, edits, reorders, removes and undoes devices through the controls, and edits a device while playing.

The [device editing receipt](../validation/device-editing.json) records the run.

For automation (#77):

- `engine-contracts` checks that a device-gain lane replaces the static factor sample by sample. A transpose lane read at note-on turns an A4 into an A5, keeps the note's on and off frames, and silences a note pushed past 127. Output is identical across block sizes.
- `authored-project-io` checks lane validation, the compiled factor lanes, the removal of the automated transpose from the static compile, persistence, and each refusal, including +25 dB against other gain devices.
- `deviceAutomationInSession` matches a −60 → 0 dB lane against an independent factor oracle in the export and shows a transpose lane lifting only the later notes by an octave. It also covers Save/reopen, duplicate remapping, removal with the device, Undo, and the one-lane-per-kind refusal.
- `qmlDeviceAutomation` draws gain points at the lane's top and bottom (+12 and −60 dB), types one, and undoes.

The [device automation receipt](../validation/device-automation.json) records the run.

For qualification (#78), `qmlDeviceJourney` runs the whole path through the production QML. It adds a transpose and a gain device, sets +12 and −6, automates the gain from −60 dB to 0 over bar 1, plays, edits the transpose live and undoes it live, stops, saves and exports. The float export matches an independent oracle at every 1,999th frame: template notes an octave up, at the velocity × level × automated factor, and silent between notes. Zero crossings at twice the original pitches confirm the octave. A fresh window reopens and exports the same samples. A burst of about 80 device edits while playing, with a panic in the middle, leaves no voice sounding. Undo back to the reopened state exports the original samples again. The device tests also pass under AddressSanitizer in a local desktop build, and the [qualification receipt](../validation/device-qualification.json) records both runs.
