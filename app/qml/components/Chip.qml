import QtQuick
import MazeConnect.App

/**
 * A small status chip: a line icon and a word or two.
 *
 * Used for the facts about a phone that are states rather than quantities —
 * on Wi-Fi, silenced, in power saver. `tone` tints the icon (and only the
 * icon) the same way the rest of the suite uses colour: as a second signal
 * beside words that already say it.
 */
Rectangle {
    id: root

    property string icon: ""
    property string text: ""
    /// -1 neutral, 0 bad, 1 good, 2 warn.
    property int tone: -1

    implicitWidth: row.implicitWidth + 20
    implicitHeight: 26
    radius: height / 2
    color: Theme.pill
    border.width: 1
    border.color: Theme.hairline

    readonly property color toneColor:
        tone === 1 ? Theme.ok
      : tone === 0 ? Theme.bad
      : tone === 2 ? Theme.warn
      : Theme.dim

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 6

        MazeIcon {
            visible: root.icon !== ""
            name: root.icon
            color: root.toneColor
            width: 14
            height: 14
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            text: root.text
            color: Theme.text
            font.family: Theme.fontSans
            font.pixelSize: 11
            anchors.verticalCenter: parent.verticalCenter
        }
    }
}
