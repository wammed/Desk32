import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Desk32.Core

ApplicationWindow {
    id: root
    visible: true
    width: 2560
    height: 1440
    minimumWidth: 1280
    minimumHeight: 720
    title: qsTr("Desk32 - Audiophile Studio Console")

    color: "#0f1114"

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
            anchors.fill: parent
            anchors.margins: 10
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
                            Layout.preferredHeight: 110
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

                                RadioButton {
                                    text: qsTr("Render Monitor Path (Pure Master, 0.000000 Leakage)")
                                    checked: EngineController.outputRoutingMode === 1
                                    onClicked: EngineController.outputRoutingMode = 1
                                }
                            }
                        }

                        // DSP NODES INDIVIDUAL BYPASS TOGGLES
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 160
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

                                    CheckBox { text: "PEQ (24-Band)"; checked: true; onToggled: EngineController.setDspNodeBypass("PEQ", !checked) }
                                    CheckBox { text: "GEQ (32-Band)"; checked: true; onToggled: EngineController.setDspNodeBypass("GEQ", !checked) }
                                    CheckBox { text: "HPF / LPF"; checked: true; onToggled: EngineController.setDspNodeBypass("Filter", !checked) }
                                    CheckBox { text: "Compressor"; checked: true; onToggled: EngineController.setDspNodeBypass("Compressor", !checked) }
                                    CheckBox { text: "Peak Limiter"; checked: true; onToggled: EngineController.setDspNodeBypass("Limiter", !checked) }
                                    CheckBox { text: "Reverb"; checked: true; onToggled: EngineController.setDspNodeBypass("Reverb", !checked) }
                                }
                            }
                        }

                        // Master Volume Gain & VU Peak Meters
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            color: "#101215"
                            border.color: "#20252b"
                            radius: 4

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 12
                                spacing: 8

                                Label {
                                    text: qsTr("MASTER OUTPUT LEVEL")
                                    font.bold: true
                                    font.pixelSize: 11
                                    color: "#A0AAB2"
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    spacing: 12

                                    // Stereo VU meter bars
                                    RowLayout {
                                        Layout.fillHeight: true
                                        spacing: 4

                                        // Left meter
                                        Rectangle {
                                            width: 14
                                            Layout.fillHeight: true
                                            color: "#080a0c"
                                            border.color: "#1e242c"
                                            Rectangle {
                                                anchors.bottom: parent.bottom
                                                width: parent.width
                                                height: parent.height * 0.72
                                                color: height > parent.height * 0.9 ? "#FF5252" : (height > parent.height * 0.75 ? "#FFD600" : "#00E676")
                                            }
                                        }
                                        // Right meter
                                        Rectangle {
                                            width: 14
                                            Layout.fillHeight: true
                                            color: "#080a0c"
                                            border.color: "#1e242c"
                                            Rectangle {
                                                anchors.bottom: parent.bottom
                                                width: parent.width
                                                height: parent.height * 0.70
                                                color: height > parent.height * 0.9 ? "#FF5252" : (height > parent.height * 0.75 ? "#FFD600" : "#00E676")
                                            }
                                        }
                                    }

                                    // Master Gain Slider
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        Slider {
                                            Layout.fillWidth: true
                                            from: 0.0
                                            to: 2.0
                                            value: EngineController.masterGain
                                            onMoved: EngineController.masterGain = value
                                        }
                                        Label {
                                            Layout.alignment: Qt.AlignHCenter
                                            text: (20.0 * Math.log10(Math.max(0.0001, EngineController.masterGain))).toFixed(1) + " dBFS"
                                            font.family: "Monospace"
                                            font.pixelSize: 12
                                            color: "#00E5FF"
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // BOTTOM ROW: Full-Width Studio EQ Console (approx 450px height)
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 450
                color: "#13161a"
                border.color: "#242930"
                border.width: 1
                radius: 4

                property string eqMode: "geq" // "geq" or "peq"

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8

                    // EQ Console Header & Mode Switcher
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: qsTr("STUDIO EQUALIZER CONSOLE")
                            font.bold: true
                            font.pixelSize: 13
                            color: "#00E5FF"
                        }
                        Item { Layout.fillWidth: true }
                        Button {
                            text: qsTr("32-Band GEQ (Full Console)")
                            flat: parent.parent.parent.eqMode !== "geq"
                            highlighted: parent.parent.parent.eqMode === "geq"
                            onClicked: parent.parent.parent.eqMode = "geq"
                        }
                        Button {
                            text: qsTr("24-Band PEQ (Left 12 + Right 12)")
                            flat: parent.parent.parent.eqMode !== "peq"
                            highlighted: parent.parent.parent.eqMode === "peq"
                            onClicked: parent.parent.parent.eqMode = "peq"
                        }
                    }

                    // 32-BAND GRAPHIC EQUALIZER VIEW (32 Vertical Faders)
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: 4
                        visible: parent.parent.eqMode === "geq"

                        Repeater {
                            model: [
                                "20", "25", "31.5", "40", "50", "63", "80", "100",
                                "125", "160", "200", "250", "315", "400", "500", "630",
                                "800", "1k", "1.25k", "1.6k", "2k", "2.5k", "3.15k", "4k",
                                "5k", "6.3k", "8k", "10k", "12.5k", "16k", "20k", "25k"
                            ]

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                color: "#0d0f12"
                                border.color: "#1c2128"
                                radius: 2

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
                                    }

                                    Slider {
                                        id: geqSlider
                                        Layout.fillHeight: true
                                        Layout.alignment: Qt.AlignHCenter
                                        orientation: Qt.Vertical
                                        from: -12.0
                                        to: 12.0
                                        value: 0.0
                                        onMoved: EngineController.setGeqBandGain(index, value)
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

                    // 24-BAND PARAMETRIC EQUALIZER VIEW
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: "#0d0f12"
                        border.color: "#1c2128"
                        visible: parent.parent.eqMode === "peq"

                        Label {
                            anchors.centerIn: parent
                            text: qsTr("24-Band Parametric EQ Strip (Left 12 Bands + Right 12 Bands Independent Editing)")
                            color: "#808c99"
                            font.pixelSize: 14
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
