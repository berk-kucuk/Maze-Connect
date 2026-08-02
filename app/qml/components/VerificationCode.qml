// The per-digit delegates reach this file's root for the code text.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import MazeConnect.App

/**
 * The six-digit pairing code, set as a readout rather than as text.
 *
 * This is the one moment where security depends entirely on a person: if
 * the digits here differ from the digits on the other screen, someone is in
 * the middle. So it is the loudest thing in the application — each digit
 * gets its own hairline cell, at display size, in mono, so that comparing
 * two screens is a glance rather than a squint.
 *
 * FAIL-SAFE VISIBILITY
 * --------------------
 * The digits are opaque by default and the entrance animation only ever
 * fades them *in*. An earlier version started them at `opacity: 0` and
 * relied on an animation to reveal them; the animation targeted the wrong
 * object and every digit stayed invisible — the dialog rendered with an
 * empty code area and the user would have been asked to confirm nothing.
 * Anything decorative here must degrade to "visible", never to "hidden".
 */
Item {
    id: root

    property string code: ""

    // Application.styleHints is the QtQuick accessor; guarded because the
    // property is not present on every Qt build, and a missing hint must
    // not leave `reduceMotion` undefined (which would read as falsy in some
    // places and break bindings in others).
    // qmllint disable missing-property
    // Flagged statically because the property is absent from some Qt builds —
    // which is exactly what the `&&` above guards against at runtime. The
    // check is correct; only the static analysis cannot see that.
    readonly property bool reduceMotion:
        Application.styleHints && Application.styleHints.prefersReducedMotion === true
    // qmllint enable missing-property

    implicitWidth: row.implicitWidth
    implicitHeight: 96

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 8

        Repeater {
            model: root.code.length

            delegate: Rectangle {
                id: cell
                required property int index

                width: 54
                height: 82
                radius: 8
                color: "#141418"
                border.width: 1
                border.color: Theme.panelBorder

                Text {
                    anchors.centerIn: parent
                    text: root.code.charAt(cell.index)
                    color: Theme.text
                    font.family: Theme.fontMono
                    font.pixelSize: 40
                    font.weight: Font.Light
                }

                // Digits settle left to right, so the readout resolves
                // rather than simply appearing. Starts and ends opaque:
                // if this never runs, the code is still readable.
                SequentialAnimation on opacity {
                    running: !root.reduceMotion
                    PauseAnimation { duration: cell.index * 45 }
                    NumberAnimation { from: 0; to: 1; duration: 180; easing.type: Easing.OutCubic }
                }
            }
        }
    }

    // A single scanline pass, borrowed from the Maze site's sweep keyframe:
    // it reads as the value having just been measured, not stored.
    Rectangle {
        id: sweep
        width: root.width
        height: 1
        color: Qt.rgba(1, 1, 1, 0.45)
        visible: !root.reduceMotion
        opacity: 0

        SequentialAnimation {
            running: !root.reduceMotion
            PropertyAction { target: sweep; property: "opacity"; value: 0.9 }
            NumberAnimation {
                target: sweep
                property: "y"
                from: 0
                to: root.height
                duration: 620
                easing.type: Easing.InOutQuad
            }
            NumberAnimation { target: sweep; property: "opacity"; to: 0; duration: 200 }
        }
    }
}
