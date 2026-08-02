import QtQuick
import QtQuick.Window
import MazeConnect.App

/**
 * Frameless shell, matching maze-control-center and maze-welcome.
 *
 * The window has no Plasma decoration: the rounded glass panel *is* the
 * window edge, and TitleBar supplies the drag handle and the window
 * buttons. A square decoration drawn around a rounded panel is what the
 * rest of the suite deliberately avoids.
 */
Window {
    id: root

    width: 1000
    height: 680
    minimumWidth: 860
    minimumHeight: 560
    // Shown from C++ once it is known whether this is a normal launch or the
    // autostart one (`--tray`). Starting visible and hiding afterwards let the
    // window flash on screen at every login, which is the thing autostart in
    // the tray is meant to avoid.
    visible: false
    title: qsTr("Maze Connect")

    flags: Qt.Window | Qt.FramelessWindowHint
    color: "transparent"

    // Blur behind the panel is applied from C++ (WindowEffects), since it is
    // a property of the window rather than of the scene.

    property int currentView: 0

    // Escape closes, as it does across the suite.
    Shortcut {
        sequence: "Escape"
        onActivated: root.close()
    }

    // Direct access to each page, so the sidebar is not the only way there.
    Shortcut { sequence: "Ctrl+1"; onActivated: root.currentView = 0 }
    Shortcut { sequence: "Ctrl+2"; onActivated: root.currentView = 1 }
    Shortcut { sequence: "Ctrl+3"; onActivated: root.currentView = 2 }
    Shortcut { sequence: "Ctrl+4"; onActivated: root.currentView = 3 }
    Shortcut { sequence: "Ctrl+5"; onActivated: root.currentView = 4 }
    Shortcut {
        sequence: "Ctrl+6"
        onActivated: { root.currentView = 5; Backend.activity.markAllSeen() }
    }
    Shortcut { sequence: "Ctrl+7"; onActivated: root.currentView = 6 }

    GlassPanel {
        id: glass
        anchors.fill: parent
        anchors.margins: 1

        // ---- Sidebar -----------------------------------------------------
        Rectangle {
            id: sidebar
            width: Theme.sidebarWidth
            height: parent.height
            color: Theme.sidebarBg
            topLeftRadius: Theme.radiusPanel
            bottomLeftRadius: Theme.radiusPanel

            Column {
                anchors.fill: parent
                anchors.margins: 16
                anchors.topMargin: 26
                spacing: 6

                Image {
                    source: Theme.logoPath
                    width: 56
                    height: 56
                    fillMode: Image.PreserveAspectFit
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: status === Image.Ready
                }

                Item { width: 1; height: 10 }

                Text {
                    text: qsTr("Maze Connect")
                    color: Theme.text
                    font.family: Theme.fontSans
                    font.pixelSize: 14
                    font.weight: Font.Bold
                    anchors.horizontalCenter: parent.horizontalCenter
                }

                Text {
                    text: Backend.connectedCount > 0
                          ? qsTr("%1 linked").arg(Backend.connectedCount)
                          : qsTr("Nothing linked")
                    color: Backend.connectedCount > 0 ? Theme.ok : Theme.faint
                    font.family: Theme.fontMono
                    font.pixelSize: 10
                    anchors.horizontalCenter: parent.horizontalCenter
                }

                Item { width: 1; height: 18 }

                NavButton {
                    icon: "devices"
                    label: qsTr("Devices")
                    current: root.currentView === 0
                    badge: Backend.pairedCount
                    onClicked: root.currentView = 0
                }
                NavButton {
                    icon: "transfers"
                    label: qsTr("Transfers")
                    current: root.currentView === 1
                    badge: Backend.transfers.activeCount
                    onClicked: root.currentView = 1
                }
                NavButton {
                    icon: "dashboard"
                    label: qsTr("Dashboard")
                    current: root.currentView === 2
                    onClicked: root.currentView = 2
                }
                NavButton {
                    icon: "commands"
                    label: qsTr("Commands")
                    current: root.currentView === 3
                    badge: Backend.commands.length
                    onClicked: root.currentView = 3
                }
                // No Maze AI page here on purpose: Maze AI has its own
                // desktop application, and duplicating it would be a worse
                // copy of something already installed. What this app adds is
                // reaching that Ollama *from the phone* — the bridge stays,
                // only the window is gone.
                NavButton {
                    icon: "guard"
                    label: qsTr("Guard")
                    current: root.currentView === 4
                    onClicked: root.currentView = 4
                }
                NavButton {
                    icon: "activity"
                    label: qsTr("Activity")
                    current: root.currentView === 5
                    badge: Backend.activity.unseen
                    onClicked: {
                        root.currentView = 5
                        Backend.activity.markAllSeen()
                    }
                }
                NavButton {
                    icon: "settings"
                    label: qsTr("Settings")
                    current: root.currentView === 6
                    onClicked: root.currentView = 6
                }
            }

            // This machine's own fingerprint, always visible: it is what a
            // user reads out when someone asks them to confirm identity.
            Column {
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.margins: 16
                spacing: 4

                SectionLabel { text: qsTr("This device") }

                Text {
                    text: Backend.shortFingerprint(Backend.fingerprint)
                    color: Theme.dim
                    font.family: Theme.fontMono
                    font.pixelSize: 10
                    width: parent.width
                    elide: Text.ElideRight
                }
            }
        }

        // ---- Content -----------------------------------------------------
        Item {
            anchors.left: sidebar.right
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.leftMargin: Theme.gutter
            anchors.rightMargin: Theme.gutter
            anchors.topMargin: 14
            anchors.bottomMargin: 18

            TitleBar {
                id: titleBar
                width: parent.width
                window: root
                title: [qsTr("Devices"), qsTr("Transfers"), qsTr("Dashboard"),
                        qsTr("Commands"), qsTr("Guard"), qsTr("Activity"),
                        qsTr("Settings")][root.currentView]
                subtitle: Backend.statusMessage
            }

            Item {
                anchors.top: titleBar.bottom
                anchors.topMargin: 8
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom

                DevicesView { anchors.fill: parent; visible: root.currentView === 0 }
                TransfersView { anchors.fill: parent; visible: root.currentView === 1 }
                DashboardView { anchors.fill: parent; visible: root.currentView === 2 }
                CommandsView { anchors.fill: parent; visible: root.currentView === 3 }
                GuardView { anchors.fill: parent; visible: root.currentView === 4 }
                ActivityView { anchors.fill: parent; visible: root.currentView === 5 }
                SettingsView { anchors.fill: parent; visible: root.currentView === 6 }
            }
        }
    }

    // Resize grip, since there is no decoration to drag.
    Item {
        width: 18
        height: 18
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        HoverHandler { cursorShape: Qt.SizeFDiagCursor }
        DragHandler {
            target: null
            onActiveChanged: if (active) root.startSystemResize(Qt.BottomEdge | Qt.RightEdge)
        }
    }

    PairingOverlay {}
    FileOfferOverlay { id: fileOffer }

    Connections {
        target: Backend
        function onFileOffered(deviceId, transferId, filename, sizeBytes) {
            fileOffer.show(deviceId, transferId, filename, sizeBytes)
        }
    }
}
