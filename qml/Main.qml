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

    function openBitPerfectReport() {
        bitPerfectReport = EngineController.getBitPerfectVerificationReport()
        bitPerfectPopup.open()
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
    // Native File / Folder Dialogs & Startup Workspace Dialogs
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
            EngineController.startFolderSession(mainWindow.pendingDeckIdForFolder, localPath)
        }
    }

    // Export Path Render Modal (hosts verified RenderPanel inside ScrollableModalPopup)
    ScrollableModalPopup {
        id: renderPopup
        titleText: qsTr("OFFLINE EXPORT RENDER ENGINE")
        preferredWidth: 760
        preferredHeight: 620
        accentColor: "#F5B841"

        RenderPanel {
            Layout.fillWidth: true
            Layout.preferredHeight: 520
            visible: true
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

        Rectangle { Layout.fillWidth: true; height: 1; color: "#222933" }

        Label {
            Layout.fillWidth: true
            text: JSON.stringify(mainWindow.bitPerfectReport, null, 2)
            font.family: "Monospace"
            font.pixelSize: 11
            color: "#9AA9B8"
            wrapMode: Text.WrapAnywhere
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
        function onDeckWorkspaceImportFinished(deckId, ok, detail) {
            if (!ok) {
                importErrorPopup.msg = qsTr("Deck %1 workspace import failed:\n%2").arg(deckId).arg(detail)
                importErrorPopup.open()
            }
        }
    }
}
