import QtQuick
import MazeConnect.App

/**
 * The labelled progress bar from maze-control-center's dashboard: name on the
 * left, reading on the right, a filled track underneath.
 *
 * The fill animates between readings so a refresh reads as movement rather
 * than as a redraw. Nothing here starts hidden — the track and both labels are
 * drawn at full opacity from the first frame, and only the fill's *width* is
 * animated. A meter that animated its way into visibility would show an empty
 * panel to anyone whose first glance lands before the animation.
 */
Item {
    id: root

    property string label: ""
    property real percent: 0
    property string detail: ""

    width: parent ? parent.width : 0
    height: 46

    readonly property real clamped: Math.max(0, Math.min(100, percent))

    // Colour is a second signal on top of the number, never the only one.
    readonly property color fillColor:
        clamped >= 90 ? Theme.bad
      : clamped >= 75 ? Theme.warn
      : Theme.ok

    Text {
        id: name
        anchors.left: parent.left
        anchors.top: parent.top
        text: root.label
        color: Theme.text
        font.family: Theme.fontSans
        font.pixelSize: 12
    }

    Text {
        anchors.right: parent.right
        anchors.baseline: name.baseline
        text: root.detail !== ""
              ? qsTr("%1%  ·  %2").arg(Math.round(root.clamped)).arg(root.detail)
              : qsTr("%1%").arg(Math.round(root.clamped))
        color: Theme.dim
        font.family: Theme.fontMono
        font.pixelSize: 11
    }

    Rectangle {
        id: track
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: name.bottom
        anchors.topMargin: 8
        height: 6
        radius: 3
        color: Theme.pill

        Rectangle {
            width: track.width * (root.clamped / 100)
            height: parent.height
            radius: parent.radius
            color: root.fillColor

            Behavior on width { NumberAnimation { duration: 320; easing.type: Easing.OutCubic } }
            Behavior on color { ColorAnimation { duration: 320 } }
        }
    }
}
