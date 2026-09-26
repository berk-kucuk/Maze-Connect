import QtQuick
import MazeConnect.App

/**
 * One paired phone on the dashboard.
 *
 * Everything drawn here comes from `phone.status`, which the core has already
 * validated field by field (see phonestatus::sanitize). A field that did not
 * survive is absent, and this card leaves its row out rather than inventing a
 * value for it — the same rule as the rest of the suite: an empty reading and
 * a reading of zero are different things and must not look alike.
 *
 * A phone that has gone out of reach keeps its last reading, dimmed and
 * labelled with its age, because "82% three minutes ago" still answers the
 * question the user came here with.
 */
Rectangle {
    id: card

    /// One entry of Backend.phones.
    property var phone: ({})
    /// Wall-clock milliseconds, ticked by the dashboard so ages stay current
    /// without every card running its own timer.
    property real now: Date.now()

    signal sendFileRequested(string deviceId)
    signal sendTextRequested(string deviceId, string name)

    readonly property var status: phone.status || ({})
    readonly property bool connected: phone.connected === true
    readonly property bool hasReading: Object.keys(status).length > 0

    width: parent ? parent.width : 0
    implicitHeight: body.height + footer.height + 1
    radius: 16
    color: Theme.pill
    border.width: 1
    border.color: drop.containsDrag ? Theme.ok : phone.ringing ? Theme.warn : Theme.hairline
    Behavior on border.color { ColorAnimation { duration: 200 } }

    // ---- Formatting ------------------------------------------------------

    function bytes(value) {
        if (value === undefined) return ""
        const gib = value / (1024 * 1024 * 1024)
        if (gib >= 100) return qsTr("%1 GB").arg(Math.round(gib))
        if (gib >= 1) return qsTr("%1 GB").arg(gib.toFixed(1))
        return qsTr("%1 MB").arg(Math.round(value / (1024 * 1024)))
    }

    function usedPercent(free, total) {
        if (free === undefined || total === undefined || total <= 0) return -1
        return Math.round((1 - free / total) * 100)
    }

    function age(ms) {
        if (!ms) return ""
        const seconds = Math.max(0, Math.round((card.now - ms) / 1000))
        if (seconds < 5) return qsTr("just now")
        if (seconds < 60) return qsTr("%1 s ago").arg(seconds)
        const minutes = Math.round(seconds / 60)
        if (minutes < 60) return qsTr("%1 min ago").arg(minutes)
        const hours = Math.round(minutes / 60)
        if (hours < 48) return qsTr("%1 h ago").arg(hours)
        return qsTr("%1 days ago").arg(Math.round(hours / 24))
    }

    function uptime(ms) {
        if (ms === undefined) return ""
        const minutes = Math.floor(ms / 60000)
        const days = Math.floor(minutes / 1440)
        const hours = Math.floor((minutes % 1440) / 60)
        if (days > 0) return qsTr("up %1 d %2 h").arg(days).arg(hours)
        if (hours > 0) return qsTr("up %1 h %2 min").arg(hours).arg(minutes % 60)
        return qsTr("up %1 min").arg(minutes)
    }

    readonly property string subtitle: {
        const parts = []
        const maker = status.manufacturer || ""
        const model = status.model || ""
        // "samsung SM-S918B" reads better than "SM-S918B" alone, but a model
        // string that already starts with the maker should not repeat it.
        if (model !== "") {
            parts.push(maker !== "" && !model.toLowerCase().startsWith(maker.toLowerCase())
                       ? maker.charAt(0).toUpperCase() + maker.slice(1) + " " + model
                       : model)
        }
        if (status.android) parts.push(qsTr("Android %1").arg(status.android))
        if (status.uptimeMs !== undefined) parts.push(uptime(status.uptimeMs))
        return parts.join("  ·  ")
    }

    readonly property string batteryLine: {
        if (status.batteryLevel === undefined) return ""
        const parts = []
        if (status.charging) {
            const plug = { ac: qsTr("wall charger"), usb: qsTr("USB"),
                           wireless: qsTr("wireless"), dock: qsTr("dock") }[status.plug]
            parts.push(plug ? qsTr("Charging · %1").arg(plug) : qsTr("Charging"))
        } else {
            parts.push(qsTr("On battery"))
        }
        if (status.batteryTemp !== undefined) parts.push(qsTr("%1 °C").arg(status.batteryTemp.toFixed(1)))
        // One fact per line: the column is narrow, and a wrapped "·" left
        // dangling at the start of a line reads as a typo.
        return parts.join("\n")
    }

    // Files dropped on the card go to this phone, each through the usual
    // offer — the phone still asks before anything is written there.
    DropArea {
        id: drop
        anchors.fill: parent
        enabled: card.connected && card.phone.canFiles === true
        keys: ["text/uri-list"]
        onDropped: (event) => {
            for (const url of event.urls) {
                Backend.sendFile(card.phone.deviceId, url)
            }
            event.acceptProposedAction()
        }
    }

    Text {
        anchors.centerIn: parent
        z: 2
        visible: drop.containsDrag
        text: qsTr("Drop to send to %1").arg(card.phone.name || "")
        color: Theme.ok
        font.family: Theme.fontSans
        font.pixelSize: 15
        font.weight: Font.DemiBold
    }

    // ---- Body ------------------------------------------------------------

    Item {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: Math.max(gaugeColumn.height, details.height) + 40

        Column {
            id: gaugeColumn
            anchors.left: parent.left
            anchors.leftMargin: 22
            anchors.top: parent.top
            anchors.topMargin: 20
            width: 128
            spacing: 10

            BatteryGauge {
                id: gauge
                anchors.horizontalCenter: parent.horizontalCenter
                width: 112
                height: 112
                level: card.status.batteryLevel !== undefined ? card.status.batteryLevel : -1
                charging: card.status.charging === true
                dimmed: !card.connected

                // A slow pulse while the phone rings, so the card that is
                // making the noise is the one the eye lands on. Only the
                // scale moves; the reading is never faded out.
                SequentialAnimation on scale {
                    running: card.phone.ringing === true
                    loops: Animation.Infinite
                    alwaysRunToEnd: true
                    NumberAnimation { to: 1.06; duration: 380; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 1.0; duration: 380; easing.type: Easing.InOutSine }
                }
            }

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                visible: text !== ""
                text: card.batteryLine
                color: Theme.dim
                font.family: Theme.fontSans
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
        }

        Column {
            id: details
            anchors.left: gaugeColumn.right
            anchors.leftMargin: 22
            anchors.right: parent.right
            anchors.rightMargin: 22
            anchors.top: parent.top
            anchors.topMargin: 20
            spacing: 12

            // Name and link state.
            Item {
                width: parent.width
                height: Math.max(nameText.height, stateChip.height)

                Text {
                    id: nameText
                    anchors.left: parent.left
                    anchors.right: stateChip.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    text: card.phone.name || ""
                    color: Theme.text
                    font.family: Theme.fontSans
                    font.pixelSize: 19
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Chip {
                    id: stateChip
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    icon: card.phone.ringing ? "bell"
                          : card.phone.controlling ? "phone"
                          : card.connected ? "link" : "offline"
                    tone: card.phone.ringing || card.phone.controlling ? 2 : card.connected ? 1 : -1
                    text: card.phone.ringing ? qsTr("Ringing")
                          : card.phone.controlling ? qsTr("Controlling")
                          : card.connected ? qsTr("Linked")
                          : qsTr("Not reachable")
                }
            }

            Text {
                width: parent.width
                visible: text !== ""
                text: card.subtitle
                color: Theme.dim
                font.family: Theme.fontMono
                font.pixelSize: 11
                elide: Text.ElideRight
            }

            // States, as chips.
            Flow {
                width: parent.width
                spacing: 6
                visible: chipRepeater.count > 0

                Repeater {
                    id: chipRepeater
                    model: {
                        const s = card.status
                        const chips = []
                        if (s.network === "wifi") {
                            chips.push({ icon: "wifi", text: s.signal !== undefined
                                         ? qsTr("Wi-Fi · %1/4").arg(s.signal) : qsTr("Wi-Fi"),
                                         tone: s.signal !== undefined && s.signal <= 1 ? 2 : 1 })
                        } else if (s.network === "cellular") {
                            chips.push({ icon: "cellular", text: qsTr("Mobile data"), tone: 2 })
                        } else if (s.network === "ethernet") {
                            chips.push({ icon: "ethernet", text: qsTr("Ethernet"), tone: 1 })
                        } else if (s.network === "vpn") {
                            chips.push({ icon: "link", text: qsTr("VPN"), tone: 1 })
                        } else if (s.network === "none") {
                            chips.push({ icon: "offline", text: qsTr("Offline"), tone: 0 })
                        }
                        if (s.metered === true && s.network !== "cellular") {
                            chips.push({ icon: "cellular", text: qsTr("Metered"), tone: 2 })
                        }
                        if (s.ringer === "silent") {
                            chips.push({ icon: "bellOff", text: qsTr("Silent"), tone: 2 })
                        } else if (s.ringer === "vibrate") {
                            chips.push({ icon: "vibrate", text: qsTr("Vibrate"), tone: -1 })
                        } else if (s.ringer === "normal") {
                            chips.push({ icon: "bell", text: qsTr("Sound on"), tone: -1 })
                        }
                        if (s.dnd === true) {
                            chips.push({ icon: "moon", text: qsTr("Do not disturb"), tone: 2 })
                        }
                        if (s.powerSave === true) {
                            chips.push({ icon: "leaf", text: qsTr("Power saver"), tone: 2 })
                        }
                        if (s.batteryHealth && s.batteryHealth !== "good"
                                && s.batteryHealth !== "unknown") {
                            const health = { overheat: qsTr("Battery overheating"),
                                             dead: qsTr("Battery dead"),
                                             cold: qsTr("Battery too cold"),
                                             overvoltage: qsTr("Battery overvoltage"),
                                             failure: qsTr("Battery failure") }[s.batteryHealth]
                            if (health) chips.push({ icon: "bolt", text: health, tone: 0 })
                        }
                        return chips
                    }

                    Chip {
                        required property var modelData
                        icon: modelData.icon
                        text: modelData.text
                        tone: modelData.tone
                    }
                }
            }

            Meter {
                width: parent.width
                visible: card.status.storageTotal !== undefined
                label: qsTr("Storage")
                percent: Math.max(0, card.usedPercent(card.status.storageFree, card.status.storageTotal))
                detail: qsTr("%1 free of %2").arg(card.bytes(card.status.storageFree))
                                             .arg(card.bytes(card.status.storageTotal))
            }

            Meter {
                width: parent.width
                visible: card.status.memoryTotal !== undefined
                label: qsTr("Memory")
                percent: Math.max(0, card.usedPercent(card.status.memoryAvailable, card.status.memoryTotal))
                detail: qsTr("%1 free of %2").arg(card.bytes(card.status.memoryAvailable))
                                             .arg(card.bytes(card.status.memoryTotal))
            }

            // Why there is nothing to show, said out loud. A phone with the
            // sharing switched off and a phone that has not answered yet look
            // the same without it.
            Text {
                width: parent.width
                visible: !card.hasReading
                text: card.phone.error ? card.phone.error
                      : !card.connected ? qsTr("Not reachable. Open Maze Connect on the phone, on the same network.")
                      : !card.phone.canStatus ? qsTr("This phone's Maze Connect does not share its status yet — update it to 0.15.0 or later.")
                      : qsTr("Asking the phone…")
                color: card.phone.error ? Theme.warn : Theme.dim
                font.family: Theme.fontSans
                font.pixelSize: 12
                wrapMode: Text.WordWrap
                lineHeight: 1.35
            }
        }
    }

    // ---- Footer: freshness and actions -------------------------------------

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: body.bottom
        height: 1
        color: Theme.hairline
    }

    Item {
        id: footer
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: body.bottom
        anchors.topMargin: 1
        height: 58

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 22
            anchors.right: actions.left
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: {
                if (card.phone.ringNotice) return card.phone.ringNotice
                if (!card.phone.updatedMs) return card.connected ? qsTr("Waiting for a reading") : ""
                const when = qsTr("Updated %1").arg(card.age(card.phone.updatedMs))
                return card.hasReading && card.phone.error
                       ? when + "  ·  " + card.phone.error : when
            }
            color: card.phone.ringNotice || (card.hasReading && card.phone.error)
                   ? Theme.warn : Theme.faint
            font.family: Theme.fontMono
            font.pixelSize: 10
            elide: Text.ElideRight
        }

        Row {
            id: actions
            anchors.right: parent.right
            anchors.rightMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            spacing: 7

            MazeButton {
                text: card.phone.ringing ? qsTr("Stop ringing") : qsTr("Ring")
                primary: true
                enabled: card.connected && card.phone.canRing === true
                onClicked: card.phone.ringing ? Backend.stopRinging(card.phone.deviceId)
                                              : Backend.ringPhone(card.phone.deviceId)
            }
            MazeButton {
                text: qsTr("Send file")
                enabled: card.connected && card.phone.canFiles === true
                onClicked: card.sendFileRequested(card.phone.deviceId)
            }
            MazeButton {
                text: qsTr("Send text")
                enabled: card.connected && card.phone.canOpen === true
                onClicked: card.sendTextRequested(card.phone.deviceId, card.phone.name || "")
            }
            MazeButton {
                text: qsTr("Clipboard")
                enabled: card.connected && card.phone.canOpen === true
                onClicked: Backend.sendClipboardTo(card.phone.deviceId)
            }
        }
    }
}
