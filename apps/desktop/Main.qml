import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
    width: 1060; height: 1000
    minimumWidth: 900; minimumHeight: 620
    visible: true
    property bool discardOnClose: false
    property bool textOwnsUndo: (activeFocusItem instanceof TextInput || activeFocusItem instanceof TextEdit) && !activeFocusItem.readOnly
    Shortcut { sequences: [StandardKey.Undo]; enabled: session.canUndo && !window.textOwnsUndo && !closeDialog.visible && !renameDialog.visible; onActivated: session.undo() }
    Shortcut { sequences: [StandardKey.Redo]; enabled: session.canRedo && !window.textOwnsUndo && !closeDialog.visible && !renameDialog.visible; onActivated: session.redo() }
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
            Button { objectName: "applyRename"; text: "Apply"; enabled: session.canEdit; onClicked: renameDialog.commit() }
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
        RowLayout {
            Button { objectName: "playButton"; text: "Play"; enabled: session.audioAvailable; onClicked: session.play() }
            Button { objectName: "retryAudioButton"; text: "Retry audio"; visible: !session.audioAvailable; onClicked: session.reconnectAudio() }
            Button { objectName: "stopButton"; text: "Stop"; onClicked: session.stop() }
            Button { text: "Return to start"; enabled: !session.running && !session.pending; onClicked: session.rewind() }
            Label { objectName: "transportState"; text: session.running ? "Playing" : "Stopped"; Layout.preferredWidth: 80 }
            Label { text: session.position; font.pixelSize: 24; font.family: "monospace"; Layout.fillWidth: true }
            Label { text: "BPM"; color: "#9daabd" }
            SpinBox { objectName: "tempoSpin"; from: 20; to: 300; value: session.tempo; enabled: session.canEdit; editable: true; onValueModified: session.setTempo(value); Accessible.name: "Tempo in beats per minute" }
            Button { text: "Silence now"; onClicked: session.panic() }
        }
        RowLayout {
            Layout.fillWidth: true
            TextField { id: projectFile; objectName: "projectFile"; Layout.fillWidth: true; text: session.projectPath; placeholderText: "Project file path"; Accessible.name: "Project file path" }
            Button { objectName: "saveProjectButton"; text: "Save"; onClicked: session.saveProject(projectFile.text) }
            Button { objectName: "openProjectButton"; text: "Open"; enabled: session.canEdit; onClicked: session.loadProject(projectFile.text) }
            Label { text: session.dirty ? "Unsaved changes" : "No pending edits"; color: "#e4bc75" }
        }
        RowLayout {
            Layout.fillWidth: true
            TextField { id: exportFile; objectName: "exportFile"; Layout.fillWidth: true; placeholderText: "New WAV file path"; Accessible.name: "New WAV export path" }
            Label { text: "Bars" }
            SpinBox { id: exportBars; objectName: "exportBars"; from: 1; to: 16; value: 4; enabled: session.canEdit; Accessible.name: "Export length in bars" }
            Button { objectName: "exportWavButton"; text: "Export all tracks"; visible: !session.exporting; enabled: session.canEdit; onClicked: session.exportWav(exportFile.text, exportBars.value) }
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
            Button { objectName: "createScene"; text: "New scene"; enabled: session.canEdit && session.sceneIds.length < 256; onClicked: session.createScene() }
            Button { objectName: "copyScene"; text: "Copy arrangement"; enabled: session.canEdit && session.sceneIds.length < 256; onClicked: session.copyScene(session.sceneId); ToolTip.visible: hovered; ToolTip.text: "Copies slot choices into a new scene. Clip sources remain shared." }
            Button { objectName: "sceneUp"; text: "Move up"; enabled: session.canEdit && session.selectedSceneIndex > 0; onClicked: session.moveScene(session.sceneId, -1) }
            Button { objectName: "sceneDown"; text: "Move down"; enabled: session.canEdit && session.selectedSceneIndex+1 < session.sceneIds.length; onClicked: session.moveScene(session.sceneId, 1) }
            Button { objectName: "deleteScene"; text: "Delete scene"; enabled: session.canEdit && session.sceneIds.length > 1; onClicked: session.deleteScene(session.sceneId) }
        }
        RowLayout {
            Layout.fillWidth: true
            Label { text: "SCENE"; color: "#9daabd" }
            Label { objectName: "sceneName"; text: session.sceneName; textFormat: Text.PlainText; elide: Text.ElideRight; Layout.fillWidth: true; font.bold: true }
            ToolButton { id: sceneRename; objectName: "renameScene"; text: "Rename"; enabled: session.canEdit; Accessible.name: "Rename scene"; onClicked: renameDialog.edit(false, session.sceneId, session.sceneName, sceneRename, true) }
            Label { text: "Timing" }
            ComboBox { id: sceneTiming; objectName: "sceneTiming"; model: ["Immediate", "One bar"]; currentIndex: 1; Accessible.name: "Scene launch and stop timing" }
            Button { objectName: "launchScene"; text: "Launch scene"; enabled: session.audioAvailable; onClicked: session.launchScene(session.sceneId, sceneTiming.currentIndex === 0 ? "immediate" : "one-bar") }
            Button { objectName: "stopScene"; text: "Stop scene"; enabled: session.audioAvailable; onClicked: session.stopScene(session.sceneId, sceneTiming.currentIndex === 0 ? "immediate" : "one-bar") }
            Label { objectName: "sceneState"; text: session.sceneState; Layout.preferredWidth: 180; wrapMode: Text.WordWrap; color: "#68d9a4" }
        }
        RowLayout {
            Layout.fillWidth: true
            Label { text: "TRACKS"; color: "#9daabd" }
            Button { objectName: "addAudioTrack"; text: "Add audio track"; enabled: session.canEdit && session.trackCount < 64; onClicked: session.addTrack("audio") }
            Button { objectName: "addMidiTrack"; text: "Add MIDI track"; enabled: session.canEdit && session.trackCount < 64; onClicked: session.addTrack("midi") }
            Item { Layout.fillWidth: true }
            Label { text: session.trackCount + (session.trackCount === 1 ? " track" : " tracks") + " · one clip each"; color: "#9daabd" }
        }
        Rectangle {
            color: "#242c37"; radius: 8
            Layout.fillWidth: true; Layout.fillHeight: true
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 16; spacing: 12
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
                            // An integer model keeps delegates (and focus) stable across status updates.
                            model: session.trackCount
                            ColumnLayout {
                                id: trackColumn
                                required property int index
                                readonly property string trackId: session.trackIds[index] || ""
                                readonly property bool midi: session.trackKinds[index] === "midi"
                                Layout.preferredWidth: 300; Layout.alignment: Qt.AlignTop
                                spacing: 8
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label { objectName: "trackName" + index; text: session.trackNames[index]; textFormat: Text.PlainText; font.bold: true; font.pixelSize: 18; Layout.fillWidth: true; elide: Text.ElideRight }
                                    Label { objectName: "trackKind" + index; text: trackColumn.midi ? "MIDI" : "AUDIO"; color: trackColumn.midi ? "#e4bc75" : "#68d9a4"; font.pixelSize: 11 }
                                    ToolButton { id: trackRename; objectName: "renameTrack" + index; text: "Rename"; enabled: session.canEdit; Accessible.name: "Rename track " + (index + 1); onClicked: renameDialog.edit(true, trackColumn.trackId, session.trackNames[index], trackRename) }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Button {
                                        id: clipButton
                                        objectName: "clip" + index
                                        Layout.fillWidth: true; Layout.preferredHeight: 64
                                        highlighted: session.activeClips[index] === 0
                                        text: (session.activeClips[index] === 0 ? "●  " : "▶  ") + session.clipNames[index] + "\n" + session.trackSources[index]
                                        contentItem: Text { text: clipButton.text; textFormat: Text.PlainText; color: clipButton.palette.buttonText; font: clipButton.font; wrapMode: Text.Wrap; elide: Text.ElideRight; maximumLineCount: 2; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                                        onClicked: session.launchClip(session.clipIds[index])
                                        Accessible.name: "Launch " + session.clipNames[index]
                                        Accessible.description: session.clipStates[index]
                                    }
                                    ToolButton { id: clipRename; objectName: "renameClip" + index; text: "Rename"; enabled: session.canEdit; Accessible.name: "Rename clip " + (index + 1); onClicked: renameDialog.edit(false, session.clipIds[index], session.clipNames[index], clipRename) }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label { objectName: "clipState" + index; text: session.clipStates[index]; Layout.fillWidth: true; color: session.activeClips[index] >= 0 ? "#68d9a4" : "#b7c5d8" }
                                    CheckBox { objectName: "sceneIncluded" + index; text: "Include in scene"; checked: session.sceneIncluded[index]; enabled: session.canEdit; onClicked: session.setSceneIncluded(trackColumn.trackId, checked); Accessible.name: "Include track " + (index+1) + " in scene"; ToolTip.visible: hovered; ToolTip.text: "An excluded track stops when this scene launches." }
                                }
                                Button { text: "Stop track on next bar"; Layout.fillWidth: true; onClicked: session.stopTrackId(trackColumn.trackId) }
                                Label { objectName: "trackSource" + index; text: session.trackSources[index]; textFormat: Text.PlainText; elide: Text.ElideMiddle; Layout.fillWidth: true; color: text.startsWith("Missing") ? "#e4bc75" : "#b7c5d8" }
                                Button {
                                    objectName: "editPattern" + index; visible: trackColumn.midi; Layout.fillWidth: true
                                    text: session.patternTrackIndex === index ? "Editing pattern below" : "Edit pattern"
                                    highlighted: session.patternTrackIndex === index
                                    onClicked: session.editPattern(trackColumn.trackId)
                                }
                                RowLayout {
                                    visible: !trackColumn.midi; Layout.fillWidth: true
                                    TextField { id: audioPath; objectName: "audioPath" + index; Layout.fillWidth: true; placeholderText: "WAV file path"; Accessible.name: "WAV file to import into track " + (index + 1) }
                                    Button { objectName: "importAudio" + index; text: "Import"; enabled: !session.importing; onClicked: session.importAudio(trackColumn.trackId, audioPath.text); ToolTip.visible: hovered; ToolTip.text: "Mono or stereo WAV at 48 kHz. Decoding runs in the background; the clip changes once transport is stopped." }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label { text: "Level %"; color: "#9daabd" }
                                    SpinBox { objectName: "level" + index; from: 0; to: 100; value: session.levels[index]; enabled: session.canEdit; onValueModified: session.setLevel(index, value); Accessible.name: "Track " + (index + 1) + " level percent" }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label { text: "Pan"; color: "#9daabd" }
                                    SpinBox {
                                        objectName: "pan" + index; from: -100; to: 100; stepSize: 5; editable: true
                                        value: session.pans[index]; enabled: session.canEdit
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
                                    CheckBox { objectName: "mute" + index; text: "Mute"; checked: session.mutes[index]; enabled: session.canEdit; onClicked: session.setMute(trackColumn.trackId, checked); Accessible.name: "Mute track " + (index + 1) }
                                    CheckBox { objectName: "solo" + index; text: "Solo"; checked: session.solos[index]; enabled: session.canEdit; onClicked: session.setSolo(trackColumn.trackId, checked); Accessible.name: "Solo track " + (index + 1); ToolTip.visible: hovered; ToolTip.text: "While any track is soloed, only soloed tracks play. Mute wins over solo." }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Button { objectName: "duplicateTrack" + index; text: "Duplicate"; enabled: session.canEdit && session.trackCount < 64; onClicked: session.duplicateTrack(trackColumn.trackId); Accessible.name: "Duplicate track " + (index + 1) }
                                    Button { objectName: "deleteTrack" + index; text: "Delete"; enabled: session.canEdit; onClicked: session.deleteTrack(trackColumn.trackId); Accessible.name: "Delete track " + (index + 1); ToolTip.visible: hovered; ToolTip.text: "Removes the track, its clip and its scene slots. Undo restores them." }
                                }
                            }
                        }
                    }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: "#333e4d" }
                Label { visible: session.patternTrackIndex < 0; text: "Add a MIDI track to edit a step pattern."; color: "#b7c5d8" }
                ColumnLayout {
                    visible: session.patternTrackIndex >= 0
                    Layout.fillWidth: true; spacing: 8
                    RowLayout {
                        Layout.fillWidth: true
                        Label { objectName: "patternTitle"; text: "PATTERN · " + (session.trackNames[session.patternTrackIndex] || ""); textFormat: Text.PlainText; font.bold: true; Layout.fillWidth: true; elide: Text.ElideRight }
                        Label { text: "Quarter-note steps" }
                        SpinBox { objectName: "stepCount"; from: 1; to: 16; value: session.stepCount; enabled: session.canEdit; onValueModified: session.setStepCount(value); Accessible.name: "Pattern length in quarter notes" }
                    }
                    GridLayout {
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
                        Layout.fillWidth: true
                        Label { text: "Step " + (session.selectedStep + 1) }
                        CheckBox { objectName: "stepEnabled"; text: "Note enabled"; checked: session.stepEnabled[session.selectedStep]; enabled: session.canEdit; onClicked: session.setStepEnabled(session.selectedStep, checked) }
                        Label { text: "Pitch (MIDI)" }
                        SpinBox { objectName: "selectedPitch"; from: 0; to: 127; value: session.pitches[session.selectedStep]; enabled: session.canEdit; onValueModified: session.setPitch(session.selectedStep,value); Accessible.name: "Selected step MIDI pitch" }
                        Label { text: "Velocity %" }
                        SpinBox { objectName: "selectedVelocity"; from: 0; to: 100; value: session.velocities[session.selectedStep]; enabled: session.canEdit; onValueModified: session.setVelocity(session.selectedStep,value); Accessible.name: "Selected step velocity percent" }
                        Label { text: "Gate %" }
                        SpinBox { objectName: "selectedGate"; from: 1; to: 100; value: session.gates[session.selectedStep]; enabled: session.canEdit; onValueModified: session.setGate(session.selectedStep,value); Accessible.name: "Selected step gate percent" }
                        Item { Layout.fillWidth: true }
                    }
                }
                Item { Layout.fillHeight: true }
            }
        }
        Label {
            Layout.fillWidth: true; wrapMode: Text.WordWrap
            text: "Edit while stopped. Changes return to the start. Launch a scene, then press Play. Scene timing applies to launch and stop; clips use one bar. Immediate actions keep future queued actions. 4/4."
            color: "#9daabd"
        }
        Label { objectName: "statusMessage"; Layout.fillWidth: true; text: session.message; wrapMode: Text.WordWrap }
    }
}
