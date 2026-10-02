// Delegates call helpers defined on this file's root.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Dialogs
import MazeConnect.App

Item {
    id: root

    // Where "Send" goes: the first linked phone that takes files. The
    // dashboard's phone cards still pick one explicitly when several are
    // linked.
    readonly property var target: {
        for (const p of Backend.phones) {
            if (p.connected && p.canFiles === true)
                return p
        }
        return null
    }

    FileDialog {
        id: fileDialog
        title: qsTr("Choose files to send")
        fileMode: FileDialog.OpenFiles
        onAccepted: {
            if (!root.target)
                return
            for (const url of selectedFiles)
                Backend.sendFile(root.target.deviceId, url)
        }
    }

    // Files dropped anywhere on the page go the same way, each through the
    // usual offer — the phone still asks before anything is written there.
    DropArea {
        anchors.fill: parent
        enabled: root.target !== null
        keys: ["text/uri-list"]
        onDropped: (event) => {
            for (const url of event.urls)
                Backend.sendFile(root.target.deviceId, url)
            event.acceptProposedAction()
        }
    }

    function humanSize(bytes) {
        if (bytes < 1024)
            return qsTr("%1 B").arg(bytes)
        if (bytes < 1024 * 1024)
            return qsTr("%1 KB").arg((bytes / 1024).toFixed(1))
        if (bytes < 1024 * 1024 * 1024)
            return qsTr("%1 MB").arg((bytes / (1024 * 1024)).toFixed(1))
        return qsTr("%1 GB").arg((bytes / (1024 * 1024 * 1024)).toFixed(2))
    }

    Item {
        id: header
        width: parent.width
        height: clearButton.height

        SectionLabel {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Transfers")
        }

        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 12

            MazeButton {
                text: root.target ? qsTr("Send to %1").arg(root.target.name) : qsTr("Send")
                enabled: root.target !== null
                onClicked: fileDialog.open()
            }

            MazeButton {
                id: clearButton
                text: qsTr("Clear finished")
                enabled: Backend.transfers.count > Backend.transfers.activeCount
                onClicked: Backend.transfers.clearFinished()
            }
        }
    }

    ListView {
        anchors.top: header.bottom
        anchors.topMargin: 14
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        spacing: 8
        model: Backend.transfers
        boundsBehavior: Flickable.StopAtBounds

        Column {
            anchors.centerIn: parent
            spacing: 10
            width: 400
            visible: Backend.transfers.count === 0

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("No transfers")
                color: Theme.text
                font.family: Theme.fontSans
                font.pixelSize: 18
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: root.target
                      ? qsTr("Send a file, or drop one here. Files you send or accept appear here while they move, and stay until you clear them.")
                      : qsTr("Link a phone to send it files. Files you send or accept appear here while they move, and stay until you clear them.")
                color: Theme.dim
                font.family: Theme.fontSans
                font.pixelSize: 13
                lineHeight: 1.45
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Received files go to %1").arg(Backend.inboxPath)
                color: Theme.faint
                font.family: Theme.fontMono
                font.pixelSize: 10
                elide: Text.ElideMiddle
            }
        }

        delegate: Rectangle {
            id: item

            required property int transferId
            required property string deviceName
            required property string filename
            required property real received
            required property real total
            required property real progress
            required property int transferState
            required property string detail
            required property bool incoming

            width: ListView.view.width
            height: 74
            radius: Theme.radiusPill
            color: Theme.pill

            readonly property color stateColor:
                item.transferState === 1 ? Theme.ok
              : item.transferState === 2 ? Theme.bad
              : Theme.text

            Row {
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.top: parent.top
                anchors.topMargin: 14
                spacing: 10

                Text {
                    text: item.incoming ? "↧" : "↥"
                    color: item.stateColor
                    font.family: Theme.fontMono
                    font.pixelSize: 13
                }

                Text {
                    text: item.filename
                    color: Theme.text
                    font.family: Theme.fontSans
                    font.pixelSize: 13
                    elide: Text.ElideMiddle
                    width: Math.min(implicitWidth, item.width - 300)
                }
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.top: parent.top
                anchors.topMargin: 15
                text: item.transferState === 1 ? qsTr("Done")
                    : item.transferState === 2 ? qsTr("Failed")
                    : root.humanSize(item.received) + " / " + root.humanSize(item.total)
                color: item.stateColor
                font.family: Theme.fontMono
                font.pixelSize: 11
            }

            // Progress track. Stays visible when finished so a completed row
            // still reads as a full bar rather than an empty one.
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 26
                height: 3
                radius: 1.5
                color: Qt.rgba(1, 1, 1, 0.08)

                Rectangle {
                    width: parent.width * Math.max(0, Math.min(1, item.progress))
                    height: parent.height
                    radius: parent.radius
                    color: item.stateColor
                    Behavior on width { NumberAnimation { duration: 180 } }
                }
            }

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 9
                width: parent.width - 32
                elide: Text.ElideMiddle
                text: item.detail !== ""
                      ? item.detail
                      : (item.incoming ? qsTr("From %1") : qsTr("To %1")).arg(item.deviceName)
                color: Theme.faint
                font.family: Theme.fontMono
                font.pixelSize: 10
            }
        }
    }
}
