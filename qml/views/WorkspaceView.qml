import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Desk32.Core
import "../components"

Item {
    id: wsRoot

    implicitWidth: 1600
    implicitHeight: 900

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

    property string targetDeckId: EngineController.activeDeckId || "A"
    property var deckStates: ({ "A": ({}), "B": ({}), "C": ({}), "D": ({}) })
    property string lockRejectedMsg: ""
    property var sessionDiscardedInfo: null

    function setNotice(msg, sev) {
        statusNotice = msg || ""
        statusSeverity = sev || "info"
    }

    function refreshDeckStates() {
        var next = ({})
        var ids = ["A", "B", "C", "D"]
        for (var i = 0; i < ids.length; ++i) {
            var id = ids[i]
            next[id] = EngineController.getDeckState(id) || ({})
        }
        deckStates = next
    }

    function selectTargetDeck(id) {
        refreshDeckStates()
        var st = deckStates[id]
        if (st && st.isLocked) {
            lockRejectedMsg = qsTr("Cannot select Deck %1: Deck is LOCKED (audio context protected). Unlock Deck %1 in Console first.").arg(id)
            setNotice(lockRejectedMsg, "error")
            return
        }
        lockRejectedMsg = ""
        targetDeckId = id
    }

    function loadTrackRelToTargetDeck(relPath) {
        if (!WorkspaceManager.hasOpenWorkspace || !relPath || relPath.length === 0) {
            return false
        }
        refreshDeckStates()
        var id = targetDeckId || "A"
        var st = deckStates[id]
        if (st && st.isLocked) {
            lockRejectedMsg = qsTr("Load rejected: Deck %1 is LOCKED (audio context protected). Select an unlocked deck or unlock Deck %1 in Console.").arg(id)
            setNotice(lockRejectedMsg, "error")
            return false
        }
        var wasFolder = !!(st && st.folderActive)
        var prevIdx = st ? ((st.folderCurrentIndex || 0) + 1) : 0
        var prevTotal = st ? (st.folderTotalTracks || 0) : 0
        var ok = EngineController.loadWorkspaceTrack(id, relPath)
        refreshDeckStates()
        if (ok) {
            lockRejectedMsg = ""
            if (wasFolder) {
                sessionDiscardedInfo = {
                    deckId: id,
                    trackIndex: prevIdx,
                    totalTracks: prevTotal,
                    loadedFile: relPath
                }
                setNotice(qsTr("Loaded '%1' into Deck %2 — Active Folder Session (%3/%4) ended & discarded (§2.7.5).")
                          .arg(relPath, id).arg(prevIdx).arg(prevTotal), "warning")
            } else {
                sessionDiscardedInfo = null
                setNotice(qsTr("Loaded '%1' into Deck %2").arg(relPath, id), "success")
            }
        } else {
            var stAfter = deckStates[id]
            if (stAfter && stAfter.isLocked) {
                lockRejectedMsg = qsTr("Load rejected: Deck %1 is LOCKED (audio context protected).").arg(id)
                setNotice(lockRejectedMsg, "error")
            } else {
                setNotice(qsTr("Failed to load '%1' into Deck %2").arg(relPath, id), "error")
            }
        }
        return ok
    }

    function loadAbsPathToTargetDeck(absPath, displayName) {
        if (!absPath || absPath.length === 0) return false
        refreshDeckStates()
        var id = targetDeckId || "A"
        var st = deckStates[id]
        if (st && st.isLocked) {
            lockRejectedMsg = qsTr("Load rejected: Deck %1 is LOCKED (audio context protected).").arg(id)
            setNotice(lockRejectedMsg, "error")
            return false
        }
        var wasFolder = !!(st && st.folderActive)
        var prevIdx = st ? ((st.folderCurrentIndex || 0) + 1) : 0
        var prevTotal = st ? (st.folderTotalTracks || 0) : 0
        var ok = EngineController.loadTrack(id, absPath)
        refreshDeckStates()
        if (ok) {
            lockRejectedMsg = ""
            if (wasFolder) {
                sessionDiscardedInfo = {
                    deckId: id,
                    trackIndex: prevIdx,
                    totalTracks: prevTotal,
                    loadedFile: displayName
                }
                setNotice(qsTr("Loaded snapshot '%1' into Deck %2 — Active Folder Session (%3/%4) ended & discarded (§2.7.5).")
                          .arg(displayName, id).arg(prevIdx).arg(prevTotal), "warning")
            } else {
                sessionDiscardedInfo = null
                setNotice(qsTr("Loaded snapshot '%1' into Deck %2").arg(displayName, id), "info")
            }
        } else {
            setNotice(qsTr("Failed to load '%1' into Deck %2").arg(displayName, id), "error")
        }
        return ok
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

    Component.onCompleted: {
        refresh()
        refreshDeckStates()
        var initSt = deckStates[targetDeckId]
        if (initSt && initSt.isLocked) {
            var ids = ["A", "B", "C", "D"]
            for (var i = 0; i < ids.length; ++i) {
                if (deckStates[ids[i]] && !deckStates[ids[i]].isLocked) {
                    targetDeckId = ids[i]
                    break
                }
            }
        }
    }
    onVisibleChanged: if (visible) refreshDeckStates()

    Connections {
        target: WorkspaceManager
        function onWorkspaceStateChanged() { wsRoot.refresh() }
        function onWorkspaceContentsChanged() { wsRoot.refresh() }
        function onExternalModificationDetected() { wsRoot.refresh() }
    }

    Connections {
        target: EngineController
        function onDeckStateChanged() { wsRoot.refreshDeckStates() }
        function onActiveDeckChanged() { wsRoot.refreshDeckStates() }
        function onFolderSessionEnded() { wsRoot.refreshDeckStates() }
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
        color: "#080a0e"
    }

    Rectangle {
        id: wsFrame
        anchors.centerIn: parent
        width: parent && parent.width > 0 ? Math.min(parent.width, 1600) : 1600
        height: parent && parent.height > 0 ? Math.min(parent.height, 900) : 900
        color: "#0e1116"
        border.color: "#252e3a"
        border.width: 1
        radius: 6
        clip: true

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
                    Layout.fillWidth: true
                    Layout.minimumWidth: 260
                    Layout.maximumWidth: Math.max(260, Math.min(640, Math.max(wsTitleLbl.implicitWidth, wsSubLbl.implicitWidth) + 24))
                    Layout.preferredHeight: 40
                    height: 40
                    radius: 4
                    color: WorkspaceManager.hasOpenWorkspace ? "#091E16" : "#22180C"
                    border.color: WorkspaceManager.hasOpenWorkspace ? "#247A56" : "#7C5B21"
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 11
                        anchors.rightMargin: 11
                        anchors.topMargin: 4
                        anchors.bottomMargin: 4
                        spacing: 1
                        Label {
                            id: wsTitleLbl
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            elide: Text.ElideRight
                            text: WorkspaceManager.hasOpenWorkspace
                                  ? qsTr("WORKSPACE OPEN (LOCKED TO SESSION)")
                                  : (WorkspaceManager.selectionFinalized
                                     ? qsTr("READ-ONLY MODE (RESTART REQUIRED)")
                                     : qsTr("READ-ONLY MODE (UNSELECTED)"))
                            font.family: "Monospace"; font.pixelSize: 10; font.bold: true
                            color: WorkspaceManager.hasOpenWorkspace ? "#00E676" : "#F5B841"
                        }
                        Label {
                            id: wsSubLbl
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: WorkspaceManager.hasOpenWorkspace
                                  ? WorkspaceManager.openWorkspacePath
                                  : (WorkspaceManager.selectionFinalized
                                     ? qsTr("Session workspace selection finalized — restart Desk32 to open or switch workspaces")
                                     : qsTr("Register or open 1 workspace during this session to enable COW import, tagging & checkpoints"))
                            font.family: "Monospace"; font.pixelSize: 10
                            color: "#AFC0D0"
                            elide: Text.ElideMiddle
                        }
                    }
                }

                // Store Stats + External Modification Guard
                Rectangle {
                    visible: WorkspaceManager.hasOpenWorkspace
                    implicitWidth: storeStatCol.implicitWidth + 18
                    Layout.preferredHeight: 40
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
                    text: qsTr("REGISTER & OPEN...")
                    implicitWidth: 146; implicitHeight: 34; fontSize: 10; accentColor: "#E8BA6B"
                    enabled: WorkspaceManager.canOpenWorkspaceInSession
                    toolTipText: WorkspaceManager.canOpenWorkspaceInSession
                                 ? qsTr("Register and open a workspace folder (allowed once per session)")
                                 : qsTr("Session workspace already finalized — restart Desk32 to switch workspaces")
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
            Layout.preferredHeight: 28
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
            // LEFT PANE (43%): FOLDER TREE SIDEBAR + WORKSPACE TRACK LIST
            // -----------------------------------------------------------------
            BrushedMetalPanel {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.preferredWidth: 670
                Layout.fillHeight: true
                clip: true
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
                            Layout.preferredWidth: 220
                            placeholderText: qsTr("Filter filename, title, artist...")
                            onTextChanged: {
                                wsRoot.searchFilter = text
                                wsRoot.applyFilters()
                            }
                        }
                    }

                    // Unified Target Deck (A-D) Selector + Prominent Primary Load CTA + Session Warning/Discarded Bar
                    Rectangle {
                        id: targetDeckPanel
                        Layout.fillWidth: true
                        Layout.preferredHeight: 98
                        implicitHeight: 98
                        radius: 5
                        color: "#090D13"
                        border.color: "#283444"

                        readonly property var tgtSt: wsRoot.deckStates[wsRoot.targetDeckId] || ({})
                        readonly property bool tgtLocked: !!(tgtSt && tgtSt.isLocked)
                        readonly property bool tgtFolder: !!(tgtSt && tgtSt.folderActive)
                        readonly property bool canLoadNow: WorkspaceManager.hasOpenWorkspace
                                                           && wsRoot.selectedFileRel.length > 0
                                                           && !tgtLocked

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 6

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6

                                Label {
                                    text: qsTr("TARGET DECK")
                                    font.family: "Monospace"; font.pixelSize: 9; font.bold: true; color: "#8FA4B8"
                                }

                                Repeater {
                                    model: ["A", "B", "C", "D"]
                                    Rectangle {
                                        id: deckPill
                                        readonly property string dId: modelData
                                        readonly property var st: wsRoot.deckStates[dId] || ({})
                                        readonly property bool isSel: wsRoot.targetDeckId === dId
                                        readonly property bool locked: !!(st && st.isLocked)
                                        readonly property bool folder: !!(st && st.folderActive)
                                        readonly property bool loaded: !!(st && st.path && st.path.length > 0)

                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 46
                                        height: 46
                                        radius: 4
                                        color: locked
                                               ? "#241115"
                                               : (isSel ? "#12293A" : (deckMouse.containsMouse ? "#151C26" : "#0D1219"))
                                        border.color: locked
                                                      ? "#C8404A"
                                                      : (isSel ? "#00E5FF" : (folder ? "#F5B841" : "#293545"))
                                        border.width: isSel && !locked ? 2 : 1

                                        ColumnLayout {
                                            anchors.centerIn: parent
                                            spacing: 2
                                            RowLayout {
                                                Layout.alignment: Qt.AlignHCenter
                                                spacing: 5
                                                Rectangle {
                                                    Layout.preferredWidth: 7; Layout.preferredHeight: 7
                                                    width: 7; height: 7; radius: 3.5
                                                    color: deckPill.locked
                                                           ? "#FF5252"
                                                           : (deckPill.folder
                                                              ? "#F5B841"
                                                              : (deckPill.isSel ? "#00E5FF" : (deckPill.loaded ? "#00E676" : "#3E4A59")))
                                                }
                                                Label {
                                                    text: qsTr("DECK %1").arg(deckPill.dId)
                                                    font.family: "Monospace"; font.pixelSize: 11; font.bold: true
                                                    color: deckPill.locked ? "#FF8A8A" : (deckPill.isSel ? "#EBF8FF" : "#C5D2DF")
                                                }
                                            }
                                            Label {
                                                Layout.alignment: Qt.AlignHCenter
                                                text: deckPill.locked
                                                      ? qsTr("🔒 LOCKED")
                                                      : (deckPill.folder
                                                         ? qsTr("FOLDER %1/%2").arg((deckPill.st.folderCurrentIndex || 0) + 1).arg(deckPill.st.folderTotalTracks || 0)
                                                         : (deckPill.loaded ? qsTr("LOADED") : qsTr("EMPTY")))
                                                font.family: "Monospace"; font.pixelSize: 8; font.bold: deckPill.locked || deckPill.folder
                                                color: deckPill.locked ? "#FF6B74" : (deckPill.folder ? "#F5B841" : (deckPill.loaded ? "#78D8F0" : "#677584"))
                                            }
                                        }

                                        MouseArea {
                                            id: deckMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: deckPill.locked ? Qt.ForbiddenCursor : Qt.PointingHandCursor
                                            onClicked: wsRoot.selectTargetDeck(deckPill.dId)
                                        }
                                    }
                                }

                                // Large, High-Contrast Primary LOAD Button
                                Rectangle {
                                    id: primaryLoadBtn
                                    readonly property bool activeOk: targetDeckPanel.canLoadNow
                                    readonly property bool tgtLockedNow: targetDeckPanel.tgtLocked
                                    Layout.preferredWidth: 210
                                    Layout.preferredHeight: 46
                                    height: 46
                                    radius: 5
                                    border.width: activeOk ? 2 : 1
                                    border.color: tgtLockedNow
                                                  ? "#C8404A"
                                                  : (activeOk ? "#00E676" : "#2D3846")
                                    gradient: Gradient {
                                        GradientStop {
                                            position: 0.0
                                            color: primaryLoadBtn.tgtLockedNow
                                                   ? "#3A161B"
                                                   : (primaryLoadBtn.activeOk
                                                      ? (primaryLoadMouse.pressed ? "#186442" : (primaryLoadMouse.containsMouse ? "#289A68" : "#1F7D54"))
                                                      : "#171D26")
                                        }
                                        GradientStop {
                                            position: 1.0
                                            color: primaryLoadBtn.tgtLockedNow
                                                   ? "#210C0F"
                                                   : (primaryLoadBtn.activeOk
                                                      ? (primaryLoadMouse.pressed ? "#0D3B27" : "#0F462F")
                                                      : "#0F1319")
                                        }
                                    }

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 12; anchors.rightMargin: 12
                                        spacing: 1
                                        Item { Layout.fillHeight: true }
                                        Label {
                                            Layout.alignment: Qt.AlignHCenter
                                            text: primaryLoadBtn.tgtLockedNow
                                                  ? qsTr("🔒 DECK %1 LOCKED").arg(wsRoot.targetDeckId)
                                                  : qsTr("▶  LOAD TO DECK %1").arg(wsRoot.targetDeckId)
                                            font.family: "Monospace"; font.pixelSize: 13; font.bold: true
                                            color: primaryLoadBtn.tgtLockedNow
                                                   ? "#FF8A8A"
                                                   : (primaryLoadBtn.activeOk ? "#E8FFF4" : "#5F6E7E")
                                        }
                                        Label {
                                            Layout.fillWidth: true; Layout.minimumWidth: 0
                                            horizontalAlignment: Text.AlignHCenter
                                            elide: Text.ElideMiddle
                                            text: wsRoot.selectedFileRel.length > 0
                                                  ? wsRoot.selectedFileRel.split('/').pop()
                                                  : qsTr("Select a file below")
                                            font.family: "Monospace"; font.pixelSize: 9
                                            color: primaryLoadBtn.activeOk ? "#A6F4CC" : "#4E5C6B"
                                        }
                                        Item { Layout.fillHeight: true }
                                    }

                                    MouseArea {
                                        id: primaryLoadMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: primaryLoadBtn.tgtLockedNow
                                                     ? Qt.ForbiddenCursor
                                                     : (primaryLoadBtn.activeOk ? Qt.PointingHandCursor : Qt.ArrowCursor)
                                        onClicked: wsRoot.loadTrackRelToTargetDeck(wsRoot.selectedFileRel)
                                    }
                                }
                            }

                            // Target Deck Context / Lock Protection / Folder Session Warning & Discarded Display Strip
                            Rectangle {
                                id: targetCtxStrip
                                readonly property bool hasDiscarded: wsRoot.sessionDiscardedInfo !== null
                                readonly property bool hasLockErr: wsRoot.lockRejectedMsg.length > 0 || targetDeckPanel.tgtLocked
                                readonly property bool hasFolderWarn: !hasDiscarded && !hasLockErr && targetDeckPanel.tgtFolder

                                Layout.fillWidth: true
                                Layout.preferredHeight: 30
                                height: 30
                                radius: 4
                                color: hasLockErr
                                       ? "#2D1216"
                                       : ((hasDiscarded || hasFolderWarn) ? "#2B200B" : "#070A0F")
                                border.color: hasLockErr
                                              ? "#FF5252"
                                              : ((hasDiscarded || hasFolderWarn) ? "#F5B841" : "#1F2935")

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 8; anchors.rightMargin: 8
                                    spacing: 8

                                    Rectangle {
                                        implicitWidth: ctxPillLbl.implicitWidth + 10
                                        Layout.preferredHeight: 18
                                        height: 18; radius: 2
                                        color: targetCtxStrip.hasLockErr
                                               ? "#4D171D"
                                               : ((targetCtxStrip.hasDiscarded || targetCtxStrip.hasFolderWarn) ? "#47330E" : "#12212E")
                                        border.color: targetCtxStrip.hasLockErr
                                                      ? "#FF6B74"
                                                      : ((targetCtxStrip.hasDiscarded || targetCtxStrip.hasFolderWarn) ? "#F5B841" : "#2D6A84")
                                        Label {
                                            id: ctxPillLbl
                                            anchors.centerIn: parent
                                            font.family: "Monospace"; font.pixelSize: 8; font.bold: true
                                            color: targetCtxStrip.hasLockErr
                                                   ? "#FFB4B8"
                                                   : ((targetCtxStrip.hasDiscarded || targetCtxStrip.hasFolderWarn) ? "#FFE099" : "#78D8F0")
                                            text: targetCtxStrip.hasDiscarded
                                                  ? qsTr("SESSION DISCARDED")
                                                  : (targetCtxStrip.hasLockErr
                                                     ? qsTr("LOCK PROTECTED")
                                                     : (targetCtxStrip.hasFolderWarn
                                                        ? qsTr("FOLDER SESSION ACTIVE")
                                                        : qsTr("DECK %1 READY").arg(wsRoot.targetDeckId)))
                                        }
                                    }

                                    Label {
                                        Layout.fillWidth: true; Layout.minimumWidth: 0
                                        elide: Text.ElideMiddle
                                        font.family: "Monospace"; font.pixelSize: 10
                                        color: targetCtxStrip.hasLockErr
                                               ? "#FFD2D5"
                                               : ((targetCtxStrip.hasDiscarded || targetCtxStrip.hasFolderWarn) ? "#FFE5AD" : "#B8C7D6")
                                        text: {
                                            if (targetCtxStrip.hasDiscarded) {
                                                var d = wsRoot.sessionDiscardedInfo
                                                return qsTr("Deck %1 Folder Session (%2/%3) ended & discarded (§2.7.5) — Replaced by '%4'")
                                                       .arg(d.deckId).arg(d.trackIndex).arg(d.totalTracks).arg(d.loadedFile)
                                            }
                                            if (wsRoot.lockRejectedMsg.length > 0) return wsRoot.lockRejectedMsg
                                            if (targetDeckPanel.tgtLocked) {
                                                return qsTr("Deck %1 is LOCKED — Protected audio context cannot be overwritten. Select an unlocked deck.").arg(wsRoot.targetDeckId)
                                            }
                                            if (targetDeckPanel.tgtFolder) {
                                                var st = targetDeckPanel.tgtSt
                                                return qsTr("Deck %1 is running a Folder Session (%2/%3) — Loading here will end & discard that session (§2.7.5).")
                                                       .arg(wsRoot.targetDeckId).arg((st.folderCurrentIndex || 0) + 1).arg(st.folderTotalTracks || 0)
                                            }
                                            var curP = (targetDeckPanel.tgtSt && targetDeckPanel.tgtSt.path) ? targetDeckPanel.tgtSt.path : ""
                                            return curP.length > 0
                                                   ? qsTr("Current Deck %1 track: %2").arg(wsRoot.targetDeckId, curP.split('/').pop())
                                                   : qsTr("Deck %1 is empty — double-click a file or press LOAD TO DECK %1").arg(wsRoot.targetDeckId)
                                        }
                                    }

                                    ConsoleButton {
                                        visible: targetCtxStrip.hasDiscarded || wsRoot.lockRejectedMsg.length > 0
                                        text: "✕"
                                        implicitWidth: 22; implicitHeight: 20; fontSize: 9
                                        onClicked: {
                                            wsRoot.sessionDiscardedInfo = null
                                            wsRoot.lockRejectedMsg = ""
                                        }
                                    }
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: 8

                        // Sub-column 1: Folder Tree Panel
                        Rectangle {
                            Layout.preferredWidth: 185
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
                                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                                delegate: Rectangle {
                                    width: ListView.view.width - 10
                                    height: 54
                                    radius: 4
                                    clip: true
                                    readonly property bool isSel: wsRoot.selectedFileRel === modelData.relativePath
                                    color: isSel ? "#132634" : (modelData.externallyModified ? "#241216" : "#0E131A")
                                    border.color: isSel ? "#00E5FF" : (modelData.externallyModified ? "#B83B44" : "#222B37")

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 10; anchors.rightMargin: 10
                                        spacing: 8

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            spacing: 2
                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 6
                                                Label {
                                                    Layout.maximumWidth: 260
                                                    Layout.minimumWidth: 0
                                                    text: modelData.fileName || modelData.relativePath
                                                    font.pixelSize: 12; font.bold: true; color: "#E6EEF6"
                                                    elide: Text.ElideRight
                                                }
                                                Rectangle {
                                                    implicitWidth: provLbl.implicitWidth + 8; Layout.preferredHeight: 16; height: 16; radius: 2
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
                                                    implicitWidth: extModLbl.implicitWidth + 8; Layout.preferredHeight: 16; height: 16; radius: 2
                                                    color: "#361217"; border.color: "#FF5252"
                                                    Label {
                                                        id: extModLbl; anchors.centerIn: parent
                                                        text: qsTr("EXT MODIFIED")
                                                        font.family: "Monospace"; font.pixelSize: 8; font.bold: true; color: "#FF8A8A"
                                                    }
                                                }
                                                Item { Layout.fillWidth: true }
                                            }
                                            Label {
                                                Layout.fillWidth: true
                                                Layout.minimumWidth: 0
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
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        z: -1
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: wsRoot.loadFileInspector(modelData.relativePath)
                                        onDoubleClicked: {
                                            wsRoot.loadFileInspector(modelData.relativePath)
                                            wsRoot.loadTrackRelToTargetDeck(modelData.relativePath)
                                        }
                                    }
                                }

                                Label {
                                    anchors.centerIn: parent
                                    visible: wsRoot.filteredFilesList.length === 0
                                    text: WorkspaceManager.hasOpenWorkspace
                                          ? qsTr("No matching tracks in workspace folder")
                                          : (WorkspaceManager.selectionFinalized
                                             ? qsTr("Session is in finalized Read-Only mode — restart Desk32 to open a workspace")
                                             : qsTr("Register or open a workspace folder (once per session) to view managed project files"))
                                    font.family: "Monospace"; font.pixelSize: 11; color: "#5C6B7C"
                                }
                            }
                        }
                    }
                }
            }

            // -----------------------------------------------------------------
            // RIGHT PANE (57%): PROVENANCE/TAGLIB COW | CHECKPOINTS & DIFF | REGISTRY
            // -----------------------------------------------------------------
            BrushedMetalPanel {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.preferredWidth: 900
                Layout.fillHeight: true
                clip: true
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

                                        DarkField { id: moveFileDirField; Layout.preferredWidth: 120; placeholderText: qsTr("Target subdir...") }
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
                                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                                    delegate: Rectangle {
                                        width: ListView.view.width - 10
                                        height: 58
                                        radius: 4
                                        color: "#0E131A"
                                        border.color: modelData.isAutoSave ? "#8F6B1E" : "#24303D"
                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.margins: 8
                                            spacing: 6
                                            ColumnLayout {
                                                Layout.fillWidth: true; Layout.minimumWidth: 0; spacing: 2
                                                Label {
                                                    Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight
                                                    text: (modelData.name || qsTr("Untitled Save")) + (modelData.isAutoSave ? qsTr("  [AUTO-SAVE]") : "")
                                                    font.pixelSize: 12; font.bold: true
                                                    color: modelData.isAutoSave ? "#F5B841" : "#E4ECF4"
                                                }
                                                Label {
                                                    Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight
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
                                                toolTipText: qsTr("Audition checkpoint snapshot on Deck %1").arg(wsRoot.targetDeckId)
                                                onClicked: {
                                                    var p = WorkspaceManager.prepareSavedTrackForAuditionFromUi(modelData.saveId, wsRoot.selectedFileRel)
                                                    if (p.length > 0) {
                                                        wsRoot.loadAbsPathToTargetDeck(p, wsRoot.selectedFileRel + " [" + modelData.saveId + "]")
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
                                        contentWidth: availableWidth
                                        TextArea {
                                            readOnly: true
                                            wrapMode: TextEdit.Wrap
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
                                Rectangle {
                                    implicitWidth: sessionRuleLbl.implicitWidth + 16
                                    Layout.preferredHeight: 28
                                    height: 28
                                    radius: 4
                                    color: WorkspaceManager.canOpenWorkspaceInSession ? "#1A160B" : "#111821"
                                    border.color: WorkspaceManager.canOpenWorkspaceInSession ? "#7C5B21" : "#2D4257"
                                    Label {
                                        id: sessionRuleLbl
                                        anchors.centerIn: parent
                                        font.family: "Monospace"; font.pixelSize: 9; font.bold: true
                                        color: WorkspaceManager.canOpenWorkspaceInSession ? "#F5B841" : "#78D8F0"
                                        text: WorkspaceManager.canOpenWorkspaceInSession
                                              ? qsTr("1 WORKSPACE OPEN ALLOWED IN SESSION")
                                              : qsTr("RESTART REQUIRED TO SWITCH WORKSPACE")
                                    }
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
                                                text: modelData.lockState === 1
                                                      ? qsTr("ACTIVE")
                                                      : (modelData.lockState === 2 ? qsTr("IN USE") : qsTr("OPEN"))
                                                implicitWidth: 96; implicitHeight: 30; fontSize: 9; accentColor: "#00E5FF"
                                                enabled: WorkspaceManager.canOpenWorkspaceInSession && modelData.lockState === 0
                                                toolTipText: modelData.lockState === 2
                                                             ? qsTr("In use by another Desk32 process")
                                                             : (!WorkspaceManager.canOpenWorkspaceInSession && modelData.lockState !== 1
                                                                ? qsTr("Restart Desk32 to switch or open another workspace")
                                                                : qsTr("Open this workspace for the remainder of the session"))
                                                onClicked: {
                                                    if (!WorkspaceManager.openRegisteredWorkspace(modelData.path)) {
                                                        wsRoot.setNotice(WorkspaceManager.lastError(), "error")
                                                    } else {
                                                        wsRoot.setNotice(qsTr("Opened workspace %1 for this session").arg(modelData.path), "success")
                                                    }
                                                    wsRoot.refresh()
                                                }
                                            }
                                            ConsoleButton {
                                                text: qsTr("UNREGISTER")
                                                implicitWidth: 92; implicitHeight: 30; fontSize: 9; accentColor: "#FF5252"
                                                enabled: modelData.lockState !== 2
                                                onClicked: {
                                                    unregisterConfirmPopup.targetPath = modelData.path
                                                    unregisterConfirmPopup.isCurrentlyOpen = (modelData.lockState === 1)
                                                    unregisterConfirmPopup.open()
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
    }

    ScrollableModalPopup {
        id: unregisterConfirmPopup
        property string targetPath: ""
        property bool isCurrentlyOpen: false
        titleText: qsTr("UNREGISTER WORKSPACE?")
        preferredWidth: 500
        preferredHeight: 280
        accentColor: "#FF5252"

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.pixelSize: 12
            color: "#DCE6F0"
            text: qsTr("Remove this folder from Desk32's Registered Workspaces list? Audio files and the .desk32 folder on disk will NOT be deleted.")
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: unregTargetLbl.implicitHeight + 14
            radius: 4
            color: "#090C11"
            border.color: "#2B3543"
            Label {
                id: unregTargetLbl
                anchors.fill: parent
                anchors.margins: 7
                wrapMode: Text.WrapAnywhere
                font.family: "Monospace"
                font.pixelSize: 10
                color: "#00E5FF"
                text: unregisterConfirmPopup.targetPath
            }
        }

        Label {
            visible: unregisterConfirmPopup.isCurrentlyOpen
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.pixelSize: 11
            font.bold: true
            color: "#F5B841"
            text: qsTr("Warning: This workspace is currently open. Unregistering will stop playback from it, switch this session to Read-Only mode, and require restarting Desk32 to open another workspace.")
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Item { Layout.fillWidth: true }
            ConsoleButton {
                text: qsTr("CANCEL")
                implicitWidth: 96; implicitHeight: 32; fontSize: 10
                onClicked: unregisterConfirmPopup.close()
            }
            ConsoleButton {
                text: qsTr("CONFIRM UNREGISTER")
                implicitWidth: 160; implicitHeight: 32; fontSize: 10
                accentColor: "#FF5252"
                anodizedFill: true; anodizedTop: "#B33339"; anodizedBottom: "#61171B"; anodizedText: "#FFE8EA"
                onClicked: {
                    var p = unregisterConfirmPopup.targetPath
                    unregisterConfirmPopup.close()
                    if (WorkspaceManager.unregisterWorkspaceFromUi(p)) {
                        wsRoot.setNotice(qsTr("Unregistered workspace: %1").arg(p), "success")
                    } else {
                        wsRoot.setNotice(WorkspaceManager.lastError(), "error")
                    }
                    wsRoot.refresh()
                }
            }
        }
    }

    FolderDialog {
        id: openWorkspaceFolderDlg
        title: qsTr("Register & Open Dedicated Workspace Folder")
        onAccepted: {
            var p = decodeURIComponent(selectedFolder.toString().replace(/^file:\/\//, ""))
            if (p.length > 0) {
                if (WorkspaceManager.registerAndOpenWorkspaceFromUi(p)) {
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
            var p = decodeURIComponent(selectedFile.toString().replace(/^file:\/\//, ""))
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
            var p = decodeURIComponent(selectedFolder.toString().replace(/^file:\/\//, ""))
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
