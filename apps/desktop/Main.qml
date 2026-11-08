import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import FlLive

ApplicationWindow {
    id: window
    width: 1060; height: 1000
    minimumWidth: 900; minimumHeight: 620
    visible: true
    property bool discardOnClose: false
    // Notifies only when the number of tracks actually changes, so the track
    // Repeater keeps its delegates (and their focus and edits) otherwise.
    readonly property int trackSlots: session.trackCount
    // Keyboard focus must stay visible: scroll the track strip to a control
    // that receives focus outside its viewport.
    onActiveFocusItemChanged: reveal(activeFocusItem)
    // Scrolls the track strip horizontally and the pane vertically until the
    // item is inside both viewports.
    function reveal(target) {
        if (!target) return
        for (const view of [trackScroll.contentItem, paneScroll.contentItem]) {
            if (!view || !view.contentItem) continue
            let inside = false
            for (let item = target; item; item = item.parent) if (item === view.contentItem) { inside = true; break }
            if (!inside) continue
            const at = target.mapToItem(view, 0, 0)
            if (view === trackScroll.contentItem) {
                if (at.x < 0) view.contentX = Math.max(0, view.contentX + at.x - 16)
                else if (at.x + target.width > view.width)
                    view.contentX = Math.max(0, Math.min(view.contentWidth - view.width, view.contentX + at.x + target.width - view.width + 16))
            } else {
                if (at.y < 0) view.contentY = Math.max(0, view.contentY + at.y - 16)
                else if (at.y + target.height > view.height)
                    view.contentY = Math.max(0, Math.min(view.contentHeight - view.height, view.contentY + at.y + target.height - view.height + 16))
            }
        }
    }
    property bool textOwnsUndo: (activeFocusItem instanceof TextInput || activeFocusItem instanceof TextEdit) && !activeFocusItem.readOnly
    Shortcut { sequences: [StandardKey.Undo]; enabled: session.canUndo && !window.textOwnsUndo && !closeDialog.visible && !renameDialog.visible; onActivated: session.undo() }
    Shortcut { sequences: [StandardKey.Redo]; enabled: session.canRedo && !window.textOwnsUndo && !closeDialog.visible && !renameDialog.visible; onActivated: session.redo() }
    // Keyboard contract (see docs/keyboard.md). Text fields keep their own
    // editing keys; Space toggles transport only when no control would use it.
    readonly property bool dialogOpen: closeDialog.visible || renameDialog.visible
    readonly property bool focusUsesSpace: activeFocusItem !== null && activeFocusItem !== window.contentItem && (window.textOwnsUndo || activeFocusItem.hasOwnProperty("checkable") || activeFocusItem.hasOwnProperty("currentIndex"))
    Shortcut { objectName: "saveShortcut"; sequences: [StandardKey.Save]; enabled: !window.dialogOpen; onActivated: session.saveProject(projectFile.text) }
    Shortcut { objectName: "openShortcut"; sequences: [StandardKey.Open]; enabled: !window.dialogOpen && session.canEdit; onActivated: session.loadProject(projectFile.text) }
    Shortcut { objectName: "transportShortcut"; sequence: "Space"; enabled: !window.dialogOpen && !window.focusUsesSpace && session.audioAvailable; onActivated: session.running ? session.stop() : session.play() }
    Shortcut { objectName: "panicShortcut"; sequence: "Ctrl+."; enabled: !window.dialogOpen; onActivated: session.panic() }
    Shortcut { objectName: "addAudioShortcut"; sequence: "Ctrl+Shift+A"; enabled: !window.dialogOpen && session.canChange; onActivated: session.addTrack("audio") }
    Shortcut { objectName: "addMidiShortcut"; sequence: "Ctrl+Shift+M"; enabled: !window.dialogOpen && session.canChange; onActivated: session.addTrack("midi") }
    Shortcut { objectName: "exportShortcut"; sequence: "Ctrl+E"; enabled: !window.dialogOpen && session.canEdit && !session.exporting; onActivated: exportButton.clicked() }
    // After a track is deleted, focus moves to the same control on the track
    // that took its place (or the previous one), never to nothing.
    function focusAfterDeletion(index) {
        Qt.callLater(function() {
            const target = trackRepeater.count === 0 ? addAudioButton : trackRepeater.itemAt(Math.min(index, trackRepeater.count - 1)).deleteButton
            target.forceActiveFocus(Qt.TabFocusReason)
        })
    }
    title: "fl-live · Session" + (session.dirty ? " · Unsaved changes" : "")
    onClosing: function(close) {
        if (session.dirty && !discardOnClose) {
            close.accepted = false
            closeDialog.open()
        }
    }
    Dialog {
        id: closeDialog
        objectName: "closeDialog"
        anchors.centerIn: parent
        title: "Unsaved changes"
        modal: true
        Label { text: "Save the project before closing, or discard your edits." }
        footer: RowLayout {
            Button { text: "Save and close"; onClicked: { if (session.saveProject(projectFile.text)) { window.discardOnClose = true; window.close() } } }
            Button { text: "Discard and close"; onClicked: { window.discardOnClose = true; window.close() } }
            Button { objectName: "cancelClose"; text: "Cancel"; onClicked: closeDialog.close() }
        }
    }
    color: "#191d24"
    Dialog {
        id: renameDialog
        objectName: "renameDialog"
        anchors.centerIn: parent
        modal: true
        title: scene ? "Rename scene" : track ? "Rename track" : "Rename clip"
        property bool track: true
        property bool scene: false
        property string entityId
        property string errorText: ""
        property var returnFocus
        function edit(isTrack, identity, name, control, isScene = false) {
            track = isTrack; scene = isScene; entityId = identity
            // Force a fresh text-undo boundary even when reopening the same name.
            renameValue.text = ""; renameValue.text = name
            returnFocus = control; errorText = ""; open()
        }
        function commit() {
            const ok = scene ? session.renameScene(entityId, renameValue.text) : track ? session.renameTrack(entityId, renameValue.text) : session.renameClip(entityId, renameValue.text)
            if (ok) close(); else errorText = session.message
        }
        onOpened: { renameValue.forceActiveFocus(); renameValue.selectAll() }
        onClosed: { if (returnFocus) returnFocus.forceActiveFocus() }
        contentItem: ColumnLayout {
            TextField { id: renameValue; objectName: "renameValue"; Layout.preferredWidth: 320; selectByMouse: true; Accessible.name: "New name"; onAccepted: renameDialog.commit() }
            Label { Layout.preferredWidth: 320; text: renameDialog.errorText; textFormat: Text.PlainText; visible: text.length > 0; wrapMode: Text.WordWrap; color: "#e4bc75" }
        }
        footer: RowLayout {
            Button { objectName: "applyRename"; text: "Apply"; enabled: session.canChange; onClicked: renameDialog.commit() }
            Button { objectName: "cancelRename"; text: "Cancel"; onClicked: renameDialog.close() }
        }
    }
    palette.windowText: "#e8edf5"
    palette.buttonText: "#e8edf5"
    palette.button: "#333e4d"
    palette.base: "#242c37"
    palette.text: "#e8edf5"
    palette.highlight: "#68d9a4"
    ColumnLayout {
        anchors.fill: parent; anchors.margins: 24; spacing: 12
        RowLayout {
            Layout.fillWidth: true
            Label { text: "fl-live"; font.pixelSize: 26; font.bold: true }
            Label { text: "SESSION / 001"; color: "#9daabd"; Layout.fillWidth: true }
            Button { objectName: "undoButton"; text: "Undo"; enabled: session.canUndo; onClicked: session.undo(); ToolTip.visible: hovered; ToolTip.text: session.undoText }
            Button { objectName: "redoButton"; text: "Redo"; enabled: session.canRedo; onClicked: session.redo(); ToolTip.visible: hovered; ToolTip.text: session.redoText }
            Label { text: session.mode; color: "#e4bc75" }
        }
        Repeater {
            // Snapshots left by sessions that ended without a normal shutdown.
            model: session.recoveryCandidates
            Rectangle {
                required property var modelData
                required property int index
                Layout.fillWidth: true; implicitHeight: recoveryRow.implicitHeight + 16; radius: 6
                color: modelData.valid ? "#2c3f35" : "#45352a"
                RowLayout {
                    id: recoveryRow
                    anchors.fill: parent; anchors.margins: 8
                    Label {
                        objectName: "recoveryText" + index; Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText
                        text: modelData.valid
                            ? "Unsaved work from " + modelData.time + " was found (" + (modelData.project ? modelData.project : "never saved") + ", " + modelData.tracks + " tracks). Restore it, or discard it."
                            : "A recovery file could not be used: " + modelData.reason + ". It was left in place; discard it when you no longer need it."
                    }
                    Button { objectName: "restoreRecovery" + index; text: "Restore"; Accessible.name: "Restore recovered work from " + modelData.time; visible: modelData.valid; enabled: session.canEdit && !session.dirty; onClicked: session.restoreRecovery(modelData.file); ToolTip.visible: hovered; ToolTip.text: "Opens the recovered work unsaved. The project file is not changed until you Save." }
                    Button { objectName: "discardRecovery" + index; text: "Discard"; Accessible.name: "Discard recovery file from " + modelData.time; onClicked: session.discardRecovery(modelData.file) }
                }
            }
        }
        RowLayout {
            Button { objectName: "playButton"; text: "Play"; enabled: session.audioAvailable; onClicked: session.play() }
            Button { objectName: "retryAudioButton"; text: "Retry audio"; visible: !session.audioAvailable; onClicked: session.reconnectAudio() }
            Button { objectName: "stopButton"; text: "Stop"; onClicked: session.stop() }
            TextField {
                id: inputTargetField; objectName: "inputTarget"; placeholderText: "Audio input node"
                property string sessionValue: session.inputTarget; onSessionValueChanged: text = sessionValue; Component.onCompleted: text = sessionValue
                Layout.preferredWidth: 150
                enabled: !session.recording; onEditingFinished: if (text !== session.inputTarget && !session.setInputTarget(text)) text = session.inputTarget
                Accessible.name: "Audio input node to record from"
                ToolTip.visible: hovered; ToolTip.text: "The PipeWire node the audio input links to. Empty waits for a link made elsewhere."
            }
            TextField {
                id: midiInputTargetField; objectName: "midiInputTarget"; placeholderText: "MIDI device or port"
                property string sessionValue: session.midiInputTarget; onSessionValueChanged: text = sessionValue; Component.onCompleted: text = sessionValue
                Layout.preferredWidth: 150
                enabled: !session.recording; onEditingFinished: if (text !== session.midiInputTarget && !session.setMidiInputTarget(text)) text = session.midiInputTarget
                Accessible.name: "MIDI device or port to record from"
                ToolTip.visible: hovered; ToolTip.text: "Part of a MIDI port name, such as MPK Mini, or a node name. fl-live links the matching MIDI outputs. Empty waits for a link made elsewhere."
            }
            Button {
                objectName: "recoverTakes"; text: "Recover takes"; visible: session.interruptedTakes.length > 0
                onClicked: session.recoverTakes()
                Accessible.name: "Recover " + session.interruptedTakes.length + " interrupted takes as clips"
                ToolTip.visible: hovered; ToolTip.text: "Takes left unfinished by a crash. Recovery writes each to a finished file and adds it to its track; nothing is replaced."
            }
            Button {
                objectName: "recordButton"; text: session.recording ? "● Recording" : "Record"
                enabled: session.audioAvailable && !session.recording && session.armed.indexOf(true) >= 0
                palette.button: session.recording ? "#8c2f39" : palette.button
                onClicked: session.record()
                Accessible.name: session.recording ? "Recording; press Stop to finish the takes" : "Record armed tracks"
                ToolTip.visible: hovered; ToolTip.text: "Records every armed audio track into a new take in the project's recordings folder. Stop finishes the takes as new clips."
            }
            Button { text: "Return to start"; enabled: !session.running && !session.pending; onClicked: session.rewind() }
            Label { objectName: "transportState"; text: session.running ? "Playing" : "Stopped"; Layout.preferredWidth: 80 }
            Label { text: session.position; font.pixelSize: 24; font.family: "monospace"; Layout.fillWidth: true }
            Label { text: "BPM"; color: "#9daabd" }
            SpinBox { objectName: "tempoSpin"; from: 20; to: 300; value: session.tempo; enabled: session.canEdit; editable: true; onValueModified: session.setTempo(value); Accessible.name: "Tempo in beats per minute" }
            Button { text: "Silence now"; onClicked: session.panic() }
        }
        RowLayout {
            Layout.fillWidth: true
            TextField { id: projectFile; objectName: "projectFile"; Layout.fillWidth: true
                property string sessionValue: session.projectPath; onSessionValueChanged: text = sessionValue; Component.onCompleted: text = sessionValue
                placeholderText: "Project file path"; Accessible.name: "Project file path" }
            Button { objectName: "newProjectButton"; text: "New"; enabled: session.canEdit && !session.dirty; onClicked: session.newProject(); Accessible.name: "New project"; ToolTip.visible: hovered; ToolTip.text: "Start the default project. Save or undo your edits first." }
            Button { objectName: "saveProjectButton"; text: "Save"; onClicked: session.saveProject(projectFile.text) }
            Button { objectName: "collectAssets"; text: "Collect audio"; enabled: session.canEdit; onClicked: session.collectAssets(); ToolTip.visible: hovered; ToolTip.text: "Copy imported WAVs from outside the project folder into a folder beside the saved project. Nothing is overwritten." }
            Button { objectName: "openProjectButton"; text: "Open"; enabled: session.canEdit; onClicked: session.loadProject(projectFile.text) }
            Label { text: session.dirty ? "Unsaved changes" : "No pending edits"; color: "#e4bc75" }
        }
        RowLayout {
            Layout.fillWidth: true
            TextField { id: exportFile; objectName: "exportFile"; Layout.fillWidth: true; placeholderText: "New WAV file path"; Accessible.name: "New WAV export path" }
            Label { text: "From bar" }
            SpinBox { id: exportStart; objectName: "exportStart"; from: 1; to: 256; value: 1; editable: true; enabled: session.canEdit; Layout.preferredWidth: 84; Accessible.name: "Export start bar" }
            Label { text: "Bars" }
            SpinBox { id: exportBars; objectName: "exportBars"; from: 1; to: 64; value: 4; editable: true; enabled: session.canEdit; Layout.preferredWidth: 84; Accessible.name: "Export length in bars" }
            Label { text: "Tail ms" }
            SpinBox { id: exportTail; objectName: "exportTail"; from: 0; to: 10000; stepSize: 100; value: 0; editable: true; enabled: session.canEdit; Layout.preferredWidth: 96; Accessible.name: "Export tail in milliseconds"; ToolTip.visible: hovered; ToolTip.text: "Time rendered after every track stops at the end of the range. Silent until effects or release envelopes exist." }
            ComboBox { id: exportChannels; objectName: "exportChannels"; model: ["Stereo", "Mono"]; Layout.preferredWidth: 92; Accessible.name: "Export channels" }
            ComboBox { id: exportFormat; objectName: "exportFormat"; model: ["16-bit", "24-bit", "32-bit float"]; Layout.preferredWidth: 120; Accessible.name: "Export sample format" }
            Button { id: exportButton; objectName: "exportWavButton"; text: "Export all tracks"; visible: !session.exporting; enabled: session.canEdit; onClicked: session.exportAudio(exportFile.text, exportStart.value, exportBars.value, exportTail.value, exportChannels.currentIndex === 0 ? 2 : 1, ["pcm16", "pcm24", "float32"][exportFormat.currentIndex]) }
            ProgressBar { objectName: "exportProgress"; visible: session.exporting; from: 0; to: 1; value: session.exportProgress; Layout.preferredWidth: 120; Accessible.name: "Export progress" }
            Button { objectName: "cancelExportButton"; text: "Cancel export"; visible: session.exporting; onClicked: session.cancelExport(); Accessible.description: "Stops the export and removes the partial file" }
        }
        RowLayout {
            Layout.fillWidth: true
            Label { text: "Scenes" }
            ComboBox {
                id: sceneSelector
                objectName: "sceneSelector"; Layout.fillWidth: true; model: session.sceneNames
                currentIndex: session.selectedSceneIndex
                onActivated: function(index) { session.selectScene(session.sceneIds[index]) }
                Accessible.name: "Selected scene"
                contentItem: Text { text: sceneSelector.currentText; textFormat: Text.PlainText; color: sceneSelector.palette.text; elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter }
                delegate: ItemDelegate {
                    required property string modelData
                    required property int index
                    width: sceneSelector.width; highlighted: sceneSelector.highlightedIndex === index
                    contentItem: Text { text: modelData; textFormat: Text.PlainText; color: sceneSelector.palette.text; elide: Text.ElideRight }
                }
            }
            Button { objectName: "createScene"; text: "New scene"; enabled: session.canChange && session.sceneIds.length < 256; onClicked: session.createScene() }
            Button { objectName: "copyScene"; text: "Copy arrangement"; enabled: session.canChange && session.sceneIds.length < 256; onClicked: session.copyScene(session.sceneId); ToolTip.visible: hovered; ToolTip.text: "Copies slot choices into a new scene. Clip sources remain shared." }
            Button { objectName: "sceneUp"; text: "Move up"; enabled: session.canChange && session.selectedSceneIndex > 0; onClicked: session.moveScene(session.sceneId, -1) }
            Button { objectName: "sceneDown"; text: "Move down"; enabled: session.canChange && session.selectedSceneIndex+1 < session.sceneIds.length; onClicked: session.moveScene(session.sceneId, 1) }
            Button { objectName: "deleteScene"; text: "Delete scene"; enabled: session.canChange && session.sceneIds.length > 1; onClicked: session.deleteScene(session.sceneId) }
        }
        RowLayout {
            Layout.fillWidth: true
            Label { text: "SCENE"; color: "#9daabd" }
            Label { objectName: "sceneName"; text: session.sceneName; textFormat: Text.PlainText; elide: Text.ElideRight; Layout.fillWidth: true; font.bold: true }
            ToolButton { id: sceneRename; objectName: "renameScene"; text: "Rename"; enabled: session.canChange; Accessible.name: "Rename scene"; onClicked: renameDialog.edit(false, session.sceneId, session.sceneName, sceneRename, true) }
            Label { text: "Timing" }
            ComboBox { id: sceneTiming; objectName: "sceneTiming"; model: ["Immediate", "One bar"]; currentIndex: 1; Accessible.name: "Scene launch and stop timing" }
            Button { objectName: "launchScene"; text: "Launch scene"; enabled: session.audioAvailable; onClicked: session.launchScene(session.sceneId, sceneTiming.currentIndex === 0 ? "immediate" : "one-bar") }
            Button { objectName: "stopScene"; text: "Stop scene"; enabled: session.audioAvailable; onClicked: session.stopScene(session.sceneId, sceneTiming.currentIndex === 0 ? "immediate" : "one-bar") }
            Label { objectName: "sceneState"; text: session.sceneState; Layout.preferredWidth: 180; wrapMode: Text.WordWrap; color: "#68d9a4" }
        }
        RowLayout {
            Layout.fillWidth: true
            Label { text: "TRACKS"; color: "#9daabd" }
            Button { id: addAudioButton; objectName: "addAudioTrack"; text: "Add audio track"; enabled: session.canChange && session.trackCount < 64; onClicked: session.addTrack("audio") }
            Button { objectName: "addMidiTrack"; text: "Add MIDI track"; enabled: session.canChange && session.trackCount < 64; onClicked: session.addTrack("midi") }
            Item { Layout.fillWidth: true }
            Label { text: session.trackCount + (session.trackCount === 1 ? " track" : " tracks"); color: "#9daabd" }
        }
        Rectangle {
            color: "#242c37"; radius: 8
            Layout.fillWidth: true; Layout.fillHeight: true
            // The pane scrolls vertically when open editors make it taller than
            // the window; otherwise its content fills the pane exactly.
            ScrollView {
                id: paneScroll
                objectName: "paneScroll"
                anchors.fill: parent; anchors.margins: 16
                contentWidth: availableWidth
                clip: true
            ColumnLayout {
                width: paneScroll.availableWidth
                height: Math.max(implicitHeight, paneScroll.availableHeight)
                spacing: 12
                ScrollView {
                    id: trackScroll
                    objectName: "trackScroll"
                    Layout.fillWidth: true
                    Layout.preferredHeight: trackRow.implicitHeight + 18
                    contentWidth: trackRow.implicitWidth; contentHeight: trackRow.implicitHeight
                    clip: true
                    RowLayout {
                        id: trackRow; spacing: 16
                        Label { visible: session.trackCount === 0; text: "No tracks. Add an audio or MIDI track."; color: "#b7c5d8" }
                        Repeater {
                            id: trackRepeater
                            // An integer model keeps delegates (and focus) stable across status updates.
                            model: window.trackSlots
                            ColumnLayout {
                                id: trackColumn
                                property alias deleteButton: deleteButton
                                required property int index
                                readonly property string trackId: session.trackIds[index] || ""
                                readonly property bool midi: session.trackKinds[index] === "midi"
                                // The track's clips and the selected one, which the controls below act on.
                                readonly property var clips: session.trackClips[index] || []
                                readonly property int selectedIndex: Math.max(0, clips.findIndex(c => c.id === session.selectedClips[index]))
                                // Wide enough for the range row; every row must fit so columns never overlap.
                                Layout.preferredWidth: 340; Layout.maximumWidth: 340; Layout.alignment: Qt.AlignTop
                                spacing: 8
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label { objectName: "trackName" + index; text: session.trackNames[index]; textFormat: Text.PlainText; font.bold: true; font.pixelSize: 18; Layout.fillWidth: true; elide: Text.ElideRight }
                                    Label { objectName: "trackKind" + index; text: trackColumn.midi ? "MIDI" : "AUDIO"; color: trackColumn.midi ? "#e4bc75" : "#68d9a4"; font.pixelSize: 11 }
                                    ToolButton { id: trackRename; objectName: "renameTrack" + index; text: "Rename"; enabled: session.canChange; Accessible.name: "Rename track " + (index + 1); onClicked: renameDialog.edit(true, trackColumn.trackId, session.trackNames[index], trackRename) }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Button {
                                        id: clipButton
                                        objectName: "clip" + index
                                        Layout.fillWidth: true; Layout.preferredHeight: 64
                                        highlighted: session.activeClips[index] === trackColumn.selectedIndex
                                        text: (session.activeClips[index] === trackColumn.selectedIndex ? "●  " : "▶  ") + session.clipNames[index] + "\n" + session.trackSources[index]
                                        contentItem: Text { text: clipButton.text; textFormat: Text.PlainText; color: clipButton.palette.buttonText; font: clipButton.font; wrapMode: Text.Wrap; elide: Text.ElideRight; maximumLineCount: 2; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                                        onClicked: session.launchClip(session.clipIds[index])
                                        Accessible.name: "Launch " + session.clipNames[index] + " on track " + (index + 1)
                                        Accessible.description: session.clipStates[index]
                                    }
                                    ToolButton { id: clipRename; objectName: "renameClip" + index; text: "Rename"; enabled: session.canChange; Accessible.name: "Rename clip " + (index + 1); onClicked: renameDialog.edit(false, session.clipIds[index], session.clipNames[index], clipRename) }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    ComboBox {
                                        objectName: "clipSelector" + index; Layout.fillWidth: true
                                        model: trackColumn.clips.map(c => c.name); currentIndex: trackColumn.selectedIndex
                                        enabled: trackColumn.clips.length > 1
                                        onActivated: function(i) { session.selectClip(trackColumn.clips[i].id) }
                                        Accessible.name: "Selected clip of track " + (index + 1)
                                    }
                                    Button { objectName: "addClip" + index; text: "+ Clip"; enabled: session.canChange && trackColumn.clips.length < 8; onClicked: session.addClip(trackColumn.trackId); Accessible.name: "Add a clip to track " + (index + 1) }
                                    Button { objectName: "duplicateClip" + index; text: "Copy"; enabled: session.canChange && trackColumn.clips.length < 8; onClicked: session.duplicateClip(session.clipIds[index]); Accessible.name: "Duplicate the selected clip of track " + (index + 1) }
                                    Button { objectName: "deleteClip" + index; text: "Delete clip"; enabled: session.canChange && trackColumn.clips.length > 1; onClicked: session.deleteClip(session.clipIds[index]); Accessible.name: "Delete the selected clip of track " + (index + 1) }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label { objectName: "clipState" + index; text: session.clipStates[index]; Layout.fillWidth: true; color: session.activeClips[index] >= 0 ? "#68d9a4" : "#b7c5d8" }
                                    CheckBox { objectName: "monitor" + index; text: "Monitor"; checked: session.monitored[index] === true; onClicked: session.setMonitored(trackColumn.trackId, checked); Accessible.name: "Monitor the input on track " + (index + 1) + " while armed" }
                                    CheckBox { objectName: "arm" + index; text: "Arm"; checked: session.armed[index] === true; enabled: !session.recording; onClicked: session.setArmed(trackColumn.trackId, checked); Accessible.name: "Arm track " + (index + 1) + " for recording" }
                                    CheckBox { objectName: "sceneIncluded" + index; text: "Include in scene"; checked: session.sceneIncluded[index]; enabled: session.canChange; onClicked: session.setSceneIncluded(trackColumn.trackId, checked); Accessible.name: "Include track " + (index+1) + " in scene"; ToolTip.visible: hovered; ToolTip.text: "An excluded track stops when this scene launches." }
                                }
                                Button { objectName: "stopTrack" + index; text: "Stop track on next bar"; Accessible.name: "Stop track " + (index + 1) + " on next bar"; Layout.fillWidth: true; onClicked: session.stopTrackId(trackColumn.trackId) }
                                Label { objectName: "trackSource" + index; text: session.trackSources[index]; textFormat: Text.PlainText; elide: Text.ElideMiddle; Layout.fillWidth: true; color: text.startsWith("Missing") ? "#e4bc75" : "#b7c5d8" }
                                Button {
                                    objectName: "editPattern" + index; visible: trackColumn.midi; Layout.fillWidth: true
                                    Accessible.name: "Edit pattern of track " + (index + 1)
                                    text: session.patternTrackIndex === index ? "Editing pattern below" : "Edit pattern"
                                    highlighted: session.patternTrackIndex === index
                                    onClicked: session.editPattern(trackColumn.trackId)
                                }
                                RowLayout {
                                    visible: !trackColumn.midi; Layout.fillWidth: true
                                    TextField { id: audioPath; objectName: "audioPath" + index; Layout.fillWidth: true; placeholderText: "WAV file path"; Accessible.name: "WAV file to import into track " + (index + 1) }
                                    Button {
                                        objectName: "relinkAudio" + index; text: "Relink"; Accessible.name: "Relink audio of track " + (index + 1)
                                        visible: (session.clipFrames[index] || 0) > 0 || (session.trackSources[index] || "").startsWith("Missing")
                                        enabled: !session.importing; onClicked: session.relinkAudio(trackColumn.trackId, audioPath.text)
                                        ToolTip.visible: hovered; ToolTip.text: "Point this clip at another file and keep its range. Refused if the file is shorter than the range."
                                    }
                                    Button { objectName: "importAudio" + index; text: "Import"; Accessible.name: "Import WAV into track " + (index + 1); enabled: !session.importing; onClicked: session.importAudio(trackColumn.trackId, audioPath.text); ToolTip.visible: hovered; ToolTip.text: "Mono or stereo WAV at 48 kHz. Decoding runs in the background; the clip changes once transport is stopped." }
                                }
                                Canvas {
                                    id: waveform
                                    objectName: "waveform" + index
                                    readonly property real frames: session.clipFrames[index] || 0
                                    readonly property var range: session.clipRanges[index] || [0, 0, true]
                                    // A string key changes only when the drawing would, so the
                                    // 10 ms status updates do not repaint the canvas.
                                    readonly property string paintKey: frames + ":" + range[0] + ":" + range[1] + ":" + range[2] + ":" + width
                                    visible: !trackColumn.midi && frames > 0
                                    Layout.fillWidth: true; Layout.preferredHeight: 56
                                    onPaintKeyChanged: requestPaint()
                                    Accessible.role: Accessible.Graphic
                                    Accessible.name: "Waveform of track " + (index + 1) + ", playing " + (range[0] / 48000).toFixed(3) + " to " + (range[1] / 48000).toFixed(3) + " seconds" + (range[2] ? ", looped" : ", once")
                                    onPaint: {
                                        const ctx = getContext("2d")
                                        ctx.reset()
                                        ctx.fillStyle = "#1c222b"; ctx.fillRect(0, 0, width, height)
                                        if (frames <= 0 || width < 1) return
                                        const x0 = range[0] / frames * width, x1 = range[1] / frames * width
                                        ctx.fillStyle = range[2] ? "#2c4a3f" : "#4a3f2c"; ctx.fillRect(x0, 0, Math.max(1, x1 - x0), height)
                                        const peaks = session.waveform(index, Math.max(1, Math.floor(width)))
                                        ctx.strokeStyle = "#9fe3c4"; ctx.beginPath()
                                        for (let i = 0; i < peaks.length; ++i) {
                                            const lo = Math.max(-1, Math.min(1, peaks[i][0])), hi = Math.max(-1, Math.min(1, peaks[i][1]))
                                            ctx.moveTo(i + 0.5, height / 2 - hi * height / 2); ctx.lineTo(i + 0.5, height / 2 - lo * height / 2 + 1)
                                        }
                                        ctx.stroke()
                                    }
                                }
                                RowLayout {
                                    visible: waveform.visible; Layout.fillWidth: true
                                    Label { text: "Start ms"; color: "#9daabd" }
                                    SpinBox {
                                        objectName: "rangeStart" + index; editable: true; Layout.preferredWidth: 88
                                        from: 0; to: Math.max(0, Math.ceil(waveform.range[1] / 48) - 1)
                                        value: Math.round(waveform.range[0] / 48); enabled: session.canChange
                                        onValueModified: session.setClipRange(trackColumn.trackId, Math.min(value * 48, waveform.range[1] - 1), waveform.range[1], waveform.range[2])
                                        Accessible.name: "Track " + (index + 1) + " clip start in milliseconds"
                                    }
                                    Label { text: "End"; color: "#9daabd" }
                                    SpinBox {
                                        objectName: "rangeEnd" + index; editable: true; Layout.preferredWidth: 88
                                        from: Math.floor(waveform.range[0] / 48) + 1; to: Math.ceil(waveform.frames / 48)
                                        value: Math.ceil(waveform.range[1] / 48); enabled: session.canChange
                                        // The last millisecond step always reaches the true end of the source.
                                        onValueModified: session.setClipRange(trackColumn.trackId, waveform.range[0], Math.min(value * 48, waveform.frames), waveform.range[2])
                                        Accessible.name: "Track " + (index + 1) + " clip end in milliseconds"
                                    }
                                    CheckBox {
                                        objectName: "rangeLoop" + index; text: "Loop"; checked: waveform.range[2]; enabled: session.canChange
                                        onClicked: session.setClipRange(trackColumn.trackId, waveform.range[0], waveform.range[1], checked)
                                        Accessible.name: "Loop track " + (index + 1) + " clip range"; ToolTip.visible: hovered; ToolTip.text: "Off plays the range once, then the track stops."
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label { text: "Level %"; color: "#9daabd" }
                                    SpinBox { objectName: "level" + index; from: 0; to: 100; value: session.levels[index]; enabled: session.canChange; onValueModified: session.setLevel(index, value); Accessible.name: "Track " + (index + 1) + " level percent" }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label { text: "Pan"; color: "#9daabd" }
                                    SpinBox {
                                        objectName: "pan" + index; from: -100; to: 100; stepSize: 5; editable: true
                                        value: session.pans[index]; enabled: session.canChange
                                        textFromValue: function(value) { return value === 0 ? "C" : value < 0 ? "L" + (-value) : "R" + value }
                                        validator: RegularExpressionValidator { regularExpression: /^\s*([CcLlRr]\d{0,3}|-?\d{1,3})\s*$/ }
                                        valueFromText: function(text) {
                                            const t = text.trim().toUpperCase()
                                            const v = t === "C" ? 0 : t[0] === "L" ? -parseInt(t.slice(1)) : t[0] === "R" ? parseInt(t.slice(1)) : parseInt(t)
                                            return isNaN(v) ? value : Math.max(from, Math.min(to, v))
                                        }
                                        onValueModified: session.setPan(trackColumn.trackId, value)
                                        Accessible.name: "Track " + (index + 1) + " pan, left to right"
                                        ToolTip.visible: hovered; ToolTip.text: "Balance pan: the far side is reduced, the near side stays at full level."
                                        Layout.preferredWidth: 96
                                    }
                                    CheckBox { objectName: "mute" + index; text: "Mute"; checked: session.mutes[index]; enabled: session.canChange; onClicked: session.setMute(trackColumn.trackId, checked); Accessible.name: "Mute track " + (index + 1) }
                                    CheckBox { objectName: "solo" + index; text: "Solo"; checked: session.solos[index]; enabled: session.canChange; onClicked: session.setSolo(trackColumn.trackId, checked); Accessible.name: "Solo track " + (index + 1); ToolTip.visible: hovered; ToolTip.text: "While any track is soloed, only soloed tracks play. Mute wins over solo." }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Button { objectName: "duplicateTrack" + index; text: "Duplicate"; enabled: session.canChange && session.trackCount < 64; onClicked: session.duplicateTrack(trackColumn.trackId); Accessible.name: "Duplicate track " + (index + 1) }
                                    Button { id: deleteButton; objectName: "deleteTrack" + index; text: "Delete"; enabled: session.canChange; onClicked: {
                                            // The track list rebuilds on deletion; capture what the focus move needs first.
                                            const position = index, appWindow = ApplicationWindow.window
                                            if (session.deleteTrack(trackColumn.trackId)) appWindow.focusAfterDeletion(position)
                                        } Accessible.name: "Delete track " + (index + 1); ToolTip.visible: hovered; ToolTip.text: "Removes the track, its clip and its scene slots. Undo restores them." }
                                    CheckBox { id: automationToggle; objectName: "automationToggle" + index; text: "Automation"; Accessible.name: "Show automation for track " + (index + 1) }
                                }
                                RowLayout {
                                    readonly property var devices: session.devices[trackColumn.index] || []
                                    id: deviceHeader
                                    Layout.fillWidth: true
                                    Label { objectName: "deviceCount" + index; text: deviceHeader.devices.length === 0 ? "No devices" : deviceHeader.devices.length + (deviceHeader.devices.length === 1 ? " device" : " devices"); color: "#9daabd" }
                                    Item { Layout.fillWidth: true }
                                    Button { objectName: "addGain" + index; text: "+ Gain"; enabled: session.canChange && deviceHeader.devices.length < 8; onClicked: { if (session.addDevice(trackColumn.trackId, "gain")) devicesToggle.checked = true } Accessible.name: "Add a gain device to track " + (index + 1) }
                                    Button { objectName: "addTranspose" + index; text: "+ Transpose"; visible: trackColumn.midi; enabled: session.canChange && deviceHeader.devices.length < 8; onClicked: { if (session.addDevice(trackColumn.trackId, "transpose")) devicesToggle.checked = true } Accessible.name: "Add a transpose device to track " + (index + 1) }
                                    CheckBox { id: devicesToggle; objectName: "devicesToggle" + index; text: "Show"; enabled: deviceHeader.devices.length > 0; Accessible.name: "Show devices of track " + (index + 1) }
                                }
                                Repeater {
                                    model: devicesToggle.checked ? deviceHeader.devices : []
                                    RowLayout {
                                        required property var modelData
                                        required property int index
                                        readonly property bool gain: modelData.kind === "gain"
                                        Layout.fillWidth: true
                                        Label { text: (index + 1) + ". " + (parent.gain ? "Gain dB" : "Transpose"); Layout.preferredWidth: 96 }
                                        SpinBox {
                                            objectName: "deviceValue" + trackColumn.index + "_" + index
                                            from: parent.gain ? -60 : -48; to: parent.gain ? 12 : 48; editable: true; Layout.preferredWidth: 90
                                            value: modelData.value; enabled: session.canChange
                                            onValueModified: session.setDeviceValue(modelData.id, value)
                                            Accessible.name: (parent.gain ? "Gain in decibels" : "Transpose in semitones") + " of device " + (index + 1) + " on track " + (trackColumn.index + 1)
                                        }
                                        ToolButton { objectName: "deviceUp" + trackColumn.index + "_" + index; text: "↑"; enabled: session.canChange; onClicked: session.moveDevice(modelData.id, -1); Accessible.name: "Move device " + (index + 1) + " of track " + (trackColumn.index + 1) + " earlier" }
                                        ToolButton { objectName: "deviceDown" + trackColumn.index + "_" + index; text: "↓"; enabled: session.canChange; onClicked: session.moveDevice(modelData.id, 1); Accessible.name: "Move device " + (index + 1) + " of track " + (trackColumn.index + 1) + " later" }
                                        ToolButton { objectName: "removeDevice" + trackColumn.index + "_" + index; text: "Remove"; enabled: session.canChange; onClicked: session.removeDevice(modelData.id); Accessible.name: "Remove device " + (index + 1) + " of track " + (trackColumn.index + 1) }
                                    }
                                }
                                ColumnLayout {
                                    id: automationEditor
                                    visible: automationToggle.checked; Layout.fillWidth: true; spacing: 4
                                    // Level, Pan, then the track's devices in chain order.
                                    readonly property var devices: session.devices[index] || []
                                    readonly property var device: automationParameter.currentIndex >= 2 ? devices[automationParameter.currentIndex - 2] || null : null
                                    readonly property string parameter: automationParameter.currentIndex === 1 ? "pan" : device ? "device:" + device.id : "level"
                                    readonly property real low: parameter === "level" ? 0 : parameter === "pan" ? -100 : device.kind === "gain" ? -60 : -48
                                    readonly property real high: device ? (device.kind === "gain" ? 12 : 48) : 100
                                    // Re-read on every document change; the canvas repaints only when the points differ.
                                    readonly property var points: { session.undoText; session.dirty; return session.automationPoints(index, parameter) }
                                    readonly property real span: Math.max(1600, points.length ? points[points.length - 1][0] + 400 : 0)
                                    RowLayout {
                                        Layout.fillWidth: true
                                        ComboBox { id: automationParameter; objectName: "automationParameter" + index; model: ["Level", "Pan"].concat(automationEditor.devices.map((d, i) => (d.kind === "gain" ? "Gain " : "Transpose ") + (i + 1))); Layout.preferredWidth: 120; Accessible.name: "Automated parameter of track " + (index + 1) }
                                        Label { text: automationEditor.points.length + (automationEditor.points.length === 1 ? " point" : " points"); color: "#9daabd"; Layout.fillWidth: true }
                                        Button { objectName: "clearAutomation" + index; text: "Clear"; enabled: session.canChange && automationEditor.points.length > 0; onClicked: session.clearAutomation(trackColumn.trackId, automationEditor.parameter); Accessible.name: "Clear automation of track " + (index + 1) }
                                    }
                                    Canvas {
                                        id: lane
                                        objectName: "automationLane" + index
                                        Layout.fillWidth: true; Layout.preferredHeight: 48
                                        readonly property string paintKey: JSON.stringify(automationEditor.points) + automationEditor.parameter + width
                                        onPaintKeyChanged: requestPaint()
                                        function xAt(at) { return at / automationEditor.span * width }
                                        // The top pixel row is the parameter's maximum, the bottom row its minimum.
                                        function yAt(value) { return (height - 1) - (value - automationEditor.low) / (automationEditor.high - automationEditor.low) * (height - 1) }
                                        Accessible.role: Accessible.Graphic
                                        Accessible.name: "Automation lane of track " + (index + 1) + ", " + automationEditor.points.length + " points. Click to add a point, click a point to remove it."
                                        onPaint: {
                                            const ctx = getContext("2d"); ctx.reset()
                                            ctx.fillStyle = "#1c222b"; ctx.fillRect(0, 0, width, height)
                                            ctx.strokeStyle = "#333e4d"
                                            for (let bar = 400; bar < automationEditor.span; bar += 400) { ctx.beginPath(); ctx.moveTo(xAt(bar), 0); ctx.lineTo(xAt(bar), height); ctx.stroke() }
                                            const pts = automationEditor.points
                                            if (!pts.length) return
                                            ctx.strokeStyle = "#e4bc75"; ctx.beginPath(); ctx.moveTo(0, yAt(pts[0][1]))
                                            for (const p of pts) ctx.lineTo(xAt(p[0]), yAt(p[1]))
                                            ctx.lineTo(width, yAt(pts[pts.length - 1][1])); ctx.stroke()
                                            ctx.fillStyle = "#e4bc75"
                                            for (const p of pts) ctx.fillRect(xAt(p[0]) - 3, yAt(p[1]) - 3, 6, 6)
                                        }
                                        MouseArea {
                                            objectName: "automationLaneInput" + index
                                            anchors.fill: parent; enabled: session.canChange
                                            onClicked: function(mouse) {
                                                const editor = automationEditor
                                                for (const p of editor.points)
                                                    if (Math.abs(lane.xAt(p[0]) - mouse.x) <= 6 && Math.abs(lane.yAt(p[1]) - mouse.y) <= 10) {
                                                        session.removeAutomationPoint(trackColumn.trackId, editor.parameter, p[0]); return
                                                    }
                                                // Snap to a sixteenth note (25 hundredths of a beat) and a whole unit (percent, dB or semitone).
                                                const at = Math.round(mouse.x / lane.width * editor.span / 25) * 25
                                                const value = Math.round(editor.low + (lane.height - 1 - mouse.y) / (lane.height - 1) * (editor.high - editor.low))
                                                session.setAutomationPoint(trackColumn.trackId, editor.parameter, at, Math.max(editor.low, Math.min(editor.high, value)))
                                            }
                                        }
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label { text: "Beat"; color: "#9daabd" }
                                        SpinBox { id: automationAt; objectName: "automationAt" + index; from: 0; to: 4096; value: 0; editable: true; Layout.preferredWidth: 84
                                            textFromValue: function(v) { return (v / 4).toFixed(2) }
                                            valueFromText: function(t) { const n = Math.round(parseFloat(t) * 4); return isNaN(n) ? value : Math.max(from, Math.min(to, n)) }
                                            validator: RegularExpressionValidator { regularExpression: /^\s*\d{0,4}(\.\d{0,2})?\s*$/ }
                                            Accessible.name: "Automation point position in beats, track " + (index + 1) }
                                        Label { text: "Value"; color: "#9daabd" }
                                        SpinBox { id: automationValue; objectName: "automationValue" + index; from: automationEditor.low; to: automationEditor.high; value: automationEditor.parameter === "level" ? 70 : 0; editable: true; Layout.preferredWidth: 84; Accessible.name: "Automation point value in " + (!automationEditor.device ? "percent" : automationEditor.device.kind === "gain" ? "decibels" : "semitones") + ", track " + (index + 1) }
                                        Button { objectName: "automationAdd" + index; text: "Add point"; enabled: session.canChange; onClicked: session.setAutomationPoint(trackColumn.trackId, automationEditor.parameter, automationAt.value * 25, automationValue.value); Accessible.name: "Add automation point to track " + (index + 1) }
                                    }
                                }
                            }
                        }
                    }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: "#333e4d" }
                Label { visible: session.patternTrackIndex < 0; text: "Add a MIDI track to edit a step pattern."; color: "#b7c5d8" }
                ColumnLayout {
                    visible: session.patternTrackIndex >= 0
                    Layout.fillWidth: true; Layout.fillHeight: session.pianoMode; spacing: 8
                    RowLayout {
                        Layout.fillWidth: true
                        Label { objectName: "patternTitle"; text: "PATTERN · " + (session.trackNames[session.patternTrackIndex] || ""); textFormat: Text.PlainText; font.bold: true; Layout.fillWidth: true; elide: Text.ElideRight }
                        ComboBox { objectName: "synthWaveform"; model: ["Sine", "Triangle", "Square", "Saw"]; currentIndex: session.synth[0]; enabled: session.canChange; Layout.preferredWidth: 100; onActivated: function(i) { session.setSynth("waveform", i) }; Accessible.name: "Synth waveform" }
                        Repeater {
                            model: [["A", "attack", 1, 10000], ["D", "decay", 2, 10000], ["S %", "sustain", 3, 100], ["R", "release", 4, 10000]]
                            RowLayout {
                                required property var modelData
                                spacing: 2
                                Label { text: modelData[0]; color: "#9daabd" }
                                SpinBox {
                                    objectName: "synth" + modelData[1].charAt(0).toUpperCase() + modelData[1].slice(1)
                                    from: 0; to: modelData[3]; stepSize: modelData[1] === "sustain" ? 5 : 10; editable: true
                                    value: session.synth[modelData[2]]; enabled: session.canChange; Layout.preferredWidth: 78
                                    onValueModified: session.setSynth(modelData[1], value)
                                    Accessible.name: "Synth " + modelData[1] + (modelData[1] === "sustain" ? " percent" : " in milliseconds")
                                }
                            }
                        }
                        CheckBox { objectName: "pianoMode"; text: "Piano roll"; checked: session.pianoMode; enabled: session.canChange; onClicked: session.usePianoRoll(checked); Accessible.name: "Use the piano roll for this clip"; ToolTip.visible: hovered; ToolTip.text: "Turning it on converts the step pattern into notes once. Turning it off plays the step pattern again." }
                        Label { text: "Quarter-note steps"; visible: !session.pianoMode }
                        SpinBox { objectName: "stepCount"; from: 1; to: 16; value: session.stepCount; visible: !session.pianoMode; enabled: session.canChange; onValueModified: session.setStepCount(value); Accessible.name: "Pattern length in quarter notes" }
                    }
                    ColumnLayout {
                        visible: session.pianoMode
                        Layout.fillWidth: true; Layout.fillHeight: true; spacing: 6
                        RowLayout {
                            Button { objectName: "pianoOctaveDown"; text: "Octave down"; enabled: pianoView.base > 0; onClicked: pianoView.base = Math.max(0, pianoView.base - 12) }
                            Button { objectName: "pianoOctaveUp"; text: "Octave up"; enabled: pianoView.base + pianoView.rows < 128; onClicked: pianoView.base = Math.min(128 - pianoView.rows, pianoView.base + 12) }
                            Label { objectName: "pianoRange"; text: "Pitches " + pianoView.base + "–" + (pianoView.base + pianoView.rows - 1) + " · " + (session.pianoLength / 96) + " beats"; color: "#b7c5d8" }
                            Label { text: "Length (16ths)" }
                            SpinBox {
                                objectName: "pianoLengthSpin"; from: 1; to: 256; editable: true; Layout.preferredWidth: 90
                                value: Math.ceil(session.pianoLength / 24); enabled: session.canChange
                                onValueModified: session.setPianoLength(value * 24)
                                Accessible.name: "Clip length in sixteenth notes"
                                ToolTip.visible: hovered; ToolTip.text: "Notes past a shortened end are kept but silent; lengthening plays them again."
                            }
                            CheckBox { objectName: "pianoLoop"; text: "Loop"; checked: session.pianoLoop; enabled: session.canChange; onClicked: session.setPianoLoop(checked); Accessible.name: "Loop the clip" }
                            Label { text: "Snap" }
                            ComboBox { id: snapBox; objectName: "pianoSnap"; model: ["1/16", "1/8", "1/4", "Off"]; Layout.preferredWidth: 80; Accessible.name: "Piano-roll snap" }
                            Label { text: "Velocity %" }
                            SpinBox {
                                objectName: "selectedNotesVelocity"; from: 1; to: 100; editable: true; Layout.preferredWidth: 90
                                enabled: session.canChange && session.selectedNotes.length > 0
                                value: session.selectedVelocity
                                onValueModified: session.setNotesVelocity(session.selectedNotes, value)
                                Accessible.name: "Velocity of the selected notes"
                            }
                            Item { Layout.fillWidth: true }
                            Button { objectName: "deleteNotes"; text: "Delete selected"; enabled: session.canChange && session.selectedNotes.length > 0; onClicked: session.deleteNotes(session.selectedNotes); Accessible.name: "Delete selected notes" }
                        }
                        Rectangle {
                            id: pianoView
                            objectName: "pianoRoll"
                            property int base: 48
                            // Two octaves; rows share whatever height the pane has left.
                            readonly property int rows: 24
                            readonly property real rowHeight: height / rows
                            readonly property real tickWidth: width / Math.max(1, session.pianoLength)
                            readonly property int snap: [24, 48, 96, 1][snapBox.currentIndex]
                            Layout.fillWidth: true; Layout.fillHeight: true
                            Layout.minimumHeight: 120; Layout.maximumHeight: 288
                            color: "#0f141c"; border.color: activeFocus ? "#e4bc75" : "#2e3a4a"
                            clip: true // notes past the clip length are silent and not drawn
                            activeFocusOnTab: true
                            Accessible.role: Accessible.Grouping
                            Accessible.name: "Piano roll, " + session.pianoNoteCount + " notes, " + session.selectedNotes.length + " selected. Click to add or select, drag to move or resize; arrows move the selection, Shift with Left or Right resizes, Alt with Left or Right selects the previous or next note, Delete removes."
                            Keys.onPressed: function(event) {
                                if (event.key === Qt.Key_Escape) { session.clearNoteSelection(); event.accepted = true; return }
                                if ((event.modifiers & Qt.AltModifier) && (event.key === Qt.Key_Left || event.key === Qt.Key_Right)) {
                                    session.selectAdjacentNote(event.key === Qt.Key_Left ? -1 : 1); event.accepted = true; return
                                }
                                if (!session.canChange || session.selectedNotes.length === 0) return
                                const shift = (event.modifiers & Qt.ShiftModifier) !== 0
                                if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) session.deleteNotes(session.selectedNotes)
                                else if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
                                    const delta = event.key === Qt.Key_Left ? -pianoView.snap : pianoView.snap
                                    if (shift) session.resizeNotes(session.selectedNotes, delta)
                                    else session.moveNotes(session.selectedNotes, delta, 0)
                                } else if (event.key === Qt.Key_Up || event.key === Qt.Key_Down)
                                    session.moveNotes(session.selectedNotes, 0, (event.key === Qt.Key_Up ? 1 : -1) * (shift ? 12 : 1))
                                else return
                                event.accepted = true
                            }
                            PianoNotesView {
                                id: notesView
                                objectName: "pianoNotesView"
                                anchors.fill: parent
                                notes: session.pianoNotes; selected: session.selectedNotes
                                base: pianoView.base; rows: pianoView.rows; length: session.pianoLength
                                dragging: pianoInput.dragging; resizing: pianoInput.resizing
                                previewTicks: pianoInput.previewTicks; previewPitch: pianoInput.previewPitch
                            }
                            MouseArea {
                                id: pianoInput
                                objectName: "pianoRollInput"
                                anchors.fill: parent; enabled: session.canChange
                                property var hit: null
                                property bool resizing: false
                                property bool dragging: false
                                property real pressX: 0
                                property real pressY: 0
                                property int previewTicks: 0
                                property int previewPitch: 0
                                function noteAt(x, y) {
                                    const tick = Math.floor(x / pianoView.tickWidth)
                                    const pitch = pianoView.base + pianoView.rows - 1 - Math.floor(y / pianoView.rowHeight)
                                    const notes = session.pianoNotes
                                    for (let i = notes.length - 1; i >= 0; --i)
                                        if (notes[i].pitch === pitch && tick >= notes[i].start && tick < notes[i].start + notes[i].length) return notes[i]
                                    return null
                                }
                                function snapped(ticks) { return pianoView.snap > 1 ? Math.round(ticks / pianoView.snap) * pianoView.snap : Math.round(ticks) }
                                onPressed: function(mouse) {
                                    pianoView.forceActiveFocus()
                                    pressX = mouse.x; pressY = mouse.y; dragging = false; previewTicks = 0; previewPitch = 0
                                    hit = noteAt(mouse.x, mouse.y)
                                    // The last five pixels of a note resize it; the rest moves it.
                                    resizing = hit !== null && mouse.x >= (hit.start + hit.length) * pianoView.tickWidth - 5
                                }
                                onPositionChanged: function(mouse) {
                                    if (!hit) return
                                    if (!dragging && Math.abs(mouse.x - pressX) + Math.abs(mouse.y - pressY) < 5) return
                                    if (!dragging) { dragging = true; if (session.selectedNotes.indexOf(hit.id) < 0) session.selectNote(hit.id, false) }
                                    previewTicks = snapped((mouse.x - pressX) / pianoView.tickWidth)
                                    previewPitch = resizing ? 0 : -Math.round((mouse.y - pressY) / pianoView.rowHeight)
                                }
                                onReleased: function(mouse) {
                                    if (dragging) {
                                        if (resizing) { if (previewTicks !== 0) session.resizeNotes(session.selectedNotes, previewTicks) }
                                        else if (previewTicks !== 0 || previewPitch !== 0) session.moveNotes(session.selectedNotes, previewTicks, previewPitch)
                                    } else if (hit) session.selectNote(hit.id, (mouse.modifiers & Qt.ShiftModifier) !== 0)
                                    else {
                                        const tick = Math.floor(mouse.x / pianoView.tickWidth)
                                        const pitch = pianoView.base + pianoView.rows - 1 - Math.floor(mouse.y / pianoView.rowHeight)
                                        const grid = pianoView.snap > 1 ? pianoView.snap : 24
                                        session.addNote(pianoView.snap > 1 ? Math.floor(tick / grid) * grid : tick, grid, pitch, 80)
                                    }
                                    dragging = false; hit = null; previewTicks = 0; previewPitch = 0
                                }
                            }
                        }
                        RowLayout {
                            Label { text: "Start (16ths)" }
                            SpinBox { id: noteStart; objectName: "noteStart"; from: 0; to: Math.max(0, Math.ceil(session.pianoLength / 24) - 1); editable: true; Accessible.name: "New note start in sixteenth notes" }
                            Label { text: "Length (16ths)" }
                            SpinBox { id: noteLength; objectName: "noteLength"; from: 1; to: 256; value: 1; editable: true; Accessible.name: "New note length in sixteenth notes" }
                            Label { text: "Pitch (MIDI)" }
                            SpinBox { id: notePitch; objectName: "notePitch"; from: 0; to: 127; value: 60; editable: true; Accessible.name: "New note pitch" }
                            Label { text: "Velocity %" }
                            SpinBox { id: noteVelocity; objectName: "noteVelocity"; from: 1; to: 100; value: 80; editable: true; Accessible.name: "New note velocity percent" }
                            Button { objectName: "addNoteButton"; text: "Add note"; enabled: session.canChange; onClicked: session.addNote(noteStart.value * 24, noteLength.value * 24, notePitch.value, noteVelocity.value) }
                            Item { Layout.fillWidth: true }
                        }
                    }
                    GridLayout {
                        visible: !session.pianoMode
                        columns: 8
                        Layout.fillWidth: true
                        Repeater {
                            model: 16
                            Button {
                                required property int index
                                objectName: "step" + index
                                Layout.fillWidth: true; Layout.preferredHeight: 34
                                text: (index + 1) + " · " + (session.stepEnabled[index] ? session.pitches[index] : "Rest")
                                enabled: index < session.stepCount
                                checkable: true; checked: session.selectedStep === index
                                highlighted: session.selectedStep === index
                                palette.highlight: "#e4bc75"
                                onClicked: session.selectStep(index)
                                Accessible.name: "Select step " + (index + 1)
                            }
                        }
                    }
                    RowLayout {
                        visible: !session.pianoMode
                        Layout.fillWidth: true
                        Label { text: "Step " + (session.selectedStep + 1) }
                        CheckBox { objectName: "stepEnabled"; text: "Note enabled"; checked: session.stepEnabled[session.selectedStep]; enabled: session.canChange; onClicked: session.setStepEnabled(session.selectedStep, checked) }
                        Label { text: "Pitch (MIDI)" }
                        SpinBox { objectName: "selectedPitch"; from: 0; to: 127; value: session.pitches[session.selectedStep]; enabled: session.canChange; onValueModified: session.setPitch(session.selectedStep,value); Accessible.name: "Selected step MIDI pitch" }
                        Label { text: "Velocity %" }
                        SpinBox { objectName: "selectedVelocity"; from: 0; to: 100; value: session.velocities[session.selectedStep]; enabled: session.canChange; onValueModified: session.setVelocity(session.selectedStep,value); Accessible.name: "Selected step velocity percent" }
                        Label { text: "Gate %" }
                        SpinBox { objectName: "selectedGate"; from: 1; to: 100; value: session.gates[session.selectedStep]; enabled: session.canChange; onValueModified: session.setGate(session.selectedStep,value); Accessible.name: "Selected step gate percent" }
                        Item { Layout.fillWidth: true }
                    }
                }
                Item { Layout.fillHeight: true; visible: !session.pianoMode }
            }
            }
        }
        Label {
            Layout.fillWidth: true; wrapMode: Text.WordWrap
            text: "Edits apply while playing, except tempo, Open, New, import and export. Launch a scene, then press Play. Scene timing applies to launch and stop; clips use one bar. Immediate actions keep future queued actions. 4/4."
            color: "#9daabd"
        }
        // A fixed two-line status keeps the layout above it from jumping as messages change.
        Label { objectName: "statusMessage"; Layout.fillWidth: true; text: session.message; wrapMode: Text.WordWrap
            maximumLineCount: 2; elide: Text.ElideRight; Layout.preferredHeight: 2 * fontMetrics.lineSpacing; verticalAlignment: Text.AlignTop
            FontMetrics { id: fontMetrics } }
    }
}
