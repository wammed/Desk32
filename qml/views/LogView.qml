import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Desk32.Core
import "../components"

Item {
    id: logViewRoot

    function refresh() {
        logTable.refresh()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12

        // Filter & Mode Header
        BrushedMetalPanel {
            Layout.fillWidth: true
            Layout.preferredHeight: 58
            baseTint: "#161b22"
            borderColor: "#313945"
            showScrews: true

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 20
                spacing: 8

                ConsoleButton {
                    text: qsTr("ALL LOGS")
                    implicitWidth: 110
                    highlighted: !logTable.playbackHistoryOnly
                    onClicked: {
                        logTable.playbackHistoryOnly = false
                        logTable.refresh()
                    }
                }

                ConsoleButton {
                    text: qsTr("PLAYBACK HISTORY")
                    implicitWidth: 148
                    highlighted: logTable.playbackHistoryOnly
                    onClicked: {
                        logTable.playbackHistoryOnly = true
                        logTable.refresh()
                    }
                }

                Rectangle { width: 1; height: 22; color: "#262e3a" }

                ComboBox {
                    id: catFilter
                    implicitWidth: 150
                    model: [qsTr("All Categories"), "Session", "Transport", "Deck", "FolderSession", "AudioEngine", "DSP", "Routing", "ExternalInsert", "PresetManagement", "Render", "MasterControl", "System"]
                    onActivated: logTable.refresh()
                }

                ComboBox {
                    id: statusFilter
                    implicitWidth: 130
                    model: [qsTr("All Statuses"), "Success", "Failed", "Warning", "Info", "Cancelled"]
                    onActivated: logTable.refresh()
                }

                TextField {
                    id: searchField
                    Layout.fillWidth: true
                    Layout.minimumWidth: 160
                    placeholderText: qsTr("Search events, targets, details...")
                    color: "#E4ECF2"
                    onAccepted: logTable.refresh()
                }

                ConsoleButton {
                    text: qsTr("Search")
                    implicitWidth: 76
                    onClicked: logTable.refresh()
                }

                ConsoleButton {
                    text: qsTr("Export JSONL")
                    implicitWidth: 112
                    accentColor: "#00E676"
                    onClicked: {
                        var path = "/tmp/desk32_log_export_" + Date.now() + ".jsonl"
                        var ok = LogService.exportFilteredToJsonl(path, logTable.buildFilter(0))
                        exportStatusLbl.text = ok ? qsTr("Exported: %1").arg(path) : qsTr("Export failed")
                    }
                }
            }
        }

        // 2-Pane Body: Log Table (Left) + Operation Details (Right)
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            BrushedMetalPanel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                baseTint: "#141920"
                borderColor: "#313945"

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6

                    ListView {
                        id: logTable
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: []
                        property bool playbackHistoryOnly: false
                        property var selectedEvent: null

                        function statusColor(s) {
                            if (s === "Success") return "#00E676"
                            if (s === "Failed") return "#FF5252"
                            if (s === "Warning") return "#F5B841"
                            if (s === "Cancelled") return "#FFB300"
                            return "#00E5FF"
                        }

                        function buildFilter(limitCount) {
                            var f = {}
                            if (catFilter.currentIndex > 0) f["category"] = catFilter.currentText
                            if (statusFilter.currentIndex > 0) f["status"] = statusFilter.currentText
                            if (playbackHistoryOnly) f["playbackHistoryOnly"] = true
                            if (searchField.text.length > 0) f["searchText"] = searchField.text
                            f["limit"] = limitCount
                            return f
                        }

                        function refresh() {
                            if (playbackHistoryOnly) {
                                var hist = LogService.fetchPlaybackHistory(200)
                                model = hist.map(function(h) {
                                    return {
                                        eventId: h.eventId,
                                        operationId: "",
                                        parentOperationId: "",
                                        timestampUtc: h.timestampUtc || "",
                                        localUtcOffset: h.localUtcOffset || "",
                                        category: h.category || "Transport",
                                        operation: h.operation || "Play",
                                        status: "Success",
                                        targetType: "Deck",
                                        target: h.deck || "",
                                        message: "[Deck " + (h.deck || "?") + "] " + (h.fileName || h.trackPath || h.message || ""),
                                        errorCode: 0,
                                        durationMs: h.durationMs || 0.0,
                                        detailsJson: h.detailsJson || "{}"
                                    }
                                })
                            } else {
                                model = LogService.fetchLogs(buildFilter(300))
                            }
                        }

                        Component.onCompleted: refresh()

                        delegate: Rectangle {
                            width: logTable.width
                            height: 34
                            color: (logTable.selectedEvent && logTable.selectedEvent.eventId === modelData.eventId)
                                   ? "#13222b" : (index % 2 === 0 ? "#0d1014" : "#101419")
                            border.color: "#1b212a"

                            MouseArea {
                                anchors.fill: parent
                                onClicked: logTable.selectedEvent = modelData
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 10

                                Label {
                                    text: modelData.timestampUtc || ""
                                    font.family: "Monospace"
                                    font.pixelSize: 10
                                    color: "#8E9AA8"
                                    Layout.preferredWidth: 165
                                    elide: Text.ElideRight
                                }
                                Label {
                                    text: modelData.status || ""
                                    font.family: "Monospace"
                                    font.pixelSize: 10
                                    font.bold: true
                                    color: logTable.statusColor(modelData.status)
                                    Layout.preferredWidth: 74
                                }
                                Label {
                                    text: (modelData.category || "") + "/" + (modelData.operation || "")
                                    font.family: "Monospace"
                                    font.pixelSize: 10
                                    color: "#B4C2CF"
                                    Layout.preferredWidth: 210
                                    elide: Text.ElideRight
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    text: modelData.message || ""
                                    font.pixelSize: 11
                                    color: "#E4ECF2"
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }

                    Label {
                        id: exportStatusLbl
                        Layout.fillWidth: true
                        text: qsTr("Retention: 180 days / 300,000 root operations (SQLite WAL)")
                        font.family: "Monospace"
                        font.pixelSize: 10
                        color: "#768492"
                        elide: Text.ElideRight
                    }
                }
            }

            // Right Detail Pane
            BrushedMetalPanel {
                Layout.preferredWidth: 460
                Layout.fillHeight: true
                baseTint: "#0c0f13"
                borderColor: "#242c37"

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 8

                    Label {
                        text: qsTr("OPERATION DETAILS")
                        font.family: "Monospace"
                        font.pixelSize: 12
                        font.bold: true
                        color: "#00E5FF"
                    }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.family: "Monospace"
                        font.pixelSize: 11
                        color: "#C8D3DF"
                        text: logTable.selectedEvent
                              ? qsTr("Operation ID: %1\nParent ID: %2\nStatus: %3\nError Code: %4\nDuration: %5 ms\n\nMessage:\n%6")
                                    .arg(logTable.selectedEvent.operationId || "-")
                                    .arg(logTable.selectedEvent.parentOperationId || "-")
                                    .arg(logTable.selectedEvent.status || "-")
                                    .arg(logTable.selectedEvent.errorCode || 0)
                                    .arg(logTable.selectedEvent.durationMs || 0)
                                    .arg(logTable.selectedEvent.message || "")
                              : qsTr("Select an event row to inspect details and details_json.")
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: "#202732" }

                    Label {
                        text: "details_json"
                        font.family: "Monospace"
                        font.pixelSize: 10
                        font.bold: true
                        color: "#8090A0"
                    }

                    ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true

                        TextArea {
                            readOnly: true
                            wrapMode: TextEdit.WrapAnywhere
                            font.family: "Monospace"
                            font.pixelSize: 10
                            color: "#A5D6A7"
                            background: Rectangle { color: "#07090c"; border.color: "#1b212a"; radius: 3 }
                            text: {
                                if (!logTable.selectedEvent || !logTable.selectedEvent.detailsJson) return "{}"
                                try {
                                    return JSON.stringify(JSON.parse(logTable.selectedEvent.detailsJson), null, 2)
                                } catch (e) {
                                    return logTable.selectedEvent.detailsJson
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
