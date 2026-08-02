// Delegates below reach `root` for the label table. Without this pragma
// that is an unqualified access, and qmllint is right to flag it: the id
// would resolve at runtime rather than being bound at compile time.
pragma ComponentBehavior: Bound

import QtQuick
import MazeConnect.App

/**
 * maze-guard's killswitches.
 *
 * The privileged page. Two things about it are deliberate and should stay:
 *
 * There is no "panic" button, and no way to reach one. PANIC and RESTORE are
 * destructive and belong to `maze-guard` at the terminal, in front of the
 * machine — not to anything that can be driven from a phone.
 *
 * The state shown is what the broker last reported, never what was asked for.
 * A request maze-guardd accepted is not proof the hardware complied, so a
 * switch here follows reality rather than intent.
 */
Item {
    id: root

    onVisibleChanged: if (visible) Backend.refreshGuard()
    Component.onCompleted: if (visible) Backend.refreshGuard()

    readonly property var labels: ({
        "camera": qsTr("Camera"),
        "microphone": qsTr("Microphone"),
        "bluetooth": qsTr("Bluetooth"),
        "wifi": qsTr("Wi-Fi"),
        "usb": qsTr("USB")
    })

    Row {
        id: header
        width: parent.width
        spacing: 12

        SectionLabel {
            text: qsTr("Killswitches")
            anchors.verticalCenter: parent.verticalCenter
        }

        Item { width: parent.width - 240; height: 1 }

        MazeButton {
            text: qsTr("Refresh")
            onClicked: Backend.refreshGuard()
        }
    }

    // A remote change is announced here as well as logged. It stays until the
    // next one: a banner that faded after three seconds would be missable,
    // which defeats the point of having it.
    Rectangle {
        id: banner
        anchors.top: header.bottom
        anchors.topMargin: 12
        anchors.left: parent.left
        anchors.right: parent.right
        height: Backend.guardBanner === "" ? 0 : 40
        radius: Theme.radiusPill
        color: Qt.rgba(0.88, 0.69, 0.40, 0.14)
        clip: true
        visible: height > 0

        Behavior on height { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            text: Backend.guardBanner
            color: Theme.warn
            font.family: Theme.fontSans
            font.pixelSize: 12
            elide: Text.ElideRight
        }
    }

    // ---- Broker unavailable ----------------------------------------------
    Column {
        anchors.centerIn: parent
        spacing: 10
        width: 440
        visible: Backend.guardDevices.length === 0

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: Backend.guardError !== "" ? qsTr("Killswitches unavailable")
                                            : qsTr("Reading killswitches…")
            color: Backend.guardError !== "" ? Theme.warn : Theme.text
            font.family: Theme.fontSans
            font.pixelSize: 18
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: Backend.guardError !== ""
                  ? Backend.guardError
                  : qsTr("Asking maze-guardd for the state of each switch.")
            color: Theme.dim
            font.family: Theme.fontSans
            font.pixelSize: 13
            lineHeight: 1.45
        }
    }

    // ---- The switches ----------------------------------------------------
    Column {
        anchors.top: banner.bottom
        anchors.topMargin: 14
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 6
        visible: Backend.guardDevices.length > 0

        Repeater {
            model: Backend.guardDevices

            Rectangle {
                id: row
                required property var modelData

                readonly property bool blocked: modelData.state === "off"
                readonly property bool absent: modelData.state === "none"
                readonly property bool unknown: modelData.state === "unknown"

                width: parent.width
                height: 56
                radius: Theme.radiusPill
                color: hover.hovered && !row.absent ? Theme.pillHover : Theme.pill
                Behavior on color { ColorAnimation { duration: 120 } }

                HoverHandler { id: hover; enabled: !row.absent }

                Rectangle {
                    id: dot
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    width: 8
                    height: 8
                    radius: 4
                    // Blocked is the *protected* state, so it is the good one
                    // here — the opposite of most status dots in this app,
                    // which is why the text beside it always says which.
                    color: row.absent ? Theme.unknown
                         : row.unknown ? Theme.unknown
                         : row.blocked ? Theme.ok
                         : Theme.dim
                }

                MazeIcon {
                    id: deviceIcon
                    anchors.left: dot.right
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    width: 20
                    height: 20
                    name: row.modelData.device
                    // Follows the dot's reading of the row rather than a
                    // fixed colour, so the icon is part of the state and not
                    // decoration sitting next to it.
                    color: row.absent ? Theme.unknown
                         : row.blocked ? Theme.ok
                         : Theme.text
                }

                Column {
                    anchors.left: deviceIcon.right
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3

                    Text {
                        text: root.labels[row.modelData.device] || row.modelData.device
                        color: row.absent ? Theme.faint : Theme.text
                        font.family: Theme.fontSans
                        font.pixelSize: 14
                    }
                    Text {
                        text: row.absent ? qsTr("not present on this machine")
                            : row.unknown ? qsTr("state unknown")
                            : row.blocked ? qsTr("blocked")
                            : qsTr("allowed")
                        color: Theme.faint
                        font.family: Theme.fontMono
                        font.pixelSize: 10
                    }
                }

                MazeButton {
                    anchors.right: parent.right
                    anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    // A machine with no Bluetooth has nothing to block, so
                    // there is nothing to press.
                    visible: !row.absent
                    text: row.blocked ? qsTr("Allow") : qsTr("Block")
                    // The argument is the *device's* wanted state, not the
                    // switch's. If it is blocked now, the button says "Allow"
                    // and asks for enabled = true.
                    //
                    // This was `!row.blocked`, which is the same expression
                    // read through the opposite vocabulary — and it made
                    // Block unblock. Both readings look right in isolation;
                    // that is why the parameter is now named after the device.
                    onClicked: Backend.setGuardKill(row.modelData.device, row.blocked)
                }
            }
        }

        Item { width: 1; height: 8 }

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            // Says plainly what this page will not do, so nobody goes looking
            // for the button.
            text: qsTr("Panic and restore are not available here or from a paired phone — they are destructive, and belong at the terminal in front of this machine. Use `maze-guard` for those.")
            color: Theme.faint
            font.family: Theme.fontSans
            font.pixelSize: 11
            lineHeight: 1.4
        }
    }
}
