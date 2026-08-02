import QtQuick
import MazeConnect.App

/**
 * Action button in the suite's pill idiom.
 *
 * `primary` fills with soft white; the rest stay outlined. Deliberately not
 * the website's invert-on-hover block: the applications use rounded pills
 * that lighten, and mixing the two idioms in one window reads as a mistake.
 */
Item {
    id: root

    property string text: ""
    property bool primary: false
    property bool destructive: false
    property string glyph: ""

    signal clicked()

    implicitWidth: content.implicitWidth + 30
    implicitHeight: 34
    opacity: enabled ? 1.0 : 0.35

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusPill
        color: root.primary
               ? (hover.hovered ? Qt.rgba(1, 1, 1, 0.22) : Qt.rgba(1, 1, 1, 0.14))
               : (hover.hovered ? Theme.pillHover : Theme.pill)
        border.width: 1
        border.color: hover.hovered ? Theme.panelBorder : "transparent"
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    Row {
        id: content
        anchors.centerIn: parent
        spacing: 7

        Text {
            visible: root.glyph !== ""
            text: root.glyph
            color: root.destructive && hover.hovered ? Theme.bad : Theme.text
            font.family: Theme.fontMono
            font.pixelSize: 12
            anchors.verticalCenter: parent.verticalCenter
        }

        Text {
            text: root.text
            color: root.destructive && hover.hovered ? Theme.bad : Theme.text
            font.family: Theme.fontSans
            font.pixelSize: 12
            font.weight: root.primary ? Font.DemiBold : Font.Normal
            anchors.verticalCenter: parent.verticalCenter
            Behavior on color { ColorAnimation { duration: 120 } }
        }
    }

    HoverHandler { id: hover; enabled: root.enabled; cursorShape: Qt.PointingHandCursor }
    TapHandler { enabled: root.enabled; onTapped: root.clicked() }

    focus: true
    activeFocusOnTab: root.enabled
    Keys.onReturnPressed: if (root.enabled) root.clicked()
    Keys.onSpacePressed: if (root.enabled) root.clicked()

    Rectangle {
        anchors.fill: parent
        anchors.margins: -3
        radius: Theme.radiusPill + 3
        color: "transparent"
        border.width: 1
        border.color: Theme.text
        visible: root.activeFocus
    }
}
