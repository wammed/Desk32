import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Desk32.Core

Rectangle {
    id: wsRoot

    implicitWidth: 540
    implicitHeight: 820
    color: "#121519"
    border.color: "#262b32"
    border.width: 1
    radius: 6
    clip: true

    signal closeRequested()

    property var workspacesList: []
    property var filesList: []
    property var savesList: []
    property string selectedFileRel: ""
    property var selectedFileDetails: ({})
    property string statusNotice: ""
    property string statusSeverity: "info" // "info" | "success" | "warning" | "error"
    property string importFeedbackTitle: ""
    property string importFeedbackSummary: ""
    property var importFeedbackItems: []
    property string diffSummaryText: ""

    function setNotice(message, severity) {
        statusNotice = message || ""
        statusSeverity = severity || "info"
    }

    function noticeBorderColor() {
        if (statusSeverity === "error") return "#FF5252"
        if (statusSeverity === "warning") return "#FFB300"
        if (statusSeverity === "success") return "#00E676"
        return "#00E5FF"
    }

    function noticeBgColor() {
        if (statusSeverity === "error") return "#FF52521C"
        if (statusSeverity === "warning") return "#FFB3001A"
        if (statusSeverity === "success") return "#00E67616"
        return "#00E5FF14"
    }

    function noticeTextColor() {
        if (statusSeverity === "error") return "#FFCDD2"
        if (statusSeverity === "warning") return "#FFE082"
        if (statusSeverity === "success") return "#B9F6CA"
        return "#B2EBF2"
    }

    function handleImportFileResult(res, openDialogOnFailure) {
        var shouldPopup = (openDialogOnFailure === undefined) ? true : openDialogOnFailure
        var st = res ? res.status : 3
        var msg = (res && res.message) ? res.message : qsTr("Import failed")
        if (st === 0) {
            setNotice(msg, "success")
            importFeedbackItems = []
            return
        }
        if (st === 1) {
            setNotice(msg, "warning")
            importFeedbackItems = []
            return
        }
        setNotice(msg, "error")
        var statusLbl = (res && res.statusText) ? res.statusText : (st === 2 ? "Conflict" : "Failed")
        importFeedbackTitle = (st === 2)
            ? qsTr("Import Conflict — File Not Overwritten")
            : qsTr("Import Failed")
        importFeedbackSummary = msg
        importFeedbackItems = [{
            statusText: statusLbl,
            sourcePath: (res && res.sourcePath) ? res.sourcePath : "",
            message: msg
        }]
        if (shouldPopup) {
            importFeedbackDialog.open()
        }
    }

    function handleImportFolderResults(results, openDialogOnFailure) {
        var shouldPopup = (openDialogOnFailure === undefined) ? true : openDialogOnFailure
        var arr = results || []
        if (arr.length === 0) {
            var errMsg = WorkspaceManager.lastError()
            if (errMsg && errMsg.length > 0) {
                setNotice(errMsg, "error")
                importFeedbackTitle = qsTr("Folder Import Failed")
                importFeedbackSummary = errMsg
                importFeedbackItems = [{ statusText: "Failed", sourcePath: "", message: errMsg }]
                if (shouldPopup) importFeedbackDialog.open()
            } else {
                setNotice(qsTr("No supported audio files found in folder (symbolic links are excluded)."), "warning")
                importFeedbackItems = []
            }
            return
        }
        var imported = 0
        var skipped = 0
        var conflicts = 0
        var failed = 0
        var issues = []
        for (var i = 0; i < arr.length; ++i) {
            var r = arr[i]
            if (r.status === 0) imported++
            else if (r.status === 1) skipped++
            else if (r.status === 2) {
                conflicts++
                issues.push(r)
            } else {
                failed++
                issues.push(r)
            }
        }
        var summary = qsTr("Folder import: %1 imported, %2 skipped, %3 conflict(s), %4 failed")
                      .arg(imported).arg(skipped).arg(conflicts).arg(failed)
        if (failed > 0 || conflicts > 0) {
            setNotice(summary + " — " + (issues[0].message || ""), "error")
            importFeedbackTitle = qsTr("Folder Import Completed with Failures / Conflicts")
            importFeedbackSummary = summary
            importFeedbackItems = issues
            if (shouldPopup) {
                importFeedbackDialog.open()
            }
        } else if (skipped > 0 && imported === 0) {
            setNotice(summary, "warning")
            importFeedbackItems = []
        } else {
            setNotice(summary, "success")
            importFeedbackItems = []
        }
    }

    function stripFileScheme(urlStr) {
        var s = urlStr ? urlStr.toString() : ""
        if (s.indexOf("file://") === 0) return decodeURIComponent(s.slice(7))
        return s
    }

    function refreshAll() {
        workspacesList = WorkspaceManager.getRegisteredWorkspaces()
        filesList = WorkspaceManager.hasOpenWorkspace ? WorkspaceManager.getWorkspaceFiles("") : []
        savesList = WorkspaceManager.hasOpenWorkspace ? WorkspaceManager.getSaves() : []
        if (selectedFileRel.length > 0 && WorkspaceManager.hasOpenWorkspace) {
            selectedFileDetails = WorkspaceManager.getFileDetails(selectedFileRel)
            var meta = selectedFileDetails.metadata || ({})
            editTitleField.text = meta.title || ""
            editArtistField.text = meta.artist || ""
            editAlbumField.text = meta.album || ""
            editTrackNumField.text = meta.trackNumber > 0 ? String(meta.trackNumber) : ""
            editDateField.text = meta.dateOrYear || ""
            editGenreField.text = meta.genre || ""
        } else {
            selectedFileDetails = ({})
            editTitleField.text = ""
            editArtistField.text = ""
            editAlbumField.text = ""
            editTrackNumField.text = ""
            editDateField.text = ""
            editGenreField.text = ""
        }
    }

    Component.onCompleted: refreshAll()

    Connections {
        target: WorkspaceManager
        function onWorkspaceStateChanged() { wsRoot.refreshAll() }
        function onWorkspaceContentsChanged() { wsRoot.refreshAll() }
        function onExternalModificationDetected(path, detail) {
            wsRoot.setNotice(qsTr("External modification: %1 (%2)").arg(path).arg(detail), "warning")
            wsRoot.refreshAll()
        }
        function onWorkspaceErrorOccurred(op, msg) {
            wsRoot.setNotice(op + ": " + msg, "error")
        }
    }

    Connections {
        target: EngineController
        function onDeckWorkspaceImportFinished(deckId, ok, statusText, message) {
            if (ok) {
                wsRoot.setNotice(message, statusText === "Skipped" ? "warning" : "success")
            } else {
                wsRoot.setNotice(message, statusText === "SelfCopy" ? "warning" : "error")
            }
            wsRoot.refreshAll()
        }
    }

    FolderDialog {
        id: registerWsFolderDialog
        title: qsTr("Register & Open Dedicated Workspace Directory")
        onAccepted: {
            var p = wsRoot.stripFileScheme(selectedFolder)
            if (WorkspaceManager.registerAndSelectWorkspaceAtStartup(p)) {
                wsRoot.setNotice(qsTr("Opened workspace: %1").arg(p), "success")
            } else {
                wsRoot.setNotice(WorkspaceManager.lastError(), "error")
            }
            wsRoot.refreshAll()
        }
    }

    FileDialog {
        id: importFileDialog
        title: qsTr("Import Audio File into Workspace")
        nameFilters: [qsTr("Audio (*.flac *.wav *.mp3 *.ogg *.opus *.m4a *.aac)"), qsTr("All Files (*)")]
        onAccepted: {
            var p = wsRoot.stripFileScheme(selectedFile)
            var res = WorkspaceManager.importFileFromUi(p, "")
            wsRoot.handleImportFileResult(res, true)
            wsRoot.refreshAll()
        }
    }

    FolderDialog {
        id: importFolderDialog
        title: qsTr("Import Folder of Audio Files into Workspace")
        onAccepted: {
            var p = wsRoot.stripFileScheme(selectedFolder)
            var results = WorkspaceManager.importFolderFromUi(p, "")
            wsRoot.handleImportFolderResults(results, true)
            wsRoot.refreshAll()
        }
    }

    // Import failure / conflict detail dialog
    Dialog {
        id: importFeedbackDialog
        title: wsRoot.importFeedbackTitle.length > 0 ? wsRoot.importFeedbackTitle : qsTr("Import Failed")
        modal: true
        anchors.centerIn: Overlay.overlay
        width: 480
        standardButtons: Dialog.Ok

        contentItem: ColumnLayout {
            spacing: 8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: 11
                font.bold: true
                color: "#FF8A80"
                text: wsRoot.importFeedbackSummary
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(240, Math.max(80, wsRoot.importFeedbackItems.length * 64))
                visible: wsRoot.importFeedbackItems.length > 0
                clip: true
                contentWidth: availableWidth

                ColumnLayout {
                    width: 440
                    spacing: 6

                    Repeater {
                        model: wsRoot.importFeedbackItems
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: itemCol.implicitHeight + 12
                            radius: 4
                            color: "#1a1214"
                            border.color: modelData.statusText === "Conflict" ? "#FFB300" : "#FF5252"

                            ColumnLayout {
                                id: itemCol
                                anchors.fill: parent
                                anchors.margins: 6
                                spacing: 2

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6
                                    Rectangle {
                                        implicitWidth: stBadgeLbl.implicitWidth + 8
                                        height: 16
                                        radius: 3
                                        color: modelData.statusText === "Conflict" ? "#FFB30022" : "#FF525222"
                                        border.color: modelData.statusText === "Conflict" ? "#FFB300" : "#FF5252"
                                        Label {
                                            id: stBadgeLbl
                                            anchors.centerIn: parent
                                            text: (modelData.statusText || "FAILED").toUpperCase()
                                            font.pixelSize: 8
                                            font.bold: true
                                            font.family: "Monospace"
                                            color: modelData.statusText === "Conflict" ? "#FFB300" : "#FF5252"
                                        }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        visible: !!(modelData.sourcePath && modelData.sourcePath.length > 0)
                                        text: modelData.sourcePath || ""
                                        font.pixelSize: 9
                                        font.family: "Monospace"
                                        color: "#B0BEC5"
                                        elide: Text.ElideMiddle
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.message || ""
                                    font.pixelSize: 10
                                    color: "#FFCDD2"
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Two-step confirmation dialog to prevent accidental workspace unregistration
    Dialog {
        id: unregisterConfirmDialog
        title: qsTr("Unregister Workspace?")
        modal: true
        anchors.centerIn: Overlay.overlay
        width: 420

        property string targetPath: ""
        property bool isCurrentlyOpen: false

        contentItem: ColumnLayout {
            spacing: 8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: 11
                color: "#E0E6ED"
                text: qsTr("Remove this folder from Desk32's Registered Workspaces list? Audio files and the .desk32 folder on disk will NOT be deleted.")
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: targetPathLabel.implicitHeight + 12
                radius: 4
                color: "#181c22"
                border.color: "#2c3440"
                Label {
                    id: targetPathLabel
                    anchors.fill: parent
                    anchors.margins: 6
                    wrapMode: Text.WrapAnywhere
                    font.pixelSize: 10
                    font.family: "Monospace"
                    color: "#00E5FF"
                    text: unregisterConfirmDialog.targetPath
                }
            }
            Label {
                visible: unregisterConfirmDialog.isCurrentlyOpen
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: 10
                font.bold: true
                color: "#FFB300"
                text: qsTr("Warning: This workspace is currently open. Unregistering will stop playback from it and switch this session to Read-Only mode.")
            }
        }

        footer: RowLayout {
            spacing: 8
            Item { Layout.fillWidth: true }
            Button {
                text: qsTr("Cancel")
                font.pixelSize: 10
                onClicked: unregisterConfirmDialog.reject()
            }
            Button {
                text: qsTr("Confirm Unregister")
                font.pixelSize: 10
                font.bold: true
                palette.button: "#B71C1C"
                palette.buttonText: "#FFFFFF"
                onClicked: unregisterConfirmDialog.accept()
            }
        }

        onAccepted: {
            if (WorkspaceManager.unregisterWorkspaceFromUi(targetPath)) {
                wsRoot.setNotice(qsTr("Unregistered workspace: %1").arg(targetPath), "success")
            } else {
                wsRoot.setNotice(WorkspaceManager.lastError(), "error")
            }
            wsRoot.refreshAll()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        // Header
        RowLayout {
            Layout.fillWidth: true
            Rectangle { width: 8; height: 8; radius: 4; color: WorkspaceManager.hasOpenWorkspace ? "#00E676" : "#FFB300" }
            Label {
                text: qsTr("DEDICATED WORKSPACE // v3.8.1")
                font.bold: true; font.pixelSize: 12; font.family: "Monospace"
                color: WorkspaceManager.hasOpenWorkspace ? "#00E676" : "#FFB300"
            }
            Item { Layout.fillWidth: true }
            Button {
                text: qsTr("Refresh")
                font.pixelSize: 10
                Layout.preferredHeight: 24
                onClicked: wsRoot.refreshAll()
            }
            Button {
                text: "✕"
                font.bold: true; font.pixelSize: 11
                Layout.preferredWidth: 26; Layout.preferredHeight: 24
                onClicked: wsRoot.closeRequested()
            }
        }

        // Current Workspace Status Banner
        Rectangle {
            Layout.fillWidth: true
            height: 46
            radius: 4
            color: WorkspaceManager.hasOpenWorkspace ? "#0d1f18" : "#221b10"
            border.color: WorkspaceManager.hasOpenWorkspace ? "#00E67660" : "#FFB30060"
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 6
                spacing: 2
                Label {
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                    font.pixelSize: 10; font.bold: true
                    color: WorkspaceManager.hasOpenWorkspace ? "#00E676" : "#FFB300"
                    text: WorkspaceManager.hasOpenWorkspace
                          ? qsTr("OPEN WORKSPACE: %1").arg(WorkspaceManager.openWorkspacePath)
                          : qsTr("READ-ONLY MODE (No Workspace Open — Render Disabled)")
                }
                Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    font.pixelSize: 9; color: "#B0BEC5"
                    text: qsTr("Ext Mods: %1 | Save Store: %2 KB")
                          .arg(WorkspaceManager.externalModificationCount)
                          .arg(Math.round(WorkspaceManager.saveStoreTotalBytes / 1024))
                }
            }
        }

        // Prominent Operation / Import Feedback Banner (color-coded & word-wrapped)
        Rectangle {
            id: noticeBanner
            Layout.fillWidth: true
            visible: wsRoot.statusNotice.length > 0
            implicitHeight: noticeRow.implicitHeight + 12
            radius: 4
            color: wsRoot.noticeBgColor()
            border.color: wsRoot.noticeBorderColor()

            RowLayout {
                id: noticeRow
                anchors.fill: parent
                anchors.margins: 6
                spacing: 6

                Label {
                    Layout.fillWidth: true
                    text: wsRoot.statusNotice
                    font.pixelSize: 10
                    font.bold: wsRoot.statusSeverity === "error"
                    color: wsRoot.noticeTextColor()
                    wrapMode: Text.WordWrap
                }

                Button {
                    text: qsTr("Details")
                    font.pixelSize: 9
                    Layout.preferredHeight: 20
                    visible: wsRoot.importFeedbackItems.length > 0
                    onClicked: importFeedbackDialog.open()
                }

                Button {
                    text: "✕"
                    flat: true
                    font.pixelSize: 9
                    Layout.preferredWidth: 20
                    Layout.preferredHeight: 20
                    onClicked: wsRoot.statusNotice = ""
                }
            }
        }

        // Sub-tabs: Select / Tracks / Saves
        TabBar {
            id: wsTabBar
            Layout.fillWidth: true
            TabButton { text: qsTr("1. Workspaces") }
            TabButton { text: qsTr("2. Tracks & Tags") }
            TabButton { text: qsTr("3. Saves & Rollback") }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: wsTabBar.currentIndex

            // TAB 1: WORKSPACE SELECTION & LOCKS
            ColumnLayout {
                spacing: 6
                RowLayout {
                    Layout.fillWidth: true
                    Button {
                        text: qsTr("Register & Open Folder…")
                        font.pixelSize: 10
                        highlighted: true
                        enabled: WorkspaceManager.canOpenWorkspaceInSession
                        onClicked: registerWsFolderDialog.open()
                    }
                    Button {
                        text: qsTr("Continue Read-Only")
                        font.pixelSize: 10
                        enabled: WorkspaceManager.canOpenWorkspaceInSession
                        onClicked: {
                            WorkspaceManager.continueInReadOnlyMode()
                            wsRoot.setNotice(qsTr("Switched to Read-Only mode"), "info")
                            wsRoot.refreshAll()
                        }
                    }
                }
                Label {
                    text: qsTr("Registered Workspaces (Single-open; others remain Locked / In-Use):")
                    font.pixelSize: 10; color: "#90A4AE"
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 4
                    model: wsRoot.workspacesList
                    delegate: Rectangle {
                        width: ListView.view.width
                        height: 44
                        radius: 4
                        color: modelData.lockState === 1 ? "#13261d" : "#181c22"
                        border.color: modelData.lockState === 1 ? "#00E676" : (modelData.lockState === 2 ? "#FF5252" : "#2a313b")
                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 6
                            spacing: 6
                            Label {
                                Layout.fillWidth: true
                                elide: Text.ElideMiddle
                                font.pixelSize: 10; color: "#E0E6ED"
                                text: modelData.path
                            }
                            Label {
                                font.pixelSize: 9; font.bold: true; font.family: "Monospace"
                                color: modelData.lockState === 1 ? "#00E676" : (modelData.lockState === 2 ? "#FF5252" : "#FFB300")
                                text: modelData.lockStateText.toUpperCase()
                            }
                            Button {
                                text: modelData.lockState === 1 ? qsTr("Active") : qsTr("Open")
                                font.pixelSize: 9
                                enabled: WorkspaceManager.canOpenWorkspaceInSession && modelData.lockState === 0
                                onClicked: {
                                    if (!WorkspaceManager.openRegisteredWorkspace(modelData.path)) {
                                        wsRoot.setNotice(WorkspaceManager.lastError(), "error")
                                    }
                                    wsRoot.refreshAll()
                                }
                            }
                            Button {
                                text: qsTr("Unregister")
                                font.pixelSize: 9
                                enabled: modelData.lockState !== 2
                                onClicked: {
                                    unregisterConfirmDialog.targetPath = modelData.path
                                    unregisterConfirmDialog.isCurrentlyOpen = (modelData.lockState === 1)
                                    unregisterConfirmDialog.open()
                                }
                            }
                        }
                    }
                }
            }

            // TAB 2: TRACKS, IMPORT, TAG EDITING
            ColumnLayout {
                spacing: 6
                RowLayout {
                    Layout.fillWidth: true
                    Button {
                        text: qsTr("Import File…")
                        font.pixelSize: 10
                        enabled: WorkspaceManager.hasOpenWorkspace
                        onClicked: importFileDialog.open()
                    }
                    Button {
                        text: qsTr("Import Folder…")
                        font.pixelSize: 10
                        enabled: WorkspaceManager.hasOpenWorkspace
                        onClicked: importFolderDialog.open()
                    }
                    Button {
                        text: qsTr("Verify Integrity")
                        font.pixelSize: 10
                        enabled: WorkspaceManager.hasOpenWorkspace
                        onClicked: {
                            var mods = WorkspaceManager.checkExternalChangesFromUi()
                            if (mods.length === 0) {
                                wsRoot.setNotice(qsTr("All files match index SHA-256"), "success")
                            } else {
                                wsRoot.setNotice(mods.join("; "), "warning")
                            }
                            wsRoot.refreshAll()
                        }
                    }
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 3
                    model: wsRoot.filesList
                    delegate: Rectangle {
                        width: ListView.view.width
                        height: 44
                        radius: 3
                        color: wsRoot.selectedFileRel === modelData.relativePath ? "#1c2a36" : "#171b20"
                        border.color: modelData.externallyModified ? "#FFB300" : "#252c35"
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                wsRoot.selectedFileRel = modelData.relativePath
                                wsRoot.refreshAll()
                            }
                        }
                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 6
                            spacing: 6
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                Label {
                                    Layout.fillWidth: true; elide: Text.ElideMiddle
                                    font.pixelSize: 10; font.bold: true; color: "#E0E6ED"
                                    text: modelData.relativePath + (modelData.externallyModified ? " [!EXT]" : "")
                                }
                                Label {
                                    Layout.fillWidth: true; elide: Text.ElideRight
                                    font.pixelSize: 9; color: "#90A4AE"
                                    text: ((modelData.metadata && modelData.metadata.title) ? modelData.metadata.title : "-")
                                        + " / "
                                        + ((modelData.metadata && modelData.metadata.artist) ? modelData.metadata.artist : "-")
                                        + "  [" + (modelData.sha256 ? modelData.sha256.slice(0, 10) : "") + "]"
                                }
                            }
                            Button {
                                text: qsTr("->Deck %1").arg(EngineController.activeDeckId)
                                font.pixelSize: 9; Layout.preferredHeight: 24
                                onClicked: EngineController.loadWorkspaceTrack(EngineController.activeDeckId, modelData.relativePath)
                            }
                            Button {
                                text: qsTr("Del")
                                font.pixelSize: 9; Layout.preferredHeight: 24
                                onClicked: {
                                    if (!WorkspaceManager.deleteFileFromUi(modelData.relativePath)) {
                                        wsRoot.setNotice(WorkspaceManager.lastError(), "error")
                                    }
                                    if (wsRoot.selectedFileRel === modelData.relativePath) wsRoot.selectedFileRel = ""
                                    wsRoot.refreshAll()
                                }
                            }
                        }
                    }
                }
                GroupBox {
                    Layout.fillWidth: true
                    title: wsRoot.selectedFileRel.length > 0 ? qsTr("Edit Tags: %1").arg(wsRoot.selectedFileRel) : qsTr("Select a file to edit tags / rename")
                    enabled: WorkspaceManager.hasOpenWorkspace && wsRoot.selectedFileRel.length > 0
                    GridLayout {
                        anchors.fill: parent
                        columns: 4
                        columnSpacing: 4; rowSpacing: 4
                        Label { text: qsTr("Title"); font.pixelSize: 9; color: "#90A4AE" }
                        TextField { id: editTitleField; Layout.fillWidth: true; font.pixelSize: 10 }
                        Label { text: qsTr("Artist"); font.pixelSize: 9; color: "#90A4AE" }
                        TextField { id: editArtistField; Layout.fillWidth: true; font.pixelSize: 10 }
                        Label { text: qsTr("Album"); font.pixelSize: 9; color: "#90A4AE" }
                        TextField { id: editAlbumField; Layout.fillWidth: true; font.pixelSize: 10 }
                        Label { text: qsTr("Track#"); font.pixelSize: 9; color: "#90A4AE" }
                        TextField { id: editTrackNumField; Layout.fillWidth: true; font.pixelSize: 10 }
                        Label { text: qsTr("Date"); font.pixelSize: 9; color: "#90A4AE" }
                        TextField { id: editDateField; Layout.fillWidth: true; font.pixelSize: 10 }
                        Label { text: qsTr("Genre"); font.pixelSize: 9; color: "#90A4AE" }
                        TextField { id: editGenreField; Layout.fillWidth: true; font.pixelSize: 10 }
                        Button {
                            Layout.columnSpan: 2; Layout.fillWidth: true
                            text: qsTr("Save Tags (COW)")
                            font.pixelSize: 10; highlighted: true
                            onClicked: {
                                var m = {
                                    title: editTitleField.text,
                                    artist: editArtistField.text,
                                    album: editAlbumField.text,
                                    trackNumber: parseInt(editTrackNumField.text, 10) || 0,
                                    dateOrYear: editDateField.text,
                                    genre: editGenreField.text
                                }
                                if (WorkspaceManager.editFileTagsFromUi(wsRoot.selectedFileRel, m)) {
                                    wsRoot.setNotice(qsTr("Tags saved to %1").arg(wsRoot.selectedFileRel), "success")
                                } else {
                                    wsRoot.setNotice(WorkspaceManager.lastError(), "error")
                                }
                                wsRoot.refreshAll()
                            }
                        }
                        TextField { id: renameField; Layout.fillWidth: true; placeholderText: qsTr("New filename…"); font.pixelSize: 10 }
                        Button {
                            text: qsTr("Rename")
                            font.pixelSize: 10
                            enabled: renameField.text.trim().length > 0
                            onClicked: {
                                if (WorkspaceManager.renameFileFromUi(wsRoot.selectedFileRel, renameField.text.trim())) {
                                    wsRoot.setNotice(qsTr("Renamed to %1").arg(renameField.text.trim()), "success")
                                    wsRoot.selectedFileRel = renameField.text.trim()
                                    renameField.text = ""
                                } else {
                                    wsRoot.setNotice(WorkspaceManager.lastError(), "error")
                                }
                                wsRoot.refreshAll()
                            }
                        }
                    }
                }
            }

            // TAB 3: SAVES, DIFF, AUDITION, ROLLBACK, GC
            ColumnLayout {
                spacing: 6
                RowLayout {
                    Layout.fillWidth: true
                    TextField { id: newSaveNameField; Layout.fillWidth: true; placeholderText: qsTr("Checkpoint name…"); font.pixelSize: 10 }
                    Button {
                        text: qsTr("Create Save")
                        font.pixelSize: 10; highlighted: true
                        enabled: WorkspaceManager.hasOpenWorkspace
                        onClicked: {
                            var sid = WorkspaceManager.createSaveFromUi(newSaveNameField.text.trim())
                            if (sid.length > 0) {
                                newSaveNameField.text = ""
                                wsRoot.setNotice(qsTr("Created save %1").arg(sid), "success")
                            } else {
                                wsRoot.setNotice(WorkspaceManager.lastError(), "error")
                            }
                            wsRoot.refreshAll()
                        }
                    }
                    Button {
                        text: qsTr("Run GC")
                        font.pixelSize: 10
                        enabled: WorkspaceManager.hasOpenWorkspace
                        onClicked: {
                            var removed = WorkspaceManager.runGarbageCollectionFromUi()
                            wsRoot.setNotice(qsTr("GC removed %1 unreferenced object(s)").arg(removed), "info")
                            wsRoot.refreshAll()
                        }
                    }
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 4
                    model: wsRoot.savesList
                    delegate: Rectangle {
                        width: ListView.view.width
                        height: 54
                        radius: 4
                        color: "#171b20"
                        border.color: modelData.isAutoSave ? "#FFB30060" : "#252c35"
                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 6
                            spacing: 6
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                Label {
                                    Layout.fillWidth: true; elide: Text.ElideRight
                                    font.pixelSize: 10; font.bold: true; color: "#E0E6ED"
                                    text: modelData.name + (modelData.isAutoSave ? " [AutoSave]" : "")
                                }
                                Label {
                                    Layout.fillWidth: true; elide: Text.ElideRight
                                    font.pixelSize: 9; color: "#90A4AE"
                                    text: modelData.saveId + " • " + modelData.fileCount + " files • " + modelData.createdUtc
                                }
                            }
                            Button {
                                text: qsTr("Set A")
                                font.pixelSize: 9; Layout.preferredHeight: 24
                                onClicked: fromSaveField.text = modelData.saveId
                            }
                            Button {
                                text: qsTr("Set B")
                                font.pixelSize: 9; Layout.preferredHeight: 24
                                onClicked: toSaveField.text = modelData.saveId
                            }
                            Button {
                                text: qsTr("Audition")
                                font.pixelSize: 9; Layout.preferredHeight: 24
                                enabled: wsRoot.selectedFileRel.length > 0
                                onClicked: {
                                    var p = WorkspaceManager.prepareSavedTrackForAuditionFromUi(modelData.saveId, wsRoot.selectedFileRel)
                                    if (p.length > 0) EngineController.loadTrack(EngineController.activeDeckId, p)
                                    else wsRoot.setNotice(WorkspaceManager.lastError(), "error")
                                }
                            }
                            Button {
                                text: qsTr("Rollback")
                                font.pixelSize: 9; Layout.preferredHeight: 24
                                onClicked: {
                                    var r = WorkspaceManager.adoptSaveFromUi(modelData.saveId)
                                    if (r.success) {
                                        wsRoot.setNotice(qsTr("Rolled back to %1 (auto-save: %2)").arg(modelData.saveId).arg(r.autoSaveId || "-"), "success")
                                    } else {
                                        wsRoot.setNotice(r.errorMessage || qsTr("Rollback failed"), "error")
                                    }
                                    wsRoot.refreshAll()
                                }
                            }
                            Button {
                                text: qsTr("Del")
                                font.pixelSize: 9; Layout.preferredHeight: 24
                                onClicked: {
                                    WorkspaceManager.deleteSaveFromUi(modelData.saveId)
                                    wsRoot.refreshAll()
                                }
                            }
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    TextField { id: fromSaveField; Layout.fillWidth: true; placeholderText: qsTr("From saveId"); font.pixelSize: 9 }
                    TextField { id: toSaveField; Layout.fillWidth: true; placeholderText: qsTr("To saveId"); font.pixelSize: 9 }
                    Button {
                        text: qsTr("Diff Saves")
                        font.pixelSize: 9
                        onClicked: {
                            var d = WorkspaceManager.compareSavesFromUi(fromSaveField.text.trim(), toSaveField.text.trim())
                            if (!d.valid) {
                                wsRoot.diffSummaryText = d.errorMessage || qsTr("Invalid save selection")
                            } else {
                                var lines = [qsTr("Added: %1, Removed: %2, Modified: %3, Unchanged: %4")
                                             .arg(d.addedCount).arg(d.removedCount).arg(d.modifiedCount).arg(d.unchangedCount)]
                                for (var i = 0; i < d.diffs.length; ++i) {
                                    var fd = d.diffs[i]
                                    lines.push(fd.changeType + ": " + fd.relativePath)
                                }
                                wsRoot.diffSummaryText = lines.join("\n")
                            }
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: wsRoot.diffSummaryText.length > 0
                    text: wsRoot.diffSummaryText
                    wrapMode: Text.WordWrap
                    font.pixelSize: 9; font.family: "Monospace"; color: "#80DEEA"
                }
            }
        }
    }
}
