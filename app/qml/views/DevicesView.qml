// Delegates reach ids from this file (the selected device, the file
// dialog). Binding component behaviour makes those resolve at compile time
// instead of being looked up by name at runtime.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Dialogs
import MazeConnect.App

Item {
    id: root

    property string selectedDeviceId: ""

    FileDialog {
        id: fileDialog
        title: qsTr("Choose a file to send")
        onAccepted: Backend.sendFile(root.selectedDeviceId, selectedFile)
    }

    // A header rather than a floating button: a list showing a stale device
    // needs a rescan exactly as much as an empty one does, and this view has
    // nothing else to press.
    Item {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: scanButton.height

        SectionLabel {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: list.count === 1 ? qsTr("1 device") : qsTr("%1 devices").arg(list.count)
        }

        MazeButton {
            id: scanButton
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Scan")
            onClicked: Backend.rescanDevices()
        }
    }

    ListView {
        id: list
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.topMargin: 12
        clip: true
        spacing: 10
        model: Backend.devices
        boundsBehavior: Flickable.StopAtBounds

        // Tells you what to do next rather than only that there is nothing.
        Column {
            anchors.centerIn: parent
            spacing: 14
            visible: list.count === 0
            width: 420

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("No devices yet")
                color: Theme.text
                font.family: Theme.fontSans
                font.pixelSize: 19
            }

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: qsTr("Open Maze Connect on your phone with both devices on the same network. Devices announce themselves and appear here.")
                color: Theme.dim
                font.family: Theme.fontSans
                font.pixelSize: 13
                lineHeight: 1.45
            }

            Pill {
                width: parent.width
                label: qsTr("Or pair by address")
                value: Backend.listenAddress
            }

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: qsTr("If your network blocks discovery, type that address into the phone.")
                color: Theme.faint
                font.family: Theme.fontSans
                font.pixelSize: 11
            }

            MazeButton {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Scan again")
                primary: true
                onClicked: Backend.rescanDevices()
            }
        }

        delegate: Rectangle {
            id: card

            required property string deviceId
            required property string name
            required property string deviceType
            required property bool paired
            required property bool connected
            required property string fingerprint
            required property string address
            required property var capabilities

            width: list.width
            height: card.paired ? 104 : 86
            radius: Theme.radiusPill + 2
            color: hover.hovered ? Theme.pillHover : Theme.pill
            Behavior on color { ColorAnimation { duration: 130 } }

            HoverHandler { id: hover }

            // Actions are anchored first and the identity block is sized
            // against them: previously both were free-floating and the
            // fingerprint line ran underneath the buttons on narrow windows.
            Row {
                id: actions
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.top: parent.top
                anchors.topMargin: card.paired ? 18 : 24
                spacing: 7

                MazeButton {
                    text: qsTr("Pair")
                    glyph: "⇄"
                    primary: true
                    visible: !card.paired
                    onClicked: Backend.requestPairing(card.deviceId)
                }

                MazeButton {
                    text: qsTr("Send file")
                    glyph: "↥"
                    visible: card.paired
                    enabled: card.connected
                    onClicked: {
                        root.selectedDeviceId = card.deviceId
                        fileDialog.open()
                    }
                }

                MazeButton {
                    text: qsTr("Forget")
                    glyph: "✕"
                    destructive: true
                    visible: card.paired
                    onClicked: Backend.unpair(card.deviceId)
                }
            }

            // ---- Identity -----------------------------------------------
            // Name sits beside the buttons; the status and fingerprint get
            // their own full-width line below. Sharing one row with the
            // action buttons left no room for the fingerprint, which was
            // elided down to nonsense.
            Row {
                id: nameRow
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.right: actions.left
                anchors.rightMargin: 16
                anchors.top: parent.top
                anchors.topMargin: card.paired ? 22 : 26
                spacing: 11

                Rectangle {
                    width: 8
                    height: 8
                    radius: 4
                    anchors.verticalCenter: parent.verticalCenter
                    color: card.paired
                           ? (card.connected ? Theme.ok : Theme.unknown)
                           : Theme.warn
                }

                Text {
                    text: card.name
                    color: Theme.text
                    font.family: Theme.fontSans
                    font.pixelSize: 14
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(0, parent.width - 19)
                    elide: Text.ElideRight
                }
            }

            Text {
                // The fingerprint is the device's real identity, so it is
                // shown rather than hidden behind a details panel — it is
                // what a user compares if they ever need to check who they
                // are actually talking to.
                anchors.left: parent.left
                anchors.leftMargin: 35
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.top: nameRow.bottom
                anchors.topMargin: 5
                visible: card.paired
                text: (card.connected ? qsTr("Connected") : qsTr("Paired · not reachable"))
                      + "   ·   " + Backend.shortFingerprint(card.fingerprint)
                color: card.connected ? Theme.ok : Theme.dim
                font.family: Theme.fontMono
                font.pixelSize: 10
                elide: Text.ElideRight
            }

            Text {
                anchors.left: nameRow.left
                anchors.leftMargin: 19
                anchors.right: actions.left
                anchors.rightMargin: 16
                anchors.top: nameRow.bottom
                anchors.topMargin: 5
                visible: !card.paired
                text: qsTr("Not paired") + "   ·   " + card.address
                color: Theme.dim
                font.family: Theme.fontMono
                font.pixelSize: 10
                elide: Text.ElideRight
            }

            /*
             * The per-capability switches that used to sit here are gone.
             *
             * Pairing is the decision: a person compared six digits on two
             * screens and agreed on both. A second row of switches after that
             * added no choice — it only meant a freshly paired phone sat on
             * "asking the computer…" until somebody found them.
             *
             * The mechanism is still there (DeviceStore keeps the set, and
             * DeviceManager re-checks it when an answer is delivered), so a
             * way to revoke can be reintroduced without protocol changes. It
             * simply has no surface at the moment.
             */
        }
    }
}
