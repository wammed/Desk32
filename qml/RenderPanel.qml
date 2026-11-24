import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Desk32.Core
import "components"

// Phase 4 Step 3: Render operation / progress / monitor / audition / cancel / result panel.
// Unified 1600x900 2-column studio console layout.
// All render state is read directly from EngineController, which delegates to RenderEngine
// (isIdle(), workerState(), queuedTasksCount(), currentTaskInfo()). No local state duplication.
Rectangle {
    id: renderRoot

    implicitWidth: 1600
    implicitHeight: 900
    color: showHeader ? "#121519" : "transparent"
    border.color: "#262b32"
    border.width: showHeader ? 1 : 0
    radius: showHeader ? 6 : 0
    clip: true

    property bool showHeader: true
    signal closeRequested()
    signal openLogRequested()

    property string albumFolder: ""
    property string auditionDeck: "B"

    readonly property var bitDepthLabels: [qsTr("16-bit PCM"), qsTr("24-bit PCM"), qsTr("32-bit IEEE Float")]
    readonly property var rateValues: [44100, 48000, 88200, 96000, 176400, 192000]
    readonly property bool engineIdle: EngineController.isRenderIdle
    readonly property bool terminal: EngineController.renderStatus === 2
                                     || EngineController.renderStatus === 3
                                     || EngineController.renderStatus === 4

    function dbText(g) {
        if (g <= 0.0001) return "-∞ dB"
        var db = 20.0 * Math.log10(g)
        return (db > 0 ? "+" : "") + db.toFixed(1) + " dB"
    }

    function statusColor(s) {
        if (s === "Completed") return "#00E676"
        if (s === "Failed") return "#FF5252"
        if (s === "Cancelled") return "#FFB300"
        if (s === "Processing") return "#00E5FF"
        return "#78909C"
    }

    function deckLocked(id) {
        var st = EngineController.getDeckState(id)
        return !!(st && st.isLocked)
    }

    component RenderSectionCard: Rectangle {
        radius: 5
        color: "#090D13"
        border.color: "#222C38"
        border.width: 1
    }

    component DarkCheckBox: CheckBox {
        id: cb
        font.pixelSize: 11
        spacing: 8
        indicator: Rectangle {
            implicitWidth: 18
            implicitHeight: 18
            x: cb.leftPadding
            y: parent.height / 2 - height / 2
            radius: 3
            color: cb.enabled ? "#070A0F" : "#0D1117"
            border.color: cb.checked && cb.enabled ? "#00E5FF" : "#324050"
            border.width: 1

            Label {
                anchors.centerIn: parent
                visible: cb.checked
                text: "✓"
                font.pixelSize: 12
                font.bold: true
                color: cb.enabled ? "#00E5FF" : "#4E5D6C"
            }
        }
        contentItem: Label {
            text: cb.text
            font: cb.font
            color: cb.enabled ? "#DCE6F0" : "#5D6B7A"
            verticalAlignment: Text.AlignVCenter
            leftPadding: cb.indicator.width + cb.spacing
        }
    }

    FolderDialog {
        id: albumFolderDialog
        title: qsTr("Select Folder for Album Render")
        onAccepted: {
            var p = selectedFolder.toString()
            if (p.startsWith("file://")) p = decodeURIComponent(p.substring(7))
            renderRoot.albumFolder = p
        }
    }

    Dialog {
        id: cancelAllDialog
        title: qsTr("Cancel all renders?")
        modal: true
        anchors.centerIn: Overlay.overlay
        standardButtons: Dialog.Yes | Dialog.No
        Label {
            text: qsTr("The running render and all queued renders will be cancelled. Staged files are rolled back; original sources are untouched.")
            wrapMode: Text.WordWrap
            width: 360
        }
        onAccepted: EngineController.cancelAllRenders()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: renderRoot.showHeader ? 14 : 4
        spacing: 12

        RowLayout {
            visible: renderRoot.showHeader
            Layout.fillWidth: true
            Rectangle { Layout.preferredWidth: 8; Layout.preferredHeight: 8; width: 8; height: 8; radius: 4; color: "#00E5FF" }
            Label {
                text: qsTr("EXPORT / RENDER")
                font.bold: true
                font.pixelSize: 12
                font.family: "Monospace"
                color: "#00E5FF"
            }
            Item { Layout.fillWidth: true }
            ConsoleButton {
                text: "✕"
                implicitWidth: 28
                implicitHeight: 24
                onClicked: renderRoot.closeRequested()
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth

            RowLayout {
                width: Math.max(640, renderRoot.width - (renderRoot.showHeader ? 28 : 8))
                spacing: 14

                // =============================================================
                // LEFT COLUMN: RENDER SETTINGS + RENDER MONITOR (AUDITION)
                // =============================================================
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    Layout.alignment: Qt.AlignTop
                    spacing: 14

                    // ============ 1. RENDER OPERATION ============
                    RenderSectionCard {
                        Layout.fillWidth: true
                        implicitHeight: settingsCol.implicitHeight + 32

                        ColumnLayout {
                            id: settingsCol
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 12

                            Label {
                                text: qsTr("RENDER SETTINGS")
                                font.family: "Monospace"
                                font.pixelSize: 12
                                font.bold: true
                                color: "#E8BA6B"
                            }

                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; height: 1; color: "#1E2632" }

                            RowLayout {
                                spacing: 12
                                Label { text: qsTr("Format:"); font.pixelSize: 12; color: "#B8C4D0"; Layout.preferredWidth: 118 }
                                ConsoleComboBox {
                                    id: bitDepthCombo
                                    Layout.fillWidth: true
                                    implicitHeight: 36
                                    model: renderRoot.bitDepthLabels
                                    currentIndex: 1
                                    font.pixelSize: 11
                                }
                            }
                            RowLayout {
                                spacing: 12
                                Label { text: qsTr("Sample Rate:"); font.pixelSize: 12; color: "#B8C4D0"; Layout.preferredWidth: 118 }
                                ConsoleComboBox {
                                    id: rateModeCombo
                                    Layout.preferredWidth: 190
                                    implicitHeight: 36
                                    model: [qsTr("Original Rate"), qsTr("Custom Rate")]
                                    font.pixelSize: 11
                                }
                                ConsoleComboBox {
                                    id: customRateCombo
                                    Layout.fillWidth: true
                                    implicitHeight: 36
                                    enabled: rateModeCombo.currentIndex === 1
                                    model: ["44.1 kHz", "48 kHz", "88.2 kHz", "96 kHz", "176.4 kHz", "192 kHz"]
                                    currentIndex: 1
                                    font.pixelSize: 11
                                }
                            }
                            RowLayout {
                                spacing: 20
                                DarkCheckBox {
                                    id: ditherCheck
                                    text: qsTr("Apply TPDF Dither (integer PCM only)")
                                    checked: true
                                    enabled: bitDepthCombo.currentIndex !== 2
                                }
                                DarkCheckBox {
                                    id: masterDspCheck
                                    text: qsTr("Enable Master Chain DSP")
                                    checked: true
                                }
                            }

                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; height: 1; color: "#1E2632" }

                            // Active Workspace Output Target Indicator (Spec v3.8.1 §19 D-2)
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 36
                                height: 36
                                radius: 4
                                color: WorkspaceManager.hasOpenWorkspace ? "#0d1f18" : "#2b1616"
                                border.color: WorkspaceManager.hasOpenWorkspace ? "#00E67660" : "#FF5252"
                                Label {
                                    anchors.fill: parent
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 12
                                    verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideMiddle
                                    font.family: "Monospace"
                                    font.pixelSize: 11
                                    font.bold: true
                                    color: WorkspaceManager.hasOpenWorkspace ? "#00E676" : "#FF8A80"
                                    text: WorkspaceManager.hasOpenWorkspace
                                          ? qsTr("Output Workspace: %1").arg(WorkspaceManager.openWorkspacePath)
                                          : qsTr("No Workspace Open (Read-Only Mode) — Open a Workspace to Render")
                                }
                            }

                            // Single track render from a Reference Deck
                            RowLayout {
                                spacing: 12
                                Label { text: qsTr("Track Render:"); font.pixelSize: 12; color: "#B8C4D0"; Layout.preferredWidth: 118 }
                                ConsoleComboBox {
                                    id: sourceDeckCombo
                                    Layout.preferredWidth: 108
                                    implicitHeight: 36
                                    model: ["A", "B", "C", "D"]
                                    font.pixelSize: 11
                                    Component.onCompleted: currentIndex = Math.max(0, model.indexOf(EngineController.activeDeckId))
                                }
                                ConsoleButton {
                                    Layout.fillWidth: true
                                    implicitHeight: 38
                                    text: qsTr("Render Deck %1 Track").arg(sourceDeckCombo.currentText)
                                    fontSize: 11
                                    accentColor: "#F5B841"
                                    enabled: WorkspaceManager.hasOpenWorkspace
                                    onClicked: {
                                        EngineController.startTrackRender(
                                            sourceDeckCombo.currentText,
                                            bitDepthCombo.currentIndex,
                                            rateModeCombo.currentIndex,
                                            renderRoot.rateValues[customRateCombo.currentIndex],
                                            ditherCheck.checked && bitDepthCombo.currentIndex !== 2,
                                            masterDspCheck.checked)
                                    }
                                }
                            }

                            // Album render
                            RowLayout {
                                spacing: 12
                                Label { text: qsTr("Album Render:"); font.pixelSize: 12; color: "#B8C4D0"; Layout.preferredWidth: 118 }
                                ConsoleButton {
                                    text: qsTr("Choose Folder…")
                                    implicitWidth: 164
                                    implicitHeight: 36
                                    fontSize: 11
                                    onClicked: albumFolderDialog.open()
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    text: renderRoot.albumFolder.length > 0 ? renderRoot.albumFolder : qsTr("(no folder)")
                                    elide: Text.ElideMiddle
                                    font.family: "Monospace"
                                    font.pixelSize: 11
                                    color: "#8EA0B0"
                                }
                            }
                            ConsoleButton {
                                Layout.fillWidth: true
                                implicitHeight: 40
                                text: qsTr("Start Album Render")
                                fontSize: 12
                                accentColor: "#00E5FF"
                                enabled: WorkspaceManager.hasOpenWorkspace && renderRoot.albumFolder.length > 0
                                onClicked: {
                                    EngineController.startAlbumRender(
                                        renderRoot.albumFolder,
                                        bitDepthCombo.currentIndex,
                                        rateModeCombo.currentIndex,
                                        renderRoot.rateValues[customRateCombo.currentIndex],
                                        ditherCheck.checked && bitDepthCombo.currentIndex !== 2,
                                        masterDspCheck.checked)
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                font.pixelSize: 10
                                color: "#7A8998"
                                text: qsTr("Output is written to a new {Serial}{YYYYMMDD_HHMMSS_mmm} folder inside the open dedicated workspace. Originals are never modified.")
                            }
                        }
                    }

                    // ============ 3. RENDER MONITOR ============
                    RenderSectionCard {
                        Layout.fillWidth: true
                        implicitHeight: monitorCol.implicitHeight + 32

                        ColumnLayout {
                            id: monitorCol
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 12

                            Label {
                                text: qsTr("RENDER MONITOR (AUDITION)")
                                font.family: "Monospace"
                                font.pixelSize: 12
                                font.bold: true
                                color: "#78D8F0"
                            }

                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; height: 1; color: "#1E2632" }

                            RowLayout {
                                spacing: 12
                                ConsoleButton {
                                    Layout.fillWidth: true
                                    implicitHeight: 40
                                    text: qsTr("Listening Path")
                                    fontSize: 11
                                    showLed: true
                                    ledColor: EngineController.outputRoutingMode === 0 ? "#00E5FF" : "#3A4452"
                                    accentColor: "#00E5FF"
                                    highlighted: EngineController.outputRoutingMode === 0
                                    onClicked: EngineController.outputRoutingMode = 0
                                }
                                ConsoleButton {
                                    Layout.fillWidth: true
                                    implicitHeight: 40
                                    text: qsTr("Render Monitor Path")
                                    fontSize: 11
                                    showLed: true
                                    ledColor: EngineController.outputRoutingMode === 1 ? "#F5B841" : "#3A4452"
                                    accentColor: "#F5B841"
                                    highlighted: EngineController.outputRoutingMode === 1
                                    onClicked: EngineController.outputRoutingMode = 1
                                }
                            }
                            RowLayout {
                                spacing: 12
                                enabled: EngineController.outputRoutingMode === 1
                                opacity: enabled ? 1.0 : 0.5
                                Label { text: qsTr("Monitor Vol:"); font.pixelSize: 12; color: "#B8C4D0"; Layout.preferredWidth: 118 }
                                Slider {
                                    id: monVolSlider
                                    Layout.fillWidth: true
                                    from: 0.0; to: 2.0
                                    value: EngineController.renderMonitorVolume
                                    onMoved: EngineController.renderMonitorVolume = value
                                    background: Rectangle {
                                        x: monVolSlider.leftPadding
                                        y: monVolSlider.topPadding + monVolSlider.availableHeight / 2 - height / 2
                                        width: monVolSlider.availableWidth
                                        height: 6
                                        radius: 3
                                        color: "#06090E"
                                        border.color: "#222C38"
                                        Rectangle {
                                            width: monVolSlider.visualPosition * parent.width
                                            height: parent.height
                                            radius: 3
                                            color: "#00E5FF"
                                        }
                                    }
                                    handle: Rectangle {
                                        x: monVolSlider.leftPadding + monVolSlider.visualPosition * (monVolSlider.availableWidth - width)
                                        y: monVolSlider.topPadding + monVolSlider.availableHeight / 2 - height / 2
                                        width: 16
                                        height: 16
                                        radius: 8
                                        color: monVolSlider.pressed ? "#33E3FF" : "#DCE6F0"
                                        border.color: "#00E5FF"
                                        border.width: 2
                                    }
                                }
                                Label {
                                    text: renderRoot.dbText(EngineController.renderMonitorVolume)
                                    font.pixelSize: 12; font.family: "Monospace"; font.bold: true; color: "#00E5FF"
                                    Layout.preferredWidth: 78
                                    horizontalAlignment: Text.AlignRight
                                }
                                ConsoleButton {
                                    text: "0 dB"
                                    implicitWidth: 68
                                    implicitHeight: 32
                                    fontSize: 10
                                    onClicked: EngineController.renderMonitorVolume = 1.0
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                font.pixelSize: 10
                                color: "#7A8998"
                                text: qsTr("Monitor volume affects monitoring only. It never changes rendered file PCM or the Render Bus.")
                            }
                        }
                    }
                }

                // =============================================================
                // RIGHT COLUMN: PROGRESS + RESULT + RENDERED FILE AUDITION
                // =============================================================
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    Layout.alignment: Qt.AlignTop
                    spacing: 14

                    // ============ 2. PROGRESS + 5. CANCEL ============
                    RenderSectionCard {
                        Layout.fillWidth: true
                        implicitHeight: progressCol.implicitHeight + 32

                        ColumnLayout {
                            id: progressCol
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 12

                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    text: qsTr("PROGRESS")
                                    font.family: "Monospace"
                                    font.pixelSize: 12
                                    font.bold: true
                                    color: "#78D8F0"
                                }
                                Item { Layout.fillWidth: true }
                                Label {
                                    text: EngineController.renderStatusText.toUpperCase()
                                    font.pixelSize: 11; font.bold: true; font.family: "Monospace"
                                    color: renderRoot.statusColor(EngineController.renderStatusText)
                                }
                            }

                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; height: 1; color: "#1E2632" }

                            RowLayout {
                                spacing: 12
                                Rectangle {
                                    Layout.preferredWidth: 118; Layout.preferredHeight: 26
                                    width: 118; height: 26; radius: 3
                                    color: "#06090E"
                                    border.color: EngineController.renderWorkerState === 0 ? "#2a313b" : "#00E5FF"
                                    Label {
                                        anchors.centerIn: parent
                                        text: EngineController.renderWorkerStateText.toUpperCase()
                                        font.pixelSize: 10; font.bold: true; font.family: "Monospace"
                                        color: EngineController.renderWorkerState === 0 ? "#78909C" : "#00E5FF"
                                    }
                                }
                                Label {
                                    text: qsTr("Queued: %1").arg(EngineController.renderQueuedCount)
                                    font.pixelSize: 11; font.family: "Monospace"; color: "#B8C4D0"
                                }
                                Item { Layout.fillWidth: true }
                            }

                            ProgressBar {
                                id: renderProgressBar
                                Layout.fillWidth: true
                                implicitHeight: 14
                                from: 0; to: 1
                                value: EngineController.renderProgress
                                background: Rectangle {
                                    implicitHeight: 14
                                    color: "#06090E"
                                    border.color: "#222C38"
                                    radius: 4
                                }
                                contentItem: Item {
                                    implicitHeight: 14
                                    Rectangle {
                                        width: renderProgressBar.visualPosition * parent.width
                                        height: parent.height
                                        radius: 4
                                        color: "#00E5FF"
                                    }
                                }
                            }
                            RowLayout {
                                Label {
                                    text: Math.round(EngineController.renderProgress * 100) + " %"
                                    font.pixelSize: 14; font.bold: true; font.family: "Monospace"; color: "#00E5FF"
                                }
                                Item { Layout.fillWidth: true }
                                Label {
                                    visible: EngineController.renderTotalTracks > 0
                                    text: EngineController.renderTrackIndex + " / " + EngineController.renderTotalTracks
                                    font.pixelSize: 12; font.family: "Monospace"; color: "#E0E6ED"
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: EngineController.renderCurrentTrackName.length > 0
                                text: EngineController.renderCurrentTrackName
                                elide: Text.ElideMiddle
                                font.pixelSize: 11; color: "#90CAF9"
                            }

                            RowLayout {
                                spacing: 12
                                ConsoleButton {
                                    Layout.fillWidth: true
                                    implicitHeight: 36
                                    text: qsTr("Cancel Current")
                                    fontSize: 11
                                    accentColor: "#FFB300"
                                    enabled: !renderRoot.engineIdle
                                    onClicked: EngineController.cancelRender()
                                }
                                ConsoleButton {
                                    Layout.fillWidth: true
                                    implicitHeight: 36
                                    text: qsTr("Cancel All")
                                    fontSize: 11
                                    accentColor: "#FF5252"
                                    enabled: !renderRoot.engineIdle
                                    onClicked: cancelAllDialog.open()
                                }
                            }
                        }
                    }

                    // ============ 6. RESULT / ERROR ============
                    RenderSectionCard {
                        Layout.fillWidth: true
                        implicitHeight: resultCol.implicitHeight + 32
                        visible: renderRoot.terminal

                        ColumnLayout {
                            id: resultCol
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 10

                            Label {
                                text: qsTr("RESULT")
                                font.family: "Monospace"
                                font.pixelSize: 12
                                font.bold: true
                                color: "#E8BA6B"
                            }

                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; height: 1; color: "#1E2632" }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 32
                                height: 32
                                radius: 4
                                color: "#06090E"
                                border.color: renderRoot.statusColor(EngineController.renderStatusText)
                                Label {
                                    anchors.centerIn: parent
                                    text: EngineController.renderStatusText.toUpperCase()
                                        + (EngineController.renderSerial.length > 0 ? "  [" + EngineController.renderSerial + "]" : "")
                                    font.bold: true; font.pixelSize: 12; font.family: "Monospace"
                                    color: renderRoot.statusColor(EngineController.renderStatusText)
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: EngineController.renderStatus === 2
                                text: qsTr("Output: %1").arg(EngineController.renderLastOutputDir.split('/').pop())
                                font.pixelSize: 11; font.family: "Monospace"; color: "#E0E6ED"
                                elide: Text.ElideMiddle
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: EngineController.renderErrorMessage.length > 0
                                text: EngineController.renderErrorMessage
                                wrapMode: Text.WordWrap
                                font.pixelSize: 11; color: "#FF8A80"
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: EngineController.renderRollbackPerformed
                                text: EngineController.renderRollbackSucceeded
                                      ? qsTr("Rollback: staging folder removed, no partial output remains.")
                                      : qsTr("Rollback FAILED: staging folder may remain. See Log Tab.")
                                wrapMode: Text.WordWrap
                                font.pixelSize: 11
                                color: EngineController.renderRollbackSucceeded ? "#A0AAB2" : "#FF5252"
                            }

                            // Per-track status list (Completed / Failed / Cancelled / Unexecuted)
                            Repeater {
                                model: EngineController.renderTotalTracks > 1 ? EngineController.renderTrackResults : []
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    Label {
                                        text: modelData.trackIndex
                                        font.pixelSize: 10; font.family: "Monospace"; color: "#78909C"
                                        Layout.preferredWidth: 24
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: modelData.fileName
                                        elide: Text.ElideMiddle
                                        font.pixelSize: 11; color: "#E0E6ED"
                                    }
                                    Label {
                                        text: modelData.status === "Queued" ? qsTr("Unexecuted") : modelData.status
                                        font.pixelSize: 10; font.bold: true
                                        color: renderRoot.statusColor(modelData.status)
                                    }
                                }
                            }

                            RowLayout {
                                spacing: 10
                                ConsoleButton {
                                    text: qsTr("Open in Log Tab")
                                    implicitWidth: 160
                                    implicitHeight: 34
                                    fontSize: 11
                                    accentColor: "#00E5FF"
                                    onClicked: renderRoot.openLogRequested()
                                }
                                ConsoleButton {
                                    text: qsTr("Dismiss")
                                    implicitWidth: 110
                                    implicitHeight: 34
                                    fontSize: 11
                                    enabled: renderRoot.engineIdle
                                    onClicked: EngineController.resetRenderStatus()
                                }
                            }
                        }
                    }

                    // ============ 4. RENDERED FILE AUDITION ============
                    RenderSectionCard {
                        Layout.fillWidth: true
                        implicitHeight: auditionCol.implicitHeight + 32

                        ColumnLayout {
                            id: auditionCol
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 12

                            Label {
                                text: qsTr("RENDERED FILE AUDITION")
                                font.family: "Monospace"
                                font.pixelSize: 12
                                font.bold: true
                                color: "#E8BA6B"
                            }

                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; height: 1; color: "#1E2632" }

                            Rectangle {
                                Layout.fillWidth: true
                                visible: EngineController.outputRoutingMode === 0
                                implicitHeight: warnRow.implicitHeight + 16
                                color: "#2a2410"
                                border.color: "#FFB300"
                                radius: 4
                                RowLayout {
                                    id: warnRow
                                    anchors.fill: parent
                                    anchors.margins: 8
                                    spacing: 10
                                    Label {
                                        id: warnLabel
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        font.pixelSize: 11
                                        color: "#FFD54F"
                                        text: qsTr("Listening Path active: Listening Correction is currently applied to playback.")
                                    }
                                    ConsoleButton {
                                        text: qsTr("Use Render Monitor")
                                        implicitWidth: 164
                                        implicitHeight: 30
                                        fontSize: 10
                                        accentColor: "#FFB300"
                                        onClicked: EngineController.outputRoutingMode = 1
                                    }
                                }
                            }

                            RowLayout {
                                spacing: 10
                                Label { text: qsTr("Load to:"); font.pixelSize: 12; color: "#B8C4D0"; Layout.preferredWidth: 80 }
                                Repeater {
                                    model: ["A", "B", "C", "D"]
                                    ConsoleButton {
                                        property bool locked: false
                                        Layout.fillWidth: true
                                        implicitHeight: 36
                                        text: modelData + (locked ? " 🔒" : "")
                                        fontSize: 11
                                        showLed: true
                                        ledColor: renderRoot.auditionDeck === modelData ? "#00E5FF" : "#3A4452"
                                        checkable: true
                                        checked: renderRoot.auditionDeck === modelData
                                        accentColor: "#00E5FF"
                                        Component.onCompleted: locked = renderRoot.deckLocked(modelData)
                                        Connections {
                                            target: EngineController
                                            function onDeckStateChanged() { locked = renderRoot.deckLocked(modelData) }
                                        }
                                        onClicked: renderRoot.auditionDeck = modelData
                                    }
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                visible: EngineController.renderOutputFiles.length === 0
                                text: qsTr("No rendered files yet. Complete a render to audition its output.")
                                font.pixelSize: 11; color: "#7A8998"
                                wrapMode: Text.WordWrap
                            }

                            Repeater {
                                model: EngineController.renderOutputFiles
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 10
                                    Label {
                                        Layout.fillWidth: true
                                        text: modelData.split('/').pop()
                                        elide: Text.ElideMiddle
                                        font.pixelSize: 11; color: "#E0E6ED"
                                    }
                                    ConsoleButton {
                                        text: qsTr("Audition on Deck %1").arg(renderRoot.auditionDeck)
                                        implicitWidth: 174
                                        implicitHeight: 32
                                        fontSize: 10
                                        accentColor: "#00E676"
                                        onClicked: {
                                            // Keep the user's current path choice visible: the warning banner above
                                            // already tells them if Listening Correction is applied. Default engages
                                            // Render Monitor Path per §5.11.5.
                                            if (!EngineController.auditionRenderedFile(renderRoot.auditionDeck, modelData)) {
                                                auditionErr.text = qsTr("Audition failed (deck locked or file unreadable). See Log Tab.")
                                            } else {
                                                auditionErr.text = ""
                                            }
                                        }
                                    }
                                }
                            }
                            Label {
                                id: auditionErr
                                Layout.fillWidth: true
                                visible: text.length > 0
                                wrapMode: Text.WordWrap
                                font.pixelSize: 10
                                color: "#FF8A80"
                            }
                        }
                    }
                }
            }
        }
    }
}
