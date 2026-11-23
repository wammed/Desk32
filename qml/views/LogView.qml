import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Desk32.Core
import "../components"

Item {
    id: logViewRoot

    property var rawLogs: []
    property var treeRows: []
    property var expandedOps: ({})
    property var selectedLog: null
    property var relatedLogs: []
    property bool playbackOnlyMode: false
    property string categoryFilter: "All"
    property string statusFilter: "All"
    property string searchFilter: ""
    property int limitCount: 500
    property string exportFeedbackText: ""

    function formatPrettyJson(raw) {
        if (!raw || raw.length === 0) return "{}"
        try {
            return JSON.stringify(JSON.parse(raw), null, 2)
        } catch (e) {
            return raw
        }
    }

    function statusColor(status) {
        if (status === "Success") return "#00E676"
        if (status === "Info") return "#33E3FF"
        if (status === "Warning") return "#FFB300"
        if (status === "Failed") return "#FF5252"
        return "#9AA9B8"
    }

    function rebuildTreeRows() {
        var logs = rawLogs || []
        var byOpId = ({})
        var childrenMap = ({})

        for (var i = 0; i < logs.length; ++i) {
            var ev = logs[i]
            var oid = ev.operationId || ("ev_" + ev.eventId)
            if (!byOpId[oid]) byOpId[oid] = ev
            var pid = ev.parentOperationId || ""
            if (pid.length > 0 && pid !== oid) {
                if (!childrenMap[pid]) childrenMap[pid] = []
                childrenMap[pid].push(ev)
            }
        }

        var flat = []
        var visited = ({})

        function appendNode(ev, depth) {
            var rowKey = String(ev.eventId) + ":" + (ev.operationId || "")
            if (visited[rowKey] || depth > 4) return
            visited[rowKey] = true

            var oid = ev.operationId || ""
            var kids = (oid.length > 0 && childrenMap[oid]) ? childrenMap[oid] : []
            var isExpanded = !!expandedOps[oid]
            flat.push({
                event: ev,
                depth: depth,
                hasChildren: kids.length > 0,
                childCount: kids.length,
                expanded: isExpanded
            })
            if (isExpanded && kids.length > 0) {
                for (var c = 0; c < kids.length; ++c) {
                    appendNode(kids[c], depth + 1)
                }
            }
        }

        for (var j = 0; j < logs.length; ++j) {
            var row = logs[j]
            var p = row.parentOperationId || ""
            if (p.length === 0 || !byOpId[p]) {
                appendNode(row, 0)
            }
        }

        treeRows = flat
    }

    function toggleExpand(opId) {
        if (!opId || opId.length === 0) return
        var copy = Object.assign({}, expandedOps)
        copy[opId] = !copy[opId]
        expandedOps = copy
        rebuildTreeRows()
    }

    function selectEvent(ev) {
        selectedLog = ev
        if (ev && ev.operationId && ev.operationId.length > 0) {
            relatedLogs = LogService.fetchRelatedLogs(ev.operationId) || []
        } else {
            relatedLogs = []
        }
    }

    function buildFilterMap() {
        var f = { limit: limitCount }
        if (playbackOnlyMode) f.playbackHistoryOnly = true
        if (categoryFilter !== "All") f.category = categoryFilter
        if (statusFilter !== "All") f.status = statusFilter
        if (searchFilter.trim().length > 0) f.searchText = searchFilter.trim()
        return f
    }

    function refresh() {
        rawLogs = LogService.fetchLogs(buildFilterMap()) || []
        rebuildTreeRows()
        if (!selectedLog && rawLogs.length > 0) {
            selectEvent(rawLogs[0])
        }
    }

    Component.onCompleted: refresh()
    onVisibleChanged: if (visible) refresh()

    Timer {
        id: logRefreshDebounce
        interval: 150
        repeat: false
        onTriggered: {
            if (logViewRoot.visible) logViewRoot.refresh()
        }
    }

    Connections {
        target: LogService
        function onLogAppended() {
            if (logViewRoot.visible) logRefreshDebounce.restart()
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
        // TOP FILTER & AUDIT TOOLBAR
        // =====================================================================
        BrushedMetalPanel {
            Layout.fillWidth: true
            Layout.preferredHeight: 60
            baseTint: "#14181f"
            borderColor: "#333c4a"
            showScrews: true
            cornerRadius: 5

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                spacing: 8

                Label {
                    text: qsTr("AUDIT LOGS")
                    font.family: "Monospace"; font.pixelSize: 12; font.bold: true; color: "#E8BA6B"
                }

                ConsoleComboBox {
                    Layout.preferredWidth: 146
                    model: ["All", "Session", "AudioEngine", "Deck", "Transport", "FolderSession", "Playback", "DSP", "ExternalInsert", "Render", "Workspace", "MasterControl", "Preset", "Settings"]
                    onActivated: {
                        logViewRoot.categoryFilter = currentText
                        logViewRoot.refresh()
                    }
                }

                ConsoleComboBox {
                    Layout.preferredWidth: 112
                    model: ["All", "Success", "Info", "Warning", "Failed"]
                    onActivated: {
                        logViewRoot.statusFilter = currentText
                        logViewRoot.refresh()
                    }
                }

                ConsoleButton {
                    text: qsTr("PLAYBACK HIST")
                    implicitWidth: 118; implicitHeight: 30; fontSize: 10
                    highlighted: logViewRoot.playbackOnlyMode
                    accentColor: "#33E3FF"
                    onClicked: {
                        logViewRoot.playbackOnlyMode = !logViewRoot.playbackOnlyMode
                        logViewRoot.refresh()
                    }
                }

                TextField {
                    Layout.fillWidth: true
                    implicitHeight: 30
                    placeholderText: qsTr("Search message, target, or operation ID...")
                    color: "#DCE6F0"
                    placeholderTextColor: "#5C6B7C"
                    font.pixelSize: 11
                    leftPadding: 8; rightPadding: 8
                    background: Rectangle {
                        radius: 4
                        color: "#080B10"
                        border.color: parent.activeFocus ? "#33E3FF" : "#2E3846"
                    }
                    onTextChanged: {
                        logViewRoot.searchFilter = text
                        logViewRoot.refresh()
                    }
                }

                ConsoleComboBox {
                    Layout.preferredWidth: 112
                    model: ["200", "500", "1000"]
                    currentIndex: 1
                    onActivated: {
                        logViewRoot.limitCount = parseInt(currentText, 10) || 500
                        logViewRoot.refresh()
                    }
                }

                ConsoleButton {
                    text: qsTr("REFRESH")
                    implicitWidth: 84; implicitHeight: 30; fontSize: 10; accentColor: "#00E5FF"
                    onClicked: logViewRoot.refresh()
                }

                ConsoleButton {
                    text: qsTr("EXPORT JSONL...")
                    implicitWidth: 126; implicitHeight: 30; fontSize: 10; accentColor: "#00E676"
                    onClicked: exportJsonlDlg.open()
                }
            }
        }

        // =====================================================================
        // SPLIT BODY: COLUMN-HEADED TREE TABLE (64%) + INSPECTOR (36%)
        // =====================================================================
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 10

            // LEFT: COLUMN-HEADED PARENT/CHILD OPERATION TREE TABLE
            BrushedMetalPanel {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.preferredWidth: 1620
                Layout.fillHeight: true
                clip: true
                baseTint: "#13171d"
                borderColor: "#2f3743"
                showScrews: true
                cornerRadius: 5

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 4

                    // Column Header Row
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 28
                        Layout.minimumHeight: 28
                        height: 28
                        radius: 3
                        color: "#0A0D12"
                        border.color: "#293342"

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10; anchors.rightMargin: 20
                            spacing: 8
                            Label { Layout.fillWidth: false; Layout.preferredWidth: 176; Layout.minimumWidth: 176; Layout.maximumWidth: 176; text: qsTr("TIMESTAMP (UTC)"); font.family: "Monospace"; font.pixelSize: 10; font.bold: true; color: "#7F94A8"; elide: Text.ElideRight }
                            Label { Layout.fillWidth: false; Layout.preferredWidth: 112; Layout.minimumWidth: 112; Layout.maximumWidth: 112; text: qsTr("CATEGORY"); font.family: "Monospace"; font.pixelSize: 10; font.bold: true; color: "#7F94A8"; elide: Text.ElideRight }
                            Label { Layout.fillWidth: false; Layout.preferredWidth: 148; Layout.minimumWidth: 148; Layout.maximumWidth: 148; text: qsTr("OPERATION"); font.family: "Monospace"; font.pixelSize: 10; font.bold: true; color: "#7F94A8"; elide: Text.ElideRight }
                            Label { Layout.fillWidth: false; Layout.preferredWidth: 76;  Layout.minimumWidth: 76;  Layout.maximumWidth: 76;  text: qsTr("STATUS"); font.family: "Monospace"; font.pixelSize: 10; font.bold: true; color: "#7F94A8"; elide: Text.ElideRight }
                            Label { Layout.fillWidth: false; Layout.preferredWidth: 176; Layout.minimumWidth: 176; Layout.maximumWidth: 176; text: qsTr("TARGET"); font.family: "Monospace"; font.pixelSize: 10; font.bold: true; color: "#7F94A8"; elide: Text.ElideRight }
                            Label { Layout.fillWidth: false; Layout.preferredWidth: 68;  Layout.minimumWidth: 68;  Layout.maximumWidth: 68;  text: qsTr("DUR(ms)"); font.family: "Monospace"; font.pixelSize: 10; font.bold: true; color: "#7F94A8"; horizontalAlignment: Text.AlignRight }
                            Label { Layout.fillWidth: true;  Layout.minimumWidth: 0;   leftPadding: 8; text: qsTr("MESSAGE"); font.family: "Monospace"; font.pixelSize: 10; font.bold: true; color: "#7F94A8"; elide: Text.ElideRight }
                        }
                    }

                    // Tree Rows List
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: 3
                        color: "#070A0F"
                        border.color: "#1E2632"

                        ListView {
                            id: logListView
                            anchors.fill: parent
                            anchors.margins: 4
                            clip: true
                            spacing: 2
                            model: logViewRoot.treeRows
                            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                            delegate: Rectangle {
                                width: logListView.width - 12
                                height: 28
                                radius: 2
                                clip: true
                                readonly property var ev: modelData.event
                                readonly property bool isSelected: logViewRoot.selectedLog && logViewRoot.selectedLog.eventId === ev.eventId
                                color: isSelected ? "#13293A" : (index % 2 === 0 ? "#0B0F15" : "#0E131B")
                                border.color: isSelected ? "#00E5FF" : "transparent"

                                // Indentation guide bar for child events
                                Rectangle {
                                    visible: modelData.depth > 0
                                    x: 6 + (modelData.depth - 1) * 18
                                    y: 0; width: 2; height: parent.height
                                    color: "#225D73"
                                }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 6 + modelData.depth * 18
                                    anchors.rightMargin: 8
                                    spacing: 8

                                    // Tree Expand Chevron + Timestamp
                                    RowLayout {
                                        readonly property int colW: Math.max(80, 176 - modelData.depth * 18)
                                        Layout.fillWidth: false
                                        Layout.preferredWidth: colW
                                        Layout.minimumWidth: colW
                                        Layout.maximumWidth: colW
                                        spacing: 4
                                        Rectangle {
                                            visible: modelData.hasChildren
                                            Layout.preferredWidth: 16
                                            Layout.minimumWidth: 16
                                            Layout.preferredHeight: 16
                                            Layout.alignment: Qt.AlignVCenter
                                            width: 16; height: 16; radius: 2
                                            color: "#162432"; border.color: "#316882"
                                            Label {
                                                anchors.centerIn: parent
                                                text: modelData.expanded ? "−" : "+"
                                                font.family: "Monospace"; font.pixelSize: 11; font.bold: true; color: "#33E3FF"
                                            }
                                            MouseArea {
                                                anchors.fill: parent
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: logViewRoot.toggleExpand(ev.operationId)
                                            }
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            text: (ev.timestampUtc || "").replace("T", " ").replace("Z", "")
                                            font.family: "Monospace"; font.pixelSize: 10; color: "#90A4B8"
                                            elide: Text.ElideRight
                                        }
                                    }

                                    Label {
                                        Layout.fillWidth: false; Layout.preferredWidth: 112; Layout.minimumWidth: 112; Layout.maximumWidth: 112
                                        text: ev.category || ""
                                        font.family: "Monospace"; font.pixelSize: 10; font.bold: true; color: "#7ED9F0"
                                        elide: Text.ElideRight
                                    }

                                    Label {
                                        Layout.fillWidth: false; Layout.preferredWidth: 148; Layout.minimumWidth: 148; Layout.maximumWidth: 148
                                        text: ev.operation || ""
                                        font.family: "Monospace"; font.pixelSize: 10; color: "#DCE6F0"
                                        elide: Text.ElideRight
                                    }

                                    Rectangle {
                                        Layout.fillWidth: false; Layout.preferredWidth: 76; Layout.minimumWidth: 76; Layout.maximumWidth: 76
                                        Layout.preferredHeight: 18
                                        Layout.alignment: Qt.AlignVCenter
                                        height: 18; radius: 3
                                        color: "#090D12"
                                        border.color: logViewRoot.statusColor(ev.status)
                                        Label {
                                            anchors.centerIn: parent
                                            text: ev.status || ""
                                            font.family: "Monospace"; font.pixelSize: 9; font.bold: true
                                            color: logViewRoot.statusColor(ev.status)
                                        }
                                    }

                                    Label {
                                        Layout.fillWidth: false; Layout.preferredWidth: 176; Layout.minimumWidth: 176; Layout.maximumWidth: 176
                                        text: ev.target || "-"
                                        font.family: "Monospace"; font.pixelSize: 10; color: "#B8C7D6"
                                        elide: Text.ElideMiddle
                                    }

                                    Label {
                                        Layout.fillWidth: false; Layout.preferredWidth: 68; Layout.minimumWidth: 68; Layout.maximumWidth: 68
                                        text: ev.durationMs > 0 ? ev.durationMs.toFixed(1) : "-"
                                        font.family: "Monospace"; font.pixelSize: 10; color: "#8C9EAE"
                                        horizontalAlignment: Text.AlignRight
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        leftPadding: 8
                                        text: ev.message || ""
                                        font.pixelSize: 11; color: "#D4DFEA"
                                        elide: Text.ElideRight
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    z: -1
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: logViewRoot.selectEvent(ev)
                                }
                            }
                        }
                    }
                }
            }

            // RIGHT: OPERATION DETAILS, RELATED EVENTS & DETAILS_JSON
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
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: qsTr("OPERATION DETAILS")
                            font.family: "Monospace"; font.pixelSize: 12; font.bold: true; color: "#E8BA6B"
                        }
                        Item { Layout.fillWidth: true }
                        ConsoleButton {
                            text: qsTr("COPY JSON")
                            implicitWidth: 88; implicitHeight: 26; fontSize: 9; accentColor: "#00E5FF"
                            enabled: logViewRoot.selectedLog !== null
                            onClicked: {
                                if (logViewRoot.selectedLog) {
                                    EngineController.copyToClipboard(logViewRoot.formatPrettyJson(logViewRoot.selectedLog.detailsJson))
                                }
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 162
                        implicitHeight: 162
                        clip: true
                        radius: 4; color: "#070A0F"; border.color: "#222C38"
                        ColumnLayout {
                            id: opDetailsCol
                            anchors.fill: parent; anchors.margins: 10; spacing: 3
                            readonly property var s: logViewRoot.selectedLog
                            Label {
                                text: opDetailsCol.s ? qsTr("Category: %1  •  Op: %2  •  Status: %3").arg(opDetailsCol.s.category || "").arg(opDetailsCol.s.operation || "").arg(opDetailsCol.s.status || "") : qsTr("No log event selected")
                                font.family: "Monospace"; font.pixelSize: 11; font.bold: true
                                color: opDetailsCol.s ? logViewRoot.statusColor(opDetailsCol.s.status) : "#5D6E7F"
                                Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight
                            }
                            Label {
                                text: opDetailsCol.s ? qsTr("Timestamp: %1 (%2)").arg(opDetailsCol.s.timestampUtc || "-").arg(opDetailsCol.s.localUtcOffset || "UTC") : ""
                                font.family: "Monospace"; font.pixelSize: 10; color: "#9AA9B8"; Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight
                            }
                            Label {
                                text: opDetailsCol.s ? qsTr("Op ID: %1   |   Parent Op: %2").arg(opDetailsCol.s.operationId || "-").arg(opDetailsCol.s.parentOperationId || "(root)") : ""
                                font.family: "Monospace"; font.pixelSize: 10; color: "#78D8F0"; Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideMiddle
                            }
                            Label {
                                text: opDetailsCol.s ? qsTr("Target: [%1] %2   |   ErrorCode: %3   |   Duration: %4 ms")
                                          .arg(opDetailsCol.s.targetType || "-").arg(opDetailsCol.s.target || "-").arg(opDetailsCol.s.errorCode || 0).arg((opDetailsCol.s.durationMs || 0).toFixed(2)) : ""
                                font.family: "Monospace"; font.pixelSize: 10; color: "#B8C7D6"; Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight
                            }
                            Label {
                                text: opDetailsCol.s ? (opDetailsCol.s.message || "") : ""
                                font.pixelSize: 11; color: "#E4ECF4"; wrapMode: Text.WordWrap; Layout.fillWidth: true; Layout.minimumWidth: 0; maximumLineCount: 2; elide: Text.ElideRight
                            }
                        }
                    }

                    Label {
                        text: qsTr("RELATED EVENTS (%1)").arg(logViewRoot.relatedLogs.length)
                        font.family: "Monospace"; font.pixelSize: 11; font.bold: true; color: "#78D8F0"
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 150
                        radius: 4; color: "#070A0F"; border.color: "#222C38"
                        ListView {
                            id: relatedLogsList
                            anchors.fill: parent; anchors.margins: 6; clip: true; spacing: 3
                            model: logViewRoot.relatedLogs
                            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                            delegate: Rectangle {
                                width: relatedLogsList.width - 10; height: 24; radius: 2; clip: true
                                color: "#0E131B"; border.color: "#1E2836"
                                RowLayout {
                                    anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 6
                                    Label { text: modelData.category + "/" + modelData.operation; font.family: "Monospace"; font.pixelSize: 9; font.bold: true; color: "#7ED9F0" }
                                    Label { text: modelData.status; font.family: "Monospace"; font.pixelSize: 9; color: logViewRoot.statusColor(modelData.status) }
                                    Label { Layout.fillWidth: true; Layout.minimumWidth: 0; text: modelData.message; font.pixelSize: 10; color: "#C6D3E0"; elide: Text.ElideRight }
                                }
                                MouseArea {
                                    anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                    onClicked: logViewRoot.selectEvent(modelData)
                                }
                            }
                        }
                    }

                    Label {
                        text: qsTr("DETAILS JSON")
                        font.family: "Monospace"; font.pixelSize: 11; font.bold: true; color: "#78D8F0"
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: 4; color: "#06090D"; border.color: "#222C38"
                        ScrollView {
                            anchors.fill: parent; anchors.margins: 6; clip: true
                            contentWidth: availableWidth
                            TextArea {
                                readOnly: true
                                selectByMouse: true
                                wrapMode: TextEdit.Wrap
                                text: logViewRoot.selectedLog ? logViewRoot.formatPrettyJson(logViewRoot.selectedLog.detailsJson) : qsTr("{}")
                                font.family: "Monospace"; font.pixelSize: 10; color: "#90E4F5"
                                background: Rectangle { color: "transparent" }
                            }
                        }
                    }
                }
            }
        }

        // =====================================================================
        // FOOTER RETENTION & SESSION DIAGNOSTIC BAR
        // =====================================================================
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 26
            Layout.minimumHeight: 26
            height: 26
            radius: 3
            color: "#090D12"
            border.color: "#212A36"
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 14
                Label {
                    text: qsTr("DISPLAYED: %1 EVENTS (%2 TREE ROWS)   |   RETENTION POLICY: 180 DAYS / 300,000 EVENTS   |   SESSION: %3")
                          .arg(logViewRoot.rawLogs.length)
                          .arg(logViewRoot.treeRows.length)
                          .arg(LogService.sessionId())
                    font.family: "Monospace"; font.pixelSize: 10; color: "#8899AA"
                    Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight
                }
                Label {
                    visible: logViewRoot.exportFeedbackText.length > 0
                    text: logViewRoot.exportFeedbackText
                    font.family: "Monospace"; font.pixelSize: 10; color: "#00E676"
                }
            }
        }
    }

    FileDialog {
        id: exportJsonlDlg
        title: qsTr("Export Filtered Audit Logs as JSON Lines (.jsonl)")
        fileMode: FileDialog.SaveFile
        nameFilters: ["JSON Lines (*.jsonl)", "All Files (*)"]
        defaultSuffix: "jsonl"
        onAccepted: {
            var p = decodeURIComponent(selectedFile.toString().replace(/^file:\/\//, ""))
            if (p.length > 0) {
                var ok = LogService.exportFilteredToJsonl(p, logViewRoot.buildFilterMap())
                logViewRoot.exportFeedbackText = ok ? qsTr("Exported JSONL: %1").arg(p) : qsTr("Export failed")
            }
        }
    }
}
