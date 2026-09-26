import QtQuick
import MazeConnect.App

/**
 * The frosted panel the whole window sits in.
 *
 * Reproduces maze_ui.Glass from the Python suite: a near-opaque black
 * rounded rect, a soft radial glow falling from the top, a faint corner
 * watermark, and a hairline border. Getting this right is what makes the
 * app read as part of the same family as maze-control-center rather than
 * as a lookalike.
 */
Item {
    id: root

    Rectangle {
        id: body
        anchors.fill: parent
        radius: Theme.radiusPanel
        // Opaque enough that content never competes with whatever is
        // behind the window; the compositor blur shows through the small
        // remaining translucency and at the rounded edges.
        color: Qt.rgba(0.024, 0.024, 0.031, 0.90)
        border.width: 1
        border.color: Theme.panelBorder
    }

    // Radial glow from the top centre, mirroring Glass.paintEvent's
    // QRadialGradient (white at alpha 18 fading to nothing).
    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusPanel
        color: "transparent"
        clip: true

        Rectangle {
            width: parent.width * 1.5
            height: width
            x: (parent.width - width) / 2
            y: -width / 2 + 70
            radius: width / 2
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.rgba(1, 1, 1, 0.07) }
                GradientStop { position: 1.0; color: Qt.rgba(1, 1, 1, 0.0) }
            }
        }
    }

    // Corner watermark at 7%, exactly as the Python Glass does.
    Image {
        source: Theme.logoPath
        width: 260
        height: 260
        fillMode: Image.PreserveAspectFit
        opacity: 0.07
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: -40
        anchors.bottomMargin: -40
        visible: status === Image.Ready
    }
}
