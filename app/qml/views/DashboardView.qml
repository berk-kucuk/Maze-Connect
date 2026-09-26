pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Dialogs
import QtQuick.Window
import MazeConnect.App

/**
 * The phones linked to this computer, and what they are doing.
 *
 * This page used to draw *this* machine's CPU, memory and disk — the reading
 * of the computer the user was already sitting in front of, which every
 * desktop shows better in its own panel. What the desktop cannot see on its
 * own is the phone: whether it is charging, how full it is, whether it is on
 * silent and where it went. That is the dashboard now.
 *
 * The computer's own snapshot has not gone anywhere: it is still what a phone
 * sees on *its* dashboard. It is simply not drawn here, where it answered a
 * question nobody asked.
 */
Item {
    id: root

    /// Asks Main.qml to switch to the Devices page (pairing lives there).
    signal openDevices()
    /// Asks Main.qml to open the send-text dialog, which covers the window.
    signal sendTextRequested(string deviceId, string name)

    readonly property var phones: Backend.phones
    readonly property int linkedCount: {
        let n = 0
        for (const p of phones) if (p.connected) n += 1
        return n
    }
    property real now: Date.now()

    // On screen means this page is selected *and* the window is showing. The
    // item stays "visible" while the window sits hidden in the tray, and
    // polling a phone every few seconds for a window nobody can see is waste.
    readonly property bool onScreen: visible && Window.window !== null && Window.window.visible

    // Tell the backend when the page is on screen: phones are read every few
    // seconds while someone is looking and once a minute otherwise.
    onOnScreenChanged: Backend.dashboardVisible = onScreen
    Component.onCompleted: {
        Backend.dashboardVisible = onScreen
        // The command count below reads the file; nothing else loads it
        // until the Commands page is opened.
        Backend.refreshCommands()
    }
    Component.onDestruction: Backend.dashboardVisible = false

    // Keeps "updated 12 s ago" honest. Only while visible — ages nobody is
    // looking at do not need re-rendering.
    Timer {
        interval: 1000
        repeat: true
        running: root.onScreen
        onTriggered: root.now = Date.now()
    }

    FileDialog {
        id: fileDialog
        property string deviceId: ""
        title: qsTr("Choose a file to send")
        onAccepted: Backend.sendFile(deviceId, selectedFile)
    }

    // ---- No phone paired ---------------------------------------------------
    Column {
        anchors.centerIn: parent
        width: Math.min(440, parent.width - 40)
        spacing: 14
        visible: root.phones.length === 0

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 72
            height: 72
            radius: 36
            color: Theme.pill
            border.width: 1
            border.color: Theme.hairline

            MazeIcon {
                anchors.centerIn: parent
                name: "phone"
                color: Theme.dim
                width: 30
                height: 30
            }
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("No phone paired yet")
            color: Theme.text
            font.family: Theme.fontSans
            font.pixelSize: 19
        }

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: qsTr("Pair your phone to see its battery, storage and network here, ring it when it is lost, send files and links both ways, and lock this computer when the phone leaves.")
            color: Theme.dim
            font.family: Theme.fontSans
            font.pixelSize: 13
            lineHeight: 1.45
        }

        MazeButton {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("Pair a phone")
            primary: true
            onClicked: root.openDevices()
        }
    }

    // ---- The phones --------------------------------------------------------
    Flickable {
        anchors.fill: parent
        visible: root.phones.length > 0
        clip: true
        contentHeight: content.height
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: content
            width: parent.width
            spacing: 14

            Item {
                width: parent.width
                height: refreshButton.height

                SectionLabel {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.phones.length === 1
                          ? (root.linkedCount === 1 ? qsTr("Your phone · linked")
                                                    : qsTr("Your phone · not reachable"))
                          : qsTr("%1 of %2 phones linked").arg(root.linkedCount)
                                                            .arg(root.phones.length)
                }

                MazeButton {
                    id: refreshButton
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Refresh")
                    enabled: root.linkedCount > 0
                    onClicked: Backend.refreshPhones()
                }
            }

            Repeater {
                model: root.phones

                PhoneCard {
                    required property var modelData
                    width: content.width
                    height: implicitHeight
                    phone: modelData
                    now: root.now
                    onSendTextRequested: (deviceId, name) => root.sendTextRequested(deviceId, name)
                    onSendFileRequested: (deviceId) => {
                        fileDialog.deviceId = deviceId
                        fileDialog.open()
                    }
                }
            }

            // ---- At a glance -------------------------------------------------
            SectionLabel {
                topPadding: 6
                text: qsTr("This computer")
            }

            Row {
                id: tiles
                width: parent.width
                spacing: 10

                readonly property real tileWidth: (width - spacing * 3) / 4

                Repeater {
                    model: [
                        { label: qsTr("Transfers"),
                          value: Backend.transfers.activeCount > 0
                                 ? qsTr("%1 active").arg(Backend.transfers.activeCount)
                                 : qsTr("Idle") },
                        { label: qsTr("Commands"),
                          value: qsTr("%1 defined").arg(Backend.commands.length) },
                        { label: qsTr("Guard"),
                          value: Backend.guardAvailable ? qsTr("Available") : qsTr("Not installed") },
                        { label: qsTr("Pair by address"),
                          value: Backend.listenAddress !== "" ? Backend.listenAddress : "—" }
                    ]

                    Rectangle {
                        id: tile
                        required property var modelData
                        width: tiles.tileWidth
                        height: 64
                        radius: Theme.radiusPill + 2
                        color: Theme.pill

                        Column {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: 14
                            anchors.rightMargin: 14
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 4

                            SectionLabel {
                                size: 9
                                text: tile.modelData.label
                            }
                            Text {
                                width: parent.width
                                text: tile.modelData.value
                                color: Theme.text
                                font.family: Theme.fontMono
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
            }

            Item { width: 1; height: 4 }
        }
    }
}
