import QtQuick
import MazeConnect.App

/**
 * Incoming files always wait for an answer. Nothing is written to disk
 * before the user accepts, so a paired device cannot quietly drop files
 * onto this machine.
 */
Item {
    id: root
    anchors.fill: parent
    visible: false

    property string deviceId: ""
    property int transferId: 0
    property string filename: ""
    property real sizeBytes: 0

    function show(device, transfer, name, size) {
        deviceId = device
        transferId = transfer
        filename = name
        sizeBytes = size
        visible = true
    }

    function humanSize(bytes) {
        if (bytes < 1024)
            return qsTr("%1 bytes").arg(bytes)
        if (bytes < 1024 * 1024)
            return qsTr("%1 KB").arg((bytes / 1024).toFixed(1))
        if (bytes < 1024 * 1024 * 1024)
            return qsTr("%1 MB").arg((bytes / (1024 * 1024)).toFixed(1))
        return qsTr("%1 GB").arg((bytes / (1024 * 1024 * 1024)).toFixed(2))
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onClicked: {}
    }

    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: 0.86
    }

    Rectangle {
        anchors.centerIn: parent
        width: 460
        height: column.implicitHeight + 64
        radius: Theme.radiusPanel
        color: "#0B0B0E"
        border.width: 1
        border.color: Theme.panelBorder

        Column {
            id: column
            anchors.centerIn: parent
            width: parent.width - 64
            spacing: 18

            SectionLabel { text: qsTr("Incoming file") }

            Text {
                width: parent.width
                wrapMode: Text.WrapAnywhere
                text: root.filename
                color: Theme.text
                font.family: Theme.fontMono
                font.pixelSize: 15
            }

            Text {
                text: root.humanSize(root.sizeBytes)
                color: Theme.dim
                font.family: Theme.fontSans
                font.pixelSize: 13
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("Saves to %1").arg(Backend.inboxPath)
                color: Theme.dim
                font.family: Theme.fontMono
                font.pixelSize: 11
            }

            Row {
                anchors.right: parent.right
                spacing: 10

                MazeButton {
                    text: qsTr("Decline")
                    glyph: "✕"
                    destructive: true
                    onClicked: {
                        Backend.respondToFileOffer(root.deviceId, root.transferId, false)
                        root.visible = false
                    }
                }

                MazeButton {
                    text: qsTr("Accept")
                    glyph: "✓"
                    primary: true
                    onClicked: {
                        Backend.respondToFileOffer(root.deviceId, root.transferId, true)
                        root.visible = false
                    }
                }
            }
        }
    }
}
