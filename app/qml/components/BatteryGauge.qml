import QtQuick
import QtQuick.Shapes
import MazeConnect.App

/**
 * A phone's battery as a ring: the level as an arc, the number in the middle,
 * and a bolt while it charges.
 *
 * The number is the reading; the arc and its colour only help it be read at a
 * glance, so neither is ever the only signal. `level` below zero means "no
 * reading" — drawn as an empty track and a dash, never as 0%, which would say
 * the phone is about to die when nothing of the sort was reported.
 *
 * Nothing here starts hidden: only the arc's sweep animates, from wherever it
 * was, so a first glance always lands on the number.
 */
Item {
    id: root

    property int level: -1
    property bool charging: false
    property bool dimmed: false
    property real thickness: 8

    implicitWidth: 112
    implicitHeight: 112

    readonly property bool known: level >= 0
    readonly property color arcColor:
        !known ? Theme.unknown
      : charging ? Theme.ok
      : level <= 15 ? Theme.bad
      : level <= 30 ? Theme.warn
      : Theme.ok

    property real sweep: known ? Math.max(0, Math.min(100, level)) * 3.6 : 0
    Behavior on sweep { NumberAnimation { duration: 480; easing.type: Easing.OutCubic } }

    opacity: dimmed ? 0.55 : 1.0

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        layer.enabled: true
        layer.samples: 4

        // The track.
        ShapePath {
            strokeColor: Theme.pill
            strokeWidth: root.thickness
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: root.width / 2
                centerY: root.height / 2
                radiusX: root.width / 2 - root.thickness / 2
                radiusY: root.height / 2 - root.thickness / 2
                startAngle: 0
                sweepAngle: 360
            }
        }

        // The level, clockwise from twelve o'clock.
        ShapePath {
            strokeColor: root.arcColor
            strokeWidth: root.thickness
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: root.width / 2
                centerY: root.height / 2
                radiusX: root.width / 2 - root.thickness / 2
                radiusY: root.height / 2 - root.thickness / 2
                startAngle: -90
                sweepAngle: root.sweep
            }
        }
    }

    Column {
        anchors.centerIn: parent
        spacing: 1

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 2

            MazeIcon {
                visible: root.charging && root.known
                name: "bolt"
                color: Theme.ok
                width: 16
                height: 16
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                id: levelText
                text: root.known ? root.level : "—"
                color: Theme.text
                font.family: Theme.fontSans
                font.pixelSize: root.width * 0.25
                font.weight: Font.DemiBold
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                visible: root.known
                text: "%"
                color: Theme.dim
                font.family: Theme.fontSans
                font.pixelSize: root.width * 0.12
                anchors.baseline: levelText.baseline
            }
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: !root.known ? qsTr("no reading")
                 : root.charging ? qsTr("charging")
                 : qsTr("battery")
            color: Theme.faint
            font.family: Theme.fontSans
            font.pixelSize: 10
            font.letterSpacing: 1.2
            font.capitalization: Font.AllUppercase
        }
    }
}
