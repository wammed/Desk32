import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Desk32.Core
import "components"
import "views"

ApplicationWindow {
    id: mainWindow
    objectName: "mainWindow"

    // Default WQHD 2560x1440 hardware console layout; windowed in --screenshot mode for exact capture
    visible: true
    width: 2560
    height: 1440
    minimumWidth: 1280
    minimumHeight: 720
    visibility: Qt.application.arguments.indexOf("--screenshot") >= 0
                ? Window.Windowed
                : Window.FullScreen
    title: qsTr("Desk32 — Mastering & Reference Studio Console")
    color: "#07090c"

    // Active top-level view: "console" | "workspace" | "log" | "settings" (CLI --view= support for verification)
    property string activeViewId: {
        var args = Qt.application.arguments
        for (var i = 0; i < args.length; ++i) {
            if (args[i].indexOf("--view=") === 0) return args[i].substring(7)
        }
        return "console"
    }
    property int currentViewIndex: {
        if (activeViewId === "workspace") return 1
        if (activeViewId === "log") return 2
        if (activeViewId === "settings") return 3
        return 0
    }

    // Q-10: UI Scaling (0.8 / 1.0 / 1.25 / 1.5) persisted via SettingsManager
    property double uiScale: {
        var s = Number(SettingsManager.value("ui/scale", 1.0))
        return (isFinite(s) && s >= 0.75 && s <= 1.6) ? s : 1.0
    }

    property string pendingDeckIdForFile: "A"
    property string pendingDeckIdForFolder: "A"
    property var bitPerfectReport: ({})
    property string bitPerfectExportFeedback: ""
    property bool bitPerfectExportOk: true

    function openBitPerfectReport() {
        bitPerfectReport = EngineController.getBitPerfectVerificationReport()
        bitPerfectExportFeedback = ""
        bitPerfectExportOk = true
        bitPerfectPopup.open()
    }

    Component.onCompleted: {
        if (Qt.application.arguments.indexOf("--open-render") >= 0) {
            renderPopup.open()
        }
    }

    // F11 Fullscreen / Windowed Toggle
    Shortcut {
        sequence: "F11"
        onActivated: {
            if (mainWindow.visibility === Window.FullScreen) {
                mainWindow.showNormal()
            } else {
                mainWindow.showFullScreen()
            }
        }
    }

    // -------------------------------------------------------------------------
    // Root Scaled Hardware Console Container
    // -------------------------------------------------------------------------
    Item {
        id: scaledRoot
        width: mainWindow.width / mainWindow.uiScale
        height: mainWindow.height / mainWindow.uiScale
        transformOrigin: Item.TopLeft
        scale: mainWindow.uiScale

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // Top Fixed Hardware Navigation Bar (48 px)
            TopConsoleBar {
                Layout.fillWidth: true
                currentView: mainWindow.activeViewId
                onViewSelected: function(viewId) {
                    mainWindow.activeViewId = viewId
                    if (viewId === "workspace") {
                        workspaceView.refresh()
                    } else if (viewId === "log") {
                        logView.refresh()
                    } else if (viewId === "settings") {
                        settingsView.refresh()
                    }
                }
            }

            // 4 Non-Destructive Top-Level Views
            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: mainWindow.currentViewIndex

                ConsoleView {
                    id: consoleView
                    onDeckLoadFileRequested: function(deckId) {
                        mainWindow.pendingDeckIdForFile = deckId
                        deckFileDialog.open()
                    }
                    onDeckStartFolderRequested: function(deckId) {
                        mainWindow.pendingDeckIdForFolder = deckId
                        deckFolderDialog.open()
                    }
                    onRenderPopupRequested: renderPopup.open()
                    onBitPerfectReportRequested: mainWindow.openBitPerfectReport()
                }

                WorkspaceView {
                    id: workspaceView
                }

                LogView {
                    id: logView
                }

                SettingsView {
                    id: settingsView
                    selectedUiScale: mainWindow.uiScale
                    onUiScaleChangedRequested: function(newScale) {
                        mainWindow.uiScale = newScale
                        SettingsManager.setValue("ui/scale", newScale)
                    }
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // Native File / Folder Dialogs
    // -------------------------------------------------------------------------
    FileDialog {
        id: deckFileDialog
        title: qsTr("Select Audio Track for Deck %1").arg(mainWindow.pendingDeckIdForFile)
        nameFilters: [
            qsTr("Audio Files (*.wav *.flac *.aiff *.aif *.ogg *.opus *.mp3 *.m4a *.dsf *.dff *.wv)"),
            qsTr("All Files (*)")
        ]
        onAccepted: {
            var localPath = selectedFile.toString()
            if (localPath.indexOf("file://") === 0) {
                localPath = decodeURIComponent(localPath.substring(7))
            }
            EngineController.loadTrack(mainWindow.pendingDeckIdForFile, localPath)
        }
    }

    FolderDialog {
        id: deckFolderDialog
        title: qsTr("Select Folder Session for Deck %1").arg(mainWindow.pendingDeckIdForFolder)
        onAccepted: {
            var localPath = selectedFolder.toString()
            if (localPath.indexOf("file://") === 0) {
                localPath = decodeURIComponent(localPath.substring(7))
            }
            EngineController.startFolderSession(mainWindow.pendingDeckIdForFolder, localPath, [])
        }
    }

    // Export Path Render Modal (hosts verified RenderPanel inside ScrollableModalPopup)
    ScrollableModalPopup {
        id: renderPopup
        titleText: qsTr("OFFLINE EXPORT RENDER ENGINE")
        preferredWidth: 1600
        preferredHeight: 900
        accentColor: "#F5B841"

        RenderPanel {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(520, renderPopup.height - 88)
            showHeader: false
            visible: true
            onCloseRequested: renderPopup.close()
            onOpenLogRequested: {
                renderPopup.close()
                mainWindow.activeViewId = "log"
                logView.refresh()
            }
        }
    }

    // 5-State Bit-Perfect Verification L1/L2/L3 Report Modal
    ScrollableModalPopup {
        id: bitPerfectPopup
        titleText: qsTr("BIT-PERFECT PATH VERIFICATION REPORT (L1 / L2 / L3)")
        preferredWidth: 780
        preferredHeight: 600
        accentColor: "#00E676"

        Label {
            Layout.fillWidth: true
            text: qsTr("State: %1")
                  .arg(EngineController.bitPerfectModeStateText)
            font.family: "Monospace"
            font.pixelSize: 13
            font.bold: true
            color: "#00E5FF"
            wrapMode: Text.WordWrap
        }

        Label {
            Layout.fillWidth: true
            text: mainWindow.bitPerfectReport && mainWindow.bitPerfectReport.summary
                  ? mainWindow.bitPerfectReport.summary
                  : qsTr("Detailed pipeline invariants report")
            font.pixelSize: 12
            color: "#D0DAE4"
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            ConsoleButton {
                text: qsTr("REFRESH")
                implicitWidth: 96; implicitHeight: 30; fontSize: 10
                accentColor: "#00E5FF"
                onClicked: {
                    mainWindow.bitPerfectReport = EngineController.getBitPerfectVerificationReport()
                    mainWindow.bitPerfectExportFeedback = ""
                    mainWindow.bitPerfectExportOk = true
                }
            }

            ConsoleButton {
                text: qsTr("EXPORT JSON...")
                implicitWidth: 132; implicitHeight: 30; fontSize: 10
                accentColor: "#00E676"
                onClicked: bitPerfectExportDialog.open()
            }

            Label {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                visible: mainWindow.bitPerfectExportFeedback.length > 0
                text: mainWindow.bitPerfectExportFeedback
                font.family: "Monospace"
                font.pixelSize: 10
                color: mainWindow.bitPerfectExportOk ? "#00E676" : "#FF5252"
                elide: Text.ElideMiddle
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; height: 1; color: "#222933" }

        Label {
            Layout.fillWidth: true
            text: JSON.stringify(mainWindow.bitPerfectReport, null, 2)
            font.family: "Monospace"
            font.pixelSize: 11
            color: "#9AA9B8"
            wrapMode: Text.WrapAnywhere
        }
    }

    FileDialog {
        id: bitPerfectExportDialog
        title: qsTr("Export Bit-Perfect Verification Report (.json)")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("JSON Files (*.json)"), qsTr("All Files (*)")]
        defaultSuffix: "json"
        onAccepted: {
            var localPath = selectedFile.toString()
            if (localPath.indexOf("file://") === 0) {
                localPath = decodeURIComponent(localPath.substring(7))
            }
            var ok = EngineController.exportBitPerfectReportToJson(localPath, mainWindow.bitPerfectReport)
            mainWindow.bitPerfectExportOk = ok
            mainWindow.bitPerfectExportFeedback = ok
                ? qsTr("Exported JSON: %1").arg(localPath)
                : qsTr("Failed to export JSON report")
        }
    }

    // Track Import Error Notice
    ScrollableModalPopup {
        id: importErrorPopup
        property string msg: ""
        titleText: qsTr("DECK IMPORT / ENGINE NOTICE")
        preferredWidth: 560
        preferredHeight: 260
        accentColor: "#FF5252"

        Label {
            Layout.fillWidth: true
            text: importErrorPopup.msg
            font.pixelSize: 13
            color: "#F0F4F8"
            wrapMode: Text.WordWrap
        }
    }

    Connections {
        target: EngineController
        function onDeckWorkspaceImportFinished(deckId, ok, statusText, message) {
            if (!ok) {
                importErrorPopup.msg = qsTr("Deck %1 workspace import failed:\n%2").arg(deckId).arg(message && message.length > 0 ? message : statusText)
                importErrorPopup.open()
            }
        }
    }
}
