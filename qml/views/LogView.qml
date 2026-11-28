import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Desk32.Core
import "../components"

Item {
    id: logViewRoot

    implicitWidth: 1600
    implicitHeight: 900

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
    property int totalStoredCount: 0
    property string exportScope: "displayed" // "all" | "displayed"
    property string exportFeedbackText: ""

    // Date/Time Filter State (Single Date/Time & Range)
    property bool dateRangeMode: true        // false: Single Date/Time, true: Range (From ~ To)
    property bool useLocalTime: false        // false: UTC (default, matches table), true: Local
    property string dateSingleText: ""
    property string dateFromText: ""
    property string dateToText: ""
    property string activeFromUtc: ""
    property string activeToUtc: ""
    property string activeTimePreset: "all"  // "all" | "1h" | "24h" | "today" | "7d" | "custom"
    property string dateFilterError: ""

    function pad2(n) { return (n < 10 ? "0" : "") + n }
    function pad3(n) { return (n < 10 ? "00" : (n < 100 ? "0" : "")) + n }

    function formatDateObj(dt, localMode, includeTime) {
        var y = localMode ? dt.getFullYear() : dt.getUTCFullYear()
        var mo = (localMode ? dt.getMonth() : dt.getUTCMonth()) + 1
        var d = localMode ? dt.getDate() : dt.getUTCDate()
        var datePart = y + "-" + pad2(mo) + "-" + pad2(d)
        if (!includeTime) return datePart
        var h = localMode ? dt.getHours() : dt.getUTCHours()
        var mi = localMode ? dt.getMinutes() : dt.getUTCMinutes()
        var s = localMode ? dt.getSeconds() : dt.getUTCSeconds()
        return datePart + " " + pad2(h) + ":" + pad2(mi) + ":" + pad2(s)
    }

    function formatTimestampForTable(utcStr) {
        if (!utcStr || utcStr.length === 0) return ""
        if (!useLocalTime) return utcStr.replace("T", " ").replace("Z", "")
        var dt = new Date(utcStr)
        if (isNaN(dt.getTime())) return utcStr.replace("T", " ").replace("Z", "")
        return dt.getFullYear() + "-" + pad2(dt.getMonth() + 1) + "-" + pad2(dt.getDate())
             + " " + pad2(dt.getHours()) + ":" + pad2(dt.getMinutes()) + ":" + pad2(dt.getSeconds())
             + "." + pad3(dt.getMilliseconds())
    }

    // Parse "YYYY-MM-DD", "YYYY-MM-DD HH", "YYYY-MM-DD HH:MM", "YYYY-MM-DD HH:MM:SS[.sss]" (with - or /, space or T, optional Z)
    function parseDateTimeBound(raw, isUpperBound, localMode) {
        var s = (raw || "").trim()
        if (s.length === 0) return { valid: true, utc: "" }

        var explicitUtc = /[zZ]$/.test(s)
        var core = explicitUtc ? s.slice(0, -1).trim() : s
        var re = /^(\d{4})[-\/](\d{1,2})[-\/](\d{1,2})(?:[T\s]+(\d{1,2})(?::(\d{1,2})(?::(\d{1,2})(?:\.(\d{1,3}))?)?)?)?$/
        var m = re.exec(core)
        if (!m) return { valid: false, utc: "" }

        var yr = parseInt(m[1], 10)
        var mo = parseInt(m[2], 10)
        var dy = parseInt(m[3], 10)
        var hasH = m[4] !== undefined
        var hasM = m[5] !== undefined
        var hasS = m[6] !== undefined
        var hasMs = m[7] !== undefined

        var hh = hasH ? parseInt(m[4], 10) : (isUpperBound ? 23 : 0)
        var mi = hasM ? parseInt(m[5], 10) : (isUpperBound ? 59 : 0)
        var ss = hasS ? parseInt(m[6], 10) : (isUpperBound ? 59 : 0)
        var ms = hasMs ? parseInt((m[7] + "000").slice(0, 3), 10) : (isUpperBound ? 999 : 0)

        if (mo < 1 || mo > 12 || dy < 1 || dy > 31 || hh < 0 || hh > 23 || mi < 0 || mi > 59 || ss < 0 || ss > 59) {
            return { valid: false, utc: "" }
        }

        // Validate real calendar day (rejects e.g. Feb 30)
        var check = new Date(Date.UTC(yr, mo - 1, dy, 0, 0, 0, 0))
        if (check.getUTCFullYear() !== yr || (check.getUTCMonth() + 1) !== mo || check.getUTCDate() !== dy) {
            return { valid: false, utc: "" }
        }

        var dt
        if (localMode && !explicitUtc) {
            dt = new Date(yr, mo - 1, dy, hh, mi, ss, ms)
        } else {
            dt = new Date(Date.UTC(yr, mo - 1, dy, hh, mi, ss, ms))
        }
        if (isNaN(dt.getTime())) return { valid: false, utc: "" }

        var iso = dt.getUTCFullYear() + "-" + pad2(dt.getUTCMonth() + 1) + "-" + pad2(dt.getUTCDate())
                + "T" + pad2(dt.getUTCHours()) + ":" + pad2(dt.getUTCMinutes()) + ":" + pad2(dt.getUTCSeconds())
                + "." + pad3(dt.getUTCMilliseconds()) + "Z"
        return { valid: true, utc: iso }
    }

    function applyDateFilter(triggerRefresh) {
        if (!dateRangeMode) {
            var singleTrim = (dateSingleText || "").trim()
            if (singleTrim.length === 0) {
                dateFilterError = ""
                activeFromUtc = ""
                activeToUtc = ""
                if (activeTimePreset === "custom") activeTimePreset = "all"
                if (triggerRefresh) refresh()
                return true
            }
            var lo = parseDateTimeBound(singleTrim, false, useLocalTime)
            var hi = parseDateTimeBound(singleTrim, true, useLocalTime)
            if (!lo.valid || !hi.valid) {
                dateFilterError = qsTr("Format: YYYY-MM-DD [HH[:MM[:SS]]]")
                return false
            }
            dateFilterError = ""
            activeFromUtc = lo.utc
            activeToUtc = hi.utc
            if (triggerRefresh) refresh()
            return true
        } else {
            var pFrom = parseDateTimeBound(dateFromText, false, useLocalTime)
            var pTo = parseDateTimeBound(dateToText, true, useLocalTime)
            if (!pFrom.valid || !pTo.valid) {
                dateFilterError = qsTr("Format: YYYY-MM-DD [HH[:MM[:SS]]]")
                return false
            }
            if (pFrom.utc.length > 0 && pTo.utc.length > 0 && pFrom.utc > pTo.utc) {
                dateFilterError = qsTr("Invalid range: FROM is later than TO")
                return false
            }
            dateFilterError = ""
            activeFromUtc = pFrom.utc
            activeToUtc = pTo.utc
            if (activeFromUtc.length === 0 && activeToUtc.length === 0 && activeTimePreset === "custom") {
                activeTimePreset = "all"
            }
            if (triggerRefresh) refresh()
            return true
        }
    }

    function syncDateInputsFromState() {
        if (typeof dateSingleField !== "undefined" && dateSingleField.text !== dateSingleText)
            dateSingleField.text = dateSingleText
        if (typeof dateFromField !== "undefined" && dateFromField.text !== dateFromText)
            dateFromField.text = dateFromText
        if (typeof dateToField !== "undefined" && dateToField.text !== dateToText)
            dateToField.text = dateToText
    }

    function applyTimePreset(presetId) {
        activeTimePreset = presetId
        dateFilterError = ""
        if (presetId === "all") {
            dateSingleText = ""
            dateFromText = ""
            dateToText = ""
            activeFromUtc = ""
            activeToUtc = ""
            syncDateInputsFromState()
            refresh()
            return
        }
        var now = new Date()
        if (presetId === "today") {
            dateRangeMode = false
            dateSingleText = formatDateObj(now, useLocalTime, false)
            syncDateInputsFromState()
            applyDateFilter(true)
            return
        }
        dateRangeMode = true
        var deltaMs = 0
        if (presetId === "1h") deltaMs = 3600 * 1000
        else if (presetId === "24h") deltaMs = 24 * 3600 * 1000
        else if (presetId === "7d") deltaMs = 7 * 24 * 3600 * 1000
        var startDt = new Date(now.getTime() - deltaMs)
        dateFromText = formatDateObj(startDt, useLocalTime, true)
        dateToText = formatDateObj(now, useLocalTime, true)
        syncDateInputsFromState()
        applyDateFilter(true)
    }

    function activeFilterSummary() {
        var _lang = LanguageManager.currentLanguage
        var parts = []
        parts.push(categoryFilter === "All" ? qsTr("Cat: All") : qsTr("Cat: %1").arg(categoryFilter))
        if (statusFilter !== "All") parts.push(qsTr("Status: %1").arg(statusFilter))
        if (playbackOnlyMode) parts.push(qsTr("Playback"))
        if (activeFromUtc.length > 0 || activeToUtc.length > 0) parts.push(qsTr("Time Filtered"))
        if (searchFilter.trim().length > 0) parts.push(qsTr("Search"))
        parts.push(qsTr("Limit: %1").arg(limitCount))
        return parts.join(" • ")
    }

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
        if (activeFromUtc.length > 0) f.fromUtc = activeFromUtc
        if (activeToUtc.length > 0) f.toUtc = activeToUtc
        return f
    }

    function refresh() {
        rawLogs = LogService.fetchLogs(buildFilterMap()) || []
        totalStoredCount = LogService.totalEventCount()
        rebuildTreeRows()
        if (!selectedLog && rawLogs.length > 0) {
            selectEvent(rawLogs[0])
        }
    }

    Component.onCompleted: refresh()
    onVisibleChanged: if (visible) refresh()

    Timer {
        id: dateFilterDebounce
        interval: 280
        repeat: false
        onTriggered: logViewRoot.applyDateFilter(true)
    }

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
        color: "#080a0e"
        clip: true

        Image {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: -56
            anchors.bottom: parent.bottom
            source: "qrc:/Desk32/images/Desk32-Blur.png"
            fillMode: Image.PreserveAspectCrop
            smooth: true
            mipmap: true
        }

        // Subtle dark scrim so foreground Log panel reads cleanly over blurred console
        Rectangle {
            anchors.fill: parent
            color: "#4805070b"
        }
    }

    // Soft cast drop shadow beneath floating Log frame
    BorderImage {
        anchors.fill: logFrame
        anchors.leftMargin: -22
        anchors.rightMargin: -22
        anchors.topMargin: -18
        anchors.bottomMargin: -26
        source: "qrc:/Desk32/resources/ui/panel_shadow_9slice.png"
        border { left: 22; top: 22; right: 22; bottom: 22 }
        opacity: 0.92
    }

    Rectangle {
        id: logFrame
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
        // TOP FILTER & AUDIT TOOLBAR (2 ROWS: PRIMARY FILTERS + DATE/TIME RANGE)
        // =====================================================================
        BrushedMetalPanel {
            Layout.fillWidth: true
            Layout.preferredHeight: 96
            baseTint: "#14181f"
            borderColor: "#333c4a"
            showScrews: true
            cornerRadius: 5

            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                anchors.topMargin: 8
                anchors.bottomMargin: 8
                spacing: 6

                // ROW 1: Category, Status, Playback History, Search, Row Limit, Refresh, Export Menu
                RowLayout {
                    Layout.fillWidth: true
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
                        id: exportJsonlBtn
                        text: qsTr("EXPORT JSONL ▾")
                        implicitWidth: 138; implicitHeight: 30; fontSize: 10; accentColor: "#00E676"
                        highlighted: exportMenuPopup.visible
                        onClicked: {
                            logViewRoot.totalStoredCount = LogService.totalEventCount()
                            if (exportMenuPopup.visible) {
                                exportMenuPopup.close()
                            } else {
                                exportMenuPopup.open()
                            }
                        }

                        Popup {
                            id: exportMenuPopup
                            y: exportJsonlBtn.height + 4
                            x: exportJsonlBtn.width - width
                            width: 340
                            padding: 6
                            modal: false
                            focus: true
                            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

                            background: Rectangle {
                                radius: 5
                                color: "#0D1117"
                                border.color: "#3A4859"
                                border.width: 1
                            }

                            contentItem: ColumnLayout {
                                spacing: 4

                                Label {
                                    Layout.fillWidth: true
                                    leftPadding: 6
                                    topPadding: 2
                                    bottomPadding: 2
                                    text: qsTr("SELECT JSONL EXPORT SCOPE")
                                    font.family: "Monospace"
                                    font.pixelSize: 9
                                    font.bold: true
                                    color: "#7F94A8"
                                }

                                // Option 1: All Stored Logs
                                Rectangle {
                                    Layout.fillWidth: true
                                    implicitHeight: 50
                                    radius: 4
                                    color: allExportHover.containsMouse ? "#162634" : "#11161E"
                                    border.color: allExportHover.containsMouse ? "#00E676" : "#25303E"

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10
                                        spacing: 8

                                        Rectangle {
                                            width: 4; height: 30; radius: 2
                                            color: "#00E676"
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2
                                            Label {
                                                Layout.fillWidth: true
                                                text: qsTr("All Stored Logs (%1)").arg(logViewRoot.totalStoredCount)
                                                font.family: "Monospace"
                                                font.pixelSize: 11
                                                font.bold: true
                                                color: "#E4F2EC"
                                                elide: Text.ElideRight
                                            }
                                            Label {
                                                Layout.fillWidth: true
                                                text: qsTr("Export all retained logs in the database")
                                                font.pixelSize: 10
                                                color: "#88A0B2"
                                                elide: Text.ElideRight
                                            }
                                        }
                                    }

                                    MouseArea {
                                        id: allExportHover
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            exportMenuPopup.close()
                                            logViewRoot.exportScope = "all"
                                            exportJsonlDlg.open()
                                        }
                                    }
                                }

                                // Option 2: Currently Displayed Logs (Row Limit & Filtered Category/Time/Search)
                                Rectangle {
                                    Layout.fillWidth: true
                                    implicitHeight: 50
                                    radius: 4
                                    color: dispExportHover.containsMouse ? "#162634" : "#11161E"
                                    border.color: dispExportHover.containsMouse ? "#33E3FF" : "#25303E"

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10
                                        spacing: 8

                                        Rectangle {
                                            width: 4; height: 30; radius: 2
                                            color: "#33E3FF"
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2
                                            Label {
                                                Layout.fillWidth: true
                                                text: qsTr("Displayed Logs (%1)").arg(logViewRoot.rawLogs.length)
                                                font.family: "Monospace"
                                                font.pixelSize: 11
                                                font.bold: true
                                                color: "#E2F4FA"
                                                elide: Text.ElideRight
                                            }
                                            Label {
                                                Layout.fillWidth: true
                                                text: logViewRoot.activeFilterSummary()
                                                font.family: "Monospace"
                                                font.pixelSize: 9
                                                color: "#7FA9BD"
                                                elide: Text.ElideRight
                                            }
                                        }
                                    }

                                    MouseArea {
                                        id: dispExportHover
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            exportMenuPopup.close()
                                            logViewRoot.exportScope = "displayed"
                                            exportJsonlDlg.open()
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // ROW 2: Date & Time Filter (Presets + Single Date/Time or Range From~To + UTC/LOCAL)
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Label {
                        text: qsTr("DATE / TIME")
                        font.family: "Monospace"; font.pixelSize: 10; font.bold: true; color: "#7F94A8"
                    }

                    // Quick Time Range Presets
                    ConsoleButton {
                        text: qsTr("ALL")
                        implicitWidth: 42; implicitHeight: 26; fontSize: 9
                        highlighted: logViewRoot.activeTimePreset === "all" && logViewRoot.activeFromUtc.length === 0 && logViewRoot.activeToUtc.length === 0
                        accentColor: "#33E3FF"
                        onClicked: logViewRoot.applyTimePreset("all")
                    }
                    ConsoleButton {
                        text: qsTr("1H")
                        implicitWidth: 38; implicitHeight: 26; fontSize: 9
                        highlighted: logViewRoot.activeTimePreset === "1h"
                        accentColor: "#33E3FF"
                        onClicked: logViewRoot.applyTimePreset("1h")
                    }
                    ConsoleButton {
                        text: qsTr("24H")
                        implicitWidth: 44; implicitHeight: 26; fontSize: 9
                        highlighted: logViewRoot.activeTimePreset === "24h"
                        accentColor: "#33E3FF"
                        onClicked: logViewRoot.applyTimePreset("24h")
                    }
                    ConsoleButton {
                        text: qsTr("TODAY")
                        implicitWidth: 54; implicitHeight: 26; fontSize: 9
                        highlighted: logViewRoot.activeTimePreset === "today"
                        accentColor: "#33E3FF"
                        onClicked: logViewRoot.applyTimePreset("today")
                    }
                    ConsoleButton {
                        text: qsTr("7D")
                        implicitWidth: 38; implicitHeight: 26; fontSize: 9
                        highlighted: logViewRoot.activeTimePreset === "7d"
                        accentColor: "#33E3FF"
                        onClicked: logViewRoot.applyTimePreset("7d")
                    }

                    Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 18; color: "#2D3745" }

                    // Single Date/Time vs Range Switcher
                    ConsoleButton {
                        text: qsTr("SINGLE")
                        implicitWidth: 62; implicitHeight: 26; fontSize: 9
                        highlighted: !logViewRoot.dateRangeMode
                        accentColor: "#E8BA6B"
                        toolTipText: qsTr("Filter by a single date or timestamp (e.g. 2026-11-28 or 2026-11-28 08:30)")
                        onClicked: {
                            if (logViewRoot.dateRangeMode) {
                                logViewRoot.dateRangeMode = false
                                if (logViewRoot.dateSingleText.length === 0 && logViewRoot.dateFromText.length > 0) {
                                    logViewRoot.dateSingleText = logViewRoot.dateFromText
                                    logViewRoot.syncDateInputsFromState()
                                }
                                logViewRoot.activeTimePreset = "custom"
                                logViewRoot.applyDateFilter(true)
                            }
                        }
                    }
                    ConsoleButton {
                        text: qsTr("RANGE")
                        implicitWidth: 60; implicitHeight: 26; fontSize: 9
                        highlighted: logViewRoot.dateRangeMode
                        accentColor: "#E8BA6B"
                        toolTipText: qsTr("Filter by a start/end date-time range (From ~ To)")
                        onClicked: {
                            if (!logViewRoot.dateRangeMode) {
                                logViewRoot.dateRangeMode = true
                                if (logViewRoot.dateFromText.length === 0 && logViewRoot.dateSingleText.length > 0) {
                                    logViewRoot.dateFromText = logViewRoot.dateSingleText
                                    logViewRoot.dateToText = logViewRoot.dateSingleText
                                    logViewRoot.syncDateInputsFromState()
                                }
                                logViewRoot.activeTimePreset = "custom"
                                logViewRoot.applyDateFilter(true)
                            }
                        }
                    }

                    // Single Date/Time Input
                    TextField {
                        id: dateSingleField
                        visible: !logViewRoot.dateRangeMode
                        Layout.preferredWidth: 196
                        implicitHeight: 26
                        text: logViewRoot.dateSingleText
                        placeholderText: qsTr("YYYY-MM-DD [HH:MM[:SS]]")
                        color: "#DCE6F0"
                        placeholderTextColor: "#546373"
                        font.family: "Monospace"
                        font.pixelSize: 10
                        leftPadding: 8; rightPadding: 8
                        background: Rectangle {
                            radius: 4
                            color: "#080B10"
                            border.color: logViewRoot.dateFilterError.length > 0
                                          ? "#FFB300"
                                          : (parent.activeFocus ? "#33E3FF" : "#2E3846")
                        }
                        onTextEdited: {
                            logViewRoot.dateSingleText = text
                            logViewRoot.activeTimePreset = text.trim().length > 0 ? "custom" : "all"
                            dateFilterDebounce.restart()
                        }
                        onAccepted: {
                            dateFilterDebounce.stop()
                            logViewRoot.dateSingleText = text
                            logViewRoot.applyDateFilter(true)
                        }
                    }

                    // Range FROM ~ TO Inputs
                    TextField {
                        id: dateFromField
                        visible: logViewRoot.dateRangeMode
                        Layout.preferredWidth: 184
                        implicitHeight: 26
                        text: logViewRoot.dateFromText
                        placeholderText: qsTr("From: YYYY-MM-DD HH:MM")
                        color: "#DCE6F0"
                        placeholderTextColor: "#546373"
                        font.family: "Monospace"
                        font.pixelSize: 10
                        leftPadding: 8; rightPadding: 8
                        background: Rectangle {
                            radius: 4
                            color: "#080B10"
                            border.color: logViewRoot.dateFilterError.length > 0
                                          ? "#FFB300"
                                          : (parent.activeFocus ? "#33E3FF" : "#2E3846")
                        }
                        onTextEdited: {
                            logViewRoot.dateFromText = text
                            logViewRoot.activeTimePreset = (text.trim().length > 0 || logViewRoot.dateToText.trim().length > 0) ? "custom" : "all"
                            dateFilterDebounce.restart()
                        }
                        onAccepted: {
                            dateFilterDebounce.stop()
                            logViewRoot.dateFromText = text
                            logViewRoot.applyDateFilter(true)
                        }
                    }

                    Label {
                        visible: logViewRoot.dateRangeMode
                        text: "~"
                        font.family: "Monospace"; font.pixelSize: 12; font.bold: true; color: "#7F94A8"
                    }

                    TextField {
                        id: dateToField
                        visible: logViewRoot.dateRangeMode
                        Layout.preferredWidth: 184
                        implicitHeight: 26
                        text: logViewRoot.dateToText
                        placeholderText: qsTr("To: YYYY-MM-DD HH:MM")
                        color: "#DCE6F0"
                        placeholderTextColor: "#546373"
                        font.family: "Monospace"
                        font.pixelSize: 10
                        leftPadding: 8; rightPadding: 8
                        background: Rectangle {
                            radius: 4
                            color: "#080B10"
                            border.color: logViewRoot.dateFilterError.length > 0
                                          ? "#FFB300"
                                          : (parent.activeFocus ? "#33E3FF" : "#2E3846")
                        }
                        onTextEdited: {
                            logViewRoot.dateToText = text
                            logViewRoot.activeTimePreset = (logViewRoot.dateFromText.trim().length > 0 || text.trim().length > 0) ? "custom" : "all"
                            dateFilterDebounce.restart()
                        }
                        onAccepted: {
                            dateFilterDebounce.stop()
                            logViewRoot.dateToText = text
                            logViewRoot.applyDateFilter(true)
                        }
                    }

                    ConsoleButton {
                        text: logViewRoot.useLocalTime ? qsTr("LOCAL") : qsTr("UTC")
                        implicitWidth: 56; implicitHeight: 26; fontSize: 9
                        highlighted: logViewRoot.useLocalTime
                        accentColor: "#33E3FF"
                        toolTipText: qsTr("Toggle UTC / Local timezone for date-time filter and timestamp column")
                        onClicked: {
                            logViewRoot.useLocalTime = !logViewRoot.useLocalTime
                            if (logViewRoot.activeTimePreset === "1h" || logViewRoot.activeTimePreset === "24h"
                                || logViewRoot.activeTimePreset === "today" || logViewRoot.activeTimePreset === "7d") {
                                logViewRoot.applyTimePreset(logViewRoot.activeTimePreset)
                            } else {
                                logViewRoot.applyDateFilter(true)
                            }
                        }
                    }

                    ConsoleButton {
                        text: qsTr("APPLY")
                        implicitWidth: 56; implicitHeight: 26; fontSize: 9; accentColor: "#00E5FF"
                        onClicked: {
                            dateFilterDebounce.stop()
                            logViewRoot.applyDateFilter(true)
                        }
                    }

                    ConsoleButton {
                        text: qsTr("CLEAR")
                        implicitWidth: 56; implicitHeight: 26; fontSize: 9; accentColor: "#9AA9B8"
                        enabled: logViewRoot.activeFromUtc.length > 0 || logViewRoot.activeToUtc.length > 0
                                 || logViewRoot.dateSingleText.length > 0
                                 || logViewRoot.dateFromText.length > 0
                                 || logViewRoot.dateToText.length > 0
                        onClicked: {
                            dateFilterDebounce.stop()
                            logViewRoot.applyTimePreset("all")
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                        font.family: "Monospace"
                        font.pixelSize: 9
                        color: logViewRoot.dateFilterError.length > 0 ? "#FFB300" : "#6E859A"
                        text: {
                            if (logViewRoot.dateFilterError.length > 0) return logViewRoot.dateFilterError
                            if (logViewRoot.activeFromUtc.length > 0 || logViewRoot.activeToUtc.length > 0) {
                                var fTxt = logViewRoot.activeFromUtc.length > 0 ? logViewRoot.activeFromUtc.replace("T", " ").replace(".000Z", "Z") : "*"
                                var tTxt = logViewRoot.activeToUtc.length > 0 ? logViewRoot.activeToUtc.replace("T", " ").replace(".999Z", "Z") : "*"
                                return qsTr("UTC: %1 .. %2").arg(fTxt).arg(tTxt)
                            }
                            return ""
                        }
                    }
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
                Layout.preferredWidth: 1050
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
                            Label { Layout.fillWidth: false; Layout.preferredWidth: 160; Layout.minimumWidth: 160; Layout.maximumWidth: 160; text: logViewRoot.useLocalTime ? qsTr("TIMESTAMP (LOCAL)") : qsTr("TIMESTAMP (UTC)"); font.family: "Monospace"; font.pixelSize: 10; font.bold: true; color: "#7F94A8"; elide: Text.ElideRight }
                            Label { Layout.fillWidth: false; Layout.preferredWidth: 104; Layout.minimumWidth: 104; Layout.maximumWidth: 104; text: qsTr("CATEGORY"); font.family: "Monospace"; font.pixelSize: 10; font.bold: true; color: "#7F94A8"; elide: Text.ElideRight }
                            Label { Layout.fillWidth: false; Layout.preferredWidth: 136; Layout.minimumWidth: 136; Layout.maximumWidth: 136; text: qsTr("OPERATION"); font.family: "Monospace"; font.pixelSize: 10; font.bold: true; color: "#7F94A8"; elide: Text.ElideRight }
                            Label { Layout.fillWidth: false; Layout.preferredWidth: 72;  Layout.minimumWidth: 72;  Layout.maximumWidth: 72;  text: qsTr("STATUS"); font.family: "Monospace"; font.pixelSize: 10; font.bold: true; color: "#7F94A8"; elide: Text.ElideRight }
                            Label { Layout.fillWidth: false; Layout.preferredWidth: 148; Layout.minimumWidth: 148; Layout.maximumWidth: 148; text: qsTr("TARGET"); font.family: "Monospace"; font.pixelSize: 10; font.bold: true; color: "#7F94A8"; elide: Text.ElideRight }
                            Label { Layout.fillWidth: false; Layout.preferredWidth: 60;  Layout.minimumWidth: 60;  Layout.maximumWidth: 60;  text: qsTr("DUR(ms)"); font.family: "Monospace"; font.pixelSize: 10; font.bold: true; color: "#7F94A8"; horizontalAlignment: Text.AlignRight }
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
                                        readonly property int colW: Math.max(80, 160 - modelData.depth * 18)
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
                                            text: logViewRoot.formatTimestampForTable(ev.timestampUtc)
                                            font.family: "Monospace"; font.pixelSize: 10; color: "#90A4B8"
                                            elide: Text.ElideRight
                                        }
                                    }

                                    Label {
                                        Layout.fillWidth: false; Layout.preferredWidth: 104; Layout.minimumWidth: 104; Layout.maximumWidth: 104
                                        text: ev.category || ""
                                        font.family: "Monospace"; font.pixelSize: 10; font.bold: true; color: "#7ED9F0"
                                        elide: Text.ElideRight
                                    }

                                    Label {
                                        Layout.fillWidth: false; Layout.preferredWidth: 136; Layout.minimumWidth: 136; Layout.maximumWidth: 136
                                        text: ev.operation || ""
                                        font.family: "Monospace"; font.pixelSize: 10; color: "#DCE6F0"
                                        elide: Text.ElideRight
                                    }

                                    Rectangle {
                                        Layout.fillWidth: false; Layout.preferredWidth: 72; Layout.minimumWidth: 72; Layout.maximumWidth: 72
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
                                        Layout.fillWidth: false; Layout.preferredWidth: 148; Layout.minimumWidth: 148; Layout.maximumWidth: 148
                                        text: ev.target || "-"
                                        font.family: "Monospace"; font.pixelSize: 10; color: "#B8C7D6"
                                        elide: Text.ElideMiddle
                                    }

                                    Label {
                                        Layout.fillWidth: false; Layout.preferredWidth: 60; Layout.minimumWidth: 60; Layout.maximumWidth: 60
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
                Layout.preferredWidth: 520
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
                    text: qsTr("DISPLAYED: %1 / TOTAL: %2 EVENTS (%3 TREE ROWS)   |   RETENTION POLICY: 180 DAYS / 300,000 ROOT OPERATIONS   |   SESSION: %4")
                          .arg(logViewRoot.rawLogs.length)
                          .arg(logViewRoot.totalStoredCount)
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
    }

    FileDialog {
        id: exportJsonlDlg
        title: logViewRoot.exportScope === "all"
               ? qsTr("Export All Stored Audit Logs as JSON Lines (.jsonl)")
               : qsTr("Export Displayed Audit Logs as JSON Lines (.jsonl)")
        fileMode: FileDialog.SaveFile
        nameFilters: ["JSON Lines (*.jsonl)", "All Files (*)"]
        defaultSuffix: "jsonl"
        onAccepted: {
            var p = decodeURIComponent(selectedFile.toString().replace(/^file:\/\//, ""))
            if (p.length > 0) {
                var isAll = (logViewRoot.exportScope === "all")
                var ok = isAll
                         ? LogService.exportToJsonl(p)
                         : LogService.exportFilteredToJsonl(p, logViewRoot.buildFilterMap())
                if (ok) {
                    var countTxt = isAll ? logViewRoot.totalStoredCount : logViewRoot.rawLogs.length
                    logViewRoot.exportFeedbackText = isAll
                        ? qsTr("Exported ALL (%1): %2").arg(countTxt).arg(p)
                        : qsTr("Exported DISPLAYED (%1): %2").arg(countTxt).arg(p)
                } else {
                    logViewRoot.exportFeedbackText = qsTr("Export failed")
                }
            }
        }
    }
}
