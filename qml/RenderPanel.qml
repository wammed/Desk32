import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Desk32.Core

// Phase 4 Step 3: Render operation / progress / monitor / audition / cancel / result panel.
// All render state is read directly from EngineController, which delegates to RenderEngine
// (isIdle(), workerState(), queuedTasksCount(), currentTaskInfo()). No local state duplication.
Rectangle {
    id: renderRoot

    implicitWidth: 460
    implicitHeight: 760
    color: "#121519"
    border.color: "#262b32"
    border.width: 1
    radius: 6
    clip: true

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

    FolderDialog {
        id: albumFolderDialog
        title: qsTr("Select Folder for Album Render")
        onAccepted: {
            var p = selectedFolder.toString()
            if (p.startsWith("file://")) p = p.substring(7)
            renderRoot.albumFolder = p
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            Rectangle { width: 8; height: 8; radius: 4; color: "#00E5FF" }
            Label {
                text: qsTr("EXPORT / RENDER")
                font.bold: true
                font.pixelSize: 11
                font.family: "Monospace"
                color: "#00E5FF"
            }
            Item { Layout.fillWidth: true }
            Button {
                text: "✕"
                Layout.preferredWidth: 26
                Layout.preferredHeight: 22
                onClicked: renderRoot.closeRequested()
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth

            ColumnLayout {
                width: renderRoot.width - 28
                spacing: 10

                // ============ 1. RENDER OPERATION ============
                GroupBox {
                    Layout.fillWidth: true
                    title: qsTr("RENDER SETTINGS")
                    font.pixelSize: 10
                    font.bold: true

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 6

                        RowLayout {
                            Label { text: qsTr("Format:"); font.pixelSize: 10; color: "#A0AAB2"; Layout.preferredWidth: 80 }
                            ComboBox {
                                id: bitDepthCombo
                                Layout.fillWidth: true
                                model: renderRoot.bitDepthLabels
                                currentIndex: 1
                                font.pixelSize: 10
                            }
                        }
                        RowLayout {
                            Label { text: qsTr("Sample Rate:"); font.pixelSize: 10; color: "#A0AAB2"; Layout.preferredWidth: 80 }
                            ComboBox {
                                id: rateModeCombo
                                Layout.preferredWidth: 130
                                model: [qsTr("Original Rate"), qsTr("Custom Rate")]
                                font.pixelSize: 10
                            }
                            ComboBox {
                                id: customRateCombo
                                Layout.fillWidth: true
                                enabled: rateModeCombo.currentIndex === 1
                                model: ["44.1 kHz", "48 kHz", "88.2 kHz", "96 kHz", "176.4 kHz", "192 kHz"]
                                currentIndex: 1
                                font.pixelSize: 10
                            }
                        }
                        CheckBox {
                            id: ditherCheck
                            text: qsTr("Apply TPDF Dither (integer PCM only)")
                            checked: true
                            enabled: bitDepthCombo.currentIndex !== 2
                            font.pixelSize: 10
                        }
                        CheckBox {
                            id: masterDspCheck
                            text: qsTr("Enable Master Chain DSP")
                            checked: true
                            font.pixelSize: 10
                        }

                        Rectangle { Layout.fillWidth: true; height: 1; color: "#222730" }

                        // Active Workspace Output Target Indicator (Spec v3.8.1 §19 D-2)
                        Rectangle {
                            Layout.fillWidth: true
                            height: 28
                            radius: 3
                            color: WorkspaceManager.hasOpenWorkspace ? "#0d1f18" : "#2b1616"
                            border.color: WorkspaceManager.hasOpenWorkspace ? "#00E67660" : "#FF5252"
                            Label {
                                anchors.fill: parent
                                anchors.margins: 6
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideMiddle
                                font.pixelSize: 9
                                font.bold: true
                                color: WorkspaceManager.hasOpenWorkspace ? "#00E676" : "#FF8A80"
                                text: WorkspaceManager.hasOpenWorkspace
                                      ? qsTr("Output Workspace: %1").arg(WorkspaceManager.openWorkspacePath)
                                      : qsTr("No Workspace Open (Read-Only Mode) — Open a Workspace to Render")
                            }
                        }

                        // Single track render from a Reference Deck
                        RowLayout {
                            Label { text: qsTr("Track Render:"); font.pixelSize: 10; color: "#A0AAB2"; Layout.preferredWidth: 80 }
                            ComboBox {
                                id: sourceDeckCombo
                                Layout.preferredWidth: 80
                                model: ["A", "B", "C", "D"]
                                font.pixelSize: 10
                                Component.onCompleted: currentIndex = Math.max(0, model.indexOf(EngineController.activeDeckId))
                            }
                            Button {
                                Layout.fillWidth: true
                                text: qsTr("Render Deck %1 Track").arg(sourceDeckCombo.currentText)
                                font.pixelSize: 10
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
                            Label { text: qsTr("Album Render:"); font.pixelSize: 10; color: "#A0AAB2"; Layout.preferredWidth: 80 }
                            Button {
                                text: qsTr("Choose Folder…")
                                font.pixelSize: 10
                                onClicked: albumFolderDialog.open()
                            }
                            Label {
                                Layout.fillWidth: true
                                text: renderRoot.albumFolder.length > 0 ? renderRoot.albumFolder : qsTr("(no folder)")
                                elide: Text.ElideMiddle
                                font.pixelSize: 9
                                color: "#78909C"
                            }
                        }
                        Button {
                            Layout.fillWidth: true
                            text: qsTr("Start Album Render")
                            font.pixelSize: 10
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
                            font.pixelSize: 9
                            color: "#6c7885"
                            text: qsTr("Output is written to a new {Serial}{YYYYMMDD_HHMMSS_mmm} folder inside the open dedicated workspace. Originals are never modified.")
                        }
                    }
                }

                // ============ 2. PROGRESS + 5. CANCEL ============
                GroupBox {
                    Layout.fillWidth: true
                    title: qsTr("PROGRESS")
                    font.pixelSize: 10
                    font.bold: true

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 6

                        RowLayout {
                            Rectangle {
                                width: 90; height: 20; radius: 3
                                color: "#181c22"
                                border.color: EngineController.renderWorkerState === 0 ? "#2a313b" : "#00E5FF"
                                Label {
                                    anchors.centerIn: parent
                                    text: EngineController.renderWorkerStateText.toUpperCase()
                                    font.pixelSize: 9; font.bold: true; font.family: "Monospace"
                                    color: EngineController.renderWorkerState === 0 ? "#78909C" : "#00E5FF"
                                }
                            }
                            Label {
                                text: qsTr("Queued: %1").arg(EngineController.renderQueuedCount)
                                font.pixelSize: 10; font.family: "Monospace"; color: "#A0AAB2"
                            }
                            Item { Layout.fillWidth: true }
                            Label {
                                text: EngineController.renderStatusText.toUpperCase()
                                font.pixelSize: 10; font.bold: true
                                color: renderRoot.statusColor(EngineController.renderStatusText)
                            }
                        }

                        ProgressBar {
                            Layout.fillWidth: true
                            from: 0; to: 1
                            value: EngineController.renderProgress
                        }
                        RowLayout {
                            Label {
                                text: Math.round(EngineController.renderProgress * 100) + " %"
                                font.pixelSize: 11; font.bold: true; font.family: "Monospace"; color: "#00E5FF"
                            }
                            Item { Layout.fillWidth: true }
                            Label {
                                visible: EngineController.renderTotalTracks > 0
                                text: EngineController.renderTrackIndex + " / " + EngineController.renderTotalTracks
                                font.pixelSize: 11; font.family: "Monospace"; color: "#E0E6ED"
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: EngineController.renderCurrentTrackName.length > 0
                            text: EngineController.renderCurrentTrackName
                            elide: Text.ElideMiddle
                            font.pixelSize: 10; color: "#90CAF9"
                        }

                        RowLayout {
                            spacing: 8
                            Button {
                                Layout.fillWidth: true
                                text: qsTr("Cancel Current")
                                font.pixelSize: 10
                                enabled: !renderRoot.engineIdle
                                onClicked: EngineController.cancelRender()
                            }
                            Button {
                                Layout.fillWidth: true
                                text: qsTr("Cancel All")
                                font.pixelSize: 10
                                enabled: !renderRoot.engineIdle
                                onClicked: cancelAllDialog.open()
                            }
                        }
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
                        width: 320
                    }
                    onAccepted: EngineController.cancelAllRenders()
                }

                // ============ 6. RESULT / ERROR ============
                GroupBox {
                    Layout.fillWidth: true
                    title: qsTr("RESULT")
                    font.pixelSize: 10
                    font.bold: true
                    visible: renderRoot.terminal

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 6

                        Rectangle {
                            Layout.fillWidth: true
                            height: 26
                            radius: 3
                            color: "#181c22"
                            border.color: renderRoot.statusColor(EngineController.renderStatusText)
                            Label {
                                anchors.centerIn: parent
                                text: EngineController.renderStatusText.toUpperCase()
                                    + (EngineController.renderSerial.length > 0 ? "  [" + EngineController.renderSerial + "]" : "")
                                font.bold: true; font.pixelSize: 11
                                color: renderRoot.statusColor(EngineController.renderStatusText)
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: EngineController.renderStatus === 2
                            text: qsTr("Output: %1").arg(EngineController.renderLastOutputDir.split('/').pop())
                            font.pixelSize: 10; font.family: "Monospace"; color: "#E0E6ED"
                            elide: Text.ElideMiddle
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: EngineController.renderErrorMessage.length > 0
                            text: EngineController.renderErrorMessage
                            wrapMode: Text.WordWrap
                            font.pixelSize: 10; color: "#FF8A80"
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: EngineController.renderRollbackPerformed
                            text: EngineController.renderRollbackSucceeded
                                  ? qsTr("Rollback: staging folder removed, no partial output remains.")
                                  : qsTr("Rollback FAILED: staging folder may remain. See Log Tab.")
                            wrapMode: Text.WordWrap
                            font.pixelSize: 10
                            color: EngineController.renderRollbackSucceeded ? "#A0AAB2" : "#FF5252"
                        }

                        // Per-track status list (Completed / Failed / Cancelled / Unexecuted)
                        Repeater {
                            model: EngineController.renderTotalTracks > 1 ? EngineController.renderTrackResults : []
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6
                                Label {
                                    text: modelData.trackIndex
                                    font.pixelSize: 9; font.family: "Monospace"; color: "#78909C"
                                    Layout.preferredWidth: 20
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.fileName
                                    elide: Text.ElideMiddle
                                    font.pixelSize: 10; color: "#E0E6ED"
                                }
                                Label {
                                    text: modelData.status === "Queued" ? qsTr("Unexecuted") : modelData.status
                                    font.pixelSize: 9; font.bold: true
                                    color: renderRoot.statusColor(modelData.status)
                                }
                            }
                        }

                        RowLayout {
                            Button {
                                text: qsTr("Open in Log Tab")
                                font.pixelSize: 10
                                onClicked: renderRoot.openLogRequested()
                            }
                            Button {
                                text: qsTr("Dismiss")
                                font.pixelSize: 10
                                enabled: renderRoot.engineIdle
                                onClicked: EngineController.resetRenderStatus()
                            }
                        }
                    }
                }

                // ============ 3. RENDER MONITOR ============
                GroupBox {
                    Layout.fillWidth: true
                    title: qsTr("RENDER MONITOR (AUDITION)")
                    font.pixelSize: 10
                    font.bold: true

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 6

                        RowLayout {
                            Button {
                                Layout.fillWidth: true
                                text: qsTr("Listening Path")
                                font.pixelSize: 10
                                highlighted: EngineController.outputRoutingMode === 0
                                onClicked: EngineController.outputRoutingMode = 0
                            }
                            Button {
                                Layout.fillWidth: true
                                text: qsTr("Render Monitor Path")
                                font.pixelSize: 10
                                highlighted: EngineController.outputRoutingMode === 1
                                onClicked: EngineController.outputRoutingMode = 1
                            }
                        }
                        RowLayout {
                            enabled: EngineController.outputRoutingMode === 1
                            opacity: enabled ? 1.0 : 0.5
                            Label { text: qsTr("Monitor Vol:"); font.pixelSize: 10; color: "#A0AAB2"; Layout.preferredWidth: 80 }
                            Slider {
                                Layout.fillWidth: true
                                from: 0.0; to: 2.0
                                value: EngineController.renderMonitorVolume
                                onMoved: EngineController.renderMonitorVolume = value
                            }
                            Label {
                                text: renderRoot.dbText(EngineController.renderMonitorVolume)
                                font.pixelSize: 10; font.family: "Monospace"; color: "#00E5FF"
                                Layout.preferredWidth: 60
                                horizontalAlignment: Text.AlignRight
                            }
                            Button {
                                text: "0 dB"
                                font.pixelSize: 9
                                onClicked: EngineController.renderMonitorVolume = 1.0
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            font.pixelSize: 9
                            color: "#6c7885"
                            text: qsTr("Monitor volume affects monitoring only. It never changes rendered file PCM or the Render Bus.")
                        }
                    }
                }

                // ============ 4. RENDERED FILE AUDITION ============
                GroupBox {
                    Layout.fillWidth: true
                    title: qsTr("RENDERED FILE AUDITION")
                    font.pixelSize: 10
                    font.bold: true

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 6

                        Rectangle {
                            Layout.fillWidth: true
                            visible: EngineController.outputRoutingMode === 0
                            height: warnLabel.implicitHeight + 12
                            color: "#2a2410"
                            border.color: "#FFB300"
                            radius: 3
                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 6
                                Label {
                                    id: warnLabel
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: 9
                                    color: "#FFD54F"
                                    text: qsTr("Listening Path active: Listening Correction is currently applied to playback.")
                                }
                                Button {
                                    text: qsTr("Use Render Monitor")
                                    font.pixelSize: 9
                                    onClicked: EngineController.outputRoutingMode = 1
                                }
                            }
                        }

                        RowLayout {
                            Label { text: qsTr("Load to:"); font.pixelSize: 10; color: "#A0AAB2"; Layout.preferredWidth: 80 }
                            Repeater {
                                model: ["A", "B", "C", "D"]
                                Button {
                                    property bool locked: false
                                    text: modelData + (locked ? " 🔒" : "")
                                    font.pixelSize: 10
                                    checkable: true
                                    checked: renderRoot.auditionDeck === modelData
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
                            font.pixelSize: 9; color: "#6c7885"
                            wrapMode: Text.WordWrap
                        }

                        Repeater {
                            model: EngineController.renderOutputFiles
                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.split('/').pop()
                                    elide: Text.ElideMiddle
                                    font.pixelSize: 10; color: "#E0E6ED"
                                }
                                Button {
                                    text: qsTr("Audition on Deck %1").arg(renderRoot.auditionDeck)
                                    font.pixelSize: 9
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
                            font.pixelSize: 9
                            color: "#FF8A80"
                        }
                    }
                }
            }
        }
    }
}

