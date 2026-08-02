import QtQuick
import MazeConnect.App

/// Sidebar entry, matching maze_ui.NavButton: 42px tall, left-aligned,
/// dim until hovered, filled pill when current.
Item {
    id: root

    property string label: ""
    /// A MazeIcon name; see MazeIcon.qml.
    property string icon: ""
    property bool current: false
    property int badge: 0

    signal clicked()

    width: parent ? parent.width : 178
    height: 42

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusPill
        color: root.current
               ? Theme.navActive
               : (hover.hovered ? Theme.pill : "transparent")
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    MazeIcon {
        id: glyphItem
        anchors.left: parent.left
        anchors.leftMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        width: 18
        height: 18
        name: root.icon
        color: root.current ? Theme.text : (hover.hovered ? Theme.text : Theme.dim)
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    Text {
        anchors.left: glyphItem.right
        anchors.leftMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        text: root.label
        color: root.current ? Theme.text : (hover.hovered ? Theme.text : Theme.dim)
        font.family: Theme.fontSans
        font.pixelSize: 13
        font.weight: root.current ? Font.DemiBold : Font.Normal
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    // Unread count, so activity that happened while you were on another
    // page is visible without opening it.
    Rectangle {
        visible: root.badge > 0
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(18, badgeText.implicitWidth + 10)
        height: 18
        radius: 9
        color: Theme.pillHover

        Text {
            id: badgeText
            anchors.centerIn: parent
            text: root.badge > 99 ? "99+" : root.badge
            color: Theme.text
            font.family: Theme.fontMono
            font.pixelSize: 10
        }
    }

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: root.clicked() }

    focus: true
    activeFocusOnTab: true
    Keys.onReturnPressed: root.clicked()
    Keys.onSpacePressed: root.clicked()

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusPill
        color: "transparent"
        border.width: 1
        border.color: Theme.text
        visible: root.activeFocus
    }
}
