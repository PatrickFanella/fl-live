import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
    width: 1040; height: 660
    minimumWidth: 760; minimumHeight: 520
    visible: true
    title: "fl-live · Session prototype"
    color: "#191d24"
    palette.windowText: "#e8edf5"
    palette.buttonText: "#e8edf5"
    palette.button: "#333e4d"
    palette.base: "#242c37"
    palette.text: "#e8edf5"
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
            Label { text: "120 BPM · 4/4 · 1 bar"; color: "#9daabd" }
            Button { text: "Silence now"; onClicked: session.panic() }
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
                            text: index === 0 ? "▶  Low pulse\nAudio loop · 1 beat" : "▶  Rising steps\nMIDI loop · 1 bar"
                            onClicked: session.launch(index, 0)
                            Accessible.name: index === 0 ? "Launch low pulse audio clip" : "Launch rising steps MIDI clip"
                        }
                        Button { text: "Stop track on next bar"; Layout.fillWidth: true; onClicked: session.stopTrack(index) }
                        Item { Layout.fillHeight: true }
                        Label { text: index === 0 ? "Fixed gain 70%" : "Fixed gain 25%"; color: "#9daabd" }
                    }
                }
            }
        }
        Label {
            Layout.fillWidth: true; wrapMode: Text.WordWrap
            text: "Authored demo · Launch clips, then press Play. Transport reflects the engine; clip requests are acknowledged as scheduled, with no active-clip indicator yet."
            color: "#9daabd"
        }
        Label { objectName: "statusMessage"; Layout.fillWidth: true; text: session.message; wrapMode: Text.WordWrap }
    }
}
