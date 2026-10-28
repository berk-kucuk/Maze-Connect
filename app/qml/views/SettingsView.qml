import QtQuick
import MazeConnect.App

Flickable {
    id: root

    contentHeight: column.implicitHeight
    clip: true
    boundsBehavior: Flickable.StopAtBounds

    // KDE Connect may have been installed in a terminal meanwhile.
    onVisibleChanged: if (visible) Backend.refreshKdeConnect()

    Column {
        id: column
        width: root.width
        spacing: 22

        // ---- Identity ----------------------------------------------------
        SectionLabel { text: qsTr("This device") }

        Column {
            width: parent.width
            spacing: 6

            Pill {
                label: qsTr("Name")
                value: Backend.deviceName
            }

            Pill {
                // Full grouped fingerprint: this is what a user reads out
                // when someone asks them to confirm which machine they are.
                label: qsTr("Fingerprint")
                value: Backend.shortFingerprint(Backend.fingerprint)
            }

            Pill {
                label: qsTr("Address")
                value: Backend.listenAddress
                state_: Backend.listenAddress === "" ? 0 : 1
                showDot: true
            }

            Pill {
                label: qsTr("Received files")
                value: Backend.inboxPath
            }
        }

        // ---- Startup -----------------------------------------------------
        SectionLabel { text: qsTr("Startup") }

        Row {
            width: parent.width
            spacing: 12

            Text {
                width: parent.width - toggle.width - 12
                wrapMode: Text.WordWrap
                anchors.verticalCenter: parent.verticalCenter
                text: Backend.autostartEnabled
                      ? qsTr("Maze Connect starts with your session and waits in the tray. Closing the window leaves your phone connected — only Quit, from the tray menu, ends the link.")
                      : qsTr("Maze Connect only runs when you open it. Your phone cannot reach this computer while it is closed.")
                color: Theme.dim
                font.family: Theme.fontSans
                font.pixelSize: 12
                lineHeight: 1.45
            }

            MazeButton {
                id: toggle
                anchors.verticalCenter: parent.verticalCenter
                text: Backend.autostartEnabled ? qsTr("On") : qsTr("Off")
                glyph: Backend.autostartEnabled ? "\u25cf" : "\u25cb"
                primary: Backend.autostartEnabled
                onClicked: Backend.autostartEnabled = !Backend.autostartEnabled
            }
        }

        // ---- KDE Connect compatibility ----------------------------------
        // The system's own kdeconnectd, switched from here. Not everyone wants
        // to sideload an APK, and an iPhone cannot run the Maze Connect app at
        // all — the KDE Connect app is in both stores.
        SectionLabel { text: qsTr("KDE Connect compatibility") }

        Row {
            id: kdeRow
            width: parent.width
            spacing: 12

            readonly property var st: Backend.kdeConnect

            Text {
                width: parent.width - kdeToggle.width - 12
                wrapMode: Text.WordWrap
                anchors.verticalCenter: parent.verticalCenter
                text: !kdeRow.st.installed
                      ? qsTr("Lets the KDE Connect app — Android, iPhone, or another computer — work with this computer, without the Maze Connect app. KDE Connect is not installed yet; the command is below.")
                      : kdeRow.st.enabled
                        ? qsTr("Devices with the KDE Connect app can pair with this computer too; they appear under Devices. KDE Connect has its own pairing and its own permissions — Maze Connect's six-digit check, command allow-list and approvals do not apply to it. Set what it may do in KDE Connect's settings.")
                        : qsTr("Lets the KDE Connect app — Android, iPhone, or another computer — work with this computer, without the Maze Connect app. Off: KDE Connect is not running and does not start with your session.")
                color: Theme.dim
                font.family: Theme.fontSans
                font.pixelSize: 12
                lineHeight: 1.45
            }

            MazeButton {
                id: kdeToggle
                anchors.verticalCenter: parent.verticalCenter
                enabled: kdeRow.st.installed === true
                text: kdeRow.st.enabled ? qsTr("On") : qsTr("Off")
                glyph: kdeRow.st.enabled ? "●" : "○"
                primary: kdeRow.st.enabled === true
                onClicked: Backend.setKdeConnectEnabled(!kdeRow.st.enabled)
            }
        }

        Column {
            id: kdeState
            width: parent.width
            spacing: 6
            visible: kdeRow.st.installed === true && kdeRow.st.enabled === true

            Pill {
                label: qsTr("KDE Connect")
                value: kdeRow.st.running ? qsTr("Running") : qsTr("Starting…")
                showDot: true
                state_: kdeRow.st.running ? 1 : 0
                trailing: MazeButton {
                    text: qsTr("Settings")
                    onClicked: Backend.openKdeConnectSettings()
                }
            }
        }

        // Installing is root's part: the command to paste. The firewall needs
        // nothing here — the package opened KDE Connect's ports at install.
        Rectangle {
            id: kdeSteps
            readonly property var st: Backend.kdeConnect
            readonly property string commands: st.installed ? "" : "sudo pacman -S kdeconnect"
            visible: commands !== ""
            width: parent.width
            height: kdeStepsColumn.implicitHeight + 24
            radius: Theme.radiusPill
            color: "transparent"
            border.width: 1
            border.color: Theme.hairline

            Column {
                id: kdeStepsColumn
                anchors.left: parent.left
                anchors.right: kdeCopy.left
                anchors.leftMargin: 14
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: qsTr("Install it, in a terminal:")
                    color: Theme.faint
                    font.family: Theme.fontSans
                    font.pixelSize: 11
                }
                Text {
                    width: parent.width
                    text: kdeSteps.commands
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.family: Theme.fontMono
                    font.pixelSize: 11
                    lineHeight: 1.5
                    wrapMode: Text.WrapAnywhere
                }
            }

            MazeButton {
                id: kdeCopy
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Copy")
                onClicked: Backend.copyText(kdeSteps.commands)
            }
        }

        // ---- Control from the phone -------------------------------------
        SectionLabel { text: qsTr("Mouse and keyboard from your phone") }

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("A phone allowed here can move the pointer and type — anything a keyboard can do, including opening a terminal. So it is off for every phone until you turn it on, and the first time the desktop itself asks you to confirm on this screen. A banner stays up while a phone is in control, with a Stop button. Presenting (slide keys only) needs no permission here.")
            color: Theme.dim
            font.family: Theme.fontSans
            font.pixelSize: 12
            lineHeight: 1.45
        }

        Column {
            width: parent.width
            spacing: 6

            Repeater {
                model: Backend.phones

                Pill {
                    id: controlPill
                    required property var modelData
                    label: modelData.name
                    value: modelData.controlAllowed ? qsTr("Allowed") : qsTr("Not allowed")
                    showDot: true
                    state_: modelData.controlAllowed ? 2 : -1
                    trailing: MazeButton {
                        text: controlPill.modelData.controlAllowed ? qsTr("Revoke") : qsTr("Allow")
                        destructive: controlPill.modelData.controlAllowed
                        onClicked: Backend.setRemoteControlAllowed(controlPill.modelData.deviceId,
                                                                   !controlPill.modelData.controlAllowed)
                    }
                }
            }
        }

        // ---- Unlock and approve with the phone ---------------------------
        SectionLabel { text: qsTr("Unlock and approve with your phone") }

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("A phone allowed here can unlock this screen — with your fingerprint, every time, on the phone. Set it up on the phone first (Settings › Approvals), check that the key code matches the one shown here, then allow it. If the phone ever sends a different key, it is switched off here until you allow it again.")
            color: Theme.dim
            font.family: Theme.fontSans
            font.pixelSize: 12
            lineHeight: 1.45
        }

        Column {
            width: parent.width
            spacing: 6

            Repeater {
                model: Backend.phones

                Pill {
                    id: approvalPill
                    required property var modelData
                    readonly property bool hasKey: modelData.approvalKeyCode !== ""
                    label: modelData.name
                    detail: hasKey ? qsTr("Key  %1").arg(modelData.approvalKeyCode)
                                   : qsTr("Not set up on the phone yet")
                    value: !hasKey ? qsTr("—")
                         : modelData.approvalAllowed ? qsTr("Allowed") : qsTr("Not allowed")
                    showDot: true
                    state_: modelData.approvalAllowed ? 1 : -1
                    trailing: MazeButton {
                        visible: approvalPill.hasKey
                        text: approvalPill.modelData.approvalAllowed ? qsTr("Revoke") : qsTr("Allow")
                        primary: !approvalPill.modelData.approvalAllowed
                        destructive: approvalPill.modelData.approvalAllowed
                        onClicked: Backend.setApprovalAllowed(approvalPill.modelData.deviceId,
                                                              !approvalPill.modelData.approvalAllowed)
                    }
                }
            }
        }

        // sudo / polkit: root's to set up, so this shows where it stands and
        // the exact commands for what is left — nothing here changes it.
        SectionLabel { text: qsTr("sudo and administrator prompts") }

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("sudo and polkit (Partition Manager, Discover, run0…) can ask the phone first. The prompt shows a two-digit code; type it on the phone and touch the sensor. No phone, no answer, or an SSH session — the password is asked as before.")
            color: Theme.dim
            font.family: Theme.fontSans
            font.pixelSize: 12
            lineHeight: 1.45
        }

        Column {
            id: adminState
            width: parent.width
            spacing: 6

            readonly property var st: Backend.adminApproval
            readonly property var enrolled: st.enrolled || []

            Pill {
                label: qsTr("PAM module")
                value: adminState.st.moduleInstalled ? qsTr("Installed") : qsTr("Not installed")
                showDot: true
                state_: adminState.st.moduleInstalled ? 1 : 0
            }
            Pill {
                label: qsTr("Phones enrolled by root")
                detail: adminState.enrolled.join("   ")
                value: adminState.enrolled.length === 0 ? qsTr("None")
                     : adminState.enrolled.length === 1 ? qsTr("1 phone")
                     : qsTr("%1 phones").arg(adminState.enrolled.length)
                showDot: true
                state_: adminState.enrolled.length > 0 ? 1 : -1
            }
            Pill {
                label: qsTr("sudo")
                value: adminState.st.sudoEnabled ? qsTr("Asks the phone") : qsTr("Password only")
                showDot: true
                state_: adminState.st.sudoEnabled ? 1 : -1
            }
            Pill {
                label: qsTr("polkit / run0")
                value: adminState.st.polkitEnabled ? qsTr("Asks the phone") : qsTr("Password only")
                showDot: true
                state_: adminState.st.polkitEnabled ? 1 : -1
            }
            Pill {
                visible: (adminState.st.relayError || "") !== ""
                label: qsTr("Approval relay")
                detail: adminState.st.relayError || ""
                value: qsTr("Not running")
                showDot: true
                state_: 0
                trailing: MazeButton {
                    text: qsTr("Retry")
                    onClicked: Backend.restartApprovalRelay()
                }
            }
        }

        // What is left to do, as commands to paste — only the missing steps.
        Rectangle {
            id: steps
            readonly property var st: Backend.adminApproval
            readonly property string commands: {
                let lines = []
                if ((st.enrolled || []).length === 0)
                    lines.push("sudo maze-connect-approvers enroll")
                if (!st.sudoEnabled)
                    lines.push("sudo maze-connect-approvers pam enable sudo")
                if (!st.polkitEnabled)
                    lines.push("sudo maze-connect-approvers pam enable polkit")
                return lines.join("\n")
            }
            visible: st.moduleInstalled === true && commands !== ""
            width: parent.width
            height: stepsColumn.implicitHeight + 24
            radius: Theme.radiusPill
            color: "transparent"
            border.width: 1
            border.color: Theme.hairline

            Column {
                id: stepsColumn
                anchors.left: parent.left
                anchors.right: copyButton.left
                anchors.leftMargin: 14
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                Text {
                    text: qsTr("Still to do, in a terminal:")
                    color: Theme.faint
                    font.family: Theme.fontSans
                    font.pixelSize: 11
                }
                Text {
                    width: parent.width
                    text: steps.commands
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.family: Theme.fontMono
                    font.pixelSize: 11
                    lineHeight: 1.5
                    wrapMode: Text.WrapAnywhere
                }
            }

            MazeButton {
                id: copyButton
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Copy")
                onClicked: Backend.copyText(steps.commands)
            }
        }

        // ---- Clipboard sync ------------------------------------------------
        SectionLabel { text: qsTr("Clipboard sync") }

        Row {
            width: parent.width
            spacing: 12

            Text {
                width: parent.width - clipToggle.width - 12
                wrapMode: Text.WordWrap
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("What you copy here goes to linked phones that also have sync on, and what they copy lands here. Passwords copied from a password manager are never sent. A phone can only send its clipboard while Maze Connect is open on it — Android allows nothing else.")
                color: Theme.dim
                font.family: Theme.fontSans
                font.pixelSize: 12
                lineHeight: 1.45
            }

            MazeButton {
                id: clipToggle
                anchors.verticalCenter: parent.verticalCenter
                text: Backend.clipboardSync ? qsTr("On") : qsTr("Off")
                glyph: Backend.clipboardSync ? "\u25cf" : "\u25cb"
                primary: Backend.clipboardSync
                onClicked: Backend.clipboardSync = !Backend.clipboardSync
            }
        }

        // ---- Shared folder ---------------------------------------------------
        SectionLabel { text: qsTr("Shared folder") }

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("Linked phones can browse and download from this one folder — nothing outside it, no hidden files, no links out of it. Put in it what you want on your phone.")
            color: Theme.dim
            font.family: Theme.fontSans
            font.pixelSize: 12
            lineHeight: 1.45
        }

        Pill {
            label: qsTr("Folder")
            value: Backend.sharedFolderPath
            interactive: true
            onClicked: Backend.openSharedFolder()
        }

        // ---- Lock when the phone leaves ------------------------------------
        SectionLabel { text: qsTr("Lock when your phone leaves") }

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("Locks this computer's screen when the chosen phone drops off the network and does not come back within the delay — walk away with your phone and the desk locks behind you. Coming back never unlocks anything; that takes your fingerprint (above). If this computer's own network drops, it locks too.")
            color: Theme.dim
            font.family: Theme.fontSans
            font.pixelSize: 12
            lineHeight: 1.45
        }

        Flow {
            width: parent.width
            spacing: 8

            MazeButton {
                text: qsTr("Off")
                primary: Backend.proximityLockDevice === ""
                onClicked: Backend.proximityLockDevice = ""
            }
            Repeater {
                model: Backend.phones
                MazeButton {
                    required property var modelData
                    text: modelData.name
                    primary: Backend.proximityLockDevice === modelData.deviceId
                    onClicked: Backend.proximityLockDevice = modelData.deviceId
                }
            }
        }

        Row {
            spacing: 8
            visible: Backend.proximityLockDevice !== ""

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("After")
                color: Theme.dim
                font.family: Theme.fontSans
                font.pixelSize: 12
            }
            Repeater {
                model: [15, 30, 60, 300]
                MazeButton {
                    required property int modelData
                    text: modelData < 60 ? qsTr("%1 s").arg(modelData) : qsTr("%1 min").arg(modelData / 60)
                    primary: Backend.proximityLockDelay === modelData
                    onClicked: Backend.proximityLockDelay = modelData
                }
            }
        }

        // ---- Privacy -----------------------------------------------------
        SectionLabel { text: qsTr("Privacy") }

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            // The per-capability switches are gone: this application exists so
            // the phone can drive this computer, and a link that arrives with
            // everything switched off is a link that appears broken. Say where
            // the real boundary is instead — pairing, and the allow-list.
            text: qsTr("A paired phone can use every capability. The boundary is pairing itself: nothing reaches this computer until you have confirmed the six digits on both screens. Commands are the exception in the other direction — only the entries in your allow-list can ever run, and nothing the phone sends is passed to a shell.")
            color: Theme.dim
            font.family: Theme.fontSans
            font.pixelSize: 12
            lineHeight: 1.45
        }

        // ---- Security ----------------------------------------------------
        SectionLabel { text: qsTr("Security") }

        Column {
            width: parent.width
            spacing: 6

            Pill {
                label: qsTr("Transport")
                value: qsTr("TLS 1.3, mutual")
                state_: 1
                showDot: true
            }
            Pill {
                label: qsTr("Peer authentication")
                value: qsTr("Pinned public key")
                state_: 1
                showDot: true
            }
            Pill {
                label: qsTr("Paired devices")
                value: Backend.pairedCount
            }
        }

        Item { width: 1; height: 10 }
    }
}
