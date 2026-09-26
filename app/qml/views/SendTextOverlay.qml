import QtQuick
import MazeConnect.App

/**
 * Type a note or a link and send it to one phone.
 *
 * It arrives there as a notification: a link opens on a tap, anything else is
 * copied to the phone's clipboard on a tap. Nothing opens by itself.
 */
Item {
    id: root
    anchors.fill: parent
    visible: false

    property string deviceId: ""
    property string deviceName: ""

    function show(id, name) {
        deviceId = id
        deviceName = name
        editor.text = ""
        visible = true
        editor.forceActiveFocus()
    }

    function send() {
        if (Backend.sendTextTo(root.deviceId, editor.text)) {
            root.visible = false
        }
    }

    // Swallow clicks behind the dialog.
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
        border.color: Theme.panelBorder

        Column {
            id: column
            anchors.centerIn: parent
            width: parent.width - 64
            spacing: 16

            SectionLabel { text: qsTr("Send to %1").arg(root.deviceName) }

            Rectangle {
                width: parent.width
                height: 150
                radius: Theme.radiusPill
                color: Theme.pill
                border.width: 1
                border.color: editor.activeFocus ? Theme.panelBorder : Theme.hairline

                Flickable {
                    id: flick
                    anchors.fill: parent
                    anchors.margins: 12
                    clip: true
                    contentHeight: editor.implicitHeight
                    boundsBehavior: Flickable.StopAtBounds

                    TextEdit {
                        id: editor
                        width: flick.width
                        wrapMode: TextEdit.Wrap
                        color: Theme.text
                        selectionColor: Theme.pillHover
                        font.family: Theme.fontSans
                        font.pixelSize: 13
                        // Ctrl+Enter sends; plain Enter is a new line.
                        Keys.onPressed: (event) => {
                            if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                    && (event.modifiers & Qt.ControlModifier)) {
                                root.send()
                                event.accepted = true
                            } else if (event.key === Qt.Key_Escape) {
                                root.visible = false
                                event.accepted = true
                            }
                        }
                    }
                }

                Text {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 12
                    visible: editor.text.length === 0
                    text: qsTr("A note, a link, an address…")
                    color: Theme.faint
                    font.family: Theme.fontSans
                    font.pixelSize: 13
                }
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("Arrives as a notification: a link opens when tapped, anything else is copied to the phone's clipboard. %1 / 4096").arg(editor.length)
                color: editor.length > 4096 ? Theme.bad : Theme.faint
                font.family: Theme.fontSans
                font.pixelSize: 11
                lineHeight: 1.35
            }

            Row {
                anchors.right: parent.right
                spacing: 10

                MazeButton {
                    text: qsTr("Cancel")
                    onClicked: root.visible = false
                }
                MazeButton {
                    text: qsTr("Send")
                    primary: true
                    enabled: editor.text.trim().length > 0 && editor.length <= 4096
                    onClicked: root.send()
                }
            }
        }
    }
}
