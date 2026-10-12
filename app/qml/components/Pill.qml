import QtQuick
import MazeConnect.App

/**
 * The pill row from maze_ui.status_row / info_row: rounded soft-white
 * background, optional status dot, label on the left, value on the right.
 *
 * `detail` adds a second, quieter line under the label (a key code, a path),
 * and `trailing` holds controls at the right edge — buttons go there, never
 * anchored over the row by hand. The value ends where the controls begin, so
 * the two cannot overlap however long either gets.
 */
Rectangle {
    id: root

    property string label: ""
    property string value: ""
    /// Second line under the label, in the mono face. Empty: one line.
    property string detail: ""
    /// -1 unknown, 0 bad, 1 good, 2 warn. Always accompanied by [value] text,
    /// so the colour never carries the meaning by itself.
    property int state_: -1
    property bool showDot: false
    property bool interactive: false
    /// Controls at the right edge, e.g. `trailing: MazeButton { … }`.
    property alias trailing: trailingRow.data

    signal clicked()

    width: parent ? parent.width : 0
    height: detail !== "" ? 56 : 42
    radius: Theme.radiusPill
    color: interactive && hover.hovered ? Theme.pillHover : Theme.pill
    Behavior on color { ColorAnimation { duration: 120 } }

    readonly property color stateColor:
        state_ === 1 ? Theme.ok
      : state_ === 0 ? Theme.bad
      : state_ === 2 ? Theme.warn
      : Theme.unknown

    Rectangle {
        id: dot
        visible: root.showDot
        anchors.left: parent.left
        anchors.leftMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        width: 8
        height: 8
        radius: 4
        color: root.stateColor
    }

    Column {
        id: labels
        anchors.left: root.showDot ? dot.right : parent.left
        anchors.leftMargin: root.showDot ? 10 : 14
        anchors.verticalCenter: parent.verticalCenter
        // Leave the right side to the value and controls; the label elides
        // before it would run under them.
        width: Math.max(0, valueText.x - x - 16)
        spacing: 3

        Text {
            width: parent.width
            text: root.label
            textFormat: Text.PlainText
            color: Theme.text
            font.family: Theme.fontSans
            font.pixelSize: 13
            elide: Text.ElideRight
        }
        Text {
            visible: root.detail !== ""
            width: parent.width
            text: root.detail
            textFormat: Text.PlainText
            color: Theme.faint
            font.family: Theme.fontMono
            font.pixelSize: 10
            elide: Text.ElideMiddle
        }
    }

    Text {
        id: valueText
        anchors.right: trailingRow.children.length > 0 ? trailingRow.left : parent.right
        anchors.rightMargin: trailingRow.children.length > 0 ? 12 : 14
        anchors.verticalCenter: parent.verticalCenter
        text: root.value
        textFormat: Text.PlainText
        color: root.showDot ? root.stateColor : Theme.dim
        font.family: Theme.fontMono
        font.pixelSize: 11
        elide: Text.ElideMiddle
        width: Math.min(implicitWidth, root.width * 0.45)
        horizontalAlignment: Text.AlignRight
    }

    Row {
        id: trailingRow
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6
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
