import QtQuick
import MazeConnect.App

/**
 * The pairing confirmation. Modal on purpose: this is the only decision in
 * the application that grants lasting trust, and it should not be something
 * the user clicks past while doing something else.
 */
Item {
    id: root
    anchors.fill: parent
    visible: Backend.pairingActive

    // Swallow clicks on the page behind.
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
        width: 520
        height: content.implicitHeight + 72
        radius: Theme.radiusPanel
        color: "#0B0B0E"
        border.width: 1
        border.color: Theme.panelBorder

        Column {
            id: content
            anchors.centerIn: parent
            width: parent.width - 72
            spacing: 20

            SectionLabel {
                text: Backend.pairingWeInitiated
                      ? qsTr("Pairing with %1").arg(Backend.pairingDeviceName)
                      : qsTr("%1 wants to pair").arg(Backend.pairingDeviceName)
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                // State the check as an instruction, and say what it is for.
                text: qsTr("Check that this code matches the one on the other device.")
                color: Theme.text
                font.family: Theme.fontSans
                font.pixelSize: 17
                lineHeight: 1.35
            }

            VerificationCode {
                anchors.horizontalCenter: parent.horizontalCenter
                code: Backend.pairingCode
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("If the codes are different, someone else is on the connection. Cancel and try again on a network you trust.")
                color: Theme.dim
                font.family: Theme.fontSans
                font.pixelSize: 13
                lineHeight: 1.45
            }

            // Once answered, the wait belongs to the other device. Say so —
            // leaving the buttons live and unchanged made a press that had
            // already been accepted look like nothing had happened.
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                visible: Backend.pairingAnswered
                text: qsTr("Waiting for the other device to confirm…")
                color: Theme.ok
                font.family: Theme.fontSans
                font.pixelSize: 14
            }

            Row {
                anchors.right: parent.right
                spacing: 10
                visible: !Backend.pairingAnswered

                MazeButton {
                    text: qsTr("Codes differ")
                    glyph: "✕"
                    destructive: true
                    onClicked: Backend.rejectPairing()
                }

                MazeButton {
                    text: qsTr("Codes match")
                    glyph: "✓"
                    primary: true
                    onClicked: Backend.acceptPairing()
                }
            }
        }
    }
}
