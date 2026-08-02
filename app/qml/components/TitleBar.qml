import QtQuick
import MazeConnect.App

/**
 * The window's own title bar.
 *
 * The suite runs frameless (maze-control-center and maze-welcome both set
 * FramelessWindowHint) so the rounded glass panel is the window edge, with
 * no Plasma decoration squared off around it. That means this bar has to
 * provide what the decoration otherwise would: a drag handle, minimise and
 * close, and somewhere for the title to live.
 */
Item {
    id: root

    property string title: ""
    property string subtitle: ""
    required property var window

    height: 52

    // Drag anywhere on the bar that is not a button.
    DragHandler {
        id: drag
        target: null
        onActiveChanged: if (active) root.window.startSystemMove()
    }

    Text {
        anchors.left: parent.left
        anchors.leftMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        text: root.title
        color: Theme.text
        font.family: Theme.fontSans
        font.pixelSize: 15
        font.weight: Font.DemiBold
    }

    Text {
        id: subtitleItem
        anchors.right: buttons.left
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        text: root.subtitle
        color: Theme.faint
        font.family: Theme.fontMono
        font.pixelSize: 11
        elide: Text.ElideMiddle
        width: Math.min(implicitWidth, root.width * 0.4)
        horizontalAlignment: Text.AlignRight
    }

    Row {
        id: buttons
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: 4

        WindowButton {
            glyph: "–"
            onClicked: root.window.showMinimized()
        }

        WindowButton {
            glyph: "✕"
            danger: true
            onClicked: root.window.close()
        }
    }
}
