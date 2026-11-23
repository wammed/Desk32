import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Desk32.Core
import "../components"

Item {
    id: wsRoot

    property var workspacesList: []
    property var allFilesList: []
    property var folderTreeList: []
    property var filteredFilesList: []
    property var savesList: []

    property string activeFolderFilter: ""   // "" = All Files
    property string searchFilter: ""
    property string selectedFileRel: ""
    property var selectedFileDetails: ({})
    property string diffFromId: ""
    property string diffToId: ""
    property string diffSummaryText: ""
    property int rightSubTab: 0              // 0: Provenance & Tags, 1: Checkpoints & Diff, 2: Workspaces Registry
    property string statusNotice: ""
    property string statusSeverity: "info"
    property bool deleteFileConfirmArmed: false

    function setNotice(msg, sev) {
        statusNotice = msg || ""
        statusSeverity = sev || "info"
    }

    function formatBytes(bytes) {
        if (!bytes || bytes <= 0) return "0 B"
        if (bytes < 1024) return bytes + " B"
        if (bytes < 1024 * 1024) return (bytes / 1024.0).toFixed(1) + " KB"
        return (bytes / (1024.0 * 1024.0)).toFixed(2) + " MB"
    }

    function refresh() {
        workspacesList = WorkspaceManager.getRegisteredWorkspaces() || []
        if (!WorkspaceManager.hasOpenWorkspace) {
            allFilesList = []
            folderTreeList = [{ path: "", label: qsTr("All Tracks"), count: 0 }]
            filteredFilesList = []
            savesList = []
            selectedFileRel = ""
            selectedFileDetails = ({})
            return
        }

        allFilesList = WorkspaceManager.getWorkspaceFiles("") || []
        savesList = WorkspaceManager.getSaves() || []

        // Build directory hierarchy map
        var dirCounts = ({})
        dirCounts[""] = allFilesList.length
        for (var i = 0; i < allFilesList.length; ++i) {
            var rel = allFilesList[i].relativePath || ""
            var slash = rel.lastIndexOf("/")
            if (slash > 0) {
                var dir = rel.substring(0, slash)
                dirCounts[dir] = (dirCounts[dir] || 0) + 1
            } else {
                dirCounts["/"] = (dirCounts["/"] || 0) + 1
            }
        }
        var tree = [{ path: "", label: qsTr("All Tracks"), depth: 0, count: allFilesList.length }]
        if (dirCounts["/"] > 0) {
            tree.push({ path: "/", label: qsTr("(Workspace Root)"), depth: 1, count: dirCounts["/"] })
        }
        var dirKeys = Object.keys(dirCounts).filter(function(k) { return k !== "" && k !== "/" }).sort()
        for (var d = 0; d < dirKeys.length; ++d) {
            tree.push({ path: dirKeys[d], label: dirKeys[d], depth: 1, count: dirCounts[dirKeys[d]] })
        }
        folderTreeList = tree

        applyFilters()

        if (selectedFileRel.length > 0) {
            var stillExists = false
            for (var k = 0; k < allFilesList.length; ++k) {
                if (allFilesList[k].relativePath === selectedFileRel) {
                    stillExists = true
                    break
                }
            }
            if (!stillExists) {
                selectedFileRel = ""
                selectedFileDetails = ({})
            } else {
                loadFileInspector(selectedFileRel)
            }
        }
    }

    function applyFilters() {
        var q = searchFilter.trim().toLowerCase()
        var folder = activeFolderFilter
        var out = []
        for (var i = 0; i < allFilesList.length; ++i) {
            var item = allFilesList[i]
            var rel = item.relativePath || ""
            if (folder === "/") {
                if (rel.indexOf("/") >= 0) continue
            } else if (folder.length > 0) {
                if (rel.indexOf(folder + "/") !== 0 && rel !== folder) continue
            }
            if (q.length > 0) {
                var hay = (rel + " " + (item.metadata && item.metadata.title ? item.metadata.title : "") + " " + (item.metadata && item.metadata.artist ? item.metadata.artist : "")).toLowerCase()
                if (hay.indexOf(q) < 0) continue
            }
            out.push(item)
        }
        filteredFilesList = out
    }

    function loadFileInspector(relPath) {
        selectedFileRel = relPath
        deleteFileConfirmArmed = false
        if (!relPath || relPath.length === 0) {
            selectedFileDetails = ({})
            return
        }
        var det = WorkspaceManager.getFileDetails(relPath) || ({})
        selectedFileDetails = det
        var m = det.metadata || ({})
        tagTitleField.text = m.title || ""
        tagArtistField.text = m.artist || ""
        tagAlbumField.text = m.album || ""
        tagAlbumArtistField.text = m.albumArtist || ""
        tagTrackNumField.text = (m.trackNumber && m.trackNumber > 0) ? String(m.trackNumber) : ""
        tagDateField.text = m.dateOrYear || ""
        tagGenreField.text = m.genre || ""
        tagComposerField.text = m.composer || ""
        tagLabelField.text = m.label || ""
        tagIsrcField.text = m.isrc || ""
        tagCommentField.text = m.comment || ""
        renameFileField.text = ""
        moveFileDirField.text = ""
    }

    Component.onCompleted: refresh()

    Connections {
        target: WorkspaceManager
        function onWorkspaceStateChanged() { wsRoot.refresh() }
        function onWorkspaceContentsChanged() { wsRoot.refresh() }
        function onExternalModificationDetected() { wsRoot.refresh() }
    }

    component DarkField: TextField {
        color: "#DCE6F0"
        placeholderTextColor: "#5C6B7C"
        font.pixelSize: 11
        leftPadding: 8; rightPadding: 8
        topPadding: 5; bottomPadding: 5
        background: Rectangle {
            radius: 3
            color: parent.enabled ? "#070A0F" : "#0D1117"
            border.color: parent.activeFocus ? "#33E3FF" : "#2B3543"
            border.width: 1
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "#0e1116"
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        // =====================================================================
        // TOP WORKSPACE CONTROL & INTEGRITY STRIP
        // =====================================================================
        BrushedMetalPanel {
            Layout.fillWidth: true
            Layout.preferredHeight: 68
            baseTint: "#14181f"
            borderColor: "#333c4a"
            showScrews: true
            cornerRadius: 5

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                spacing: 10

                // Workspace Status Badge
                Rectangle {
                    implicitWidth: wsBadgeCol.implicitWidth + 22
                    height: 40
                    radius: 4
                    color: WorkspaceManager.hasOpenWorkspace ? "#091E16" : "#22180C"
                    border.color: WorkspaceManager.hasOpenWorkspace ? "#247A56" : "#7C5B21"
                    Column {
                        id: wsBadgeCol
                        anchors.centerIn: parent
                        Label {
                            text: WorkspaceManager.hasOpenWorkspace ? qsTr("WORKSPACE OPEN (LOCKED TO SESSION)") : qsTr("NO WORKSPACE — READ-ONLY MODE")
                            font.family: "Monospace"; font.pixelSize: 10; font.bold: true
                            color: WorkspaceManager.hasOpenWorkspace ? "#00E676" : "#F5B841"
                        }
                        Label {
                            text: WorkspaceManager.hasOpenWorkspace
                                  ? WorkspaceManager.openWorkspacePath
                                  : qsTr("Select or register a workspace folder to enable COW import, tagging & checkpoints")
                            font.family: "Monospace"; font.pixelSize: 10
                            color: "#AFC0D0"
                            elide: Text.ElideMiddle
                            width: Math.min(760, implicitWidth)
                        }
                    }
                }

                // Store Stats + External Modification Guard
                Rectangle {
                    visible: WorkspaceManager.hasOpenWorkspace
                    implicitWidth: storeStatCol.implicitWidth + 18
                    height: 40
                    radius: 4
                    color: "#090C11"
                    border.color: WorkspaceManager.externalModificationCount > 0 ? "#D8434E" : "#26303D"
                    Column {
                        id: storeStatCol
                        anchors.centerIn: parent
                        Label {
                            text: qsTr("TRACKS: %1   |   STORE: %2")
                                  .arg(wsRoot.allFilesList.length)
                                  .arg(wsRoot.formatBytes(WorkspaceManager.saveStoreTotalBytes))
                            font.family: "Monospace"; font.pixelSize: 10; font.bold: true; color: "#78D8F0"
                        }
                        Label {
                            text: WorkspaceManager.externalModificationCount > 0
                                  ? qsTr("EXTERNAL DISK EDITS DETECTED: %1").arg(WorkspaceManager.externalModificationCount)
                                  : qsTr("INTEGRITY WATCHER: CLEAN")
                            font.family: "Monospace"; font.pixelSize: 9; font.bold: true
                            color: WorkspaceManager.externalModificationCount > 0 ? "#FF6B74" : "#00E676"
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                ConsoleButton {
                    visible: WorkspaceManager.externalModificationCount > 0
                    text: qsTr("ACK MODS")
                    implicitWidth: 88; implicitHeight: 34; fontSize: 10; accentColor: "#FFB300"
                    onClicked: {
                        WorkspaceManager.acknowledgeExternalModifications()
                        wsRoot.refresh()
                    }
                }

                ConsoleButton {
                    visible: WorkspaceManager.hasOpenWorkspace
                    text: qsTr("VERIFY DISK")
                    implicitWidth: 98; implicitHeight: 34; fontSize: 10; accentColor: "#00E5FF"
                    onClicked: {
                        var ch = WorkspaceManager.checkExternalChangesFromUi() || []
                        wsRoot.setNotice(ch.length > 0
                                         ? qsTr("External modification detected in %1 file(s)").arg(ch.length)
                                         : qsTr("SHA-256 verification clean across all workspace files"),
                                         ch.length > 0 ? "warning" : "success")
                        wsRoot.refresh()
                    }
                }

                ConsoleButton {
                    text: qsTr("OPEN / REGISTER...")
                    implicitWidth: 138; implicitHeight: 34; fontSize: 10; accentColor: "#E8BA6B"
                    onClicked: openWorkspaceFolderDlg.open()
                }

                ConsoleButton {
                    text: qsTr("IMPORT FILE...")
                    implicitWidth: 112; implicitHeight: 34; fontSize: 10; accentColor: "#00E5FF"
                    enabled: WorkspaceManager.hasOpenWorkspace
                    onClicked: importAudioFileDlg.open()
                }

                ConsoleButton {
                    text: qsTr("IMPORT FOLDER...")
                    implicitWidth: 126; implicitHeight: 34; fontSize: 10; accentColor: "#00E5FF"
                    enabled: WorkspaceManager.hasOpenWorkspace
                    onClicked: importAudioFolderDlg.open()
                }
            }
        }

        // Inline Status Notice Banner (if set)
        Rectangle {
            visible: wsRoot.statusNotice.length > 0
            Layout.fillWidth: true
            height: 28
            radius: 4
            color: wsRoot.statusSeverity === "error" ? "#2E1216"
                   : (wsRoot.statusSeverity === "warning" ? "#2B210B"
                      : (wsRoot.statusSeverity === "success" ? "#0B2418" : "#0A1B24"))
            border.color: wsRoot.statusSeverity === "error" ? "#FF5252"
                          : (wsRoot.statusSeverity === "warning" ? "#FFB300"
                             : (wsRoot.statusSeverity === "success" ? "#00E676" : "#00E5FF"))
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10; anchors.rightMargin: 8
                Label {
                    Layout.fillWidth: true
                    text: wsRoot.statusNotice
                    font.family: "Monospace"; font.pixelSize: 10; color: "#E6EEF6"; elide: Text.ElideRight
                }
                ConsoleButton {
                    text: "✕"
                    implicitWidth: 24; implicitHeight: 20; fontSize: 9
                    onClicked: wsRoot.statusNotice = ""
                }
            }
        }

        // =====================================================================
        // MAIN 2-PANE HARDWARE BODY
        // =====================================================================
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 10

            // -----------------------------------------------------------------
            // LEFT PANE (42%): FOLDER TREE SIDEBAR + WORKSPACE TRACK LIST
            // -----------------------------------------------------------------
            BrushedMetalPanel {
                Layout.fillWidth: true
                Layout.preferredWidth: 1040
                Layout.fillHeight: true
                baseTint: "#13171d"
                borderColor: "#2f3743"
                showScrews: true
                cornerRadius: 5

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            text: qsTr("PROJECT FILES & FOLDER TREE")
                            font.family: "Monospace"; font.pixelSize: 12; font.bold: true; color: "#E8BA6B"
                        }
                        Item { Layout.fillWidth: true }
                        DarkField {
                            Layout.preferredWidth: 240
                            placeholderText: qsTr("Filter filename, title, artist...")
                            onTextChanged: {
                                wsRoot.searchFilter = text
                                wsRoot.applyFilters()
                            }
                        }
                        ConsoleButton {
                            text: qsTr("LOAD → DECK %1").arg(EngineController.activeDeckId)
                            implicitWidth: 132; implicitHeight: 28; fontSize: 10; accentColor: "#00E676"
                            enabled: WorkspaceManager.hasOpenWorkspace && wsRoot.selectedFileRel.length > 0
                            onClicked: {
                                var ok = EngineController.loadWorkspaceTrack(EngineController.activeDeckId, wsRoot.selectedFileRel)
                                wsRoot.setNotice(ok
                                                 ? qsTr("Loaded '%1' into Deck %2").arg(wsRoot.selectedFileRel, EngineController.activeDeckId)
                                                 : qsTr("Failed to load '%1' into Deck %2").arg(wsRoot.selectedFileRel, EngineController.activeDeckId),
                                                 ok ? "success" : "error")
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: 8

                        // Sub-column 1: Folder Tree Panel
                        Rectangle {
                            Layout.preferredWidth: 210
                            Layout.fillHeight: true
                            radius: 4
                            color: "#080B10"
                            border.color: "#222B36"

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 6
                                spacing: 4
                                Label {
                                    text: qsTr("FOLDERS")
                                    font.family: "Monospace"; font.pixelSize: 10; font.bold: true; color: "#7F93A6"
                                }
                                ListView {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    clip: true
                                    spacing: 3
                                    model: wsRoot.folderTreeList
                                    delegate: Rectangle {
                                        width: ListView.view.width
                                        height: 28
                                        radius: 3
                                        readonly property bool isSel: wsRoot.activeFolderFilter === modelData.path
                                        color: isSel ? "#152938" : "transparent"
                                        border.color: isSel ? "#00E5FF" : "transparent"

                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 6 + (modelData.depth || 0) * 10
                                            anchors.rightMargin: 6
                                            spacing: 4
                                            Label {
                                                Layout.fillWidth: true
                                                text: (modelData.depth > 0 ? "▸ " : "▣ ") + modelData.label
                                                font.family: "Monospace"; font.pixelSize: 10
                                                color: isSel ? "#33E3FF" : "#BCC8D4"
                                                elide: Text.ElideRight
                                            }
                                            Label {
                                                text: String(modelData.count)
                                                font.family: "Monospace"; font.pixelSize: 9; color: "#6D7E8F"
                                            }
                                        }
                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                wsRoot.activeFolderFilter = modelData.path
                                                wsRoot.applyFilters()
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // Sub-column 2: Files List
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            radius: 4
                            color: "#080B10"
                            border.color: "#222B36"

                            ListView {
                                anchors.fill: parent
                                anchors.margins: 6
                                clip: true
                                spacing: 4
                                model: wsRoot.filteredFilesList
                                delegate: Rectangle {
                                    width: ListView.view.width
                                    height: 54
                                    radius: 4
                                    readonly property bool isSel: wsRoot.selectedFileRel === modelData.relativePath
                                    color: isSel ? "#132634" : (modelData.externallyModified ? "#241216" : "#0E131A")
                                    border.color: isSel ? "#00E5FF" : (modelData.externallyModified ? "#B83B44" : "#222B37")

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 10; anchors.rightMargin: 10
                                        spacing: 8

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2
                                            RowLayout {
                                                spacing: 6
                                                Label {
                                                    text: modelData.fileName || modelData.relativePath
                                                    font.pixelSize: 12; font.bold: true; color: "#E6EEF6"
                                                    elide: Text.ElideRight
                                                }
                                                Rectangle {
                                                    implicitWidth: provLbl.implicitWidth + 8; height: 16; radius: 2
                                                    color: modelData.logOnlyProvenance ? "#2A200C" : "#0A2218"
                                                    border.color: modelData.logOnlyProvenance ? "#A67C23" : "#1F7A52"
                                                    Label {
                                                        id: provLbl; anchors.centerIn: parent
                                                        text: modelData.logOnlyProvenance ? qsTr("LOG-ONLY") : qsTr("TAGLIB")
                                                        font.family: "Monospace"; font.pixelSize: 8; font.bold: true
                                                        color: modelData.logOnlyProvenance ? "#F5B841" : "#00E676"
                                                    }
                                                }
                                                Rectangle {
                                                    visible: !!modelData.externallyModified
                                                    implicitWidth: extModLbl.implicitWidth + 8; height: 16; radius: 2
                                                    color: "#361217"; border.color: "#FF5252"
                                                    Label {
                                                        id: extModLbl; anchors.centerIn: parent
                                                        text: qsTr("EXT MODIFIED")
                                                        font.family: "Monospace"; font.pixelSize: 8; font.bold: true; color: "#FF8A8A"
                                                    }
                                                }
                                            }
                                            Label {
                                                Layout.fillWidth: true
                                                text: {
                                                    var md = modelData.metadata || ({})
                                                    var sr = md.sampleRate ? ((md.sampleRate / 1000.0).toFixed(1) + " kHz") : "-"
                                                    var bd = md.bitDepth ? (md.bitDepth + "-bit") : "-"
                                                    return modelData.relativePath + "   •   " + sr + " / " + bd + "   •   "
                                                           + wsRoot.formatBytes(modelData.size) + "   •   SHA:" + (modelData.sha256 || "").slice(0, 10)
                                                }
                                                font.family: "Monospace"; font.pixelSize: 9; color: "#899BAE"
                                                elide: Text.ElideRight
                                            }
                                        }

                                        ConsoleButton {
                                            text: qsTr("LOAD")
                                            implicitWidth: 56; implicitHeight: 28; fontSize: 9; accentColor: "#00E5FF"
                                            onClicked: {
                                                wsRoot.loadFileInspector(modelData.relativePath)
                                                EngineController.loadWorkspaceTrack(EngineController.activeDeckId, modelData.relativePath)
                                            }
                                        }
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        z: -1
                                        onClicked: wsRoot.loadFileInspector(modelData.relativePath)
                                    }
                                }

                                Label {
                                    anchors.centerIn: parent
                                    visible: wsRoot.filteredFilesList.length === 0
                                    text: WorkspaceManager.hasOpenWorkspace
                                          ? qsTr("No matching tracks in workspace folder")
                                          : qsTr("Open or register a workspace folder to view managed project files")
                                    font.family: "Monospace"; font.pixelSize: 11; color: "#5C6B7C"
                                }
                            }
                        }
                    }
                }
            }

            // -----------------------------------------------------------------
            // RIGHT PANE (58%): PROVENANCE/TAGLIB COW | CHECKPOINTS & DIFF | REGISTRY
            // -----------------------------------------------------------------
            BrushedMetalPanel {
                Layout.fillWidth: true
                Layout.preferredWidth: 1460
                Layout.fillHeight: true
                baseTint: "#13171d"
                borderColor: "#2f3743"
                showScrews: true
                cornerRadius: 5

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10

                    // Sub-Navigation Tabs
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: [
                                qsTr("PROVENANCE & TAGLIB (COW)"),
                                qsTr("CHECKPOINTS, DIFF & ROLLBACK"),
                                qsTr("WORKSPACES REGISTRY")
                            ]
                            ConsoleButton {
                                Layout.fillWidth: true
                                implicitHeight: 32
                                text: modelData
                                fontSize: 10
                                accentColor: "#00E5FF"
                                highlighted: wsRoot.rightSubTab === index
                                onClicked: wsRoot.rightSubTab = index
                            }
                        }
                    }

                    StackLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        currentIndex: wsRoot.rightSubTab

                        // SUBTAB 0: PROVENANCE, SHA-256 VERIFICATION & COW TAG EDITOR
                        ColumnLayout {
                            spacing: 8

                            // Provenance & Cryptographic Integrity Card
                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 112
                                radius: 4
                                color: "#070A0F"
                                border.color: "#222C38"

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 4
                                    Label {
                                        text: wsRoot.selectedFileRel.length > 0
                                              ? qsTr("PROVENANCE & INTEGRITY — %1").arg(wsRoot.selectedFileRel)
                                              : qsTr("SELECT A TRACK TO INSPECT PROVENANCE & TAGS")
                                        font.family: "Monospace"; font.pixelSize: 11; font.bold: true; color: "#78D8F0"
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: wsRoot.selectedFileRel.length > 0
                                              ? qsTr("Disk SHA-256:     %1").arg(wsRoot.selectedFileDetails.sha256 || "-")
                                              : qsTr("Disk SHA-256:     -")
                                        font.family: "Monospace"; font.pixelSize: 10; color: "#C6D3E0"; elide: Text.ElideMiddle
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: wsRoot.selectedFileRel.length > 0
                                              ? qsTr("Original SHA-256: %1").arg(wsRoot.selectedFileDetails.originalSha256 || qsTr("(Same / Not recorded)"))
                                              : qsTr("Original SHA-256: -")
                                        font.family: "Monospace"; font.pixelSize: 10; color: "#9AA9B8"; elide: Text.ElideMiddle
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: wsRoot.selectedFileRel.length > 0
                                              ? qsTr("Security Guard:   Containment Verified  •  Symlink-Free  •  nlink=1  •  %1")
                                                .arg(wsRoot.selectedFileDetails.externallyModified
                                                     ? (wsRoot.selectedFileDetails.externalChangeDetail || qsTr("EXTERNALLY MODIFIED"))
                                                     : qsTr("Checksum Match"))
                                              : qsTr("Copy-on-Write lazy evacuation is armed before any metadata edit, rename, or delete.")
                                        font.family: "Monospace"; font.pixelSize: 9
                                        color: wsRoot.selectedFileDetails.externallyModified ? "#FF7B85" : "#00E676"
                                        elide: Text.ElideRight
                                    }
                                }
                            }

                            // Editable TagLib Grid (COW Save)
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                radius: 4
                                color: "#090D12"
                                border.color: "#222C38"
                                enabled: WorkspaceManager.hasOpenWorkspace && wsRoot.selectedFileRel.length > 0
                                opacity: enabled ? 1.0 : 0.48

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 12
                                    spacing: 10

                                    Label {
                                        text: qsTr("TAGLIB METADATA EDITOR (COPY-ON-WRITE)")
                                        font.family: "Monospace"; font.pixelSize: 11; font.bold: true; color: "#E8BA6B"
                                    }

                                    GridLayout {
                                        Layout.fillWidth: true
                                        columns: 4
                                        columnSpacing: 10
                                        rowSpacing: 8

                                        Label { text: qsTr("TITLE"); font.family: "Monospace"; font.pixelSize: 10; color: "#8E9FAD" }
                                        DarkField { id: tagTitleField; Layout.fillWidth: true }
                                        Label { text: qsTr("ARTIST"); font.family: "Monospace"; font.pixelSize: 10; color: "#8E9FAD" }
                                        DarkField { id: tagArtistField; Layout.fillWidth: true }

                                        Label { text: qsTr("ALBUM"); font.family: "Monospace"; font.pixelSize: 10; color: "#8E9FAD" }
                                        DarkField { id: tagAlbumField; Layout.fillWidth: true }
                                        Label { text: qsTr("ALBUM ARTIST"); font.family: "Monospace"; font.pixelSize: 10; color: "#8E9FAD" }
                                        DarkField { id: tagAlbumArtistField; Layout.fillWidth: true }

                                        Label { text: qsTr("TRACK #"); font.family: "Monospace"; font.pixelSize: 10; color: "#8E9FAD" }
                                        DarkField { id: tagTrackNumField; Layout.fillWidth: true }
                                        Label { text: qsTr("DATE / YEAR"); font.family: "Monospace"; font.pixelSize: 10; color: "#8E9FAD" }
                                        DarkField { id: tagDateField; Layout.fillWidth: true }

                                        Label { text: qsTr("GENRE"); font.family: "Monospace"; font.pixelSize: 10; color: "#8E9FAD" }
                                        DarkField { id: tagGenreField; Layout.fillWidth: true }
                                        Label { text: qsTr("COMPOSER"); font.family: "Monospace"; font.pixelSize: 10; color: "#8E9FAD" }
                                        DarkField { id: tagComposerField; Layout.fillWidth: true }

                                        Label { text: qsTr("LABEL"); font.family: "Monospace"; font.pixelSize: 10; color: "#8E9FAD" }
                                        DarkField { id: tagLabelField; Layout.fillWidth: true }
                                        Label { text: qsTr("ISRC"); font.family: "Monospace"; font.pixelSize: 10; color: "#8E9FAD" }
                                        DarkField { id: tagIsrcField; Layout.fillWidth: true }

                                        Label { text: qsTr("COMMENT"); font.family: "Monospace"; font.pixelSize: 10; color: "#8E9FAD" }
                                        DarkField { id: tagCommentField; Layout.columnSpan: 3; Layout.fillWidth: true }
                                    }

                                    Item { Layout.fillHeight: true }

                                    // Save Tags / Rename / Move / Delete Row
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 8
                                        ConsoleButton {
                                            text: qsTr("SAVE TAGS (COW)")
                                            implicitWidth: 146; implicitHeight: 34; fontSize: 10
                                            accentColor: "#00E676"
                                            anodizedFill: true; anodizedTop: "#1F6E50"; anodizedBottom: "#0E3C2A"; anodizedText: "#E8FFF5"
                                            onClicked: {
                                                var m = {
                                                    title: tagTitleField.text,
                                                    artist: tagArtistField.text,
                                                    album: tagAlbumField.text,
                                                    albumArtist: tagAlbumArtistField.text,
                                                    trackNumber: parseInt(tagTrackNumField.text, 10) || 0,
                                                    dateOrYear: tagDateField.text,
                                                    genre: tagGenreField.text,
                                                    composer: tagComposerField.text,
                                                    label: tagLabelField.text,
                                                    isrc: tagIsrcField.text,
                                                    comment: tagCommentField.text
                                                }
                                                if (WorkspaceManager.editFileTagsFromUi(wsRoot.selectedFileRel, m)) {
                                                    wsRoot.setNotice(qsTr("Saved COW tags for '%1'").arg(wsRoot.selectedFileRel), "success")
                                                } else {
                                                    wsRoot.setNotice(WorkspaceManager.lastError(), "error")
                                                }
                                                wsRoot.refresh()
                                            }
                                        }

                                        DarkField { id: renameFileField; Layout.fillWidth: true; placeholderText: qsTr("New filename...") }
                                        ConsoleButton {
                                            text: qsTr("RENAME")
                                            implicitWidth: 78; implicitHeight: 34; fontSize: 10
                                            enabled: renameFileField.text.trim().length > 0
                                            onClicked: {
                                                var targetName = renameFileField.text.trim()
                                                if (WorkspaceManager.renameFileFromUi(wsRoot.selectedFileRel, targetName)) {
                                                    wsRoot.setNotice(qsTr("Renamed to '%1'").arg(targetName), "success")
                                                    wsRoot.selectedFileRel = targetName
                                                    renameFileField.text = ""
                                                } else {
                                                    wsRoot.setNotice(WorkspaceManager.lastError(), "error")
                                                }
                                                wsRoot.refresh()
                                            }
                                        }

                                        DarkField { id: moveFileDirField; Layout.preferredWidth: 140; placeholderText: qsTr("Target subdir...") }
                                        ConsoleButton {
                                            text: qsTr("MOVE")
                                            implicitWidth: 68; implicitHeight: 34; fontSize: 10
                                            enabled: moveFileDirField.text.trim().length > 0
                                            onClicked: {
                                                var dir = moveFileDirField.text.trim()
                                                if (WorkspaceManager.moveFileFromUi(wsRoot.selectedFileRel, dir)) {
                                                    wsRoot.setNotice(qsTr("Moved '%1' to '%2'").arg(wsRoot.selectedFileRel, dir), "success")
                                                    moveFileDirField.text = ""
                                                    wsRoot.selectedFileRel = ""
                                                } else {
                                                    wsRoot.setNotice(WorkspaceManager.lastError(), "error")
                                                }
                                                wsRoot.refresh()
                                            }
                                        }

                                        ConsoleButton {
                                            text: wsRoot.deleteFileConfirmArmed ? qsTr("CONFIRM DEL") : qsTr("DELETE")
                                            implicitWidth: 102; implicitHeight: 34; fontSize: 10
                                            accentColor: "#FF5252"
                                            anodizedFill: wsRoot.deleteFileConfirmArmed
                                            anodizedTop: "#B33339"; anodizedBottom: "#61171B"; anodizedText: "#FFE8EA"
                                            onClicked: {
                                                if (!wsRoot.deleteFileConfirmArmed) {
                                                    wsRoot.deleteFileConfirmArmed = true
                                                    return
                                                }
                                                if (WorkspaceManager.deleteFileFromUi(wsRoot.selectedFileRel)) {
                                                    wsRoot.setNotice(qsTr("Deleted '%1' (COW backup preserved in store if saved)").arg(wsRoot.selectedFileRel), "info")
                                                    wsRoot.selectedFileRel = ""
                                                } else {
                                                    wsRoot.setNotice(WorkspaceManager.lastError(), "error")
                                                }
                                                wsRoot.deleteFileConfirmArmed = false
                                                wsRoot.refresh()
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // SUBTAB 1: CONTENT-ADDRESSED CHECKPOINTS, DIFF & ROLLBACK
                        ColumnLayout {
                            spacing: 8

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                DarkField {
                                    id: checkpointNameField
                                    Layout.fillWidth: true
                                    placeholderText: qsTr("Checkpoint description (e.g., Pre-Mastering Baseline)...")
                                }
                                ConsoleButton {
                                    text: qsTr("CREATE CHECKPOINT")
                                    implicitWidth: 156; implicitHeight: 32; fontSize: 10; accentColor: "#00E676"
                                    enabled: WorkspaceManager.hasOpenWorkspace
                                    onClicked: {
                                        var sid = WorkspaceManager.createSaveFromUi(checkpointNameField.text.trim())
                                        if (sid.length > 0) {
                                            checkpointNameField.text = ""
                                            wsRoot.setNotice(qsTr("Created checkpoint %1").arg(sid), "success")
                                        } else {
                                            wsRoot.setNotice(WorkspaceManager.lastError(), "error")
                                        }
                                        wsRoot.refresh()
                                    }
                                }
                                ConsoleButton {
                                    text: qsTr("RUN GC")
                                    implicitWidth: 84; implicitHeight: 32; fontSize: 10; accentColor: "#F5B841"
                                    enabled: WorkspaceManager.hasOpenWorkspace
                                    onClicked: {
                                        var removed = WorkspaceManager.runGarbageCollectionFromUi()
                                        wsRoot.setNotice(qsTr("Garbage collector purged %1 unreferenced object(s)").arg(removed), "info")
                                        wsRoot.refresh()
                                    }
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                radius: 4; color: "#080B10"; border.color: "#222C38"
                                ListView {
                                    anchors.fill: parent
                                    anchors.margins: 8
                                    clip: true
                                    spacing: 5
                                    model: wsRoot.savesList
                                    delegate: Rectangle {
                                        width: ListView.view.width
                                        height: 58
                                        radius: 4
                                        color: "#0E131A"
                                        border.color: modelData.isAutoSave ? "#8F6B1E" : "#24303D"
                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.margins: 8
                                            spacing: 6
                                            ColumnLayout {
                                                Layout.fillWidth: true; spacing: 2
                                                Label {
                                                    Layout.fillWidth: true; elide: Text.ElideRight
                                                    text: (modelData.name || qsTr("Untitled Save")) + (modelData.isAutoSave ? qsTr("  [AUTO-SAVE]") : "")
                                                    font.pixelSize: 12; font.bold: true
                                                    color: modelData.isAutoSave ? "#F5B841" : "#E4ECF4"
                                                }
                                                Label {
                                                    Layout.fillWidth: true; elide: Text.ElideRight
                                                    text: modelData.saveId + "   •   " + modelData.fileCount + " files   •   Δ " + wsRoot.formatBytes(modelData.incrementalBytes) + "   •   " + modelData.createdUtc
                                                    font.family: "Monospace"; font.pixelSize: 9; color: "#8C9EAE"
                                                }
                                            }
                                            ConsoleButton {
                                                text: qsTr("DIFF A")
                                                implicitWidth: 58; implicitHeight: 28; fontSize: 9
                                                highlighted: wsRoot.diffFromId === modelData.saveId
                                                accentColor: "#00E5FF"
                                                onClicked: wsRoot.diffFromId = modelData.saveId
                                            }
                                            ConsoleButton {
                                                text: qsTr("DIFF B")
                                                implicitWidth: 58; implicitHeight: 28; fontSize: 9
                                                highlighted: wsRoot.diffToId === modelData.saveId
                                                accentColor: "#00E5FF"
                                                onClicked: wsRoot.diffToId = modelData.saveId
                                            }
                                            ConsoleButton {
                                                text: qsTr("AUDITION")
                                                implicitWidth: 74; implicitHeight: 28; fontSize: 9; accentColor: "#33E3FF"
                                                enabled: wsRoot.selectedFileRel.length > 0
                                                onClicked: {
                                                    var p = WorkspaceManager.prepareSavedTrackForAuditionFromUi(modelData.saveId, wsRoot.selectedFileRel)
                                                    if (p.length > 0) {
                                                        EngineController.loadTrack(EngineController.activeDeckId, p)
                                                        wsRoot.setNotice(qsTr("Loaded snapshot track '%1' (%2) to Deck %3").arg(wsRoot.selectedFileRel, modelData.saveId, EngineController.activeDeckId), "info")
                                                    } else {
                                                        wsRoot.setNotice(WorkspaceManager.lastError(), "error")
                                                    }
                                                }
                                            }
                                            ConsoleButton {
                                                text: qsTr("ROLLBACK")
                                                implicitWidth: 78; implicitHeight: 28; fontSize: 9; accentColor: "#F5B841"
                                                onClicked: {
                                                    var r = WorkspaceManager.adoptSaveFromUi(modelData.saveId)
                                                    if (r.success) {
                                                        wsRoot.setNotice(qsTr("Rolled back workspace to %1 (auto-save backup: %2)").arg(modelData.saveId, r.autoSaveId || "-"), "success")
                                                    } else {
                                                        wsRoot.setNotice(r.errorMessage || qsTr("Rollback failed"), "error")
                                                    }
                                                    wsRoot.refresh()
                                                }
                                            }
                                            ConsoleButton {
                                                text: qsTr("DEL")
                                                implicitWidth: 46; implicitHeight: 28; fontSize: 9; accentColor: "#FF5252"
                                                onClicked: {
                                                    WorkspaceManager.deleteSaveFromUi(modelData.saveId)
                                                    wsRoot.refresh()
                                                }
                                            }
                                        }
                                    }
                                    Label {
                                        anchors.centerIn: parent
                                        visible: wsRoot.savesList.length === 0
                                        text: qsTr("No checkpoints saved yet. Create a checkpoint above to enable diff & rollback.")
                                        font.family: "Monospace"; font.pixelSize: 11; color: "#5C6B7C"
                                    }
                                }
                            }

                            // Diff Controls + Output Well
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 170
                                radius: 4; color: "#070A0F"; border.color: "#222C38"
                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 6
                                    RowLayout {
                                        Layout.fillWidth: true; spacing: 8
                                        Label { text: qsTr("DIFF INSPECTOR"); font.family: "Monospace"; font.pixelSize: 10; font.bold: true; color: "#78D8F0" }
                                        DarkField { Layout.fillWidth: true; text: wsRoot.diffFromId; placeholderText: qsTr("From Save ID"); onTextChanged: wsRoot.diffFromId = text }
                                        Label { text: "→"; color: "#8C9EAE" }
                                        DarkField { Layout.fillWidth: true; text: wsRoot.diffToId; placeholderText: qsTr("To Save ID"); onTextChanged: wsRoot.diffToId = text }
                                        ConsoleButton {
                                            text: qsTr("COMPARE SAVES")
                                            implicitWidth: 120; implicitHeight: 28; fontSize: 9; accentColor: "#00E5FF"
                                            onClicked: {
                                                var d = WorkspaceManager.compareSavesFromUi(wsRoot.diffFromId.trim(), wsRoot.diffToId.trim())
                                                if (!d.valid) {
                                                    wsRoot.diffSummaryText = d.errorMessage || qsTr("Select valid From/To checkpoints")
                                                } else {
                                                    var lines = [qsTr("Added: %1  |  Removed: %2  |  Modified: %3  |  Unchanged: %4")
                                                                 .arg(d.addedCount).arg(d.removedCount).arg(d.modifiedCount).arg(d.unchangedCount)]
                                                    for (var i = 0; i < d.diffs.length; ++i) {
                                                        var fd = d.diffs[i]
                                                        lines.push("[" + fd.changeType + "] " + fd.relativePath)
                                                        if (fd.tagDiffs) {
                                                            for (var t = 0; t < fd.tagDiffs.length; ++t) {
                                                                lines.push("    • " + fd.tagDiffs[t].key + ": '" + fd.tagDiffs[t].beforeValue + "' → '" + fd.tagDiffs[t].afterValue + "'")
                                                            }
                                                        }
                                                    }
                                                    wsRoot.diffSummaryText = lines.join("\n")
                                                }
                                            }
                                        }
                                    }
                                    ScrollView {
                                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                                        TextArea {
                                            readOnly: true
                                            text: wsRoot.diffSummaryText.length > 0 ? wsRoot.diffSummaryText : qsTr("Click DIFF A and DIFF B on two checkpoints above, then click COMPARE SAVES.")
                                            font.family: "Monospace"; font.pixelSize: 10
                                            color: wsRoot.diffSummaryText.length > 0 ? "#8FE8F5" : "#5D6E7F"
                                            background: Rectangle { color: "transparent" }
                                        }
                                    }
                                }
                            }
                        }

                        // SUBTAB 2: WORKSPACES REGISTRY & LOCKS
                        ColumnLayout {
                            spacing: 8
                            RowLayout {
                                Layout.fillWidth: true; spacing: 8
                                Label { text: qsTr("REGISTERED WORKSPACES & POSIX FLOCK LOCK STATE"); font.family: "Monospace"; font.pixelSize: 11; font.bold: true; color: "#E8BA6B" }
                                Item { Layout.fillWidth: true }
                                ConsoleButton {
                                    text: qsTr("READ-ONLY MODE")
                                    implicitWidth: 126; implicitHeight: 30; fontSize: 10
                                    onClicked: {
                                        WorkspaceManager.continueInReadOnlyMode()
                                        wsRoot.refresh()
                                    }
                                }
                                ConsoleButton {
                                    text: qsTr("REGISTER NEW...")
                                    implicitWidth: 126; implicitHeight: 30; fontSize: 10; accentColor: "#00E676"
                                    onClicked: openWorkspaceFolderDlg.open()
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true; Layout.fillHeight: true
                                radius: 4; color: "#080B10"; border.color: "#222C38"
                                ListView {
                                    anchors.fill: parent; anchors.margins: 8; clip: true; spacing: 6
                                    model: wsRoot.workspacesList
                                    delegate: Rectangle {
                                        width: ListView.view.width; height: 54; radius: 4
                                        color: "#0E131A"
                                        border.color: modelData.lockState === 1 ? "#247A56" : (modelData.lockState === 2 ? "#B83B44" : "#24303D")
                                        RowLayout {
                                            anchors.fill: parent; anchors.margins: 10; spacing: 8
                                            ColumnLayout {
                                                Layout.fillWidth: true; spacing: 2
                                                Label { text: modelData.path; font.family: "Monospace"; font.pixelSize: 11; font.bold: true; color: "#E4ECF4"; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                                Label {
                                                    text: qsTr("Lock State: %1   •   Last Used: %2")
                                                          .arg(modelData.lockState === 1 ? qsTr("Open (Session)") : (modelData.lockState === 2 ? qsTr("InUse (Other Process)") : qsTr("Locked (Idle)")))
                                                          .arg(modelData.lastUsedUtc || "-")
                                                    font.family: "Monospace"; font.pixelSize: 9
                                                    color: modelData.lockState === 1 ? "#00E676" : (modelData.lockState === 2 ? "#FF6B74" : "#899BAE")
                                                }
                                            }
                                            ConsoleButton {
                                                text: qsTr("SWITCH / OPEN")
                                                implicitWidth: 112; implicitHeight: 30; fontSize: 9; accentColor: "#00E5FF"
                                                enabled: modelData.lockState !== 1 && modelData.lockState !== 2
                                                onClicked: {
                                                    if (!WorkspaceManager.selectWorkspaceAtStartup(modelData.path)) {
                                                        wsRoot.setNotice(WorkspaceManager.lastError(), "error")
                                                    } else {
                                                        wsRoot.setNotice(qsTr("Switched active workspace to %1").arg(modelData.path), "success")
                                                    }
                                                    wsRoot.refresh()
                                                }
                                            }
                                            ConsoleButton {
                                                text: qsTr("UNREGISTER")
                                                implicitWidth: 92; implicitHeight: 30; fontSize: 9; accentColor: "#FF5252"
                                                enabled: modelData.lockState !== 1
                                                onClicked: {
                                                    WorkspaceManager.unregisterWorkspaceFromUi(modelData.path)
                                                    wsRoot.refresh()
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    FolderDialog {
        id: openWorkspaceFolderDlg
        title: qsTr("Select or Register Desk32 Project Workspace Folder")
        onAccepted: {
            var p = selectedFolder.toString().replace(/^file:\/\//, "")
            if (p.length > 0) {
                if (WorkspaceManager.registerAndSelectWorkspaceAtStartup(p)) {
                    wsRoot.setNotice(qsTr("Opened workspace: %1").arg(p), "success")
                } else {
                    wsRoot.setNotice(WorkspaceManager.lastError(), "error")
                }
                wsRoot.refresh()
            }
        }
    }

    FileDialog {
        id: importAudioFileDlg
        title: qsTr("Import Audio File to Open Workspace (SHA-256 Verified)")
        nameFilters: ["Audio Files (*.flac *.wav *.mp3 *.ogg *.opus *.m4a *.aac)", "All Files (*)"]
        onAccepted: {
            var p = selectedFile.toString().replace(/^file:\/\//, "")
            if (p.length > 0) {
                var sub = (wsRoot.activeFolderFilter !== "" && wsRoot.activeFolderFilter !== "/") ? wsRoot.activeFolderFilter : ""
                var res = WorkspaceManager.importFileFromUi(p, sub)
                wsRoot.setNotice(res.message || qsTr("Import completed"), res.status === 0 ? "success" : (res.status === 1 ? "warning" : "error"))
                wsRoot.refresh()
            }
        }
    }

    FolderDialog {
        id: importAudioFolderDlg
        title: qsTr("Import Audio Folder into Open Workspace")
        onAccepted: {
            var p = selectedFolder.toString().replace(/^file:\/\//, "")
            if (p.length > 0) {
                var sub = (wsRoot.activeFolderFilter !== "" && wsRoot.activeFolderFilter !== "/") ? wsRoot.activeFolderFilter : ""
                var results = WorkspaceManager.importFolderFromUi(p, sub) || []
                var okCnt = 0
                for (var i = 0; i < results.length; ++i) {
                    if (results[i].status === 0 || results[i].status === 1) okCnt++
                }
                wsRoot.setNotice(qsTr("Folder import finished: %1/%2 file(s) verified").arg(okCnt).arg(results.length), okCnt > 0 ? "success" : "warning")
                wsRoot.refresh()
            }
        }
    }
}
