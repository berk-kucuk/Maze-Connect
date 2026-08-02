import QtQuick
import QtQuick.Shapes

/**
 * The application's line-icon set.
 *
 * Drawn rather than typeset. The navigation and the killswitches used single
 * Unicode characters — "▤", "⛨", "◈" — which is convenient and looks like
 * whatever font happened to resolve them: different weights, different optical
 * sizes, some of them missing outright on a machine without a symbol font, and
 * none of them matching the thin even stroke of the Maze mark.
 *
 * These are one geometry on a 24-unit grid with one stroke weight, so a row of
 * them reads as a set. Stroke, never fill: the mark is line art and a solid
 * glyph next to it looks like it came from somewhere else.
 */
Item {
    id: root

    /// One of the names in `paths` below. An unknown name draws nothing,
    /// which is preferable to a placeholder box in the middle of a menu.
    property string name: ""
    property color color: "#F2F1EC"
    property real weight: 1.6

    implicitWidth: 20
    implicitHeight: 20

    // Subpaths are separated by "|" so one icon can be several strokes
    // without needing a separate ShapePath each — Shape draws them as one
    // path, which is also what keeps the joins consistent.
    readonly property var paths: ({
        // -- navigation --
        "devices":   "M3 5h12v9H3z|M7 18h4|M9 14v4|M17 8h4v11h-4z",
        "transfers": "M12 3v10|M8 10l4 4 4-4|M4 20h16",
        "dashboard": "M4 19a8 8 0 0 1 16 0|M12 19l4.5-4.5",
        "commands":  "M3 5h18v14H3z|M7 10l3 2.5-3 2.5|M13 15h4",
        "guard":     "M12 3l7 3v6c0 4-3 6.5-7 8-4-1.5-7-4-7-8V6z|M9 12l2 2 4-4",
        "activity":  "M3 12h3.5l2.5-7 4 14 2.5-7H21",
        "settings":  "M4 7h6|M14 7h6|M4 12h10|M18 12h2|M4 17h3|M11 17h9",
        // -- killswitches --
        "camera":    "M3 7h4l2-2h6l2 2h4v12H3z|M12 16.5a3.5 3.5 0 1 1 0-7 3.5 3.5 0 0 1 0 7z",
        "microphone": "M12 3a3 3 0 0 1 3 3v5a3 3 0 0 1-6 0V6a3 3 0 0 1 3-3z|M5 11a7 7 0 0 0 14 0|M12 18v3|M8 21h8",
        "wifi":      "M2.5 8.5a15 15 0 0 1 19 0|M5.5 12.5a10 10 0 0 1 13 0|M8.5 16.5a5 5 0 0 1 7 0|M12 20h0.01",
        "bluetooth": "M7 7.5l10 9-5 3.5V4l5 3.5-10 9"
    })

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        // Antialiasing of a 1.6-unit stroke is the whole legibility of these
        // at 20 px, so it is not left to the default.
        layer.enabled: true
        layer.samples: 4

        ShapePath {
            strokeColor: root.color
            fillColor: "transparent"
            strokeWidth: root.weight * (root.width / 24)
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin

            // Scaled from the 24-unit grid to whatever size the caller asked
            // for, so one definition serves the 20 px menu and anything else.
            scale: Qt.size(root.width / 24, root.height / 24)

            PathSvg {
                path: root.paths[root.name] !== undefined
                      ? String(root.paths[root.name]).split("|").join(" ")
                      : ""
            }
        }
    }
}
