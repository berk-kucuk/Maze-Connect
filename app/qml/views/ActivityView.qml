import QtQuick
import MazeConnect.App

/**
 * Session activity, including refused connections.
 *
 * The link layer already turns away unpaired peers, replayed counters and
 * malformed frames. Without somewhere to surface that, a user on a hostile
 * network would have no way to notice anything was attempted.
 */
Item {
    id: root

    Row {
        id: header
        width: parent.width
        spacing: 12

        SectionLabel {
            text: qsTr("This session")
            anchors.verticalCenter: parent.verticalCenter
        }

        Item { width: parent.width - 230; height: 1 }

        MazeButton {
            text: qsTr("Clear")
            enabled: Backend.activity.count > 0
            onClicked: Backend.activity.clear()
        }
    }

    ListView {
        anchors.top: header.bottom
        anchors.topMargin: 14
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        spacing: 2
        model: Backend.activity
        boundsBehavior: Flickable.StopAtBounds

        Column {
            anchors.centerIn: parent
            spacing: 10
            width: 400
            visible: Backend.activity.count === 0

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Nothing yet")
                color: Theme.text
                font.family: Theme.fontSans
                font.pixelSize: 18
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: qsTr("Pairings, transfers and any connection this machine refused will be listed here. Kept in memory only — nothing is written to disk.")
                color: Theme.dim
                font.family: Theme.fontSans
                font.pixelSize: 13
                lineHeight: 1.45
            }
        }

        delegate: Item {
            id: entry

            required property int kind
            required property string title
            required property string detail
            required property string time

            width: ListView.view.width
            height: 52

            readonly property color kindColor:
                entry.kind === 1 ? Theme.warn      // Security
              : entry.kind === 2 ? Theme.ok        // Transfer
              : entry.kind === 3 ? Theme.text      // Pairing
              : Theme.dim                          // Info

            Rectangle {
                anchors.fill: parent
                radius: Theme.radiusPill
                color: hover.hovered ? Theme.pill : "transparent"
                Behavior on color { ColorAnimation { duration: 120 } }
            }
            HoverHandler { id: hover }

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                text: entry.time
                color: Theme.faint
                font.family: Theme.fontMono
                font.pixelSize: 10
            }

            Rectangle {
                id: dot
                anchors.left: parent.left
                anchors.leftMargin: 84
                anchors.verticalCenter: parent.verticalCenter
                width: 6
                height: 6
                radius: 3
                color: entry.kindColor
            }

            Column {
                anchors.left: dot.right
                anchors.leftMargin: 12
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2

                Text {
                    text: entry.title
                    color: Theme.text
                    font.family: Theme.fontSans
                    font.pixelSize: 12
                    width: parent.width
                    elide: Text.ElideRight
                }
                Text {
                    visible: entry.detail !== ""
                    text: entry.detail
                    color: Theme.faint
                    font.family: Theme.fontMono
                    font.pixelSize: 10
                    width: parent.width
                    elide: Text.ElideMiddle
                }
            }
        }
    }
}
