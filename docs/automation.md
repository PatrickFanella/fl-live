# Automatable parameters

This is the parameter and interpolation contract for #51 and the lane editor for #52. Automation during playback edits is #54; the epic is #12.

## Identity, units and ranges

An automatable parameter is identified by a track's stable document ID plus a parameter name. IDs survive rename, reordering, Save/Open and Undo. Duplicating a track copies its lanes to the copy's new ID; deleting a track removes its lanes, and Undo restores them.

| Parameter | Unit | Range | Engine value | Replaces |
|---|---|---|---|---|
| `level` | percent of full gain | 0–100 | gain = value / 100 | the track's static Level |
| `pan` | percent, left to right | −100..100 | pan = value / 100, balance law | the track's static Pan |

Each parameter has at most one lane. Mute and solo still win: an inaudible track prepares silent whatever its level lane says.

## Timing and interpolation

A lane holds 1–4,096 points. Each point sits at an integer position in hundredths of a beat from the song start (0 to 102,400, which is 256 bars), and positions must strictly increase. At preparation, each position becomes a transport frame with the same upward rounding as every musical boundary: `frame = ceil(48000 × 60 × position / (tempo × 100))`. The value at frame `f` is:

- the first point's value for `f` at or before the first point;
- the last point's value for `f` at or after the last point;
- otherwise `a + (b − a) × (f − fa) / (fb − fa)`, computed in double and rounded to float, between the neighbouring points.

The engine evaluates this per sample against the transport position, so a lane runs with the song rather than restarting with each clip launch. It uses a per-track cursor: no allocation, no search per sample, and a reset when the transport is rewound. Points with out-of-range or non-finite values, non-increasing frames and lanes over 4,096 points are refused when a plan is prepared, never in the callback.

## Editing lanes

Tick **Automation** on a track to show its lane editor (a view choice; it is not saved). Choose **Level** or **Pan**. The lane shows four bars, or more once points go past them, with bar lines. The top pixel row is 100 percent and the bottom row the parameter's minimum.

- Click empty space to add a point, snapped to a sixteenth note (25 hundredths of a beat) and a whole percent. A point at an existing position replaces it.
- Click a point to remove it. Removing the last point removes the lane, so the static Level or Pan applies again.
- For exact values from the keyboard, type a position in beats and a value, then press **Add point**.
- **Clear** removes the lane.

Every change is a stopped edit and one Undo step ("Undo track 1 automation"). Invalid positions or values and unknown parameters are refused without history changes. `Session::automationPoints`, `setAutomationPoint`, `removeAutomationPoint` and `clearAutomation` expose the same operations.

## Persistence

Projects with lanes declare the required feature `automation-v1` and carry a top-level array:

```json
"automation": [
  {"track": "1", "parameter": "level", "points": [{"at": 0, "value": 0}, {"at": 400, "value": 100}]}
]
```

Every object has exactly these fields. Unknown parameters, extra fields, non-numeric values, a missing feature or a lane for an unknown track are refused before the current project is replaced. Projects without lanes keep their earlier form.

## Verification

- `engine-contracts`: gain and pan lanes on one track are compared sample by sample with an independently written statement of the policy, at blocks of 1, 64, 128, 257 and 512 frames, with no intercepted allocations. Rewind restarts the curve. Order, range, NaN and capacity are refused.
- `authored-project-io`: persistence and refusals; frame conversion at 133 BPM, including a rounded-up boundary (173,233.08 → 173,234); rendered ramp samples; and a muted track that stays silent.
- `qmlAutomationEditing`: actual pointer clicks add points and remove one; typed position and value add another; Undo/Redo restore the curve; Pan is a separate lane; Clear; refusals; Save/Open; and a float export of the reopened project that follows the curve.
- `automationIdentityStability`: rename, duplicate, delete, Undo and Save/Open keep lanes on the right track IDs. A float export follows a one-bar 0→100 level ramp exactly.
