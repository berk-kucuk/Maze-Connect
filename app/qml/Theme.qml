pragma Singleton
import QtQuick

/**
 * Design tokens, matching the rest of the Maze desktop suite.
 *
 * These come from maze-tools' shared `maze_ui.py`, which maze-control-center
 * and maze-welcome both build on — not from the Maze *website* palette. The
 * two differ on purpose: the site is strictly monochrome with sharp corners,
 * while the applications are a rounded, glassy, true-black OLED family that
 * does use colour for status. An app that followed the site would look
 * correct in isolation and wrong next to its siblings.
 */
QtObject {
    id: theme

    // ---- Surfaces --------------------------------------------------------
    readonly property color voidBlack: "#000000"
    readonly property color panelBlack: "#060608"

    // Maze Linux is an OLED-first distro — maze-oled is the shipped SDDM
    // theme and the suite is true-black throughout. A toggle offering to
    // turn that off was redundant, so the surface is simply the OLED one.
    readonly property color panel: panelBlack

    // ---- Text ------------------------------------------------------------
    readonly property color text: "#F2F3F5"
    readonly property color dim: "#9AA0AA"
    readonly property color faint: "#7A7F88"

    // Kept for the few places that intentionally use the site's warm paper.
    readonly property color paper: "#F2F1EC"

    // ---- Pills & lines ---------------------------------------------------
    readonly property color pill: Qt.rgba(1, 1, 1, 0.06)
    readonly property color pillHover: Qt.rgba(1, 1, 1, 0.13)
    readonly property color navActive: Qt.rgba(1, 1, 1, 0.12)
    readonly property color hairline: Qt.rgba(1, 1, 1, 0.08)
    readonly property color panelBorder: Qt.rgba(1, 1, 1, 0.16)
    readonly property color sidebarBg: Qt.rgba(1, 1, 1, 0.025)

    // ---- Status ----------------------------------------------------------
    // The suite uses colour for state; this is the one place hue carries
    // meaning, and it is always paired with a text label so it never carries
    // it alone.
    readonly property color ok: "#56C271"
    readonly property color bad: "#E06666"
    readonly property color unknown: "#6B7079"
    readonly property color warn: "#E0B066"

    // ---- Type ------------------------------------------------------------
    readonly property string fontSans: "IBM Plex Sans"
    readonly property string fontMono: "IBM Plex Mono"
    readonly property real trackingLabel: 0.18

    // ---- Metrics ---------------------------------------------------------
    readonly property int radiusPanel: 22
    readonly property int radiusPill: 10
    readonly property int sidebarWidth: 210
    readonly property int unit: 8
    readonly property int gutter: 28

    // Maze Connect's own mark, not the shared Maze family logo: this app now
    // has one, and the sidebar should say which application you are in rather
    // than which distribution.
    //
    // Ground-free on purpose — it is drawn on the glass panel and on the
    // corner watermark, where the black square the launcher icon carries
    // would read as a hole. Both users guard on Image.Ready, so an
    // uninstalled build simply shows no mark instead of a broken one.
    readonly property string logoPath: "file:///usr/share/pixmaps/maze-connect-mark.png"
}
