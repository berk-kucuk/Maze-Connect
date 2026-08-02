import QtQuick
import MazeConnect.App

Flickable {
    id: root

    contentHeight: column.implicitHeight
    clip: true
    boundsBehavior: Flickable.StopAtBounds

    Column {
        id: column
        width: root.width
        spacing: 22

        // ---- Identity ----------------------------------------------------
        SectionLabel { text: qsTr("This device") }

        Column {
            width: parent.width
            spacing: 6

            Pill {
                label: qsTr("Name")
                value: Backend.deviceName
            }

            Pill {
                // Full grouped fingerprint: this is what a user reads out
                // when someone asks them to confirm which machine they are.
                label: qsTr("Fingerprint")
                value: Backend.shortFingerprint(Backend.fingerprint)
            }

            Pill {
                label: qsTr("Address")
                value: Backend.listenAddress
                state_: Backend.listenAddress === "" ? 0 : 1
                showDot: true
            }

            Pill {
                label: qsTr("Received files")
                value: Backend.inboxPath
            }
        }

        // ---- Startup -----------------------------------------------------
        SectionLabel { text: qsTr("Startup") }

        Row {
            width: parent.width
            spacing: 12

            Text {
                width: parent.width - toggle.width - 12
                wrapMode: Text.WordWrap
                anchors.verticalCenter: parent.verticalCenter
                text: Backend.autostartEnabled
                      ? qsTr("Maze Connect starts with your session and waits in the tray. Closing the window leaves your phone connected — only Quit, from the tray menu, ends the link.")
                      : qsTr("Maze Connect only runs when you open it. Your phone cannot reach this computer while it is closed.")
                color: Theme.dim
                font.family: Theme.fontSans
                font.pixelSize: 12
                lineHeight: 1.45
            }

            MazeButton {
                id: toggle
                anchors.verticalCenter: parent.verticalCenter
                text: Backend.autostartEnabled ? qsTr("On") : qsTr("Off")
                glyph: Backend.autostartEnabled ? "\u25cf" : "\u25cb"
                primary: Backend.autostartEnabled
                onClicked: Backend.autostartEnabled = !Backend.autostartEnabled
            }
        }

        // ---- Privacy -----------------------------------------------------
        SectionLabel { text: qsTr("Privacy") }

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            // The per-capability switches are gone: this application exists so
            // the phone can drive this computer, and a link that arrives with
            // everything switched off is a link that appears broken. Say where
            // the real boundary is instead — pairing, and the allow-list.
            text: qsTr("A paired phone can use every capability. The boundary is pairing itself: nothing reaches this computer until you have confirmed the six digits on both screens. Commands are the exception in the other direction — only the entries in your allow-list can ever run, and nothing the phone sends is passed to a shell.")
            color: Theme.dim
            font.family: Theme.fontSans
            font.pixelSize: 12
            lineHeight: 1.45
        }

        // ---- Security ----------------------------------------------------
        SectionLabel { text: qsTr("Security") }

        Column {
            width: parent.width
            spacing: 6

            Pill {
                label: qsTr("Transport")
                value: qsTr("TLS 1.3, mutual")
                state_: 1
                showDot: true
            }
            Pill {
                label: qsTr("Peer authentication")
                value: qsTr("Pinned public key")
                state_: 1
                showDot: true
            }
            Pill {
                label: qsTr("Paired devices")
                value: Backend.pairedCount
            }
        }

        Item { width: 1; height: 10 }
    }
}
