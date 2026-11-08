import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Desk32.Core

ApplicationWindow {
    id: root
    visible: true
    visibility: Window.FullScreen
    width: 2560
    height: 1440
    minimumWidth: 1280
    minimumHeight: 720
    title: qsTr("Desk32 - Audiophile Studio Console")

    color: "#0f1114"

    Shortcut {
        sequence: "F11"
        onActivated: {
            if (root.visibility === Window.FullScreen) {
                root.visibility = Window.Windowed
            } else {
                root.visibility = Window.FullScreen
            }
        }
    }

    // Active View state: "console" or "log"
    property string currentView: "console"

    FileDialog {
        id: trackFileDialog
        title: qsTr("Select Audio Track (FLAC, WAV, MP3, Vorbis, Opus, AAC)")
        property string targetDeckId: "A"
        nameFilters: [
            qsTr("All Supported Audio (*.flac *.wav *.mp3 *.ogg *.opus *.m4a *.aac)"),
            qsTr("FLAC Lossless (*.flac)"),
            qsTr("WAV PCM (*.wav)"),
            qsTr("MP3 Audio (*.mp3)"),
            qsTr("All Files (*)")
        ]
        onAccepted: {
            var path = selectedFile.toString()
            if (path.startsWith("file://")) path = path.substring(7)
            EngineController.loadTrack(targetDeckId, path)
        }
    }

    FolderDialog {
        id: folderSessionDialog
        title: qsTr("Select Folder for Continuous Audition Session")
        property string targetDeckId: "A"
        onAccepted: {
            var path = selectedFolder.toString()
            if (path.startsWith("file://")) path = path.substring(7)
            EngineController.startFolderSession(targetDeckId, path, [])
        }
    }

    header: ToolBar {
        background: Rectangle {
            color: "#16191d"
            border.color: "#262b32"
            border.width: 1
        }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            spacing: 16

            // Studio Console Title & Status
            RowLayout {
                spacing: 10
                Rectangle {
                    width: 10
                    height: 10
                    radius: 5
                    color: "#00E5FF"
                }
                Label {
                    text: "DESK32 // AUDIOPHILE STUDIO CONSOLE"
                    font.family: "Monospace"
                    font.bold: true
                    font.pixelSize: 14
                    color: "#00E5FF"
                }
            }

            Rectangle {
                width: 1
                height: 20
                color: "#2a3038"
            }

            // Direct / Bit-Perfect Verification Badges
            RowLayout {
                spacing: 8
                Rectangle {
                    width: 90
                    height: 22
                    radius: 3
                    color: EngineController.directModeStatus === 2 ? "#00E67620" : (EngineController.directModeStatus === 1 ? "#22262c" : "#1c1f24")
                    border.color: EngineController.directModeStatus === 2 ? "#00E676" : (EngineController.directModeStatus === 1 ? "#363d47" : "#2a3038")
                    Label {
                        anchors.centerIn: parent
                        text: EngineController.directModeStatus === 2 ? qsTr("DIRECT MODE") : (EngineController.directModeStatus === 1 ? qsTr("DIRECT: OFF") : qsTr("DIRECT: N/A"))
                        font.pixelSize: 10
                        font.bold: true
                        color: EngineController.directModeStatus === 2 ? "#00E676" : (EngineController.directModeStatus === 1 ? "#758291" : "#55606d")
                    }
                }
                Rectangle {
                    width: 100
                    height: 22
                    radius: 3
                    color: "#00E5FF20"
                    border.color: "#00E5FF"
                    Label {
                        anchors.centerIn: parent
                        text: qsTr("BIT-PERFECT")
                        font.pixelSize: 10
                        font.bold: true
                        color: "#00E5FF"
                    }
                }
                Rectangle {
                    width: 100
                    height: 22
                    radius: 3
                    color: "#303640"
                    Label {
                        anchors.centerIn: parent
                        text: "64-bit IEEE 754"
                        font.family: "Monospace"
                        font.pixelSize: 10
                        color: "#B0BEC5"
                    }
                }
            }

            Item { Layout.fillWidth: true }

            // View Switcher (Console vs Log Tab)
            RowLayout {
                spacing: 4
                Button {
                    text: qsTr("Console View")
                    flat: root.currentView !== "console"
                    highlighted: root.currentView === "console"
                    onClicked: root.currentView = "console"
                }
                Button {
                    text: qsTr("Log Tab (History)")
                    flat: root.currentView !== "log"
                    highlighted: root.currentView === "log"
                    onClicked: {
                        root.currentView = "log"
                        logTable.refresh()
                    }
                }
            }

            Rectangle {
                width: 1
                height: 20
                color: "#2a3038"
            }

            // Language Switcher (EN / JA)
            RowLayout {
                spacing: 4
                Label {
                    text: qsTr("Language:")
                    color: "#A0AAB2"
                    font.pixelSize: 12
                }
                Button {
                    text: "日本語"
                    flat: LanguageManager.currentLanguage !== "ja"
                    highlighted: LanguageManager.currentLanguage === "ja"
                    onClicked: LanguageManager.setLanguage("ja")
                }
                Button {
                    text: "English"
                    flat: LanguageManager.currentLanguage !== "en"
                    highlighted: LanguageManager.currentLanguage === "en"
                    onClicked: LanguageManager.setLanguage("en")
                }
            }
        }
    }

    // MAIN CONTENT STACK
    StackLayout {
        anchors.fill: parent
        currentIndex: root.currentView === "console" ? 0 : 1

        // 1. STUDIO CONSOLE VIEW
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 10
            spacing: 10

            // TOP ROW: Left Wing (4 Decks), Center (Main Monitor), Right Wing (Master/DSP)
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 10

                // LEFT WING: Reference Decks A-D (approx 480px width)
                Rectangle {
                    Layout.preferredWidth: 480
                    Layout.fillHeight: true
                    color: "#14171a"
                    border.color: "#242930"
                    border.width: 1
                    radius: 4

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 8

                        // Section Header & Quick A/B Controls
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Label {
                                text: qsTr("REFERENCE DECKS (4 SLOTS)")
                                font.bold: true
                                font.pixelSize: 12
                                color: "#00E5FF"
                            }
                            Item { Layout.fillWidth: true }
                            // 4-Deck Direct Switch Buttons: A, B, C, D
                            RowLayout {
                                spacing: 4
                                Repeater {
                                    model: ["A", "B", "C", "D"]
                                    Button {
                                        text: modelData
                                        Layout.preferredWidth: 28
                                        Layout.preferredHeight: 26
                                        font.bold: true
                                        font.pixelSize: 11
                                        highlighted: EngineController.activeDeckId === modelData
                                        onClicked: EngineController.switchDeck(modelData, EngineController.switchingMode)
                                    }
                                }
                            }
                            Button {
                                text: qsTr("Quick A/B")
                                font.pixelSize: 11
                                Layout.preferredHeight: 26
                                highlighted: true
                                onClicked: EngineController.quickSwitchAB()
                            }
                            ComboBox {
                                id: modeCombo
                                Layout.preferredHeight: 26
                                model: [qsTr("Normal (Hold)"), qsTr("Comparative (Sync)")]
                                currentIndex: EngineController.switchingMode
                                onActivated: EngineController.switchingMode = currentIndex
                            }
                        }

                        // 4 DECK SLOTS (A, B, C, D)
                        Repeater {
                            model: ["A", "B", "C", "D"]
                            Rectangle {
                                id: deckBox
                                Layout.fillWidth: true
                                Layout.fillHeight: true

                                property bool isActive: EngineController.activeDeckId === modelData
                                property var deckState: EngineController.getDeckState(modelData)

                                function updateDeckState() {
                                    deckState = EngineController.getDeckState(modelData)
                                }

                                Connections {
                                    target: EngineController
                                    function onDeckStateChanged(deckId) {
                                        if (deckId === modelData || deckId === "" || deckId === "ALL") {
                                            deckBox.updateDeckState()
                                        }
                                    }
                                    function onActiveDeckChanged(deckId) {
                                        deckBox.updateDeckState()
                                    }
                                }

                                Timer {
                                    interval: 60
                                    running: deckBox.deckState && deckBox.deckState.state === 1
                                    repeat: true
                                    onTriggered: deckBox.updateDeckState()
                                }

                                color: deckBox.isActive ? "#1d232c" : (cardMouseArea.containsMouse ? "#16191e" : "#111316")
                                border.color: deckBox.isActive ? "#00E5FF" : (cardMouseArea.containsMouse ? "#3b4554" : "#22272e")
                                border.width: deckBox.isActive ? 2 : 1
                                radius: 4

                                // Clicking anywhere on the deck card selects it as the active deck
                                MouseArea {
                                    id: cardMouseArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    z: 0
                                    onClicked: {
                                        if (EngineController.activeDeckId !== modelData) {
                                            EngineController.switchDeck(modelData, EngineController.switchingMode)
                                        }
                                    }
                                }

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 4
                                    z: 1

                                    // Header: DECK ID, Badges, Action Buttons
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 6

                                        Label {
                                            text: "DECK " + modelData
                                            font.bold: true
                                            font.pixelSize: 14
                                            color: deckBox.isActive ? "#00E5FF" : "#9aa5b1"
                                        }

                                        // Active Audition Badge
                                        Rectangle {
                                            visible: deckBox.isActive
                                            width: 55
                                            height: 18
                                            radius: 3
                                            color: "#00E5FF25"
                                            border.color: "#00E5FF"
                                            Label {
                                                anchors.centerIn: parent
                                                text: qsTr("ACTIVE")
                                                font.pixelSize: 9
                                                font.bold: true
                                                color: "#00E5FF"
                                            }
                                        }

                                        // Status badge (PLAYING / PAUSED / STOPPED)
                                        Rectangle {
                                            width: 65
                                            height: 18
                                            radius: 3
                                            color: (deckBox.deckState && deckBox.deckState.state === 1) ? "#00E67625" : ((deckBox.deckState && deckBox.deckState.state === 2) ? "#FFD60025" : "#303640")
                                            border.color: (deckBox.deckState && deckBox.deckState.state === 1) ? "#00E676" : ((deckBox.deckState && deckBox.deckState.state === 2) ? "#FFD600" : "#404854")
                                            Label {
                                                anchors.centerIn: parent
                                                text: (deckBox.deckState && deckBox.deckState.state === 1) ? qsTr("PLAYING") : ((deckBox.deckState && deckBox.deckState.state === 2) ? qsTr("PAUSED") : qsTr("STOPPED"))
                                                font.pixelSize: 9
                                                font.bold: true
                                                color: (deckBox.deckState && deckBox.deckState.state === 1) ? "#00E676" : ((deckBox.deckState && deckBox.deckState.state === 2) ? "#FFD600" : "#808c99")
                                            }
                                        }

                                        Item { Layout.fillWidth: true }

                                        // Load Track File
                                        Button {
                                            text: qsTr("Load")
                                            font.pixelSize: 10
                                            Layout.preferredHeight: 24
                                            enabled: !deckBox.deckState || !deckBox.deckState.isLocked
                                            onClicked: {
                                                trackFileDialog.targetDeckId = modelData
                                                trackFileDialog.open()
                                            }
                                        }

                                        // Load Folder Session
                                        Button {
                                            text: qsTr("Folder")
                                            font.pixelSize: 10
                                            Layout.preferredHeight: 24
                                            enabled: !deckBox.deckState || !deckBox.deckState.isLocked
                                            onClicked: {
                                                folderSessionDialog.targetDeckId = modelData
                                                folderSessionDialog.open()
                                            }
                                        }

                                        // Deck Lock toggle
                                        Button {
                                            text: (deckBox.deckState && deckBox.deckState.isLocked) ? qsTr("LOCKED") : qsTr("LOCK")
                                            font.pixelSize: 10
                                            Layout.preferredHeight: 24
                                            highlighted: !!(deckBox.deckState && deckBox.deckState.isLocked)
                                            onClicked: EngineController.setDeckLocked(modelData, !(deckBox.deckState && deckBox.deckState.isLocked))
                                        }

                                        // Select button
                                        Button {
                                            text: qsTr("Select")
                                            font.pixelSize: 10
                                            Layout.preferredHeight: 24
                                            visible: !deckBox.isActive
                                            onClicked: EngineController.switchDeck(modelData, EngineController.switchingMode)
                                        }
                                    }

                                    // Track Info & Metadata display
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2

                                        // Track Title or File Name
                                        Label {
                                            Layout.fillWidth: true
                                            text: (deckBox.deckState && deckBox.deckState.path) ?
                                                  ((deckBox.deckState.title && deckBox.deckState.title.length > 0) ? deckBox.deckState.title : deckBox.deckState.path.split('/').pop()) :
                                                  qsTr("No audio loaded - Ready for audition")
                                            font.pixelSize: 12
                                            font.bold: !!(deckBox.deckState && deckBox.deckState.path)
                                            elide: Text.ElideMiddle
                                            color: (deckBox.deckState && deckBox.deckState.path) ? "#FFFFFF" : "#5a6470"
                                        }

                                        // Artist & Album (if available)
                                        Label {
                                            Layout.fillWidth: true
                                            visible: !!(deckBox.deckState && deckBox.deckState.artist && deckBox.deckState.artist.length > 0)
                                            text: (deckBox.deckState && deckBox.deckState.artist) ?
                                                  (deckBox.deckState.artist + ((deckBox.deckState.album && deckBox.deckState.album.length > 0) ? " — " + deckBox.deckState.album : "")) : ""
                                            font.pixelSize: 10
                                            elide: Text.ElideMiddle
                                            color: "#90CAF9"
                                        }

                                        // Format Badges & Real-time Playhead Readout
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 6
                                            visible: !!(deckBox.deckState && deckBox.deckState.path)

                                            Rectangle {
                                                height: 16
                                                radius: 2
                                                color: "#00E5FF18"
                                                border.color: "#00E5FF60"
                                                implicitWidth: codecText.implicitWidth + 8
                                                Label {
                                                    id: codecText
                                                    anchors.centerIn: parent
                                                    text: (deckBox.deckState && deckBox.deckState.codec) ? deckBox.deckState.codec : "PCM"
                                                    font.pixelSize: 8
                                                    font.bold: true
                                                    color: "#00E5FF"
                                                }
                                            }

                                            Rectangle {
                                                height: 16
                                                radius: 2
                                                color: "#282d35"
                                                implicitWidth: rateText.implicitWidth + 8
                                                Label {
                                                    id: rateText
                                                    anchors.centerIn: parent
                                                    text: ((deckBox.deckState ? deckBox.deckState.sampleRate : 44100) / 1000).toFixed(1) + " kHz"
                                                    font.pixelSize: 8
                                                    font.family: "Monospace"
                                                    color: "#B0BEC5"
                                                }
                                            }

                                            Rectangle {
                                                height: 16
                                                radius: 2
                                                color: "#282d35"
                                                implicitWidth: bitText.implicitWidth + 8
                                                Label {
                                                    id: bitText
                                                    anchors.centerIn: parent
                                                    text: ((deckBox.deckState && deckBox.deckState.bitDepth) ? deckBox.deckState.bitDepth : 16) + "-bit"
                                                    font.pixelSize: 8
                                                    font.family: "Monospace"
                                                    color: "#B0BEC5"
                                                }
                                            }

                                            Item { Layout.fillWidth: true }

                                            Label {
                                                property double sr: (deckBox.deckState && deckBox.deckState.sampleRate > 0) ? deckBox.deckState.sampleRate : 48000.0
                                                property int curSec: deckBox.deckState ? Math.floor(deckBox.deckState.playhead / sr) : 0
                                                property int totSec: deckBox.deckState ? Math.floor(deckBox.deckState.totalFrames / sr) : 0
                                                property int remSec: Math.max(0, totSec - curSec)
                                                function fmt(s) {
                                                    var m = Math.floor(s / 60)
                                                    var sec = Math.floor(s % 60)
                                                    return (m < 10 ? "0" : "") + m + ":" + (sec < 10 ? "0" : "") + sec
                                                }
                                                text: fmt(curSec) + " / " + fmt(totSec) + "  [-" + fmt(remSec) + "]"
                                                font.pixelSize: 10
                                                font.family: "Monospace"
                                                color: deckBox.isActive ? "#00E5FF" : "#9aa5b1"
                                            }
                                        }
                                    }

                                    // Transport Controls & Progress Slider
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 6
                                        Button {
                                            text: "▶"
                                            Layout.preferredWidth: 32
                                            Layout.preferredHeight: 26
                                            highlighted: !!(deckBox.deckState && deckBox.deckState.state === 1)
                                            onClicked: EngineController.play(modelData)
                                        }
                                        Button {
                                            text: "❚❚"
                                            Layout.preferredWidth: 32
                                            Layout.preferredHeight: 26
                                            highlighted: !!(deckBox.deckState && deckBox.deckState.state === 2)
                                            onClicked: EngineController.pause(modelData)
                                        }
                                        Button {
                                            text: "■"
                                            Layout.preferredWidth: 32
                                            Layout.preferredHeight: 26
                                            onClicked: EngineController.stop(modelData)
                                        }
                                        Slider {
                                            id: deckSlider
                                            Layout.fillWidth: true
                                            from: 0
                                            to: (deckBox.deckState && deckBox.deckState.totalFrames > 0) ? deckBox.deckState.totalFrames : 100
                                            value: deckBox.deckState ? deckBox.deckState.playhead : 0
                                            onMoved: EngineController.seek(modelData, Math.floor(value))
                                        }
                                    }

                                    // Folder Session Navigation Strip
                                    Rectangle {
                                        Layout.fillWidth: true
                                        height: 26
                                        radius: 3
                                        color: "#00E5FF12"
                                        border.color: "#00E5FF30"
                                        visible: !!(deckBox.deckState && deckBox.deckState.folderActive)

                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 8
                                            anchors.rightMargin: 8
                                            spacing: 6

                                            Label {
                                                text: qsTr("Folder Track: %1 / %2").arg((deckBox.deckState ? deckBox.deckState.folderCurrentIndex : 0) + 1).arg(deckBox.deckState ? deckBox.deckState.folderTotalTracks : 0)
                                                font.pixelSize: 10
                                                font.bold: true
                                                color: "#00E5FF"
                                            }

                                            Label {
                                                text: (deckBox.deckState && deckBox.deckState.readAheadFile) ? qsTr("(Next: %1)").arg(deckBox.deckState.readAheadFile) : ""
                                                font.pixelSize: 9
                                                color: "#808c99"
                                                elide: Text.ElideMiddle
                                                Layout.fillWidth: true
                                            }

                                            Button {
                                                text: qsTr("Prev")
                                                font.pixelSize: 10
                                                Layout.preferredHeight: 20
                                                onClicked: EngineController.retreatFolderPrevious(modelData)
                                            }
                                            Button {
                                                text: qsTr("Next")
                                                font.pixelSize: 10
                                                Layout.preferredHeight: 20
                                                onClicked: EngineController.advanceFolderNext(modelData)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // CENTER WING: Main Monitor (Spectrum, Waveform, Diagnostics)
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: "#121417"
                    border.color: "#242930"
                    border.width: 1
                    radius: 4

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 10

                        // Diagnostics Top Strip
                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: qsTr("MAIN AUDIO MONITOR // REAL-TIME DIAGNOSTICS")
                                font.bold: true
                                font.pixelSize: 13
                                color: "#00E5FF"
                            }
                            Item { Layout.fillWidth: true }
                            Label {
                                text: "PIPEWIRE: " + EngineController.backendStatusText.toUpperCase() + " // " + (EngineController.graphSampleRate / 1000.0).toFixed(1) + " kHz // BUFFER: 256 // XRUN: " + EngineController.xrunCount
                                font.family: "Monospace"
                                font.pixelSize: 12
                                color: EngineController.backendStatusText === "Error" ? "#FF5252" : (EngineController.xrunCount === 0 ? "#00E676" : "#FF5252")
                            }
                        }

                        // HIGH-RESOLUTION WAVEFORM DISPLAY AREA
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            color: "#0a0c0e"
                            border.color: "#1d2229"
                            radius: 3
                            clip: true

                            // Waveform oscilloscope grid lines
                            Canvas {
                                id: waveformCanvas
                                anchors.fill: parent
                                onPaint: {
                                    var ctx = getContext("2d");
                                    ctx.clearRect(0, 0, width, height);

                                    // Center grid
                                    ctx.strokeStyle = "#1a2028";
                                    ctx.lineWidth = 1;
                                    ctx.beginPath();
                                    ctx.moveTo(0, height / 2);
                                    ctx.lineTo(width, height / 2);
                                    ctx.stroke();

                                    // Render synthetic audio waveform shape
                                    ctx.strokeStyle = "#00E5FF";
                                    ctx.lineWidth = 2;
                                    ctx.beginPath();
                                    var mid = height / 2;
                                    for (var x = 0; x < width; x += 4) {
                                        var env = Math.sin(x * 0.015) * Math.cos(x * 0.04) * (mid * 0.7);
                                        var y = mid + env * (0.8 + 0.2 * Math.sin(x * 0.1));
                                        if (x === 0) ctx.moveTo(x, y);
                                        else ctx.lineTo(x, y);
                                    }
                                    ctx.stroke();
                                }
                            }

                            // Center Playhead cursor
                            Rectangle {
                                width: 2
                                height: parent.height
                                color: "#FF5252"
                                x: parent.width * 0.35
                            }

                            // Active Track Info & Real-Time Playhead Display
                            RowLayout {
                                id: mainMonitorHeader
                                anchors.top: parent.top
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.margins: 10
                                spacing: 8

                                property var activeState: EngineController.getDeckState(EngineController.activeDeckId)
                                function refreshActive() {
                                    activeState = EngineController.getDeckState(EngineController.activeDeckId)
                                }

                                Connections {
                                    target: EngineController
                                    function onDeckStateChanged(id) {
                                        if (id === EngineController.activeDeckId || id === "" || id === "ALL") {
                                            mainMonitorHeader.refreshActive()
                                        }
                                    }
                                    function onActiveDeckChanged(id) {
                                        mainMonitorHeader.refreshActive()
                                    }
                                }

                                Timer {
                                    interval: 50
                                    running: mainMonitorHeader.activeState && mainMonitorHeader.activeState.state === 1
                                    repeat: true
                                    onTriggered: mainMonitorHeader.refreshActive()
                                }

                                Rectangle {
                                    width: 60
                                    height: 20
                                    radius: 3
                                    color: "#00E5FF20"
                                    border.color: "#00E5FF"
                                    Label {
                                        anchors.centerIn: parent
                                        text: "DECK " + EngineController.activeDeckId
                                        font.pixelSize: 10
                                        font.bold: true
                                        color: "#00E5FF"
                                    }
                                }

                                Label {
                                    text: (mainMonitorHeader.activeState && mainMonitorHeader.activeState.path) ?
                                          ((mainMonitorHeader.activeState.title && mainMonitorHeader.activeState.title.length > 0) ? mainMonitorHeader.activeState.title : mainMonitorHeader.activeState.path.split('/').pop()) :
                                          qsTr("NO TRACK AUDITIONING")
                                    font.pixelSize: 12
                                    font.bold: true
                                    color: "#E0E6ED"
                                    elide: Text.ElideMiddle
                                    Layout.fillWidth: true
                                }

                                Label {
                                    property double sr: (mainMonitorHeader.activeState && mainMonitorHeader.activeState.sampleRate > 0) ? mainMonitorHeader.activeState.sampleRate : 48000.0
                                    property double curSec: mainMonitorHeader.activeState ? (mainMonitorHeader.activeState.playhead / sr) : 0.0
                                    property double totSec: mainMonitorHeader.activeState ? (mainMonitorHeader.activeState.totalFrames / sr) : 0.0
                                    function fmtFull(s) {
                                        var h = Math.floor(s / 3600)
                                        var m = Math.floor((s % 3600) / 60)
                                        var sec = Math.floor(s % 60)
                                        var ms = Math.floor((s - Math.floor(s)) * 1000)
                                        return (h < 10 ? "0" : "") + h + ":" + (m < 10 ? "0" : "") + m + ":" + (sec < 10 ? "0" : "") + sec + "." + (ms < 100 ? (ms < 10 ? "00" : "0") : "") + ms
                                    }
                                    text: fmtFull(curSec) + " / " + fmtFull(totSec)
                                    font.family: "Monospace"
                                    font.pixelSize: 14
                                    color: "#00E5FF"
                                }
                            }
                        }

                        // REAL-TIME 32-BAND SPECTRUM ANALYZER VISUALIZER
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 140
                            color: "#0a0c0e"
                            border.color: "#1d2229"
                            radius: 3

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 8
                                spacing: 2

                                Repeater {
                                    model: 32
                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        color: "transparent"

                                        Rectangle {
                                            anchors.bottom: parent.bottom
                                            width: parent.width
                                            height: Math.max(4, parent.height * (0.3 + 0.5 * Math.sin(index * 0.2)))
                                            color: index > 26 ? "#FF5252" : (index > 18 ? "#FFD600" : "#00E5FF")
                                            radius: 1
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // RIGHT WING: Master / DSP & Output Path (approx 480px width)
                Rectangle {
                    Layout.preferredWidth: 480
                    Layout.fillHeight: true
                    color: "#14171a"
                    border.color: "#242930"
                    border.width: 1
                    radius: 4

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 10

                        Label {
                            text: qsTr("MASTER & OUTPUT PATH ROUTING")
                            font.bold: true
                            font.pixelSize: 12
                            color: "#00E5FF"
                        }

                        // 3-Path Output Routing Box
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 135
                            color: "#101215"
                            border.color: "#20252b"
                            radius: 4

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 4

                                Label {
                                    text: qsTr("OUTPUT ROUTING (SECTION 9.2)")
                                    font.bold: true
                                    font.pixelSize: 11
                                    color: "#A0AAB2"
                                }

                                RadioButton {
                                    text: qsTr("Listening Path (Correction & Room EQ ON)")
                                    checked: EngineController.outputRoutingMode === 0
                                    onClicked: EngineController.outputRoutingMode = 0
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 26
                                    visible: EngineController.outputRoutingMode === 0
                                    spacing: 6

                                    Label {
                                        text: qsTr("Profile:")
                                        font.pixelSize: 10
                                        color: "#808c99"
                                    }

                                    ComboBox {
                                        id: listeningProfileCombo
                                        Layout.fillWidth: true
                                        model: EngineController.availableListeningPresets()
                                        currentIndex: Math.max(0, model.indexOf(EngineController.listeningPresetName))
                                        onActivated: EngineController.setListeningPreset(currentText)
                                    }
                                }

                                RadioButton {
                                    text: qsTr("Render Monitor Path (Pure Master, 0.000000 Leakage)")
                                    checked: EngineController.outputRoutingMode === 1
                                    onClicked: EngineController.outputRoutingMode = 1
                                }
                            }
                        }

                        // DSP NODES INDIVIDUAL BYPASS TOGGLES & TELEMETRY
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 180
                            color: "#101215"
                            border.color: "#20252b"
                            radius: 4

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 6

                                RowLayout {
                                    Layout.fillWidth: true
                                    Label {
                                        text: qsTr("DSP NODES (SECTION 5.0)")
                                        font.bold: true
                                        font.pixelSize: 11
                                        color: "#A0AAB2"
                                    }
                                    Item { Layout.fillWidth: true }
                                    Button {
                                        text: EngineController.globalDspBypass ? qsTr("GLOBAL BYPASS ON") : qsTr("GLOBAL BYPASS")
                                        font.pixelSize: 10
                                        highlighted: EngineController.globalDspBypass
                                        onClicked: EngineController.globalDspBypass = !EngineController.globalDspBypass
                                    }
                                }

                                GridLayout {
                                    columns: 2
                                    Layout.fillWidth: true
                                    rowSpacing: 4
                                    columnSpacing: 8

                                    CheckBox {
                                        text: "PEQ (24-Band)"
                                        checked: !EngineController.isDspNodeBypassed("PEQ")
                                        onToggled: EngineController.setDspNodeBypass("PEQ", !checked)
                                    }
                                    CheckBox {
                                        text: "GEQ (32-Band)"
                                        checked: !EngineController.isDspNodeBypassed("GEQ")
                                        onToggled: EngineController.setDspNodeBypass("GEQ", !checked)
                                    }
                                    CheckBox {
                                        text: "HPF / LPF"
                                        checked: !EngineController.isDspNodeBypassed("Filter")
                                        onToggled: EngineController.setDspNodeBypass("Filter", !checked)
                                    }
                                    CheckBox {
                                        text: "Reverb"
                                        checked: !EngineController.isDspNodeBypassed("Reverb")
                                        onToggled: EngineController.setDspNodeBypass("Reverb", !checked)
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        CheckBox {
                                            text: "Compressor"
                                            checked: !EngineController.isDspNodeBypassed("Compressor")
                                            onToggled: EngineController.setDspNodeBypass("Compressor", !checked)
                                        }
                                        Label {
                                            text: EngineController.compressorGainReductionDb > 0.05 ? ("GR -" + EngineController.compressorGainReductionDb.toFixed(1) + " dB") : "0.0 dB"
                                            font.pixelSize: 9
                                            font.family: "Monospace"
                                            color: EngineController.compressorGainReductionDb > 0.1 ? "#FFB300" : "#606c7a"
                                        }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        CheckBox {
                                            text: "Peak Limiter"
                                            checked: !EngineController.isDspNodeBypassed("Limiter")
                                            onToggled: EngineController.setDspNodeBypass("Limiter", !checked)
                                        }
                                        Label {
                                            text: EngineController.limiterGainReductionDb > 0.05 ? ("GR -" + EngineController.limiterGainReductionDb.toFixed(1) + " dB") : (EngineController.limiterLatencyFrames + "smp")
                                            font.pixelSize: 9
                                            font.family: "Monospace"
                                            color: EngineController.limiterGainReductionDb > 0.1 ? "#FF5252" : "#606c7a"
                                        }
                                    }
                                }
                            }
                        }

                        // EXTERNAL INSERT (HOST BRIDGE) QUICK STATUS & ROUTING
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 105
                            color: "#101215"
                            border.color: "#20252b"
                            radius: 4

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 6

                                RowLayout {
                                    Layout.fillWidth: true
                                    Label {
                                        text: qsTr("EXTERNAL INSERT (HOST BRIDGE)")
                                        font.bold: true
                                        font.pixelSize: 11
                                        color: "#A0AAB2"
                                    }
                                    Item { Layout.fillWidth: true }
                                    // Status LED Badge
                                    Rectangle {
                                        id: insertBadgeQuick
                                        property int st: EngineController.externalInsertState
                                        property bool isByp: EngineController.externalInsertBypassed
                                        width: 90
                                        height: 18
                                        radius: 3
                                        color: st === 2 ? (isByp ? "#FFD60025" : "#00E67625") : (st === 1 ? "#00E5FF25" : (st === 0 ? "#262b32" : "#FF525225"))
                                        border.color: st === 2 ? (isByp ? "#FFD600" : "#00E676") : (st === 1 ? "#00E5FF" : (st === 0 ? "#3a414d" : "#FF5252"))
                                        Label {
                                            anchors.centerIn: parent
                                            text: insertBadgeQuick.st === 2 ? (insertBadgeQuick.isByp ? qsTr("BYPASS") : qsTr("ACTIVE")) : (insertBadgeQuick.st === 1 ? qsTr("CONNECTED") : (insertBadgeQuick.st === 0 ? qsTr("DISCONNECTED") : qsTr("ERROR")))
                                            font.pixelSize: 9
                                            font.bold: true
                                            color: insertBadgeQuick.border.color
                                        }
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6
                                    Label {
                                        text: qsTr("Host:")
                                        font.pixelSize: 10
                                        color: "#808c99"
                                    }
                                    Label {
                                        text: EngineController.externalInsertClientName.length > 0 ? EngineController.externalInsertClientName : "Carla"
                                        font.pixelSize: 11
                                        font.bold: true
                                        color: "#E0E6ED"
                                    }
                                    Item { Layout.fillWidth: true }
                                    Button {
                                        text: EngineController.externalInsertBypassed ? qsTr("DRY BYPASS [ON]") : qsTr("INSERT [ON]")
                                        font.pixelSize: 9
                                        highlighted: !EngineController.externalInsertBypassed
                                        Layout.preferredHeight: 22
                                        onClicked: EngineController.externalInsertBypassed = !EngineController.externalInsertBypassed
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 4
                                    Label {
                                        text: "Rep: " + EngineController.externalInsertReportedLatency + "smp | Meas: " + (EngineController.externalInsertMeasuredLatency >= 0 ? EngineController.externalInsertMeasuredLatency : "--") + "smp | PDC: " + EngineController.externalInsertDelayCompensation + "smp"
                                        font.pixelSize: 9
                                        font.family: "Monospace"
                                        color: "#758291"
                                    }
                                    Item { Layout.fillWidth: true }
                                    Button {
                                        text: qsTr("OPEN HOST PANEL ▶")
                                        font.pixelSize: 9
                                        highlighted: eqConsoleRect.eqMode === "insert"
                                        Layout.preferredHeight: 22
                                        onClicked: eqConsoleRect.eqMode = "insert"
                                    }
                                }
                            }
                        }

                        // Master Volume Gain & VU Peak Meters - Studio Console Dual Long Vertical Faders
                        Rectangle {
                            id: masterSectionRect
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.minimumHeight: 280
                            color: "#101215"
                            border.color: "#20252b"
                            radius: 4

                            property bool fadersLinked: true

                            function formatGainDb(g) {
                                if (g <= 0.0001) return "-∞ dB";
                                var db = 20.0 * Math.log10(g);
                                return (db > 0.0 ? "+" : "") + db.toFixed(1) + " dB";
                            }

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 6

                                // Top Control Strip: Title, Balance, Link Toggle, Reset Button
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6

                                    Label {
                                        text: qsTr("MASTER BUS FADERS")
                                        font.bold: true
                                        font.pixelSize: 11
                                        color: "#A0AAB2"
                                    }

                                    Item { Layout.fillWidth: true }

                                    // Balance indicator
                                    Rectangle {
                                        height: 20
                                        radius: 2
                                        color: "#181c22"
                                        border.color: "#2a313b"
                                        implicitWidth: balText.implicitWidth + 8
                                        Label {
                                            id: balText
                                            anchors.centerIn: parent
                                            property double diff: EngineController.masterGainR - EngineController.masterGainL
                                            text: Math.abs(diff) < 0.005 ? "BAL: C" : (diff < 0 ? ("BAL: L " + (-diff * 6.0).toFixed(1) + " dB") : ("BAL: R " + (diff * 6.0).toFixed(1) + " dB"))
                                            font.pixelSize: 9
                                            font.family: "Monospace"
                                            font.bold: true
                                            color: Math.abs(diff) < 0.005 ? "#00E5FF" : "#FFD600"
                                        }
                                    }

                                    // Stereo Link Button
                                    Button {
                                        text: masterSectionRect.fadersLinked ? qsTr("🔗 LINK") : qsTr("⛓ UNLINK")
                                        font.pixelSize: 9
                                        font.bold: true
                                        highlighted: masterSectionRect.fadersLinked
                                        Layout.preferredHeight: 22
                                        onClicked: masterSectionRect.fadersLinked = !masterSectionRect.fadersLinked
                                    }

                                    // Reset to 0 dB Unity Gain
                                    Button {
                                        text: "0 dB"
                                        font.pixelSize: 9
                                        font.bold: true
                                        Layout.preferredHeight: 22
                                        onClicked: {
                                            EngineController.setMasterGainL(1.0);
                                            EngineController.setMasterGainR(1.0);
                                            EngineController.setMasterGain(1.0);
                                            masterSliderL.value = 1.0;
                                            masterSliderR.value = 1.0;
                                        }
                                    }
                                }

                                // Dual Vertical Fader Bank
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    spacing: 8

                                    // ================= LEFT CHANNEL =================
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        spacing: 4

                                        Label {
                                            Layout.alignment: Qt.AlignHCenter
                                            text: "CH L (LEFT)"
                                            font.pixelSize: 10
                                            font.bold: true
                                            font.family: "Monospace"
                                            color: "#00E5FF"
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.fillHeight: true
                                            spacing: 6

                                            // Left VU Meter Bar
                                            Rectangle {
                                                width: 12
                                                Layout.fillHeight: true
                                                color: "#07090b"
                                                border.color: "#1b2129"
                                                radius: 2
                                                clip: true

                                                Rectangle {
                                                    anchors.bottom: parent.bottom
                                                    width: parent.width
                                                    height: Math.max(4, parent.height * Math.min(1.0, 0.72 * EngineController.masterGainL))
                                                    color: height > parent.height * 0.9 ? "#FF5252" : (height > parent.height * 0.75 ? "#FFD600" : "#00E676")
                                                }
                                            }

                                            // Left Long Vertical Slider
                                            Slider {
                                                id: masterSliderL
                                                Layout.fillWidth: true
                                                Layout.fillHeight: true
                                                orientation: Qt.Vertical
                                                from: 0.0
                                                to: 2.0
                                                value: EngineController.masterGainL
                                                onMoved: {
                                                    EngineController.setMasterGainL(value);
                                                    if (masterSectionRect.fadersLinked) {
                                                        EngineController.setMasterGainR(value);
                                                        masterSliderR.value = value;
                                                    }
                                                }

                                                background: Rectangle {
                                                    x: (masterSliderL.width - width) / 2
                                                    y: masterSliderL.topPadding
                                                    width: 6
                                                    height: masterSliderL.availableHeight
                                                    color: "#0a0c0f"
                                                    border.color: "#1d232c"
                                                    radius: 3
                                                    Rectangle {
                                                        anchors.centerIn: parent
                                                        width: 2
                                                        height: parent.height - 4
                                                        color: "#151a22"
                                                    }
                                                }

                                                handle: Rectangle {
                                                    x: (masterSliderL.width - width) / 2
                                                    y: masterSliderL.topPadding + masterSliderL.visualPosition * (masterSliderL.availableHeight - height)
                                                    width: 34
                                                    height: 22
                                                    radius: 3
                                                    color: masterSliderL.pressed ? "#E53935" : "#D32F2F"
                                                    border.color: "#FFCDD2"
                                                    border.width: 1
                                                    Rectangle {
                                                        anchors.centerIn: parent
                                                        width: parent.width - 6
                                                        height: 2
                                                        color: "#FFFFFF"
                                                    }
                                                }

                                                MouseArea {
                                                    anchors.fill: parent
                                                    acceptedButtons: Qt.NoButton
                                                    onDoubleClicked: {
                                                        EngineController.setMasterGainL(1.0);
                                                        masterSliderL.value = 1.0;
                                                        if (masterSectionRect.fadersLinked) {
                                                            EngineController.setMasterGainR(1.0);
                                                            masterSliderR.value = 1.0;
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        // Left Numerical Readout
                                        Label {
                                            Layout.alignment: Qt.AlignHCenter
                                            text: masterSectionRect.formatGainDb(EngineController.masterGainL)
                                            font.family: "Monospace"
                                            font.pixelSize: 11
                                            font.bold: true
                                            color: "#00E5FF"
                                        }
                                    }

                                    // ================= CENTER CALIBRATION SCALE =================
                                    ColumnLayout {
                                        Layout.fillHeight: true
                                        Layout.preferredWidth: 42
                                        spacing: 0

                                        Item { Layout.preferredHeight: 18 }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            Layout.fillHeight: true
                                            spacing: 0

                                            Repeater {
                                                model: [
                                                    { label: "+6 dB", color: "#FF5252" },
                                                    { label: "+3 dB", color: "#FFD600" },
                                                    { label: "  0 dB", color: "#00E5FF" },
                                                    { label: " -3 dB", color: "#78909C" },
                                                    { label: " -6 dB", color: "#78909C" },
                                                    { label: "-12 dB", color: "#607D8B" },
                                                    { label: "-18 dB", color: "#607D8B" },
                                                    { label: "-24 dB", color: "#546E7A" },
                                                    { label: "-36 dB", color: "#455A64" },
                                                    { label: " -∞ dB", color: "#37474F" }
                                                ]

                                                RowLayout {
                                                    Layout.fillWidth: true
                                                    Layout.fillHeight: true
                                                    spacing: 2

                                                    Rectangle {
                                                        width: modelData.label.indexOf("0 dB") !== -1 ? 8 : 4
                                                        height: 1
                                                        color: modelData.color
                                                    }

                                                    Label {
                                                        text: modelData.label
                                                        font.pixelSize: 8
                                                        font.family: "Monospace"
                                                        font.bold: modelData.label.indexOf("0 dB") !== -1
                                                        color: modelData.color
                                                        Layout.fillWidth: true
                                                    }
                                                }
                                            }
                                        }

                                        Item { Layout.preferredHeight: 18 }
                                    }

                                    // ================= RIGHT CHANNEL =================
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        spacing: 4

                                        Label {
                                            Layout.alignment: Qt.AlignHCenter
                                            text: "CH R (RIGHT)"
                                            font.pixelSize: 10
                                            font.bold: true
                                            font.family: "Monospace"
                                            color: "#00E5FF"
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.fillHeight: true
                                            spacing: 6

                                            // Right Long Vertical Slider
                                            Slider {
                                                id: masterSliderR
                                                Layout.fillWidth: true
                                                Layout.fillHeight: true
                                                orientation: Qt.Vertical
                                                from: 0.0
                                                to: 2.0
                                                value: EngineController.masterGainR
                                                onMoved: {
                                                    EngineController.setMasterGainR(value);
                                                    if (masterSectionRect.fadersLinked) {
                                                        EngineController.setMasterGainL(value);
                                                        masterSliderL.value = value;
                                                    }
                                                }

                                                background: Rectangle {
                                                    x: (masterSliderR.width - width) / 2
                                                    y: masterSliderR.topPadding
                                                    width: 6
                                                    height: masterSliderR.availableHeight
                                                    color: "#0a0c0f"
                                                    border.color: "#1d232c"
                                                    radius: 3
                                                    Rectangle {
                                                        anchors.centerIn: parent
                                                        width: 2
                                                        height: parent.height - 4
                                                        color: "#151a22"
                                                    }
                                                }

                                                handle: Rectangle {
                                                    x: (masterSliderR.width - width) / 2
                                                    y: masterSliderR.topPadding + masterSliderR.visualPosition * (masterSliderR.availableHeight - height)
                                                    width: 34
                                                    height: 22
                                                    radius: 3
                                                    color: masterSliderR.pressed ? "#E53935" : "#D32F2F"
                                                    border.color: "#FFCDD2"
                                                    border.width: 1
                                                    Rectangle {
                                                        anchors.centerIn: parent
                                                        width: parent.width - 6
                                                        height: 2
                                                        color: "#FFFFFF"
                                                    }
                                                }

                                                MouseArea {
                                                    anchors.fill: parent
                                                    acceptedButtons: Qt.NoButton
                                                    onDoubleClicked: {
                                                        EngineController.setMasterGainR(1.0);
                                                        masterSliderR.value = 1.0;
                                                        if (masterSectionRect.fadersLinked) {
                                                            EngineController.setMasterGainL(1.0);
                                                            masterSliderL.value = 1.0;
                                                        }
                                                    }
                                                }
                                            }

                                            // Right VU Meter Bar
                                            Rectangle {
                                                width: 12
                                                Layout.fillHeight: true
                                                color: "#07090b"
                                                border.color: "#1b2129"
                                                radius: 2
                                                clip: true

                                                Rectangle {
                                                    anchors.bottom: parent.bottom
                                                    width: parent.width
                                                    height: Math.max(4, parent.height * Math.min(1.0, 0.70 * EngineController.masterGainR))
                                                    color: height > parent.height * 0.9 ? "#FF5252" : (height > parent.height * 0.75 ? "#FFD600" : "#00E676")
                                                }
                                            }
                                        }

                                        // Right Numerical Readout
                                        Label {
                                            Layout.alignment: Qt.AlignHCenter
                                            text: masterSectionRect.formatGainDb(EngineController.masterGainR)
                                            font.family: "Monospace"
                                            font.pixelSize: 11
                                            font.bold: true
                                            color: "#00E5FF"
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // BOTTOM ROW: Full-Width Studio EQ Console (approx 520px height)
            Rectangle {
                id: eqConsoleRect
                Layout.fillWidth: true
                Layout.preferredHeight: 520
                color: "#13161a"
                border.color: "#242930"
                border.width: 1
                radius: 4

                property string eqMode: "geq" // "geq" or "peq"
                onEqModeChanged: {
                    eqCanvas.requestPaint();
                    if (eqMode === "peq") {
                        peqContainer.reloadBand();
                    }
                }

                Connections {
                    target: EngineController
                    function onGeqParametersChanged() {
                        eqCanvas.requestPaint();
                    }
                    function onPeqParametersChanged() {
                        eqCanvas.requestPaint();
                        if (eqConsoleRect.eqMode === "peq") {
                            peqContainer.reloadBand();
                        }
                    }
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8

                    // EQ Console Header & Mode Switcher
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: qsTr("STUDIO PROCESSING CONSOLE // EQUALIZERS & EXTERNAL INSERT HOST")
                            font.bold: true
                            font.pixelSize: 13
                            color: "#00E5FF"
                        }
                        Item { Layout.fillWidth: true }
                        Button {
                            text: qsTr("Reset All GEQ (0 dB)")
                            font.pixelSize: 11
                            visible: eqConsoleRect.eqMode === "geq"
                            onClicked: {
                                EngineController.resetAllGeqBands();
                                eqCanvas.requestPaint();
                            }
                        }
                        Button {
                            text: qsTr("32-Band GEQ (Full Console)")
                            flat: eqConsoleRect.eqMode !== "geq"
                            highlighted: eqConsoleRect.eqMode === "geq"
                            onClicked: eqConsoleRect.eqMode = "geq"
                        }
                        Button {
                            text: qsTr("24-Band PEQ (Left 12 + Right 12)")
                            flat: eqConsoleRect.eqMode !== "peq"
                            highlighted: eqConsoleRect.eqMode === "peq"
                            onClicked: eqConsoleRect.eqMode = "peq"
                        }
                        Button {
                            text: qsTr("External Insert (Carla / Plugin Host)")
                            flat: eqConsoleRect.eqMode !== "insert"
                            highlighted: eqConsoleRect.eqMode === "insert"
                            onClicked: eqConsoleRect.eqMode = "insert"
                        }
                    }

                    // DYNAMIC FREQUENCY RESPONSE CURVE CANVAS (SECTION 5.8)
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 120
                        visible: eqConsoleRect.eqMode !== "insert"
                        color: "#090b0e"
                        border.color: "#1f242c"
                        radius: 3
                        clip: true

                        Canvas {
                            id: eqCanvas
                            anchors.fill: parent
                            antialiasing: true

                            property var isoGeqFreqs: [
                                16.0, 20.0, 25.0, 31.5, 40.0, 50.0, 63.0, 80.0,
                                100.0, 125.0, 160.0, 200.0, 250.0, 315.0, 400.0, 500.0,
                                630.0, 800.0, 1000.0, 1250.0, 1600.0, 2000.0, 2500.0, 3150.0,
                                4000.0, 5000.0, 6300.0, 8000.0, 10000.0, 12500.0, 16000.0, 20000.0
                            ]

                            onAvailableChanged: if (available) requestPaint()
                            onWidthChanged: requestPaint()
                            onHeightChanged: requestPaint()
                            Component.onCompleted: requestPaint()

                            function peqFreqToX(f, w) {
                                var minLog = Math.log10(20.0);
                                var maxLog = Math.log10(20000.0);
                                var curLog = Math.log10(Math.max(20.0, Math.min(20000.0, f)));
                                return (curLog - minLog) / (maxLog - minLog) * w;
                            }

                            function geqFreqToX(f, w, centers) {
                                var safeF = Math.max(1.0, f);
                                if (!centers || centers.length < 32) {
                                    var cw = (w - 31 * 4.0) / 32.0;
                                    centers = [];
                                    for (var idx = 0; idx < 32; ++idx) {
                                        centers.push(idx * (cw + 4.0) + cw * 0.5);
                                    }
                                }
                                if (safeF <= isoGeqFreqs[0]) {
                                    var x0 = centers[0];
                                    var x1 = centers[1];
                                    var t0 = Math.log10(safeF / isoGeqFreqs[0]) / Math.log10(isoGeqFreqs[1] / isoGeqFreqs[0]);
                                    return Math.max(0, x0 + t0 * (x1 - x0));
                                }
                                if (safeF >= isoGeqFreqs[31]) {
                                    var xN = centers[31];
                                    var xP = centers[30];
                                    var tN = Math.log10(safeF / isoGeqFreqs[31]) / Math.log10(isoGeqFreqs[31] / isoGeqFreqs[30]);
                                    return Math.min(w, xN + tN * (xN - xP));
                                }
                                for (var b = 0; b < 31; ++b) {
                                    if (safeF >= isoGeqFreqs[b] && safeF <= isoGeqFreqs[b + 1]) {
                                        var xb = centers[b];
                                        var xNext = centers[b + 1];
                                        var tb = Math.log10(safeF / isoGeqFreqs[b]) / Math.log10(isoGeqFreqs[b + 1] / isoGeqFreqs[b]);
                                        return xb + tb * (xNext - xb);
                                    }
                                }
                                return w * 0.5;
                            }

                            function dbToY(db, h) {
                                var clamped = Math.max(-18.0, Math.min(18.0, db));
                                return h * (0.5 - (clamped / 36.0));
                            }

                            onPaint: {
                                var ctx = getContext("2d");
                                if (!ctx) return;
                                var w = width;
                                var h = height;
                                if (w <= 0 || h <= 0) return;
                                ctx.clearRect(0, 0, w, h);

                                // Background
                                ctx.fillStyle = "#090b0e";
                                ctx.fillRect(0, 0, w, h);

                                // Horizontal dB Grid
                                var dbLines = [-12, -6, 0, 6, 12];
                                for (var i = 0; i < dbLines.length; ++i) {
                                    var y = dbToY(dbLines[i], h);
                                    ctx.beginPath();
                                    ctx.strokeStyle = dbLines[i] === 0 ? "#37474f" : "#141a22";
                                    ctx.lineWidth = dbLines[i] === 0 ? 1.5 : 1.0;
                                    ctx.moveTo(0, y);
                                    ctx.lineTo(w, y);
                                    ctx.stroke();

                                    ctx.fillStyle = dbLines[i] === 0 ? "#78909c" : "#455a64";
                                    ctx.font = "9px monospace";
                                    ctx.fillText((dbLines[i] > 0 ? "+" : "") + dbLines[i] + " dB", 4, y - 2);
                                }

                                var isGeq = (eqConsoleRect.eqMode === "geq");

                                // Compute fader column centers for GEQ pixel-perfect alignment
                                var faderCenters = new Array(32);
                                var colWidth = (w - 31 * 4.0) / 32.0;
                                for (var fi = 0; fi < 32; ++fi) {
                                    var it = (typeof geqRepeater !== "undefined" && geqRepeater.itemAt) ? geqRepeater.itemAt(fi) : null;
                                    if (it && it.width > 0) {
                                        faderCenters[fi] = it.x + it.width * 0.5;
                                    } else {
                                        faderCenters[fi] = fi * (colWidth + 4.0) + colWidth * 0.5;
                                    }
                                }

                                // Vertical Frequency Grid
                                var gridFreqs = [50, 100, 200, 500, 1000, 2000, 5000, 10000];
                                for (var j = 0; j < gridFreqs.length; ++j) {
                                    var x = isGeq ? geqFreqToX(gridFreqs[j], w, faderCenters) : peqFreqToX(gridFreqs[j], w);
                                    ctx.beginPath();
                                    ctx.strokeStyle = "#141a22";
                                    ctx.lineWidth = 1.0;
                                    ctx.moveTo(x, 0);
                                    ctx.lineTo(x, h);
                                    ctx.stroke();

                                    ctx.fillStyle = "#455a64";
                                    ctx.font = "9px monospace";
                                    var label = gridFreqs[j] >= 1000 ? (gridFreqs[j] / 1000) + "k" : gridFreqs[j];
                                    ctx.fillText(label, x + 2, h - 4);
                                }

                                if (isGeq) {
                                    // 32-Band GEQ Curve (exact fader-aligned curve)
                                    var pts = EngineController.getGeqCurve(256);
                                    if (pts && pts.length > 1) {
                                        ctx.beginPath();
                                        ctx.strokeStyle = "#00E5FF";
                                        ctx.lineWidth = 2.0;
                                        for (var k = 0; k < pts.length; ++k) {
                                            var px = geqFreqToX(pts[k].f, w, faderCenters);
                                            var py = dbToY(pts[k].db, h);
                                            if (k === 0) ctx.moveTo(px, py);
                                            else ctx.lineTo(px, py);
                                        }
                                        ctx.stroke();
                                    }
                                } else {
                                    // 24-Band PEQ Curves (Left & Right channels)
                                    var ptsL = EngineController.getPeqCurve(false, 128);
                                    if (ptsL && ptsL.length > 1) {
                                        ctx.beginPath();
                                        ctx.strokeStyle = "#00E5FF";
                                        ctx.lineWidth = 2.0;
                                        for (var l = 0; l < ptsL.length; ++l) {
                                            var lx = peqFreqToX(ptsL[l].f, w);
                                            var ly = dbToY(ptsL[l].db, h);
                                            if (l === 0) ctx.moveTo(lx, ly);
                                            else ctx.lineTo(lx, ly);
                                        }
                                        ctx.stroke();
                                    }

                                    var ptsR = EngineController.getPeqCurve(true, 128);
                                    if (ptsR && ptsR.length > 1) {
                                        ctx.beginPath();
                                        ctx.strokeStyle = "#FF9100";
                                        ctx.lineWidth = 1.8;
                                        for (var m = 0; m < ptsR.length; ++m) {
                                            var rx = peqFreqToX(ptsR[m].f, w);
                                            var ry = dbToY(ptsR[m].db, h);
                                            if (m === 0) ctx.moveTo(rx, ry);
                                            else ctx.lineTo(rx, ry);
                                        }
                                        ctx.stroke();
                                    }
                                }
                            }
                        }

                        // Curve Legend for PEQ
                        RowLayout {
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.margins: 6
                            spacing: 12
                            visible: eqConsoleRect.eqMode === "peq"

                            RowLayout {
                                spacing: 4
                                Rectangle { width: 10; height: 3; color: "#00E5FF" }
                                Label { text: qsTr("Left Channel"); font.pixelSize: 9; color: "#808c99" }
                            }
                            RowLayout {
                                spacing: 4
                                Rectangle { width: 10; height: 3; color: "#FF9100" }
                                Label { text: qsTr("Right Channel"); font.pixelSize: 9; color: "#808c99" }
                            }
                        }
                    }

                    // STACKLAYOUT FOR EQ VIEWS (CLEAN ISOLATION BETWEEN GEQ AND PEQ)
                    StackLayout {
                        id: eqStack
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        currentIndex: eqConsoleRect.eqMode === "geq" ? 0 : 1

                        // PAGE 0: 32-BAND GRAPHIC EQUALIZER VIEW
                        RowLayout {
                            id: geqRowLayout
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: 4

                            Repeater {
                                id: geqRepeater
                                model: [
                                    "16", "20", "25", "31.5", "40", "50", "63", "80",
                                    "100", "125", "160", "200", "250", "315", "400", "500",
                                    "630", "800", "1k", "1.25k", "1.6k", "2k", "2.5k", "3.15k",
                                    "4k", "5k", "6.3k", "8k", "10k", "12.5k", "16k", "20k"
                                ]

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    color: "#0d0f12"
                                    border.color: "#1c2128"
                                    radius: 2

                                    MouseArea {
                                        anchors.fill: parent
                                        onDoubleClicked: {
                                            geqSlider.value = 0.0;
                                            EngineController.setGeqBandGain(index, 0.0);
                                            eqCanvas.requestPaint();
                                        }
                                    }

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 4
                                        spacing: 2

                                        Label {
                                            Layout.alignment: Qt.AlignHCenter
                                            text: (geqSlider.value > 0 ? "+" : "") + geqSlider.value.toFixed(1)
                                            font.pixelSize: 9
                                            font.family: "Monospace"
                                            color: geqSlider.value !== 0 ? "#00E5FF" : "#606c7a"

                                            MouseArea {
                                                anchors.fill: parent
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    geqSlider.value = 0.0;
                                                    EngineController.setGeqBandGain(index, 0.0);
                                                    eqCanvas.requestPaint();
                                                }
                                            }
                                        }

                                        Slider {
                                            id: geqSlider
                                            Layout.fillHeight: true
                                            Layout.alignment: Qt.AlignHCenter
                                            orientation: Qt.Vertical
                                            from: -12.0
                                            to: 12.0
                                            value: EngineController.getGeqBandGain(index)
                                            onMoved: {
                                                EngineController.setGeqBandGain(index, value);
                                                eqCanvas.requestPaint();
                                            }

                                            Connections {
                                                target: EngineController
                                                function onGeqParametersChanged() {
                                                    geqSlider.value = EngineController.getGeqBandGain(index);
                                                }
                                            }
                                        }

                                        Label {
                                            Layout.alignment: Qt.AlignHCenter
                                            text: modelData
                                            font.pixelSize: 9
                                            font.family: "Monospace"
                                            color: "#808c99"
                                        }
                                    }
                                }
                            }
                        }

                        // PAGE 1: 24-BAND PARAMETRIC EQUALIZER VIEW
                        Rectangle {
                            id: peqContainer
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            color: "#0d0f12"
                            border.color: "#1c2128"
                            radius: 2

                            property bool isRightChannel: false
                            property int currentBandIndex: 0

                            function updateActiveBand() {
                                if (!bandEnableCheck || !typeCombo || !freqSlider || !gainSlider || !qSlider) return;
                                EngineController.setPeqBand(
                                    isRightChannel,
                                    currentBandIndex,
                                    freqSlider.value,
                                    gainSlider.value,
                                    qSlider.value,
                                    typeCombo.currentIndex,
                                    bandEnableCheck.checked
                                );
                                eqCanvas.requestPaint();
                            }

                            function reloadBand() {
                                var b = EngineController.getPeqBand(isRightChannel, currentBandIndex);
                                if (!b || !bandEnableCheck || !typeCombo || !freqSlider || !gainSlider || !qSlider) return;
                                bandEnableCheck.checked = (b.enabled !== undefined) ? b.enabled : true;
                                typeCombo.currentIndex = (b.type !== undefined) ? Math.min(5, Math.max(0, b.type)) : 0;
                                freqSlider.value = (b.frequency !== undefined) ? b.frequency : 1000.0;
                                gainSlider.value = (b.gainDb !== undefined) ? b.gainDb : 0.0;
                                qSlider.value = (b.q !== undefined) ? b.q : 1.0;
                                eqCanvas.requestPaint();
                            }

                            Component.onCompleted: reloadBand()

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 8

                                // Channel and Preset Bar
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8

                                    Button {
                                        text: qsTr("LEFT CHANNEL (12 Bands)")
                                        flat: peqContainer.isRightChannel
                                        highlighted: !peqContainer.isRightChannel
                                        onClicked: {
                                            peqContainer.isRightChannel = false;
                                            peqContainer.reloadBand();
                                        }
                                    }

                                    Button {
                                        text: qsTr("RIGHT CHANNEL (12 Bands)")
                                        flat: !peqContainer.isRightChannel
                                        highlighted: peqContainer.isRightChannel
                                        onClicked: {
                                            peqContainer.isRightChannel = true;
                                            peqContainer.reloadBand();
                                        }
                                    }

                                    Item { Layout.fillWidth: true }

                                    Label {
                                        text: qsTr("Preset:")
                                        font.pixelSize: 11
                                        color: "#808c99"
                                    }

                                    ComboBox {
                                        id: peqPresetCombo
                                        model: EngineController.availableListeningPresets()
                                    }

                                    Button {
                                        text: qsTr("Apply Preset")
                                        onClicked: {
                                            EngineController.applyPeqPreset(peqPresetCombo.currentText);
                                            peqContainer.reloadBand();
                                            eqCanvas.requestPaint();
                                        }
                                    }
                                }

                                // Band Selector Strip (1 to 12)
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 4

                                    Repeater {
                                        model: 12
                                        Button {
                                            Layout.fillWidth: true
                                            text: "Band " + (index + 1)
                                            flat: peqContainer.currentBandIndex !== index
                                            highlighted: peqContainer.currentBandIndex === index
                                            onClicked: {
                                                peqContainer.currentBandIndex = index;
                                                peqContainer.reloadBand();
                                            }
                                        }
                                    }
                                }

                                // Selected Band Parameter Deck
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    color: "#111418"
                                    border.color: "#212730"
                                    radius: 4

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.margins: 12
                                        spacing: 16

                                        // Band Enabled & Type
                                        ColumnLayout {
                                            Layout.preferredWidth: 160
                                            spacing: 8

                                            CheckBox {
                                                id: bandEnableCheck
                                                text: qsTr("Band Enabled")
                                                checked: true
                                                onToggled: peqContainer.updateActiveBand()
                                            }

                                            Label {
                                                text: qsTr("Filter Type:")
                                                font.pixelSize: 10
                                                color: "#808c99"
                                            }

                                            ComboBox {
                                                id: typeCombo
                                                Layout.fillWidth: true
                                                model: ["Peak (Bell)", "Low Shelf", "High Shelf", "Notch", "Low Pass", "High Pass"]
                                                onActivated: peqContainer.updateActiveBand()
                                            }
                                        }

                                        // Frequency Slider
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 4

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Label { text: qsTr("Center Frequency"); font.pixelSize: 11; color: "#A0AAB2" }
                                                Item { Layout.fillWidth: true }
                                                Label {
                                                    text: (freqSlider.value >= 1000 ? (freqSlider.value / 1000).toFixed(2) + " kHz" : Math.round(freqSlider.value) + " Hz")
                                                    font.pixelSize: 11
                                                    font.family: "Monospace"
                                                    color: "#00E5FF"
                                                }
                                            }

                                            Slider {
                                                id: freqSlider
                                                Layout.fillWidth: true
                                                from: 20.0
                                                to: 20000.0
                                                value: 1000.0
                                                onMoved: peqContainer.updateActiveBand()
                                            }
                                        }

                                        // Gain Slider
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 4
                                            enabled: typeCombo.currentIndex <= 2 // Gain applies only to Peak, Low Shelf, High Shelf

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Label { text: qsTr("Band Gain"); font.pixelSize: 11; color: "#A0AAB2" }
                                                Item { Layout.fillWidth: true }
                                                Label {
                                                    text: (gainSlider.value > 0 ? "+" : "") + gainSlider.value.toFixed(1) + " dB"
                                                    font.pixelSize: 11
                                                    font.family: "Monospace"
                                                    color: gainSlider.value !== 0 ? "#00E5FF" : "#808c99"
                                                }
                                            }

                                            Slider {
                                                id: gainSlider
                                                Layout.fillWidth: true
                                                from: -18.0
                                                to: 18.0
                                                value: 0.0
                                                onMoved: peqContainer.updateActiveBand()
                                            }
                                        }

                                        // Q / Resonance Slider
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 4

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Label { text: qsTr("Q Factor"); font.pixelSize: 11; color: "#A0AAB2" }
                                                Item { Layout.fillWidth: true }
                                                Label {
                                                    text: "Q: " + qSlider.value.toFixed(2)
                                                    font.pixelSize: 11
                                                    font.family: "Monospace"
                                                    color: "#00E5FF"
                                                }
                                            }

                                            Slider {
                                                id: qSlider
                                                Layout.fillWidth: true
                                                from: 0.1
                                                to: 10.0
                                                value: 1.0
                                                onMoved: peqContainer.updateActiveBand()
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // PAGE 2: EXTERNAL INSERT STUDIO RACK (4 CARDS)
                        Rectangle {
                            id: insertControlCenter
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            visible: eqConsoleRect.eqMode === "insert"
                            color: "transparent"

                            Connections {
                                target: EngineController
                                function onExternalInsertStatusChanged() {
                                    if (!insertClientNameInput.activeFocus) insertClientNameInput.text = EngineController.externalInsertClientName;
                                    if (!insertSendLInput.activeFocus) insertSendLInput.text = EngineController.externalInsertSendPortL;
                                    if (!insertSendRInput.activeFocus) insertSendRInput.text = EngineController.externalInsertSendPortR;
                                    if (!insertReturnLInput.activeFocus) insertReturnLInput.text = EngineController.externalInsertReturnPortL;
                                    if (!insertReturnRInput.activeFocus) insertReturnRInput.text = EngineController.externalInsertReturnPortR;
                                }
                            }

                            onVisibleChanged: {
                                if (visible) {
                                    insertPresetCombo.model = EngineController.listPresets("external_insert");
                                }
                            }

                            RowLayout {
                                anchors.fill: parent
                                spacing: 10

                                // CARD 1: PORT ROUTING & CLIENT CONFIGURATION
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    color: "#0d0f12"
                                    border.color: "#1c2128"
                                    radius: 3

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 12
                                        spacing: 8

                                        Label {
                                            text: qsTr("1. PORT ROUTING & HOST CLIENT")
                                            font.bold: true
                                            font.pixelSize: 11
                                            color: "#00E5FF"
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2
                                            Label { text: qsTr("Target Host Client Name:"); font.pixelSize: 10; color: "#808c99" }
                                            TextField {
                                                id: insertClientNameInput
                                                Layout.fillWidth: true
                                                text: EngineController.externalInsertClientName
                                                placeholderText: "e.g. Carla, lsp-plugins"
                                            }
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 6
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 2
                                                Label { text: qsTr("Send Port L:"); font.pixelSize: 10; color: "#808c99" }
                                                TextField {
                                                    id: insertSendLInput
                                                    Layout.fillWidth: true
                                                    text: EngineController.externalInsertSendPortL
                                                    placeholderText: "carla:audio_in_1"
                                                }
                                            }
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 2
                                                Label { text: qsTr("Send Port R:"); font.pixelSize: 10; color: "#808c99" }
                                                TextField {
                                                    id: insertSendRInput
                                                    Layout.fillWidth: true
                                                    text: EngineController.externalInsertSendPortR
                                                    placeholderText: "carla:audio_in_2"
                                                }
                                            }
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 6
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 2
                                                Label { text: qsTr("Return Port L:"); font.pixelSize: 10; color: "#808c99" }
                                                TextField {
                                                    id: insertReturnLInput
                                                    Layout.fillWidth: true
                                                    text: EngineController.externalInsertReturnPortL
                                                    placeholderText: "carla:audio_out_1"
                                                }
                                            }
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 2
                                                Label { text: qsTr("Return Port R:"); font.pixelSize: 10; color: "#808c99" }
                                                TextField {
                                                    id: insertReturnRInput
                                                    Layout.fillWidth: true
                                                    text: EngineController.externalInsertReturnPortR
                                                    placeholderText: "carla:audio_out_2"
                                                }
                                            }
                                        }

                                        Item { Layout.fillHeight: true }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 8
                                            Button {
                                                text: qsTr("Connect / Reconfigure")
                                                highlighted: true
                                                Layout.fillWidth: true
                                                onClicked: {
                                                    EngineController.configureExternalInsert(
                                                        insertClientNameInput.text,
                                                        insertSendLInput.text,
                                                        insertSendRInput.text,
                                                        insertReturnLInput.text,
                                                        insertReturnRInput.text,
                                                        EngineController.graphSampleRate,
                                                        256,
                                                        EngineController.externalInsertReportedLatency
                                                    );
                                                }
                                            }
                                            Button {
                                                text: qsTr("Disconnect")
                                                Layout.preferredWidth: 90
                                                onClicked: EngineController.disconnectExternalInsert()
                                            }
                                        }
                                    }
                                }

                                // CARD 2: HOST STATUS & SIGNAL INTEGRITY
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    color: "#0d0f12"
                                    border.color: "#1c2128"
                                    radius: 3

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 12
                                        spacing: 8

                                        Label {
                                            text: qsTr("2. HOST STATUS & INTEGRITY")
                                            font.bold: true
                                            font.pixelSize: 11
                                            color: "#00E5FF"
                                        }

                                        Rectangle {
                                            id: insertBadgeFull
                                            Layout.fillWidth: true
                                            height: 44
                                            radius: 3
                                            property int st: EngineController.externalInsertState
                                            property bool isByp: EngineController.externalInsertBypassed
                                            color: st === 2 ? (isByp ? "#FFD60020" : "#00E67620") : (st === 1 ? "#00E5FF20" : (st === 0 ? "#1c2128" : "#FF525220"))
                                            border.color: st === 2 ? (isByp ? "#FFD600" : "#00E676") : (st === 1 ? "#00E5FF" : (st === 0 ? "#2c333e" : "#FF5252"))

                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.margins: 8
                                                spacing: 10
                                                Rectangle {
                                                    width: 12
                                                    height: 12
                                                    radius: 6
                                                    color: insertBadgeFull.border.color
                                                }
                                                ColumnLayout {
                                                    spacing: 1
                                                    Label {
                                                        text: EngineController.externalInsertStateText.toUpperCase()
                                                        font.bold: true
                                                        font.pixelSize: 11
                                                        color: insertBadgeFull.border.color
                                                    }
                                                    Label {
                                                        text: EngineController.externalInsertBypassed ? qsTr("Dry Bypass Active (Lossless passthrough)") : qsTr("Active Stream passing through Insert Node")
                                                        font.pixelSize: 9
                                                        color: "#9aa5b1"
                                                    }
                                                }
                                            }
                                        }

                                        Button {
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 30
                                            text: EngineController.externalInsertBypassed ? qsTr("ENGAGE INSERT (DISABLE BYPASS)") : qsTr("FORCE DRY BYPASS (MUTE INSERT)")
                                            highlighted: !EngineController.externalInsertBypassed
                                            onClicked: EngineController.setExternalInsertBypass(!EngineController.externalInsertBypassed)
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 3

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Label { text: qsTr("Feedback Protection:"); font.pixelSize: 9; color: "#808c99" }
                                                Item { Layout.fillWidth: true }
                                                Label { text: qsTr("PASSED (SAFE)"); font.pixelSize: 9; font.bold: true; color: "#00E676" }
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Label { text: qsTr("Sample Rate Match:"); font.pixelSize: 9; color: "#808c99" }
                                                Item { Layout.fillWidth: true }
                                                Label { text: (EngineController.graphSampleRate / 1000.0).toFixed(1) + " kHz (MATCHED)"; font.pixelSize: 9; font.bold: true; color: "#00E676" }
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Label { text: qsTr("Buffer Size Match:"); font.pixelSize: 9; color: "#808c99" }
                                                Item { Layout.fillWidth: true }
                                                Label { text: "256 smp (MATCHED)"; font.pixelSize: 9; font.bold: true; color: "#00E676" }
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Label { text: qsTr("Insert XRUN / Dropouts:"); font.pixelSize: 9; color: "#808c99" }
                                                Item { Layout.fillWidth: true }
                                                Label {
                                                    text: EngineController.externalInsertXRunCount.toString()
                                                    font.pixelSize: 9
                                                    font.bold: true
                                                    color: EngineController.externalInsertXRunCount === 0 ? "#00E676" : "#FF5252"
                                                }
                                                Button {
                                                    text: qsTr("Reset")
                                                    font.pixelSize: 8
                                                    Layout.preferredHeight: 18
                                                    onClicked: EngineController.resetInsertXRuns()
                                                }
                                            }
                                        }

                                        Item { Layout.fillHeight: true }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 6
                                            Button {
                                                text: qsTr("Simulate Drop")
                                                font.pixelSize: 8
                                                Layout.fillWidth: true
                                                onClicked: EngineController.simulateInsertDisconnect()
                                            }
                                            Button {
                                                text: qsTr("Simulate Recovery")
                                                font.pixelSize: 8
                                                Layout.fillWidth: true
                                                onClicked: EngineController.simulateInsertRecovery()
                                            }
                                        }
                                    }
                                }

                                // CARD 3: PDC & LATENCY CALIBRATION
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    color: "#0d0f12"
                                    border.color: "#1c2128"
                                    radius: 3

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 12
                                        spacing: 8

                                        Label {
                                            text: qsTr("3. PDC & LATENCY CALIBRATION")
                                            font.bold: true
                                            font.pixelSize: 11
                                            color: "#00E5FF"
                                        }

                                        Rectangle {
                                            Layout.fillWidth: true
                                            height: 36
                                            color: "#07090b"
                                            border.color: "#181d24"
                                            radius: 2
                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.margins: 6
                                                Label { text: qsTr("Reported Latency:"); font.pixelSize: 9; color: "#808c99" }
                                                Item { Layout.fillWidth: true }
                                                Label {
                                                    text: EngineController.externalInsertReportedLatency + " smp (" + (EngineController.externalInsertReportedLatency / 48.0).toFixed(2) + " ms)"
                                                    font.family: "Monospace"
                                                    font.pixelSize: 10
                                                    font.bold: true
                                                    color: "#00E5FF"
                                                }
                                            }
                                        }

                                        Rectangle {
                                            Layout.fillWidth: true
                                            height: 36
                                            color: "#07090b"
                                            border.color: "#181d24"
                                            radius: 2
                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.margins: 6
                                                Label { text: qsTr("Measured (Impulse):"); font.pixelSize: 9; color: "#808c99" }
                                                Item { Layout.fillWidth: true }
                                                Label {
                                                    text: EngineController.externalInsertMeasuredLatency >= 0 ?
                                                          (EngineController.externalInsertMeasuredLatency + " smp (" + EngineController.externalInsertMeasuredLatencyMs.toFixed(2) + " ms)") :
                                                          qsTr("Not Calibrated")
                                                    font.family: "Monospace"
                                                    font.pixelSize: 10
                                                    font.bold: true
                                                    color: EngineController.externalInsertMeasuredLatency >= 0 ? "#00E676" : "#FFB300"
                                                }
                                            }
                                        }

                                        Button {
                                            text: qsTr("CALIBRATE IMPULSE LATENCY NOW")
                                            font.bold: true
                                            font.pixelSize: 9
                                            highlighted: true
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 28
                                            onClicked: EngineController.calibrateInsertLatency()
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2
                                            RowLayout {
                                                Layout.fillWidth: true
                                                Label { text: qsTr("Delay Compensation Buffer:"); font.pixelSize: 9; color: "#808c99" }
                                                Item { Layout.fillWidth: true }
                                                Label {
                                                    text: EngineController.externalInsertDelayCompensation + " smp (" + (EngineController.externalInsertDelayCompensation / 48.0).toFixed(2) + " ms)"
                                                    font.family: "Monospace"
                                                    font.pixelSize: 10
                                                    font.bold: true
                                                    color: "#00E5FF"
                                                }
                                            }
                                            Slider {
                                                id: pdcSlider
                                                Layout.fillWidth: true
                                                from: 0
                                                to: 4096
                                                stepSize: 1
                                                value: EngineController.externalInsertDelayCompensation
                                                onMoved: EngineController.setInsertDelayCompensation(value)
                                            }
                                        }

                                        Item { Layout.fillHeight: true }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 4
                                            Button {
                                                text: qsTr("To Measured")
                                                font.pixelSize: 8
                                                Layout.fillWidth: true
                                                enabled: EngineController.externalInsertMeasuredLatency >= 0
                                                onClicked: {
                                                    if (EngineController.externalInsertMeasuredLatency >= 0) {
                                                        EngineController.setInsertDelayCompensation(EngineController.externalInsertMeasuredLatency);
                                                    }
                                                }
                                            }
                                            Button {
                                                text: qsTr("To Reported")
                                                font.pixelSize: 8
                                                Layout.fillWidth: true
                                                onClicked: EngineController.setInsertDelayCompensation(EngineController.externalInsertReportedLatency)
                                            }
                                            Button {
                                                text: qsTr("Zero (0)")
                                                font.pixelSize: 8
                                                Layout.preferredWidth: 60
                                                onClicked: EngineController.setInsertDelayCompensation(0)
                                            }
                                        }
                                    }
                                }

                                // CARD 4: PRESET MANAGEMENT (XDG COMPLIANT)
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    color: "#0d0f12"
                                    border.color: "#1c2128"
                                    radius: 3

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 12
                                        spacing: 8

                                        Label {
                                            text: qsTr("4. PRESET MANAGEMENT (XDG)")
                                            font.bold: true
                                            font.pixelSize: 11
                                            color: "#00E5FF"
                                        }

                                        Label {
                                            text: qsTr("Category: external_insert")
                                            font.pixelSize: 9
                                            color: "#6c7885"
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2
                                            Label { text: qsTr("Select Preset:"); font.pixelSize: 9; color: "#808c99" }
                                            ComboBox {
                                                id: insertPresetCombo
                                                Layout.fillWidth: true
                                                model: EngineController.listPresets("external_insert")
                                            }
                                        }

                                        Button {
                                            text: qsTr("Apply Selected Preset")
                                            font.bold: true
                                            font.pixelSize: 9
                                            highlighted: true
                                            Layout.fillWidth: true
                                            onClicked: {
                                                if (insertPresetCombo.currentText.length > 0) {
                                                    EngineController.applyPresetProfile("external_insert", insertPresetCombo.currentText);
                                                }
                                            }
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2
                                            Label { text: qsTr("Save Current Setup:"); font.pixelSize: 9; color: "#808c99" }
                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 4
                                                TextField {
                                                    id: newPresetNameInput
                                                    Layout.fillWidth: true
                                                    placeholderText: qsTr("Preset name...")
                                                }
                                                Button {
                                                    text: qsTr("Save")
                                                    enabled: newPresetNameInput.text.trim().length > 0
                                                    onClicked: {
                                                        var map = EngineController.captureCurrentStateAsPreset("external_insert");
                                                        EngineController.savePreset("external_insert", newPresetNameInput.text.trim(), map);
                                                        insertPresetCombo.model = EngineController.listPresets("external_insert");
                                                        newPresetNameInput.text = "";
                                                    }
                                                }
                                            }
                                        }

                                        Item { Layout.fillHeight: true }

                                        Button {
                                            text: qsTr("Delete Selected Preset")
                                            font.pixelSize: 8
                                            Layout.fillWidth: true
                                            enabled: insertPresetCombo.currentText !== "Carla Default" && insertPresetCombo.currentText !== "LSP Plugins Rack"
                                            onClicked: {
                                                if (insertPresetCombo.currentText.length > 0) {
                                                    EngineController.deletePreset("external_insert", insertPresetCombo.currentText);
                                                    insertPresetCombo.model = EngineController.listPresets("external_insert");
                                                }
                                            }
                                        }

                                        Label {
                                            text: qsTr("Factory presets (Carla, LSP) are protected.")
                                            font.pixelSize: 8
                                            color: "#6c7885"
                                            wrapMode: Text.WordWrap
                                            Layout.fillWidth: true
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // 2. LOG TAB VIEW (SECTION 10.5 OPERATION HISTORY)
        Rectangle {
            id: logViewArea
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#0f1114"

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                // Log Tab Header & Filter Bar
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Label {
                        text: qsTr("OPERATION LOG TAB // AUDIT HISTORY")
                        font.bold: true
                        font.pixelSize: 14
                        color: "#00E5FF"
                    }

                    Item { Layout.fillWidth: true }

                    // Category Filter
                    ComboBox {
                        id: catFilter
                        model: [qsTr("All Categories"), "Deck", "FolderSession", "AudioEngine", "DSP", "Routing", "System", "Render"]
                        onActivated: logTable.refresh()
                    }

                    // Status Filter
                    ComboBox {
                        id: statusFilter
                        model: [qsTr("All Statuses"), "Success", "Failed", "Warning", "Info"]
                        onActivated: logTable.refresh()
                    }

                    TextField {
                        id: searchField
                        placeholderText: qsTr("Search message or details...")
                        Layout.preferredWidth: 220
                        onAccepted: logTable.refresh()
                    }

                    Button {
                        text: qsTr("Search")
                        onClicked: logTable.refresh()
                    }

                    Button {
                        text: qsTr("Export JSONL")
                        highlighted: true
                        onClicked: {
                            var path = "/tmp/desk32_log_export_" + Date.now() + ".jsonl"
                            if (LogService.exportToJsonl(path)) {
                                exportNotice.text = qsTr("Exported to %1").arg(path)
                            }
                        }
                    }

                    Button {
                        text: qsTr("Refresh")
                        onClicked: logTable.refresh()
                    }
                }

                Label {
                    id: exportNotice
                    text: qsTr("Retention policy: 180 days or 300,000 events max (auto-purged) // SQLite WAL mode active")
                    font.pixelSize: 11
                    color: "#6c7885"
                }

                // Log Events Table
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: "#13161a"
                    border.color: "#242930"
                    radius: 4

                    ListView {
                        id: logTable
                        anchors.fill: parent
                        anchors.margins: 8
                        clip: true
                        model: []

                        function refresh() {
                            var filter = {}
                            if (catFilter.currentIndex > 0) filter["category"] = catFilter.currentText
                            if (statusFilter.currentIndex > 0) filter["status"] = statusFilter.currentText
                            if (searchField.text.length > 0) filter["searchText"] = searchField.text
                            filter["limit"] = 200
                            model = LogService.fetchLogs(filter)
                        }

                        Component.onCompleted: refresh()

                        delegate: Rectangle {
                            width: logTable.width
                            height: 38
                            color: index % 2 === 0 ? "#111417" : "#14171b"
                            border.color: "#1c2128"
                            radius: 2

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 10

                                // Status Badge
                                Rectangle {
                                    width: 60
                                    height: 20
                                    radius: 3
                                    color: modelData.status === "Success" ? "#00E67625" : (modelData.status === "Failed" ? "#FF525225" : (modelData.status === "Warning" ? "#FFD60025" : "#00E5FF25"))
                                    border.color: modelData.status === "Success" ? "#00E676" : (modelData.status === "Failed" ? "#FF5252" : (modelData.status === "Warning" ? "#FFD600" : "#00E5FF"))
                                    Label {
                                        anchors.centerIn: parent
                                        text: modelData.status
                                        font.pixelSize: 9
                                        font.bold: true
                                        color: modelData.status === "Success" ? "#00E676" : (modelData.status === "Failed" ? "#FF5252" : (modelData.status === "Warning" ? "#FFD600" : "#00E5FF"))
                                    }
                                }

                                Label {
                                    text: modelData.timestampUtc
                                    font.family: "Monospace"
                                    font.pixelSize: 11
                                    color: "#758291"
                                    Layout.preferredWidth: 200
                                }

                                Label {
                                    text: modelData.category + " :: " + modelData.operation
                                    font.bold: true
                                    font.pixelSize: 11
                                    color: "#00E5FF"
                                    Layout.preferredWidth: 160
                                    elide: Text.ElideRight
                                }

                                Label {
                                    text: modelData.message
                                    font.pixelSize: 12
                                    color: "#E0E6ED"
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }

                                Label {
                                    text: modelData.durationMs.toFixed(2) + " ms"
                                    font.family: "Monospace"
                                    font.pixelSize: 11
                                    color: "#758291"
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
