pragma ComponentBehavior: Bound

import QtQuick
import MazeConnect.App

/**
 * The command allow-list, on the machine that runs it.
 *
 * This screen exists because the file is the security boundary. A phone sees
 * labels; the thing that decides what actually happens is `argv` in a file on
 * this computer, and the only sensible place to read that, check it, and try
 * it once is here — not through the phone, and not by guessing from a label
 * that says "Lock screen".
 *
 * So the argv is shown in full, in mono, next to every entry.
 */
Item {
    id: root

    // A one-line labelled text field, used three times below in the add/edit
    // overlay. Declared here rather than as a separate file: nothing outside
    // this view has a use for it.
    component LabeledField: Column {
        id: field
        property string label: ""
        property alias text: input.text
        property string placeholder: ""
        spacing: 5

        Text {
            text: field.label
            color: Theme.dim
            font.family: Theme.fontSans
            font.pixelSize: 11
        }
        Rectangle {
            width: parent.width
            height: 34
            radius: Theme.radiusPill
            color: Theme.pill
            border.width: 1
            border.color: input.activeFocus ? Theme.panelBorder : "transparent"
            Behavior on border.color { ColorAnimation { duration: 120 } }

            TextInput {
                id: input
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                verticalAlignment: TextInput.AlignVCenter
                color: Theme.text
                font.family: Theme.fontMono
                font.pixelSize: 12
                selectByMouse: true
                clip: true
            }
            Text {
                anchors.left: input.left
                anchors.verticalCenter: input.verticalCenter
                text: field.placeholder
                color: Theme.faint
                font.family: Theme.fontMono
                font.pixelSize: 12
                visible: input.text.length === 0
            }
        }
    }

    property string lastOutput: ""
    property string lastId: ""
    property int lastExit: 0

    // Editor overlay state. editOriginalId is empty for a new command and
    // holds the id being replaced for an edit — the one thing addCommand()
    // and updateCommand() need told apart.
    property bool editing: false
    property string editOriginalId: ""
    property string editId: ""
    property string editLabel: ""
    property string editArgv: ""
    property bool editConfirm: false
    property bool editPinned: false
    property string editError: ""

    function openAdd() {
        editOriginalId = ""
        editId = ""
        editLabel = ""
        editArgv = ""
        editConfirm = false
        editPinned = false
        editError = ""
        editing = true
    }

    function openEdit(entry) {
        editOriginalId = entry.id
        editId = entry.id
        editLabel = entry.label
        editArgv = entry.argv
        editConfirm = entry.confirm
        editPinned = entry.pinned
        editError = ""
        editing = true
    }

    function saveEdit() {
        const ok = editOriginalId === ""
            ? Backend.addCommand(editId, editLabel, editArgv, editConfirm, editPinned)
            : Backend.updateCommand(editOriginalId, editId, editLabel, editArgv, editConfirm, editPinned)
        if (ok) {
            editing = false
        } else {
            editError = Backend.commandsError
        }
    }

    // The file is meant to be edited while this is open, so re-read whenever
    // the page is shown rather than only at startup.
    onVisibleChanged: if (visible) Backend.refreshCommands()

    Component.onCompleted: Backend.refreshCommands()

    Connections {
        target: Backend
        function onCommandFinished(id, exitCode, output) {
            root.lastId = id
            root.lastExit = exitCode
            root.lastOutput = output
        }
    }

    Item {
        id: header
        width: parent.width
        height: Math.max(sectionLabel.implicitHeight, headerButtons.implicitHeight)

        SectionLabel {
            id: sectionLabel
            text: qsTr("Allow-list")
            anchors.verticalCenter: parent.verticalCenter
        }

        Row {
            id: headerButtons
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10

            MazeButton {
                text: qsTr("Add command")
                glyph: "+"
                primary: true
                onClicked: root.openAdd()
            }
            MazeButton {
                text: qsTr("Reload")
                onClicked: Backend.refreshCommands()
            }
            MazeButton {
                text: qsTr("Create starter file")
                visible: Backend.commands.length === 0
                onClicked: Backend.createExampleCommands()
            }
        }
    }

    // ---- Nothing defined -------------------------------------------------
    Column {
        anchors.centerIn: parent
        spacing: 10
        width: 460
        visible: Backend.commands.length === 0

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: Backend.commandsError !== "" ? qsTr("Command file refused")
                                               : qsTr("No commands yet")
            color: Backend.commandsError !== "" ? Theme.warn : Theme.text
            font.family: Theme.fontSans
            font.pixelSize: 18
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: Backend.commandsError !== ""
                  ? Backend.commandsError
                  : qsTr("Commands are defined on this computer, in a file only you can read. A paired phone sends the id of an entry — never a command — and this machine looks up what to run.")
            color: Theme.dim
            font.family: Theme.fontSans
            font.pixelSize: 13
            lineHeight: 1.45
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: Backend.commandsPath
            color: Theme.faint
            font.family: Theme.fontMono
            font.pixelSize: 10
            elide: Text.ElideMiddle
        }
    }

    // ---- The list --------------------------------------------------------
    ListView {
        anchors.top: header.bottom
        anchors.topMargin: 14
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: output.top
        anchors.bottomMargin: 12
        clip: true
        spacing: 6
        visible: Backend.commands.length > 0
        model: Backend.commands
        boundsBehavior: Flickable.StopAtBounds

        delegate: Rectangle {
            id: card
            required property var modelData

            width: ListView.view.width
            height: 74
            radius: Theme.radiusPill
            color: hover.hovered ? Theme.pillHover : Theme.pill
            Behavior on color { ColorAnimation { duration: 120 } }

            HoverHandler { id: hover }

            Column {
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.right: actions.left
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                Row {
                    spacing: 8
                    Text {
                        text: card.modelData.label
                        color: Theme.text
                        font.family: Theme.fontSans
                        font.pixelSize: 14
                    }
                    // Marked, because "confirm" is a promise made to the
                    // phone: it asks again before sending the run.
                    Text {
                        visible: card.modelData.confirm
                        text: qsTr("asks first")
                        color: Theme.warn
                        font.family: Theme.fontMono
                        font.pixelSize: 9
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        visible: card.modelData.pinned
                        text: qsTr("on widget")
                        color: Theme.dim
                        font.family: Theme.fontMono
                        font.pixelSize: 9
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                Text {
                    // The argv, verbatim. This is what actually runs; the
                    // label above is only what it is called.
                    text: card.modelData.argv
                    color: Theme.faint
                    font.family: Theme.fontMono
                    font.pixelSize: 10
                    width: parent.width
                    elide: Text.ElideRight
                }
            }

            Row {
                id: actions
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                MazeButton {
                    text: qsTr("Run")
                    onClicked: Backend.runCommand(card.modelData.id)
                }
                MazeButton {
                    text: qsTr("Edit")
                    onClicked: root.openEdit(card.modelData)
                }
                MazeButton {
                    text: qsTr("Remove")
                    destructive: true
                    onClicked: Backend.removeCommand(card.modelData.id)
                }
            }
        }
    }

    // ---- Output of the last local run ------------------------------------
    Rectangle {
        id: output
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: root.lastId === "" ? 0 : Math.min(180, parent.height * 0.4)
        radius: Theme.radiusPill
        color: Theme.pill
        clip: true
        visible: height > 0

        Behavior on height { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

        Row {
            id: outputHeader
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 12
            spacing: 10

            SectionLabel { text: root.lastId }
            Text {
                text: root.lastExit === 0 ? qsTr("exit 0") : qsTr("exit %1").arg(root.lastExit)
                color: root.lastExit === 0 ? Theme.ok : Theme.bad
                font.family: Theme.fontMono
                font.pixelSize: 10
            }
        }

        Flickable {
            anchors.top: outputHeader.bottom
            anchors.topMargin: 6
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 12
            contentHeight: outputText.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Text {
                id: outputText
                width: parent.width
                text: root.lastOutput === "" ? qsTr("(no output)") : root.lastOutput
                color: Theme.dim
                font.family: Theme.fontMono
                font.pixelSize: 10
                wrapMode: Text.Wrap
            }
        }
    }

    // ---- Add/edit overlay --------------------------------------------------
    // Modal, matching PairingOverlay.qml: this is the screen that decides
    // what a paired phone can eventually make run on this machine, and it
    // should not be something edited half-attentively behind another window.
    Item {
        anchors.fill: parent
        visible: root.editing

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            onClicked: {}
        }

        Rectangle {
            anchors.fill: parent
            color: "#000000"
            opacity: 0.86
        }

        Rectangle {
            anchors.centerIn: parent
            width: 440
            height: editContent.implicitHeight + 64
            radius: Theme.radiusPanel
            color: "#0B0B0E"
            border.width: 1
            border.color: Theme.panelBorder

            Column {
                id: editContent
                anchors.centerIn: parent
                width: parent.width - 64
                spacing: 16

                SectionLabel {
                    text: root.editOriginalId === "" ? qsTr("Add command") : qsTr("Edit command")
                }

                LabeledField {
                    width: parent.width
                    label: qsTr("Id — what a phone sends")
                    placeholder: "lock"
                    text: root.editId
                    onTextChanged: root.editId = text
                }

                LabeledField {
                    width: parent.width
                    label: qsTr("Label — what a phone shows")
                    placeholder: qsTr("Lock screen")
                    text: root.editLabel
                    onTextChanged: root.editLabel = text
                }

                LabeledField {
                    width: parent.width
                    label: qsTr("Command — program and arguments, space-separated")
                    placeholder: "loginctl lock-session"
                    text: root.editArgv
                    onTextChanged: root.editArgv = text
                }

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: qsTr("No shell runs this: each word becomes one argument, exactly as typed. There is no quoting, so an argument that itself needs a space cannot be entered here — edit %1 directly for that.").arg(Backend.commandsPath)
                    color: Theme.faint
                    font.family: Theme.fontSans
                    font.pixelSize: 11
                    lineHeight: 1.4
                }

                Row {
                    spacing: 10
                    MazeButton {
                        text: root.editConfirm ? qsTr("Asks first: on") : qsTr("Asks first: off")
                        glyph: root.editConfirm ? "✓" : ""
                        onClicked: root.editConfirm = !root.editConfirm
                    }
                    MazeButton {
                        text: root.editPinned ? qsTr("Phone widget: on") : qsTr("Phone widget: off")
                        glyph: root.editPinned ? "✓" : ""
                        onClicked: root.editPinned = !root.editPinned
                    }
                }

                Text {
                    width: parent.width
                    visible: root.editPinned
                    wrapMode: Text.WordWrap
                    text: qsTr("Shows as a button on the phone's home-screen Commands widget. Only settable here — a phone can run a pinned entry, never pin or unpin one.")
                    color: Theme.faint
                    font.family: Theme.fontSans
                    font.pixelSize: 11
                    lineHeight: 1.4
                }

                Text {
                    width: parent.width
                    visible: root.editError !== ""
                    wrapMode: Text.WordWrap
                    text: root.editError
                    color: Theme.bad
                    font.family: Theme.fontSans
                    font.pixelSize: 12
                }

                Row {
                    anchors.right: parent.right
                    spacing: 10

                    MazeButton {
                        text: qsTr("Cancel")
                        onClicked: root.editing = false
                    }
                    MazeButton {
                        text: qsTr("Save")
                        primary: true
                        enabled: root.editId.trim() !== "" && root.editArgv.trim() !== ""
                        onClicked: root.saveEdit()
                    }
                }
            }
        }
    }
}
