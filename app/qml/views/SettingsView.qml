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

        // ---- Control from the phone -------------------------------------
        SectionLabel { text: qsTr("Mouse and keyboard from your phone") }

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("A phone allowed here can move the pointer and type — anything a keyboard can do, including opening a terminal. So it is off for every phone until you turn it on, and the first time the desktop itself asks you to confirm on this screen. A banner stays up while a phone is in control, with a Stop button. Presenting (slide keys only) needs no permission here.")
            color: Theme.dim
            font.family: Theme.fontSans
            font.pixelSize: 12
            lineHeight: 1.45
        }

        Column {
            width: parent.width
            spacing: 6

            Repeater {
                model: Backend.phones

                Pill {
                    id: controlPill
                    required property var modelData
                    label: modelData.name
                    value: modelData.controlAllowed ? qsTr("Allowed") : qsTr("Not allowed")
                    showDot: true
                    state_: modelData.controlAllowed ? 2 : -1

                    MazeButton {
                        anchors.right: parent.right
                        anchors.rightMargin: 110
                        anchors.verticalCenter: parent.verticalCenter
                        text: controlPill.modelData.controlAllowed ? qsTr("Revoke") : qsTr("Allow")
                        destructive: controlPill.modelData.controlAllowed
                        onClicked: Backend.setRemoteControlAllowed(controlPill.modelData.deviceId,
                                                                   !controlPill.modelData.controlAllowed)
                    }
                }
            }
        }

        // ---- Clipboard sync ------------------------------------------------
        SectionLabel { text: qsTr("Clipboard sync") }

        Row {
            width: parent.width
            spacing: 12

            Text {
                width: parent.width - clipToggle.width - 12
                wrapMode: Text.WordWrap
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("What you copy here goes to linked phones that also have sync on, and what they copy lands here. Passwords copied from a password manager are never sent. A phone can only send its clipboard while Maze Connect is open on it — Android allows nothing else.")
                color: Theme.dim
                font.family: Theme.fontSans
                font.pixelSize: 12
                lineHeight: 1.45
            }

            MazeButton {
                id: clipToggle
                anchors.verticalCenter: parent.verticalCenter
                text: Backend.clipboardSync ? qsTr("On") : qsTr("Off")
                glyph: Backend.clipboardSync ? "\u25cf" : "\u25cb"
                primary: Backend.clipboardSync
                onClicked: Backend.clipboardSync = !Backend.clipboardSync
            }
        }

        // ---- Shared folder ---------------------------------------------------
        SectionLabel { text: qsTr("Shared folder") }

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("Linked phones can browse and download from this one folder — nothing outside it, no hidden files, no links out of it. Put in it what you want on your phone.")
            color: Theme.dim
            font.family: Theme.fontSans
            font.pixelSize: 12
            lineHeight: 1.45
        }

        Pill {
            label: qsTr("Folder")
            value: Backend.sharedFolderPath
            interactive: true
            onClicked: Backend.openSharedFolder()
        }

        // ---- Lock when the phone leaves ------------------------------------
        SectionLabel { text: qsTr("Lock when your phone leaves") }

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("Locks this computer's screen when the chosen phone drops off the network and does not come back within the delay — walk away with your phone and the desk locks behind you. It can only lock: a phone can never unlock anything. If this computer's own network drops, it locks too.")
            color: Theme.dim
            font.family: Theme.fontSans
            font.pixelSize: 12
            lineHeight: 1.45
        }

        Flow {
            width: parent.width
            spacing: 8

            MazeButton {
                text: qsTr("Off")
                primary: Backend.proximityLockDevice === ""
                onClicked: Backend.proximityLockDevice = ""
            }
            Repeater {
                model: Backend.phones
                MazeButton {
                    required property var modelData
                    text: modelData.name
                    primary: Backend.proximityLockDevice === modelData.deviceId
                    onClicked: Backend.proximityLockDevice = modelData.deviceId
                }
            }
        }

        Row {
            spacing: 8
            visible: Backend.proximityLockDevice !== ""

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("After")
                color: Theme.dim
                font.family: Theme.fontSans
                font.pixelSize: 12
            }
            Repeater {
                model: [15, 30, 60, 300]
                MazeButton {
                    required property int modelData
                    text: modelData < 60 ? qsTr("%1 s").arg(modelData) : qsTr("%1 min").arg(modelData / 60)
                    primary: Backend.proximityLockDelay === modelData
                    onClicked: Backend.proximityLockDelay = modelData
                }
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
