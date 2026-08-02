import QtQuick
import MazeConnect.App

/// Minimise/close control for the frameless title bar.
Item {
    id: root

    property string glyph: ""
    property bool danger: false

    signal clicked()

    width: 30
    height: 30

    Rectangle {
        anchors.fill: parent
        radius: 8
        color: hover.hovered
               ? (root.danger ? Qt.rgba(0.88, 0.4, 0.4, 0.18) : Theme.pillHover)
               : "transparent"
        Behavior on color { ColorAnimation { duration: 110 } }
    }

    Text {
        anchors.centerIn: parent
        text: root.glyph
        color: hover.hovered && root.danger ? Theme.bad : Theme.dim
        font.family: Theme.fontSans
        font.pixelSize: 13
        Behavior on color { ColorAnimation { duration: 110 } }
    }

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: root.clicked() }

    focus: true
    activeFocusOnTab: true
    Keys.onReturnPressed: root.clicked()

    Rectangle {
        anchors.fill: parent
        radius: 8
        color: "transparent"
        border.width: 1
        border.color: Theme.text
        visible: root.activeFocus
    }
}
