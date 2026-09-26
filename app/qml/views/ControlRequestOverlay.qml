import QtQuick
import MazeConnect.App

/**
 * A phone asks to use this computer's mouse and keyboard.
 *
 * Asked here, on the computer, because this is the machine being given
 * away: whoever is at the phone does not get to decide it. Deny is the
 * default reading of silence — the question closes itself after a minute.
 */
Item {
    id: root
    anchors.fill: parent
    visible: Backend.controlRequestDevice !== ""

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
        width: 500
        height: column.implicitHeight + 64
        radius: Theme.radiusPanel
        color: "#0B0B0E"
        border.width: 1
        border.color: Theme.warn

        Column {
            id: column
            anchors.centerIn: parent
            width: parent.width - 64
            spacing: 16

            Row {
                spacing: 10
                MazeIcon {
                    name: "phone"
                    color: Theme.warn
                    width: 20
                    height: 20
                    anchors.verticalCenter: parent.verticalCenter
                }
                SectionLabel {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Mouse and keyboard")
                }
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("%1 wants to use this computer's mouse and keyboard.").arg(Backend.controlRequestName)
                color: Theme.text
                font.family: Theme.fontSans
                font.pixelSize: 17
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("It could do anything you can do at this keyboard — including opening a terminal. Allow it only if you are the one holding that phone. The first time, the desktop will also ask once to confirm.")
                color: Theme.dim
                font.family: Theme.fontSans
                font.pixelSize: 12
                lineHeight: 1.45
            }

            Row {
                anchors.right: parent.right
                spacing: 10

                MazeButton {
                    text: qsTr("Deny")
                    destructive: true
                    onClicked: Backend.answerControlRequest(0)
                }
                MazeButton {
                    text: qsTr("Allow once")
                    onClicked: Backend.answerControlRequest(1)
                }
                MazeButton {
                    text: qsTr("Always allow")
                    primary: true
                    onClicked: Backend.answerControlRequest(2)
                }
            }
        }
    }
}
