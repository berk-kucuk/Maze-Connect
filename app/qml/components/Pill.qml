import QtQuick
import MazeConnect.App

/**
 * The pill row from maze_ui.status_row / info_row: rounded soft-white
 * background, optional status dot, label on the left, value on the right.
 */
Rectangle {
    id: root

    property string label: ""
    property string value: ""
    /// -1 unknown, 0 bad, 1 good, 2 warn. Always accompanied by [value] text,
    /// so the colour never carries the meaning by itself.
    property int state_: -1
    property bool showDot: false
    property bool interactive: false

    signal clicked()

    width: parent ? parent.width : 0
    height: 42
    radius: Theme.radiusPill
    color: interactive && hover.hovered ? Theme.pillHover : Theme.pill
    Behavior on color { ColorAnimation { duration: 120 } }

    readonly property color stateColor:
        state_ === 1 ? Theme.ok
      : state_ === 0 ? Theme.bad
      : state_ === 2 ? Theme.warn
      : Theme.unknown

    Row {
        anchors.left: parent.left
        anchors.leftMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        spacing: 10

        Rectangle {
            visible: root.showDot
            width: 8
            height: 8
            radius: 4
            color: root.stateColor
            anchors.verticalCenter: parent.verticalCenter
        }

        Text {
            text: root.label
            color: Theme.text
            font.family: Theme.fontSans
            font.pixelSize: 13
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    Text {
        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        text: root.value
        color: root.showDot ? root.stateColor : Theme.dim
        font.family: Theme.fontMono
        font.pixelSize: 11
        elide: Text.ElideMiddle
        width: Math.min(implicitWidth, root.width * 0.55)
        horizontalAlignment: Text.AlignRight
    }

    HoverHandler {
        id: hover
        enabled: root.interactive
        cursorShape: Qt.PointingHandCursor
    }
    TapHandler {
        enabled: root.interactive
        onTapped: root.clicked()
    }
}
