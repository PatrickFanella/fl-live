# Keyboard and accessibility

## Shortcuts

| Keys | Action | Active when |
|---|---|---|
| Ctrl+Z / Ctrl+Shift+Z (platform Undo/Redo) | Undo / Redo a document edit | No text field owns the keys and no dialog is open |
| Ctrl+S | Save to the project path field | No dialog is open |
| Ctrl+O | Open the project path field | Stopped, no dialog |
| Space | Play / Stop | No text field, button, check box or combo box has focus (those use Space themselves), no dialog, audio available |
| Ctrl+. | Silence now (panic) | No dialog |
| Ctrl+Shift+A / Ctrl+Shift+M | Add audio / MIDI track | No dialog |
| Ctrl+E | Export with the export row's settings | Stopped, not exporting, no dialog |
| Delete or Backspace / Escape | Delete the selected notes / clear the selection | The [piano roll](piano-roll.md) grid has focus |
| Left / Right, Shift+Left / Shift+Right | Move the selected notes one grid step / resize them | The piano-roll grid has focus |
| Up / Down, Shift+Up / Shift+Down | Move the selected notes a semitone / an octave | The piano-roll grid has focus |

Every key sequence is declared once; `keyboardContractAndFocus` expands the platform Undo/Redo bindings and fails on any duplicate. In a text field, Space types a space and Ctrl+Z undoes text, not the document. Tab moves through the controls in reading order.

## Focus

- Deleting a track from its **Delete** button moves focus to the Delete button of the track that took its place, or the previous track, and to **Add audio track** when none remain.
- Closing the Rename dialog (Apply, Cancel or Escape) returns focus to the Rename button that opened it.
- Undo and Redo from the keyboard leave focus on a visible control.

The track strip is rebuilt when the number of tracks changes, so text typed into a track's WAV path field but not yet imported is cleared by adding or deleting a track.

## Accessible names

Every visible button, check box, spin box, combo box, text field and progress bar has an accessible name. No two share one: per-track controls name their track ("Delete track 2", "Launch Pattern on track 4", "Import WAV into track 3"), and recovery banners name the snapshot time. `accessibleNames` walks the production QML through `QAccessible`, with four tracks, and checks names, roles and the checked state of a Mute box. This covers the accessibility tree only; checks with a screen reader on the qualification desktop (#80's second criterion) and the keyboard-only qualification journey (#83) remain open.
