import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
    width: 1040; height: 660
    minimumWidth: 900; minimumHeight: 620
    visible: true
    property bool discardOnClose: false
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
    palette.windowText: "#e8edf5"
    palette.buttonText: "#e8edf5"
    palette.button: "#333e4d"
    palette.base: "#242c37"
    palette.text: "#e8edf5"
    palette.highlight: "#68d9a4"
    ColumnLayout {
        anchors.fill: parent; anchors.margins: 24; spacing: 18
        RowLayout {
            Layout.fillWidth: true
            Label { text: "fl-live"; font.pixelSize: 26; font.bold: true }
            Label { text: "SESSION / 001"; color: "#9daabd"; Layout.fillWidth: true }
            Label { text: session.mode; color: "#e4bc75" }
        }
        RowLayout {
            Button { objectName: "playButton"; text: "Play"; onClicked: session.play() }
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
        Rectangle {
            color: "#242c37"; radius: 8
            Layout.fillWidth: true; Layout.fillHeight: true
            RowLayout {
                anchors.fill: parent; anchors.margins: 20; spacing: 20
                Repeater {
                    model: ["Pulse / Audio", "Steps / MIDI"]
                    ColumnLayout {
                        required property int index
                        required property string modelData
                        Layout.fillWidth: true; Layout.fillHeight: true; spacing: 14
                        Label { text: modelData; font.bold: true; font.pixelSize: 18 }
                        Button {
                            objectName: "clip" + index
                            Layout.fillWidth: true; Layout.preferredHeight: 112
                            highlighted: session.activeClips[index] === 0
                            text: (session.activeClips[index] === 0 ? "●  " : "▶  ") + (index === 0 ? "Low pulse\nAudio loop · 1 beat" : "Rising steps\nMIDI loop · 1 bar")
                            onClicked: session.launch(index, 0)
                            Accessible.name: index === 0 ? "Launch low pulse audio clip" : "Launch rising steps MIDI clip"
                            Accessible.description: session.clipStates[index]
                        }
                        Label { objectName: "clipState" + index; text: session.clipStates[index]; color: session.activeClips[index] >= 0 ? "#68d9a4" : "#b7c5d8" }
                        Button { text: "Stop track on next bar"; Layout.fillWidth: true; onClicked: session.stopTrack(index) }
                        Label { visible: index === 1; text: "MIDI pitches · quarter-note steps"; color: "#9daabd" }
                        RowLayout {
                            visible: index === 1
                            Layout.fillWidth: true
                            Repeater {
                                model: 4
                                SpinBox {
                                    required property int index
                                    objectName: "pitch" + index
                                    Layout.fillWidth: true; Layout.preferredWidth: 66
                                    from: 0; to: 127; value: session.pitches[index]; enabled: session.canEdit
                                    onValueModified: session.setPitch(index, value)
                                    Accessible.name: "MIDI pitch " + (index + 1)
                                }
                            }
                        }
                        Item { Layout.fillHeight: true }
                        RowLayout {
                            Label { text: "Level %"; color: "#9daabd" }
                            SpinBox { objectName: "level" + index; from: 0; to: 100; value: session.levels[index]; enabled: session.canEdit; onValueModified: session.setLevel(index, value); Accessible.name: "Track " + (index + 1) + " level percent" }
                        }
                    }
                }
            }
        }
        Label {
            Layout.fillWidth: true; wrapMode: Text.WordWrap
            text: "Pulse + Steps · Edit while stopped. Changes return to the start and clear queued launches. Launch clips, then press Play. 4/4 · 1-bar launch grid."
            color: "#9daabd"
        }
        Label { objectName: "statusMessage"; Layout.fillWidth: true; text: session.message; wrapMode: Text.WordWrap }
    }
}
