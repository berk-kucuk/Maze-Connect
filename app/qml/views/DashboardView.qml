import QtQuick
import MazeConnect.App

/**
 * What this machine looks like right now — the same reading a paired phone
 * gets when it asks.
 *
 * The numbers come from maze-tools' `maze_status.py` by way of the
 * `maze-connect-status` helper, so this panel and maze-control-center's
 * dashboard are showing the same probes rather than two guesses at them.
 *
 * This renders the *local* snapshot only. A statusReport from a paired device
 * is peer-supplied data and nothing on this screen draws it.
 */
Item {
    id: root

    readonly property var snapshot: Backend.systemStatus
    readonly property string error: Backend.systemStatusError
    readonly property bool hasSnapshot: snapshot && Object.keys(snapshot).length > 0

    // Refresh only while the page is on screen: the helper is cheap but not
    // free, and a dashboard nobody is looking at has no reason to run it.
    Timer {
        interval: 3000
        repeat: true
        running: root.visible
        triggeredOnStart: true
        onTriggered: Backend.refreshSystemStatus()
    }

    // ---- Nothing to show -------------------------------------------------
    Column {
        anchors.centerIn: parent
        spacing: 10
        width: 420
        visible: !root.hasSnapshot

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("No reading")
            color: Theme.text
            font.family: Theme.fontSans
            font.pixelSize: 18
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            // The reason matters: an empty dashboard and a machine with
            // nothing running look identical, and they are not the same thing.
            text: root.error !== ""
                  ? qsTr("This machine could not be read: %1").arg(root.error)
                  : qsTr("Reading this machine…")
            color: Theme.dim
            font.family: Theme.fontSans
            font.pixelSize: 13
            lineHeight: 1.45
        }
    }

    // ---- The snapshot ----------------------------------------------------
    Flickable {
        anchors.fill: parent
        visible: root.hasSnapshot
        clip: true
        contentHeight: content.height
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: content
            width: parent.width
            spacing: 18

            // Header: hostname, hardening score, and any stale-reading notice.
            Row {
                width: parent.width
                spacing: 12

                Column {
                    spacing: 3
                    width: parent.width - score.width - 12

                    Text {
                        text: root.snapshot.hostname || qsTr("this machine")
                        color: Theme.text
                        font.family: Theme.fontSans
                        font.pixelSize: 20
                        font.weight: Font.Bold
                    }
                    Text {
                        // A failed refresh leaves the previous snapshot on
                        // screen rather than blanking it, so say that it is
                        // the previous one.
                        text: root.error !== ""
                              ? qsTr("Last good reading — %1").arg(root.error)
                              : qsTr("Live")
                        color: root.error !== "" ? Theme.warn : Theme.faint
                        font.family: Theme.fontMono
                        font.pixelSize: 10
                    }
                }

                Column {
                    id: score
                    spacing: 2
                    visible: root.snapshot.hardening !== undefined

                    Text {
                        anchors.right: parent.right
                        text: qsTr("%1%").arg(root.snapshot.hardening
                                              ? root.snapshot.hardening.score : 0)
                        color: !root.snapshot.hardening ? Theme.unknown
                             : root.snapshot.hardening.score >= 80 ? Theme.ok
                             : root.snapshot.hardening.score >= 50 ? Theme.warn
                             : Theme.bad
                        font.family: Theme.fontMono
                        font.pixelSize: 22
                    }
                    SectionLabel {
                        anchors.right: parent.right
                        text: qsTr("Hardening")
                    }
                }
            }

            // ---- Meters --------------------------------------------------
            Column {
                width: parent.width
                spacing: 8
                visible: repeaterMetrics.count > 0

                SectionLabel { text: qsTr("Load") }

                Repeater {
                    id: repeaterMetrics
                    model: root.snapshot.metrics || []

                    Meter {
                        required property var modelData
                        label: modelData.label || ""
                        percent: modelData.percent || 0
                        detail: modelData.detail || ""
                    }
                }
            }

            // ---- Security services ---------------------------------------
            Column {
                width: parent.width
                spacing: 4
                visible: repeaterSecurity.count > 0

                SectionLabel { text: qsTr("Security services") }

                Repeater {
                    id: repeaterSecurity
                    model: root.snapshot.security || []

                    Pill {
                        required property var modelData
                        label: modelData.label || ""
                        showDot: true
                        // "unknown" is its own state, not a quiet "off": a
                        // service that was never installed has not failed.
                        state_: modelData.state === "active" ? 1
                              : modelData.state === "inactive" ? 0
                              : -1
                        value: modelData.state === "active" ? qsTr("Active")
                             : modelData.state === "inactive" ? qsTr("Inactive")
                             : qsTr("Unknown")
                    }
                }
            }

            // ---- Network -------------------------------------------------
            Column {
                width: parent.width
                spacing: 4
                visible: repeaterNetwork.count > 0

                SectionLabel { text: qsTr("Network") }

                Repeater {
                    id: repeaterNetwork
                    model: root.snapshot.network || []

                    Pill {
                        required property var modelData
                        label: modelData.label || ""
                        value: modelData.value || ""
                        showDot: modelData.good !== "unknown"
                        state_: modelData.good === "active" ? 1
                              : modelData.good === "inactive" ? 0
                              : -1
                    }
                }
            }

            // ---- Hardware ------------------------------------------------
            Column {
                width: parent.width
                spacing: 4
                visible: repeaterFacts.count > 0

                SectionLabel { text: qsTr("Machine") }

                Repeater {
                    id: repeaterFacts
                    model: root.snapshot.facts || []

                    Text {
                        required property var modelData
                        width: parent ? parent.width : 0
                        text: "· " + (modelData.value || "")
                        color: Theme.dim
                        font.family: Theme.fontMono
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                }
            }

            // Sections the helper could not gather. Named rather than
            // silently omitted, so a missing panel is never read as a
            // reading of zero.
            Text {
                width: parent.width
                visible: (root.snapshot.unavailable || []).length > 0
                text: qsTr("Could not read: %1")
                      .arg((root.snapshot.unavailable || []).join(", "))
                color: Theme.warn
                font.family: Theme.fontMono
                font.pixelSize: 10
                wrapMode: Text.WordWrap
            }

            Item { width: 1; height: 4 }
        }
    }
}
