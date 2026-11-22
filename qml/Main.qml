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
                Button {
                    text: EngineController.directModeEnabled
                          ? qsTr("DIRECT [ON]")
                          : (EngineController.directModeStatus === 2 ? qsTr("DIRECT MODE") : qsTr("DIRECT [OFF]"))
                    font.pixelSize: 10
                    font.bold: true
                    highlighted: EngineController.directModeEnabled
                    onClicked: EngineController.setDirectModeEnabled(!EngineController.directModeEnabled)
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Bypass DSP/gain & match track sample rate (last silence: %1 ms)")
                                  .arg(EngineController.lastRenegotiationSilenceMs.toFixed(1))
                }
                Rectangle {
                    width: 110
                    height: 22
                    radius: 3
                    color: EngineController.bitPerfectStatus === 2 ? "#00E5FF20" : (EngineController.bitPerfectStatus === 1 ? "#22262c" : "#1c1f24")
                    border.color: EngineController.bitPerfectStatus === 2 ? "#00E5FF" : (EngineController.bitPerfectStatus === 1 ? "#363d47" : "#2a3038")
                    Label {
                        anchors.centerIn: parent
                        text: EngineController.bitPerfectStatus === 2 ? qsTr("BIT-PERFECT") : (EngineController.bitPerfectStatus === 1 ? qsTr("NOT VERIFIED") : qsTr("BP: N/A"))
                        font.pixelSize: 10
                        font.bold: true
                        color: EngineController.bitPerfectStatus === 2 ? "#00E5FF" : (EngineController.bitPerfectStatus === 1 ? "#758291" : "#55606d")
                    }
                    MouseArea {
                        id: bpBadgeHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            bitPerfectDetailPopup.refreshReport()
                            bitPerfectDetailPopup.open()
                        }
                    }
                    ToolTip.visible: bpBadgeHover.containsMouse
                    ToolTip.text: (EngineController.directModeEnabled
                                  ? qsTr("Direct Mode ON: all DSP, Listening Correction, External Insert, and Master Gain/Mute are bypassed.")
                                  : (EngineController.bitPerfectStatus === 2
                                     ? qsTr("Bit-Perfect verified: no DSP, resampling, dither, or gain alteration active.")
                                     : (EngineController.bitPerfectStatus === 1
                                        ? qsTr("Not Verified: DSP, resampling, dither, or gain adjustment is active.")
                                        : qsTr("Indeterminate: waiting for a loaded track and initial Level 1 verification."))))
                                  + "  [" + qsTr("Click for L1/L2/L3 details") + "]"
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

            // DSP Parameter Preview Toggle (Section 5.9 Prior Implementation)
            Button {
                text: dspPreviewPanel.visible ? qsTr("🎛 DSP PARAMETERS [ON]") : qsTr("🎛 DSP PARAMETERS")
                font.bold: true
                font.pixelSize: 11
                highlighted: dspPreviewPanel.visible
                onClicked: {
                    dspPreviewPanel.visible = !dspPreviewPanel.visible
                    if (dspPreviewPanel.visible) {
                        dspPreviewPanel.reloadParameters()
                    }
                }
            }

            Rectangle {
                width: 1
                height: 20
                color: "#2a3038"
            }

            // Workspace Manager Panel Toggle (v3.8.1 §2.5)
            Button {
                text: WorkspaceManager.hasOpenWorkspace ? qsTr("📁 WORKSPACE [OPEN]") : qsTr("📁 WORKSPACE [R/O]")
                font.bold: true
                font.pixelSize: 11
                highlighted: workspacePanel.visible || WorkspaceManager.hasOpenWorkspace
                onClicked: workspacePanel.visible = !workspacePanel.visible
            }

            // Audio Output Device Selection Panel Toggle (§8.1 / §8.2)
            Button {
                text: outputDevicePanel.visible ? qsTr("🔊 OUTPUT [ON]") : qsTr("🔊 OUTPUT")
                font.bold: true
                font.pixelSize: 11
                highlighted: outputDevicePanel.visible
                onClicked: {
                    outputDevicePanel.visible = !outputDevicePanel.visible
                    if (outputDevicePanel.visible) {
                        outputDevicePanel.refreshDevices()
                    }
                }
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Output Device: %1 (%2)")
                              .arg(EngineController.currentOutputDevice)
                              .arg(EngineController.outputPcmFormat)
            }

            // Export / Render Panel Toggle (Phase 4 Step 3)
            Button {
                text: renderPanel.visible ? qsTr("⏺ RENDER [ON]") : qsTr("⏺ RENDER")
                font.bold: true
                font.pixelSize: 11
                highlighted: renderPanel.visible || !EngineController.isRenderIdle
                onClicked: renderPanel.visible = !renderPanel.visible
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
                    Layout.minimumWidth: 440
                    Layout.fillHeight: true
                    clip: true
                    color: "#14171a"
                    border.color: "#242930"
                    border.width: 1
                    radius: 4

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 6

                        // Section Header & Quick A/B Controls
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Label {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                elide: Text.ElideRight
                                text: qsTr("REFERENCE DECKS (4 SLOTS)")
                                font.bold: true
                                font.pixelSize: 12
                                color: "#00E5FF"
                            }
                            // 4-Deck Direct Switch Buttons: A, B, C, D
                            RowLayout {
                                spacing: 4
                                Repeater {
                                    model: ["A", "B", "C", "D"]
                                    Button {
                                        text: modelData
                                        leftPadding: 4
                                        rightPadding: 4
                                        Layout.preferredWidth: 28
                                        Layout.preferredHeight: 26
                                        font.bold: true
                                        font.pixelSize: 11
                                        highlighted: EngineController.activeDeckId === modelData
                                        onClicked: EngineController.switchDeck(modelData, EngineController.switchingMode)
                                    }
                                }
                            }
                        }

                        RowLayout {
                            id: quickControlRow
                            Layout.fillWidth: true
                            spacing: 6

                            property bool canAutoMatch: false
                            function refreshAutoMatchState() {
                                var refId = EngineController.activeDeckId
                                var refState = EngineController.getDeckState(refId)
                                if (!refState || !refState.path || refState.path === "") {
                                    canAutoMatch = false
                                    return
                                }
                                var ids = ["A", "B", "C", "D"]
                                for (var i = 0; i < ids.length; ++i) {
                                    if (ids[i] === refId) continue
                                    var st = EngineController.getDeckState(ids[i])
                                    if (st && st.path && st.path !== "") {
                                        canAutoMatch = true
                                        return
                                    }
                                }
                                canAutoMatch = false
                            }
                            Component.onCompleted: refreshAutoMatchState()
                            Connections {
                                target: EngineController
                                function onDeckStateChanged(deckId) { quickControlRow.refreshAutoMatchState() }
                                function onActiveDeckChanged(deckId) { quickControlRow.refreshAutoMatchState() }
                            }

                            Button {
                                text: qsTr("Quick A/B")
                                font.pixelSize: 11
                                leftPadding: 8
                                rightPadding: 8
                                Layout.preferredWidth: implicitContentWidth + leftPadding + rightPadding
                                Layout.preferredHeight: 26
                                highlighted: true
                                onClicked: EngineController.quickSwitchAB()
                            }
                            Button {
                                text: EngineController.levelMatchInProgress ? qsTr("Matching...") : qsTr("Auto Match")
                                font.pixelSize: 11
                                leftPadding: 8
                                rightPadding: 8
                                Layout.preferredWidth: implicitContentWidth + leftPadding + rightPadding
                                Layout.preferredHeight: 26
                                enabled: quickControlRow.canAutoMatch && !EngineController.levelMatchInProgress
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Measure integrated LUFS and match other loaded decks to active Deck %1").arg(EngineController.activeDeckId)
                                onClicked: {
                                    var refId = EngineController.activeDeckId
                                    var ids = ["A", "B", "C", "D"]
                                    var targets = []
                                    for (var i = 0; i < ids.length; ++i) {
                                        if (ids[i] === refId) continue
                                        var st = EngineController.getDeckState(ids[i])
                                        if (st && st.path && st.path !== "") {
                                            targets.push(ids[i])
                                        }
                                    }
                                    if (targets.length > 0) {
                                        EngineController.startAutoLevelMatch(refId, targets)
                                    }
                                }
                            }
                            ComboBox {
                                id: modeCombo
                                Layout.fillWidth: true
                                Layout.minimumWidth: 100
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
                                clip: true

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
                                            leftPadding: 6
                                            rightPadding: 6
                                            Layout.preferredWidth: implicitContentWidth + leftPadding + rightPadding
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
                                            leftPadding: 6
                                            rightPadding: 6
                                            Layout.preferredWidth: implicitContentWidth + leftPadding + rightPadding
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
                                            leftPadding: 6
                                            rightPadding: 6
                                            Layout.preferredWidth: implicitContentWidth + leftPadding + rightPadding
                                            Layout.preferredHeight: 24
                                            highlighted: !!(deckBox.deckState && deckBox.deckState.isLocked)
                                            onClicked: EngineController.setDeckLocked(modelData, !(deckBox.deckState && deckBox.deckState.isLocked))
                                        }

                                        // Copy deck track to active workspace
                                        Button {
                                            text: qsTr("->WS")
                                            font.pixelSize: 9
                                            leftPadding: 6
                                            rightPadding: 6
                                            Layout.preferredWidth: implicitContentWidth + leftPadding + rightPadding
                                            Layout.preferredHeight: 24
                                            enabled: WorkspaceManager.hasOpenWorkspace && !!(deckBox.deckState && deckBox.deckState.path && deckBox.deckState.path.length > 0)
                                            onClicked: EngineController.copyDeckTrackToWorkspace(modelData)
                                        }

                                        // Select button
                                        Button {
                                            text: qsTr("Select")
                                            font.pixelSize: 10
                                            leftPadding: 6
                                            rightPadding: 6
                                            Layout.preferredWidth: implicitContentWidth + leftPadding + rightPadding
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
                                            Layout.minimumWidth: 0
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
                                            Layout.minimumWidth: 0
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
                                            leftPadding: 4
                                            rightPadding: 4
                                            Layout.preferredWidth: 32
                                            Layout.preferredHeight: 26
                                            highlighted: !!(deckBox.deckState && deckBox.deckState.state === 1)
                                            onClicked: EngineController.play(modelData)
                                        }
                                        Button {
                                            text: "❚❚"
                                            leftPadding: 4
                                            rightPadding: 4
                                            Layout.preferredWidth: 32
                                            Layout.preferredHeight: 26
                                            highlighted: !!(deckBox.deckState && deckBox.deckState.state === 2)
                                            onClicked: EngineController.pause(modelData)
                                        }
                                        Button {
                                            text: "■"
                                            leftPadding: 4
                                            rightPadding: 4
                                            Layout.preferredWidth: 32
                                            Layout.preferredHeight: 26
                                            onClicked: EngineController.stop(modelData)
                                        }
                                        Slider {
                                            id: deckSlider
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 80
                                            from: 0
                                            to: (deckBox.deckState && deckBox.deckState.totalFrames > 0) ? deckBox.deckState.totalFrames : 100
                                            property bool dragMoved: false
                                            Binding {
                                                target: deckSlider
                                                property: "value"
                                                value: deckBox.deckState ? deckBox.deckState.playhead : 0
                                                when: !deckSlider.pressed
                                            }
                                            onMoved: {
                                                if (pressed) {
                                                    dragMoved = true
                                                } else {
                                                    EngineController.seek(modelData, Math.floor(value))
                                                }
                                            }
                                            onPressedChanged: {
                                                if (!pressed && dragMoved) {
                                                    dragMoved = false
                                                    EngineController.seek(modelData, Math.floor(value))
                                                }
                                            }
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
                                                Layout.minimumWidth: 20
                                            }

                                            Button {
                                                text: qsTr("Prev")
                                                font.pixelSize: 10
                                                leftPadding: 8
                                                rightPadding: 8
                                                Layout.preferredWidth: implicitContentWidth + leftPadding + rightPadding
                                                Layout.preferredHeight: 20
                                                onClicked: EngineController.retreatFolderPrevious(modelData)
                                            }
                                            Button {
                                                text: qsTr("Next")
                                                font.pixelSize: 10
                                                leftPadding: 8
                                                rightPadding: 8
                                                Layout.preferredWidth: implicitContentWidth + leftPadding + rightPadding
                                                Layout.preferredHeight: 20
                                                onClicked: EngineController.advanceFolderNext(modelData)
                                            }
                                        }
                                    }

                                    // Comparative Offset & Level Match Row (Spec §2.3 / §5.2)
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 4
                                        Label {
                                            text: qsTr("Off:%1").arg(deckBox.deckState ? deckBox.deckState.offsetFrames : 0)
                                            font.pixelSize: 9; font.family: "Monospace"; color: "#78909C"
                                        }
                                        Button { text: "-1k"; font.pixelSize: 8; padding: 2; Layout.preferredHeight: 18; Layout.preferredWidth: 28
                                            onClicked: EngineController.setDeckOffsetFrames(modelData, (deckBox.deckState ? deckBox.deckState.offsetFrames : 0) - 1000) }
                                        Button { text: "0"; font.pixelSize: 8; padding: 2; Layout.preferredHeight: 18; Layout.preferredWidth: 20
                                            onClicked: EngineController.setDeckOffsetFrames(modelData, 0) }
                                        Button { text: "+1k"; font.pixelSize: 8; padding: 2; Layout.preferredHeight: 18; Layout.preferredWidth: 28
                                            onClicked: EngineController.setDeckOffsetFrames(modelData, (deckBox.deckState ? deckBox.deckState.offsetFrames : 0) + 1000) }
                                        Item { Layout.fillWidth: true }
                                        Label {
                                            text: qsTr("Trim:%1dB").arg((deckBox.deckState && deckBox.deckState.levelMatchDb !== undefined ? deckBox.deckState.levelMatchDb : 0.0).toFixed(1))
                                            font.pixelSize: 9; font.family: "Monospace"; color: "#80DEEA"
                                            opacity: EngineController.directModeEnabled ? 0.45 : 1.0
                                        }
                                        Button { text: "-"; font.pixelSize: 9; padding: 2; Layout.preferredHeight: 18; Layout.preferredWidth: 20
                                            enabled: !EngineController.directModeEnabled
                                            opacity: EngineController.directModeEnabled ? 0.45 : 1.0
                                            onClicked: EngineController.setDeckLevelMatchDb(modelData, (deckBox.deckState && deckBox.deckState.levelMatchDb !== undefined ? deckBox.deckState.levelMatchDb : 0.0) - 0.5) }
                                        Button { text: "0"; font.pixelSize: 8; padding: 2; Layout.preferredHeight: 18; Layout.preferredWidth: 20
                                            enabled: !EngineController.directModeEnabled
                                            opacity: EngineController.directModeEnabled ? 0.45 : 1.0
                                            onClicked: EngineController.setDeckLevelMatchDb(modelData, 0.0) }
                                        Button { text: "+"; font.pixelSize: 9; padding: 2; Layout.preferredHeight: 18; Layout.preferredWidth: 20
                                            enabled: !EngineController.directModeEnabled
                                            opacity: EngineController.directModeEnabled ? 0.45 : 1.0
                                            onClicked: EngineController.setDeckLevelMatchDb(modelData, (deckBox.deckState && deckBox.deckState.levelMatchDb !== undefined ? deckBox.deckState.levelMatchDb : 0.0) + 0.5) }
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
                            id: monitorDiagStrip
                            Layout.fillWidth: true
                            spacing: 8
                            property var audioCaps: EngineController.getAudioCapabilities()
                            Connections {
                                target: EngineController
                                function onDiagnosticsChanged() {
                                    monitorDiagStrip.audioCaps = EngineController.getAudioCapabilities()
                                }
                            }
                            Label {
                                text: qsTr("MAIN AUDIO MONITOR // REAL-TIME DIAGNOSTICS")
                                font.bold: true
                                font.pixelSize: 13
                                color: "#00E5FF"
                            }
                            Item { Layout.fillWidth: true }
                            Label {
                                property int qVal: (monitorDiagStrip.audioCaps && monitorDiagStrip.audioCaps.negotiatedQuantum > 0)
                                                   ? monitorDiagStrip.audioCaps.negotiatedQuantum : 256
                                text: "PIPEWIRE: " + EngineController.backendStatusText.toUpperCase()
                                      + " // " + (EngineController.graphSampleRate / 1000.0).toFixed(1) + " kHz"
                                      + " // " + EngineController.outputPcmFormat
                                      + " // BUFFER: " + qVal
                                      + " // XRUN: " + EngineController.xrunCount
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

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Label {
                                text: qsTr("MASTER CONTROL // OUTPUT BUS (SECTION 10.2.3)")
                                font.bold: true
                                font.pixelSize: 12
                                color: "#00E5FF"
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                            }
                            Button {
                                text: EngineController.currentOutputDevice === "pipewire_default" || EngineController.currentOutputDevice === "offline_simulation"
                                      ? qsTr("Device: Default")
                                      : qsTr("Device: %1").arg(EngineController.currentOutputDevice)
                                font.pixelSize: 9
                                font.bold: true
                                Layout.preferredHeight: 22
                                Layout.maximumWidth: 165
                                onClicked: {
                                    outputDevicePanel.visible = !outputDevicePanel.visible
                                    if (outputDevicePanel.visible) {
                                        outputDevicePanel.refreshDevices()
                                    }
                                }
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Select Audio Output Device (%1)").arg(EngineController.currentOutputDevice)
                            }
                            Rectangle {
                                visible: EngineController.directModeEnabled
                                implicitWidth: masterDirectBypassLbl.implicitWidth + 10
                                height: 18
                                radius: 3
                                color: "#FFB30020"
                                border.color: "#FFB300"
                                Label {
                                    id: masterDirectBypassLbl
                                    anchors.centerIn: parent
                                    text: qsTr("BYPASSED (DIRECT MODE)")
                                    font.pixelSize: 9
                                    font.bold: true
                                    color: "#FFB300"
                                }
                            }
                        }

                        // 3-Path Output Routing Box (Section 5.11 & 10.2.3)
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 145
                            color: "#101215"
                            border.color: "#20252b"
                            radius: 4

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 4
                                enabled: !EngineController.directModeEnabled
                                opacity: EngineController.directModeEnabled ? 0.45 : 1.0

                                Label {
                                    text: qsTr("OUTPUT ROUTING (SECTION 9.2 & 5.11)")
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

                                RadioButton {
                                    text: qsTr("Render / File Output Path (Offline Export)")
                                    checked: EngineController.outputRoutingMode === 2
                                    enabled: false
                                }
                            }
                        }

                        // MASTER BUS CONTROLS: GLOBAL DSP BYPASS, MASTER MUTE & HEADROOM (SECTION 10.2.3)
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 110
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
                                        text: qsTr("MASTER BUS CONTROLS")
                                        font.bold: true
                                        font.pixelSize: 11
                                        color: "#A0AAB2"
                                    }
                                    Item { Layout.fillWidth: true }
                                    // Clip LED Badge
                                    Rectangle {
                                        width: 50
                                        height: 18
                                        radius: 3
                                        color: EngineController.masterClipped ? "#FF5252" : "#1a1e24"
                                        border.color: EngineController.masterClipped ? "#FF8A80" : "#2a3038"
                                        Label {
                                            anchors.centerIn: parent
                                            text: "CLIP"
                                            font.pixelSize: 9
                                            font.bold: true
                                            color: EngineController.masterClipped ? "#FFFFFF" : "#546E7A"
                                        }
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    enabled: !EngineController.directModeEnabled
                                    opacity: EngineController.directModeEnabled ? 0.45 : 1.0

                                    // Global DSP Bypass Button (Temporary bypass without altering individual states)
                                    Button {
                                        Layout.fillWidth: true
                                        text: EngineController.globalDspBypass ? qsTr("GLOBAL BYPASS [ON]") : qsTr("GLOBAL BYPASS [OFF]")
                                        font.pixelSize: 10
                                        font.bold: true
                                        highlighted: EngineController.globalDspBypass
                                        onClicked: EngineController.globalDspBypass = !EngineController.globalDspBypass
                                    }

                                    // Master Mute Button (Smooth ramp to 0, click-free)
                                    Button {
                                        Layout.fillWidth: true
                                        text: EngineController.masterMute ? qsTr("MUTE [ACTIVE]") : qsTr("MUTE [OFF]")
                                        font.pixelSize: 10
                                        font.bold: true
                                        highlighted: EngineController.masterMute
                                        onClicked: EngineController.masterMute = !EngineController.masterMute
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Label {
                                        text: qsTr("Headroom:")
                                        font.pixelSize: 10
                                        color: "#808c99"
                                    }
                                    Label {
                                        text: EngineController.masterClipped ? "0.0 dB (CLIP)" : (EngineController.masterHeadroomDb >= 90.0 ? "> 90.0 dB" : ("+ " + EngineController.masterHeadroomDb.toFixed(1) + " dB"))
                                        font.pixelSize: 10
                                        font.family: "Monospace"
                                        font.bold: true
                                        color: EngineController.masterClipped ? "#FF5252" : (EngineController.masterHeadroomDb < 3.0 ? "#FFD600" : "#00E5FF")
                                    }
                                    Item { Layout.fillWidth: true }
                                    Label {
                                        text: "Pk L: " + masterSectionRect.formatLevelDb(EngineController.masterPeakL) + " | R: " + masterSectionRect.formatLevelDb(EngineController.masterPeakR)
                                        font.pixelSize: 9
                                        font.family: "Monospace"
                                        color: "#78909C"
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

                            function formatLevelDb(g) {
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
                                        enabled: !EngineController.directModeEnabled
                                        opacity: EngineController.directModeEnabled ? 0.45 : 1.0
                                        onClicked: masterSectionRect.fadersLinked = !masterSectionRect.fadersLinked
                                    }

                                    // Reset to 0 dB Unity Gain
                                    Button {
                                        text: "0 dB"
                                        font.pixelSize: 9
                                        font.bold: true
                                        Layout.preferredHeight: 22
                                        enabled: !EngineController.directModeEnabled
                                        opacity: EngineController.directModeEnabled ? 0.45 : 1.0
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

                                            // Left Precision VU / Peak Meter Bar (Section 10.2.3)
                                            Rectangle {
                                                width: 14
                                                Layout.fillHeight: true
                                                color: "#07090b"
                                                border.color: "#1b2129"
                                                radius: 2
                                                clip: true

                                                // RMS Bar
                                                Rectangle {
                                                    anchors.bottom: parent.bottom
                                                    width: parent.width
                                                    height: Math.max(2, parent.height * Math.min(1.0, EngineController.masterRmsL))
                                                    color: "#00E5FF50"
                                                }

                                                // Peak Bar
                                                Rectangle {
                                                    anchors.bottom: parent.bottom
                                                    width: parent.width
                                                    height: Math.max(2, parent.height * Math.min(1.0, EngineController.masterPeakL))
                                                    color: height > parent.height * 0.98 ? "#FF5252" : (height > parent.height * 0.85 ? "#FFD600" : "#00E676")
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
                                                enabled: !EngineController.directModeEnabled
                                                opacity: EngineController.directModeEnabled ? 0.45 : 1.0
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

                                                TapHandler {
                                                    onDoubleTapped: {
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
                                                enabled: !EngineController.directModeEnabled
                                                opacity: EngineController.directModeEnabled ? 0.45 : 1.0
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

                                                TapHandler {
                                                    onDoubleTapped: {
                                                        EngineController.setMasterGainR(1.0);
                                                        masterSliderR.value = 1.0;
                                                        if (masterSectionRect.fadersLinked) {
                                                            EngineController.setMasterGainL(1.0);
                                                            masterSliderL.value = 1.0;
                                                        }
                                                    }
                                                }
                                            }

                                            // Right Precision VU / Peak Meter Bar (Section 10.2.3)
                                            Rectangle {
                                                width: 14
                                                Layout.fillHeight: true
                                                color: "#07090b"
                                                border.color: "#1b2129"
                                                radius: 2
                                                clip: true

                                                // RMS Bar
                                                Rectangle {
                                                    anchors.bottom: parent.bottom
                                                    width: parent.width
                                                    height: Math.max(2, parent.height * Math.min(1.0, EngineController.masterRmsR))
                                                    color: "#00E5FF50"
                                                }

                                                // Peak Bar
                                                Rectangle {
                                                    anchors.bottom: parent.bottom
                                                    width: parent.width
                                                    height: Math.max(2, parent.height * Math.min(1.0, EngineController.masterPeakR))
                                                    color: height > parent.height * 0.98 ? "#FF5252" : (height > parent.height * 0.85 ? "#FFD600" : "#00E676")
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
                        Rectangle {
                            visible: EngineController.directModeEnabled
                            implicitWidth: dspDirectBypassLbl.implicitWidth + 12
                            height: 20
                            radius: 3
                            color: "#FFB30020"
                            border.color: "#FFB300"
                            Label {
                                id: dspDirectBypassLbl
                                anchors.centerIn: parent
                                text: qsTr("BYPASSED BY DIRECT MODE (DSP INACTIVE)")
                                font.pixelSize: 10
                                font.bold: true
                                color: "#FFB300"
                            }
                        }
                        Item { Layout.fillWidth: true }
                        Button {
                            text: !EngineController.geqBypassed ? qsTr("GEQ [ON]") : qsTr("GEQ [BYPASS]")
                            font.pixelSize: 11
                            font.bold: true
                            highlighted: !EngineController.geqBypassed
                            visible: eqConsoleRect.eqMode === "geq"
                            enabled: !EngineController.directModeEnabled
                            opacity: EngineController.directModeEnabled ? 0.45 : 1.0
                            onClicked: EngineController.setDspNodeBypass("GEQ", !EngineController.geqBypassed)
                        }
                        Button {
                            text: !EngineController.peqBypassed ? qsTr("PEQ [ON]") : qsTr("PEQ [BYPASS]")
                            font.pixelSize: 11
                            font.bold: true
                            highlighted: !EngineController.peqBypassed
                            visible: eqConsoleRect.eqMode === "peq"
                            enabled: !EngineController.directModeEnabled
                            opacity: EngineController.directModeEnabled ? 0.45 : 1.0
                            onClicked: EngineController.setDspNodeBypass("PEQ", !EngineController.peqBypassed)
                        }
                        Button {
                            text: qsTr("Reset All GEQ (0 dB)")
                            font.pixelSize: 11
                            visible: eqConsoleRect.eqMode === "geq"
                            enabled: !EngineController.directModeEnabled
                            opacity: EngineController.directModeEnabled ? 0.45 : 1.0
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
                            text: qsTr("32-Band PEQ (Left 16 + Right 16)")
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

                    // STACKLAYOUT FOR EQ & EXTERNAL INSERT VIEWS (GEQ / PEQ / INSERT HOST)
                    StackLayout {
                        id: eqStack
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        currentIndex: eqConsoleRect.eqMode === "geq" ? 0 : (eqConsoleRect.eqMode === "peq" ? 1 : 2)

                        // PAGE 0: 32-BAND GRAPHIC EQUALIZER VIEW
                        RowLayout {
                            id: geqRowLayout
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: 4
                            enabled: !EngineController.directModeEnabled
                            opacity: EngineController.directModeEnabled ? 0.45 : 1.0

                            Repeater {
                                id: geqRepeater
                                model: [
                                    "16", "20", "25", "31.5", "40", "50", "63", "80",
                                    "100", "125", "160", "200", "250", "315", "400", "500",
                                    "630", "800", "1k", "1.25k", "1.6k", "2k", "2.5k", "3.15k",
                                    "4k", "5k", "6.3k", "8k", "10k", "12.5k", "16k", "20k"
                                ]

                                Rectangle {
                                    id: geqBandItem
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    color: "#0d0f12"
                                    border.color: "#1c2128"
                                    radius: 2

                                    property bool bandEnabled: EngineController.isGeqBandEnabled(index)

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

                                        // 仮配置: 個別バンド ON/OFF トグルボタン
                                        Rectangle {
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 16
                                            radius: 2
                                            color: geqBandItem.bandEnabled ? "#0e2a33" : "#181b20"
                                            border.color: geqBandItem.bandEnabled ? "#00E5FF" : "#37474f"

                                            Label {
                                                anchors.centerIn: parent
                                                text: geqBandItem.bandEnabled ? "ON" : "OFF"
                                                font.pixelSize: 8
                                                font.bold: true
                                                font.family: "Monospace"
                                                color: geqBandItem.bandEnabled ? "#00E5FF" : "#606c7a"
                                            }

                                            MouseArea {
                                                anchors.fill: parent
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    EngineController.setGeqBandEnabled(index, !geqBandItem.bandEnabled);
                                                    eqCanvas.requestPaint();
                                                }
                                            }
                                        }

                                        Label {
                                            Layout.alignment: Qt.AlignHCenter
                                            text: (geqSlider.value > 0 ? "+" : "") + geqSlider.value.toFixed(1)
                                            font.pixelSize: 9
                                            font.family: "Monospace"
                                            color: geqSlider.value !== 0 ? "#00E5FF" : "#606c7a"
                                            opacity: geqBandItem.bandEnabled ? 1.0 : 0.35

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
                                            opacity: geqBandItem.bandEnabled ? 1.0 : 0.35
                                            value: EngineController.getGeqBandGain(index)
                                            onMoved: {
                                                EngineController.setGeqBandGain(index, value);
                                                eqCanvas.requestPaint();
                                            }

                                            Connections {
                                                target: EngineController
                                                function onGeqParametersChanged() {
                                                    geqSlider.value = EngineController.getGeqBandGain(index);
                                                    geqBandItem.bandEnabled = EngineController.isGeqBandEnabled(index);
                                                }
                                            }
                                        }

                                        Label {
                                            Layout.alignment: Qt.AlignHCenter
                                            text: modelData
                                            font.pixelSize: 9
                                            font.family: "Monospace"
                                            color: geqBandItem.bandEnabled ? "#808c99" : "#4b5563"
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
                                enabled: !EngineController.directModeEnabled
                                opacity: EngineController.directModeEnabled ? 0.45 : 1.0

                                // Channel and Preset Bar
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8

                                    CheckBox {
                                        text: qsTr("PEQ Active")
                                        checked: !EngineController.peqBypassed
                                        onToggled: EngineController.setDspNodeBypass("PEQ", !checked)
                                    }

                                    Button {
                                        text: qsTr("LEFT CHANNEL (16 Bands)")
                                        flat: peqContainer.isRightChannel
                                        highlighted: !peqContainer.isRightChannel
                                        onClicked: {
                                            peqContainer.isRightChannel = false;
                                            peqContainer.reloadBand();
                                        }
                                    }

                                    Button {
                                        text: qsTr("RIGHT CHANNEL (16 Bands)")
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

                                // Band Selector Strip (1 to 16)
                                GridLayout {
                                    Layout.fillWidth: true
                                    columns: 8
                                    columnSpacing: 4
                                    rowSpacing: 4

                                    Repeater {
                                        model: 16
                                        Button {
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 24
                                            font.pixelSize: 10
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
                            color: "transparent"

                            property var discoveredHosts: []
                            property var insertStatus: EngineController.getExternalInsertStatus()
                            property var latencyReport: EngineController.getLatencyAlignmentReport()
                            property var audioCaps: EngineController.getAudioCapabilities()
                            property bool selectedLatencyReported: true
                            property int selectedReportedLatency: 0

                            function refreshInsertState() {
                                insertStatus = EngineController.getExternalInsertStatus();
                                latencyReport = EngineController.getLatencyAlignmentReport();
                                audioCaps = EngineController.getAudioCapabilities();
                            }

                            function scanExternalHosts() {
                                discoveredHosts = EngineController.discoverExternalInsertPorts();
                                var names = [];
                                for (var i = 0; i < discoveredHosts.length; ++i) {
                                    var h = discoveredHosts[i];
                                    var latTxt = h.latencyReported ? (h.reportedLatencyFrames + " smp") : "N/A";
                                    names.push(h.clientName + " [" + h.sampleRate + " Hz / " + h.bufferSize + " smp / lat " + latTxt + "]");
                                }
                                if (names.length === 0) {
                                    names.push(qsTr("No stereo external hosts found (start Carla / JACK host)"));
                                }
                                discoveredHostsCombo.model = names;
                            }

                            Connections {
                                target: EngineController
                                function onExternalInsertStatusChanged() {
                                    insertControlCenter.refreshInsertState();
                                    if (!insertClientNameInput.activeFocus) insertClientNameInput.text = EngineController.externalInsertClientName;
                                    if (!insertSendLInput.activeFocus) insertSendLInput.text = EngineController.externalInsertSendPortL;
                                    if (!insertSendRInput.activeFocus) insertSendRInput.text = EngineController.externalInsertSendPortR;
                                    if (!insertReturnLInput.activeFocus) insertReturnLInput.text = EngineController.externalInsertReturnPortL;
                                    if (!insertReturnRInput.activeFocus) insertReturnRInput.text = EngineController.externalInsertReturnPortR;
                                }
                                function onDiagnosticsChanged() {
                                    insertControlCenter.refreshInsertState();
                                }
                            }

                            StackLayout.onIsCurrentItemChanged: {
                                if (StackLayout.isCurrentItem) {
                                    insertPresetCombo.model = EngineController.listPresets("external_insert");
                                    insertControlCenter.refreshInsertState();
                                    insertControlCenter.scanExternalHosts();
                                }
                            }

                            RowLayout {
                                anchors.fill: parent
                                spacing: 12

                                // ================= CARD 1: HOST ROUTING & PORTS =================
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    color: "#0f1217"
                                    border.color: "#222934"
                                    border.width: 1
                                    radius: 6

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 12
                                        spacing: 6

                                        // Card Header with Accent Line
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 8
                                            Rectangle { width: 4; height: 16; radius: 2; color: "#00E5FF" }
                                            Label {
                                                text: qsTr("1. HOST ROUTING & PORTS")
                                                font.bold: true
                                                font.pixelSize: 12
                                                color: "#00E5FF"
                                            }
                                            Item { Layout.fillWidth: true }
                                            Button {
                                                text: qsTr("Scan PipeWire")
                                                font.pixelSize: 10
                                                Layout.preferredHeight: 24
                                                onClicked: insertControlCenter.scanExternalHosts()
                                            }
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 6
                                            ComboBox {
                                                id: discoveredHostsCombo
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 28
                                                font.pixelSize: 10
                                                model: [qsTr("Click Scan PipeWire to discover hosts...")]
                                                onActivated: function(idx) {
                                                    if (idx >= 0 && idx < insertControlCenter.discoveredHosts.length) {
                                                        var h = insertControlCenter.discoveredHosts[idx];
                                                        insertClientNameInput.text = h.clientName;
                                                        insertSendLInput.text = h.sendPortL;
                                                        insertSendRInput.text = h.sendPortR;
                                                        insertReturnLInput.text = h.returnPortL;
                                                        insertReturnRInput.text = h.returnPortR;
                                                        insertControlCenter.selectedLatencyReported = h.latencyReported;
                                                        insertControlCenter.selectedReportedLatency = h.reportedLatencyFrames;
                                                    }
                                                }
                                            }
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2
                                            Label { text: qsTr("Target Host Client Name:"); font.pixelSize: 10; font.bold: true; color: "#A0AAB2" }
                                            TextField {
                                                id: insertClientNameInput
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 28
                                                font.pixelSize: 11
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
                                                Label { text: qsTr("Send Port L:"); font.pixelSize: 10; font.bold: true; color: "#A0AAB2" }
                                                TextField {
                                                    id: insertSendLInput
                                                    Layout.fillWidth: true
                                                    Layout.preferredHeight: 28
                                                    font.pixelSize: 10
                                                    font.family: "Monospace"
                                                    text: EngineController.externalInsertSendPortL
                                                    placeholderText: "carla:audio_in_1"
                                                }
                                            }
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 2
                                                Label { text: qsTr("Send Port R:"); font.pixelSize: 10; font.bold: true; color: "#A0AAB2" }
                                                TextField {
                                                    id: insertSendRInput
                                                    Layout.fillWidth: true
                                                    Layout.preferredHeight: 28
                                                    font.pixelSize: 10
                                                    font.family: "Monospace"
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
                                                Label { text: qsTr("Return Port L:"); font.pixelSize: 10; font.bold: true; color: "#A0AAB2" }
                                                TextField {
                                                    id: insertReturnLInput
                                                    Layout.fillWidth: true
                                                    Layout.preferredHeight: 28
                                                    font.pixelSize: 10
                                                    font.family: "Monospace"
                                                    text: EngineController.externalInsertReturnPortL
                                                    placeholderText: "carla:audio_out_1"
                                                }
                                            }
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 2
                                                Label { text: qsTr("Return Port R:"); font.pixelSize: 10; font.bold: true; color: "#A0AAB2" }
                                                TextField {
                                                    id: insertReturnRInput
                                                    Layout.fillWidth: true
                                                    Layout.preferredHeight: 28
                                                    font.pixelSize: 10
                                                    font.family: "Monospace"
                                                    text: EngineController.externalInsertReturnPortR
                                                    placeholderText: "carla:audio_out_2"
                                                }
                                            }
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 6
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 2
                                                Label { text: qsTr("Chain Position:"); font.pixelSize: 10; color: "#A0AAB2" }
                                                ComboBox {
                                                    Layout.fillWidth: true
                                                    Layout.preferredHeight: 26
                                                    font.pixelSize: 10
                                                    model: ["Slot 0 (Pre-PEQ)", "Slot 1 (Post-PEQ)", "Slot 2 (Post-GEQ)", "Slot 3 (Post-Filter)", "Slot 4 (Post-Comp)", "Slot 5 (Post-Reverb)", "Slot 6 (Post-Limiter)"]
                                                    currentIndex: EngineController.externalInsertPosition
                                                    onActivated: function(idx) { EngineController.setExternalInsertPosition(idx); }
                                                }
                                            }
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 2
                                                Label { text: qsTr("On Disconnect:"); font.pixelSize: 10; color: "#A0AAB2" }
                                                ComboBox {
                                                    Layout.fillWidth: true
                                                    Layout.preferredHeight: 26
                                                    font.pixelSize: 10
                                                    model: [qsTr("Auto-Bypass"), qsTr("Mute Output"), qsTr("Stop Playback")]
                                                    currentIndex: EngineController.externalInsertDisconnectPolicy
                                                    onActivated: function(idx) { EngineController.setExternalInsertDisconnectPolicy(idx); }
                                                }
                                            }
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            visible: (EngineController.externalInsertLastError || "").length > 0
                                            text: EngineController.externalInsertLastError
                                            font.pixelSize: 10
                                            color: "#FF5252"
                                            wrapMode: Text.WordWrap
                                            maximumLineCount: 2
                                            elide: Text.ElideRight
                                        }

                                        Item { Layout.fillHeight: true }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 8
                                            Button {
                                                text: qsTr("Connect / Reconfigure")
                                                highlighted: true
                                                font.pixelSize: 11
                                                font.bold: true
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 32
                                                onClicked: {
                                                    var q = (insertControlCenter.audioCaps && insertControlCenter.audioCaps.negotiatedQuantum > 0)
                                                            ? insertControlCenter.audioCaps.negotiatedQuantum : 0;
                                                    EngineController.configureExternalInsertWithMetadata(
                                                        insertClientNameInput.text,
                                                        insertSendLInput.text,
                                                        insertSendRInput.text,
                                                        insertReturnLInput.text,
                                                        insertReturnRInput.text,
                                                        EngineController.graphSampleRate,
                                                        q,
                                                        insertControlCenter.selectedReportedLatency,
                                                        insertControlCenter.selectedLatencyReported,
                                                        insertClientNameInput.text,
                                                        "",
                                                        ""
                                                    );
                                                }
                                            }
                                            Button {
                                                text: qsTr("Disconnect")
                                                font.pixelSize: 11
                                                Layout.preferredWidth: 92
                                                Layout.preferredHeight: 32
                                                onClicked: EngineController.disconnectExternalInsert()
                                            }
                                        }
                                    }
                                }

                                // ================= CARD 2: SIGNAL INTEGRITY & STATUS =================
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    color: "#0f1217"
                                    border.color: "#222934"
                                    border.width: 1
                                    radius: 6

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 12
                                        spacing: 8

                                        // Card Header
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 8
                                            Rectangle { width: 4; height: 16; radius: 2; color: "#00E676" }
                                            Label {
                                                text: qsTr("2. STATUS & INTEGRITY")
                                                font.bold: true
                                                font.pixelSize: 12
                                                color: "#00E676"
                                            }
                                        }

                                        // Status Hero Panel (InsertState: 0=NotCfg,1=HostNotFound,2=PortsFound,3=Connected,4=Active,5=RateMismatch,6=BufferMismatch,7=XRun,8=Disconnected,9=Recovering,10=Error)
                                        Rectangle {
                                            id: insertBadgeFull
                                            Layout.fillWidth: true
                                            height: 48
                                            radius: 4
                                            property int st: EngineController.externalInsertState
                                            property bool isByp: EngineController.externalInsertBypassed
                                            property bool isLiveOk: (st === 3 || st === 4)
                                            color: isLiveOk ? (isByp ? "#FFD60020" : "#00E67620")
                                                            : ((st === 2 || st === 9) ? "#00E5FF20"
                                                            : (st === 0 ? "#1c2128" : "#FF525220"))
                                            border.color: isLiveOk ? (isByp ? "#FFD600" : "#00E676")
                                                                   : ((st === 2 || st === 9) ? "#00E5FF"
                                                                   : (st === 0 ? "#2c333e" : "#FF5252"))

                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.margins: 10
                                                spacing: 10
                                                Rectangle {
                                                    width: 14
                                                    height: 14
                                                    radius: 7
                                                    color: insertBadgeFull.border.color
                                                }
                                                ColumnLayout {
                                                    spacing: 2
                                                    Label {
                                                        text: EngineController.externalInsertStateText.toUpperCase()
                                                        font.bold: true
                                                        font.pixelSize: 12
                                                        font.family: "Monospace"
                                                        color: insertBadgeFull.border.color
                                                    }
                                                    Label {
                                                        text: !insertBadgeFull.isLiveOk
                                                              ? ((EngineController.externalInsertLastError || "").length > 0
                                                                 ? EngineController.externalInsertLastError
                                                                 : qsTr("Not linked to external host"))
                                                              : (EngineController.directModeEnabled
                                                                 ? qsTr("Bypassed by Direct Mode (Lossless passthrough)")
                                                                 : (EngineController.externalInsertBypassed
                                                                    ? qsTr("Dry Bypass Active (Lossless passthrough)")
                                                                    : qsTr("Active Stream passing through Insert Node")))
                                                        font.pixelSize: 10
                                                        color: "#A0AAB2"
                                                        elide: Text.ElideRight
                                                        Layout.fillWidth: true
                                                    }
                                                }
                                            }
                                        }

                                        Button {
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 32
                                            font.pixelSize: 11
                                            font.bold: true
                                            enabled: insertBadgeFull.isLiveOk && !EngineController.directModeEnabled
                                            opacity: EngineController.directModeEnabled ? 0.45 : 1.0
                                            text: EngineController.externalInsertBypassed ? qsTr("ENGAGE INSERT (DISABLE BYPASS)") : qsTr("FORCE DRY BYPASS (MUTE INSERT)")
                                            highlighted: !EngineController.externalInsertBypassed
                                            onClicked: EngineController.setExternalInsertBypass(!EngineController.externalInsertBypassed)
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 5

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Label { text: qsTr("Transport / Links:"); font.pixelSize: 11; color: "#A0AAB2" }
                                                Item { Layout.fillWidth: true }
                                                Label {
                                                    property var stMap: insertControlCenter.insertStatus
                                                    text: stMap.transportConnected
                                                          ? (stMap.transportSimulationMode
                                                             ? qsTr("SIMULATION (4/4)")
                                                             : qsTr("LIVE PIPEWIRE (%1/4 links)").arg(stMap.transportActiveLinks || 0))
                                                          : qsTr("DISCONNECTED (0/4)")
                                                    font.pixelSize: 10
                                                    font.bold: true
                                                    color: stMap.transportConnected ? (stMap.transportSimulationMode ? "#FFB300" : "#00E676") : "#A0AAB2"
                                                }
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Label { text: qsTr("Feedback Protection:"); font.pixelSize: 11; color: "#A0AAB2" }
                                                Item { Layout.fillWidth: true }
                                                Label {
                                                    property bool fbErr: EngineController.externalInsertState === 10 &&
                                                                         (EngineController.externalInsertLastError || "").indexOf("Feedback") >= 0
                                                    text: fbErr ? qsTr("BLOCKED (LOOP)") : qsTr("PASSED (SAFE)")
                                                    font.pixelSize: 11
                                                    font.bold: true
                                                    color: fbErr ? "#FF5252" : "#00E676"
                                                }
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Label { text: qsTr("Sample Rate Match:"); font.pixelSize: 11; color: "#A0AAB2" }
                                                Item { Layout.fillWidth: true }
                                                Label {
                                                    property bool rateBad: EngineController.externalInsertState === 5
                                                    text: rateBad
                                                          ? qsTr("MISMATCH")
                                                          : ((EngineController.graphSampleRate / 1000.0).toFixed(1) + " kHz (MATCHED)")
                                                    font.pixelSize: 11
                                                    font.bold: true
                                                    color: rateBad ? "#FF5252" : "#00E676"
                                                }
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Label { text: qsTr("Buffer Size Match:"); font.pixelSize: 11; color: "#A0AAB2" }
                                                Item { Layout.fillWidth: true }
                                                Label {
                                                    property bool bufBad: EngineController.externalInsertState === 6
                                                    property int qVal: (insertControlCenter.audioCaps && insertControlCenter.audioCaps.negotiatedQuantum > 0)
                                                                       ? insertControlCenter.audioCaps.negotiatedQuantum : 256
                                                    text: bufBad ? qsTr("MISMATCH") : (qVal + " smp (MATCHED)")
                                                    font.pixelSize: 11
                                                    font.bold: true
                                                    color: bufBad ? "#FF5252" : "#00E676"
                                                }
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Label { text: qsTr("Insert Dropouts (XRUN):"); font.pixelSize: 11; color: "#A0AAB2" }
                                                Item { Layout.fillWidth: true }
                                                Label {
                                                    text: EngineController.externalInsertXRunCount.toString()
                                                    font.pixelSize: 12
                                                    font.bold: true
                                                    font.family: "Monospace"
                                                    color: EngineController.externalInsertXRunCount === 0 ? "#00E676" : "#FF5252"
                                                }
                                                Button {
                                                    text: qsTr("Reset")
                                                    font.pixelSize: 10
                                                    Layout.preferredHeight: 24
                                                    onClicked: EngineController.resetInsertXRuns()
                                                }
                                            }
                                        }

                                        Item { Layout.fillHeight: true }

                                        Button {
                                            text: qsTr("Reconnect / Re-link Host")
                                            font.pixelSize: 11
                                            font.bold: true
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 30
                                            enabled: (EngineController.externalInsertClientName || "").length > 0
                                            onClicked: EngineController.reconnectExternalInsert()
                                        }
                                    }
                                }

                                // ================= CARD 3: PDC & LATENCY CALIBRATION =================
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    color: "#0f1217"
                                    border.color: "#222934"
                                    border.width: 1
                                    radius: 6

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 12
                                        spacing: 7

                                        // Card Header
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 8
                                            Rectangle { width: 4; height: 16; radius: 2; color: "#FF9100" }
                                            Label {
                                                text: qsTr("3. PDC & LATENCY CALIBRATION")
                                                font.bold: true
                                                font.pixelSize: 12
                                                color: "#FF9100"
                                            }
                                        }

                                        Rectangle {
                                            Layout.fillWidth: true
                                            height: 34
                                            color: "#07090b"
                                            border.color: "#1d232c"
                                            radius: 3
                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.margins: 8
                                                Label { text: qsTr("Reported Latency:"); font.pixelSize: 11; color: "#A0AAB2" }
                                                Item { Layout.fillWidth: true }
                                                Label {
                                                    property double srKhz: Math.max(1.0, EngineController.graphSampleRate / 1000.0)
                                                    text: (insertControlCenter.insertStatus && insertControlCenter.insertStatus.latencyReported === false)
                                                          ? qsTr("Not Reported")
                                                          : (EngineController.externalInsertReportedLatency + " smp (" + (EngineController.externalInsertReportedLatency / srKhz).toFixed(2) + " ms)")
                                                    font.family: "Monospace"
                                                    font.pixelSize: 11
                                                    font.bold: true
                                                    color: "#00E5FF"
                                                }
                                            }
                                        }

                                        Rectangle {
                                            Layout.fillWidth: true
                                            height: 34
                                            color: "#07090b"
                                            border.color: "#1d232c"
                                            radius: 3
                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.margins: 8
                                                Label { text: qsTr("Measured (Impulse):"); font.pixelSize: 11; color: "#A0AAB2" }
                                                Item { Layout.fillWidth: true }
                                                Label {
                                                    text: EngineController.externalInsertMeasuredLatency >= 0 ?
                                                          (EngineController.externalInsertMeasuredLatency + " smp (" + EngineController.externalInsertMeasuredLatencyMs.toFixed(2) + " ms)") :
                                                          qsTr("Not Calibrated")
                                                    font.family: "Monospace"
                                                    font.pixelSize: 11
                                                    font.bold: true
                                                    color: EngineController.externalInsertMeasuredLatency >= 0 ? "#00E676" : "#FFB300"
                                                }
                                            }
                                        }

                                        Button {
                                            text: EngineController.externalInsertCalibrating
                                                  ? qsTr("MEASURING (MONITOR MUTED)...")
                                                  : qsTr("CALIBRATE IMPULSE LATENCY NOW")
                                            font.bold: true
                                            font.pixelSize: 11
                                            highlighted: true
                                            enabled: !EngineController.externalInsertCalibrating &&
                                                     (EngineController.externalInsertState === 3 || EngineController.externalInsertState === 4)
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 32
                                            onClicked: EngineController.calibrateInsertLatency()
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            visible: insertControlCenter.latencyReport && (insertControlCenter.latencyReport.sessionActive || insertControlCenter.latencyReport.hasMeasurement)
                                            text: {
                                                var r = insertControlCenter.latencyReport;
                                                if (!r) return "";
                                                if (r.hasMeasurement) {
                                                    return qsTr("Candidate: %1 smp | Jitter: %2 smp | SNR: %3 dB [%4]")
                                                        .arg(r.recommendedFrames)
                                                        .arg(r.spreadFrames)
                                                        .arg(Number(r.peakSnrDb).toFixed(1))
                                                        .arg(r.confidenceText);
                                                }
                                                return r.summary || "";
                                            }
                                            font.pixelSize: 10
                                            color: (insertControlCenter.latencyReport && insertControlCenter.latencyReport.confidenceText === "High") ? "#00E676" : "#FFB300"
                                            wrapMode: Text.WordWrap
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 6
                                            visible: insertControlCenter.latencyReport && insertControlCenter.latencyReport.hasMeasurement && !insertControlCenter.latencyReport.confirmed
                                            Button {
                                                text: qsTr("Confirm & Apply PDC")
                                                font.pixelSize: 10
                                                font.bold: true
                                                highlighted: true
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 26
                                                enabled: insertControlCenter.latencyReport && insertControlCenter.latencyReport.confidenceText === "High"
                                                onClicked: EngineController.confirmRecommendedInsertLatency(false)
                                            }
                                            Button {
                                                text: qsTr("Cancel")
                                                font.pixelSize: 10
                                                Layout.preferredWidth: 68
                                                Layout.preferredHeight: 26
                                                onClicked: EngineController.cancelInsertLatencyCalibration()
                                            }
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2
                                            RowLayout {
                                                Layout.fillWidth: true
                                                Label { text: qsTr("Delay Compensation Buffer:"); font.pixelSize: 11; color: "#A0AAB2" }
                                                Item { Layout.fillWidth: true }
                                                Label {
                                                    property double srKhz: Math.max(1.0, EngineController.graphSampleRate / 1000.0)
                                                    text: EngineController.externalInsertDelayCompensation + " smp (" + (EngineController.externalInsertDelayCompensation / srKhz).toFixed(2) + " ms)"
                                                    font.family: "Monospace"
                                                    font.pixelSize: 11
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
                                            spacing: 6
                                            Button {
                                                text: qsTr("To Measured")
                                                font.pixelSize: 10
                                                font.bold: true
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 28
                                                enabled: EngineController.externalInsertMeasuredLatency >= 0
                                                onClicked: {
                                                    if (EngineController.externalInsertMeasuredLatency >= 0) {
                                                        EngineController.setInsertDelayCompensation(EngineController.externalInsertMeasuredLatency);
                                                    }
                                                }
                                            }
                                            Button {
                                                text: qsTr("To Reported")
                                                font.pixelSize: 10
                                                font.bold: true
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 28
                                                onClicked: EngineController.setInsertDelayCompensation(EngineController.externalInsertReportedLatency)
                                            }
                                            Button {
                                                text: qsTr("Zero (0)")
                                                font.pixelSize: 10
                                                Layout.preferredWidth: 70
                                                Layout.preferredHeight: 28
                                                onClicked: EngineController.setInsertDelayCompensation(0)
                                            }
                                        }
                                    }
                                }

                                // ================= CARD 4: PRESET MANAGEMENT (XDG) =================
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    color: "#0f1217"
                                    border.color: "#222934"
                                    border.width: 1
                                    radius: 6

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 10

                                        // Card Header
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 8
                                            Rectangle { width: 4; height: 16; radius: 2; color: "#B388FF" }
                                            Label {
                                                text: qsTr("4. PRESET MANAGEMENT (XDG)")
                                                font.bold: true
                                                font.pixelSize: 12
                                                color: "#B388FF"
                                            }
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 4
                                            Label { text: qsTr("Select Preset Profile:"); font.pixelSize: 11; font.bold: true; color: "#A0AAB2" }
                                            ComboBox {
                                                id: insertPresetCombo
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 34
                                                font.pixelSize: 11
                                                model: EngineController.listPresets("external_insert")
                                            }
                                        }

                                        Button {
                                            text: qsTr("Apply Selected Preset")
                                            font.bold: true
                                            font.pixelSize: 11
                                            highlighted: true
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 34
                                            onClicked: {
                                                if (insertPresetCombo.currentText.length > 0) {
                                                    EngineController.applyPresetProfile("external_insert", insertPresetCombo.currentText);
                                                }
                                            }
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 4
                                            Label { text: qsTr("Save Current Setup As Preset:"); font.pixelSize: 11; color: "#A0AAB2" }
                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 6
                                                TextField {
                                                    id: newPresetNameInput
                                                    Layout.fillWidth: true
                                                    Layout.preferredHeight: 32
                                                    font.pixelSize: 11
                                                    placeholderText: qsTr("Preset profile name...")
                                                }
                                                Button {
                                                    text: qsTr("Save")
                                                    font.bold: true
                                                    font.pixelSize: 11
                                                    Layout.preferredHeight: 32
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
                                            font.pixelSize: 10
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 28
                                            enabled: insertPresetCombo.currentText !== "Carla Default" && insertPresetCombo.currentText !== "LSP Plugins Rack"
                                            onClicked: {
                                                if (insertPresetCombo.currentText.length > 0) {
                                                    EngineController.deletePreset("external_insert", insertPresetCombo.currentText);
                                                    insertPresetCombo.model = EngineController.listPresets("external_insert");
                                                }
                                            }
                                        }

                                        Label {
                                            text: qsTr("Factory presets (Carla, LSP) are write-protected.")
                                            font.pixelSize: 10
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
                        model: [qsTr("All Categories"), "Session", "Transport", "Deck", "FolderSession", "AudioEngine", "DSP", "Routing", "ExternalInsert", "PresetManagement", "Render", "MasterControl", "System"]
                        onActivated: logTable.refresh()
                    }

                    // Status Filter
                    ComboBox {
                        id: statusFilter
                        model: [qsTr("All Statuses"), "Success", "Failed", "Warning", "Info", "Cancelled"]
                        onActivated: logTable.refresh()
                    }

                    Button {
                        id: playbackHistoryBtn
                        text: qsTr("Playback History")
                        checkable: true
                        checked: false
                        onToggled: logTable.refresh()
                    }

                    Button {
                        id: thisSessionBtn
                        text: qsTr("This Session")
                        checkable: true
                        checked: false
                        onToggled: logTable.refresh()
                    }

                    Button {
                        id: autoRefreshBtn
                        text: qsTr("Live")
                        checkable: true
                        checked: true
                    }

                    TextField {
                        id: searchField
                        placeholderText: qsTr("Search message or details...")
                        Layout.preferredWidth: 200
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
                            var f = logTable.buildFilter(0)
                            var ok = LogService.exportFilteredToJsonl(path, f)
                            if (ok) {
                                exportNotice.text = qsTr("Exported to %1").arg(path)
                            }
                        }
                    }

                    Button {
                        text: qsTr("Refresh")
                        onClicked: logTable.refresh()
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Label { text: qsTr("From (UTC):"); font.pixelSize: 11; color: "#78909C" }
                    TextField {
                        id: fromUtcField
                        placeholderText: "YYYY-MM-DDTHH:MM:SSZ"
                        Layout.preferredWidth: 190
                        font.family: "Monospace"
                        font.pixelSize: 11
                        onAccepted: logTable.refresh()
                    }
                    Label { text: qsTr("To (UTC):"); font.pixelSize: 11; color: "#78909C" }
                    TextField {
                        id: toUtcField
                        placeholderText: "YYYY-MM-DDTHH:MM:SSZ"
                        Layout.preferredWidth: 190
                        font.family: "Monospace"
                        font.pixelSize: 11
                        onAccepted: logTable.refresh()
                    }
                    Button {
                        text: qsTr("Last 1h")
                        onClicked: {
                            var now = new Date()
                            var prev = new Date(now.getTime() - 3600 * 1000)
                            fromUtcField.text = prev.toISOString()
                            toUtcField.text = ""
                            logTable.refresh()
                        }
                    }
                    Button {
                        text: qsTr("Last 12h")
                        onClicked: {
                            var now = new Date()
                            var prev = new Date(now.getTime() - 12 * 3600 * 1000)
                            fromUtcField.text = prev.toISOString()
                            toUtcField.text = ""
                            logTable.refresh()
                        }
                    }
                    Button {
                        text: qsTr("Clear Time")
                        onClicked: {
                            fromUtcField.text = ""
                            toUtcField.text = ""
                            logTable.refresh()
                        }
                    }
                    Item { Layout.fillWidth: true }
                }

                Label {
                    id: exportNotice
                    text: qsTr("Session: %1 | Retention: 180d / 300k ops (SQLite WAL)").arg(LogService.sessionId())
                    font.pixelSize: 11
                    color: "#6c7885"
                }

                Timer {
                    id: logRefreshDebounce
                    interval: 250
                    repeat: false
                    onTriggered: logTable.refresh()
                }

                Connections {
                    target: LogService
                    function onLogAppended(category, operation, status, message) {
                        if (autoRefreshBtn.checked) {
                            logRefreshDebounce.restart()
                        }
                    }
                }

                // Log Events Table + Detail pane
                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 10

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
                        property var selectedEvent: null
                        property var relatedEvents: []

                        function statusColor(s) {
                            if (s === "Success") return "#00E676"
                            if (s === "Failed") return "#FF5252"
                            if (s === "Warning") return "#FFD600"
                            if (s === "Cancelled") return "#FFB300"
                            return "#00E5FF"
                        }

                        function select(ev) {
                            selectedEvent = ev
                            relatedEvents = LogService.fetchRelatedLogs(
                                ev.parentOperationId.length > 0 ? ev.parentOperationId : ev.operationId)
                        }

                        function buildFilter(limitCount) {
                            var filter = {}
                            if (catFilter.currentIndex > 0) filter["category"] = catFilter.currentText
                            if (statusFilter.currentIndex > 0) filter["status"] = statusFilter.currentText
                            if (fromUtcField.text.length > 0) filter["fromUtc"] = fromUtcField.text.trim()
                            if (toUtcField.text.length > 0) filter["toUtc"] = toUtcField.text.trim()
                            if (thisSessionBtn.checked) filter["sessionId"] = LogService.sessionId()
                            if (playbackHistoryBtn.checked) filter["playbackHistoryOnly"] = true
                            if (searchField.text.length > 0) filter["searchText"] = searchField.text
                            filter["limit"] = limitCount
                            return filter
                        }

                        function parseDetailsJson(raw) {
                            if (!raw || raw.length === 0) return {}
                            try {
                                return JSON.parse(raw)
                            } catch (e) {
                                return raw
                            }
                        }

                        function formatDetailsJson(raw) {
                            if (!raw || raw.length === 0) return "{}"
                            try {
                                return JSON.stringify(JSON.parse(raw), null, 2)
                            } catch (e) {
                                return raw
                            }
                        }

                        function formatEventJson(ev) {
                            if (!ev) return "{}"
                            var status = ev.status || ""
                            var obj = {
                                event_id: ev.eventId !== undefined ? ev.eventId : 0,
                                operation_id: ev.operationId || "",
                                parent_operation_id: (ev.parentOperationId && ev.parentOperationId.length > 0)
                                    ? ev.parentOperationId : null,
                                timestamp_utc: ev.timestampUtc || "",
                                local_utc_offset: ev.localUtcOffset || "",
                                category: ev.category || "",
                                operation: ev.operation || "",
                                status: status,
                                target_type: ev.targetType || "",
                                target: ev.target || "",
                                message: ev.message || "",
                                error_code: ev.errorCode !== undefined ? ev.errorCode : 0,
                                duration_ms: ev.durationMs !== undefined ? ev.durationMs : 0.0,
                                details_json: parseDetailsJson(ev.detailsJson)
                            }
                            return JSON.stringify(obj, null, 2)
                        }

                        function refresh() {
                            if (playbackHistoryBtn.checked) {
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
                                return
                            }
                            var rows = LogService.fetchLogs(buildFilter(300))
                            model = rows
                        }

                        Component.onCompleted: refresh()

                        delegate: Rectangle {
                            width: logTable.width
                            height: 38
                            color: (logTable.selectedEvent && logTable.selectedEvent.eventId === modelData.eventId)
                                   ? "#1d2a33" : (index % 2 === 0 ? "#111417" : "#14171b")
                            border.color: "#1c2128"
                            radius: 2

                            MouseArea {
                                anchors.fill: parent
                                onClicked: logTable.select(modelData)
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 10

                                // Status Badge
                                Rectangle {
                                    width: 70
                                    height: 20
                                    radius: 3
                                    color: Qt.rgba(0, 0, 0, 0)
                                    border.color: logTable.statusColor(modelData.status)
                                    Label {
                                        anchors.centerIn: parent
                                        text: modelData.status
                                        font.pixelSize: 9
                                        font.bold: true
                                        color: logTable.statusColor(modelData.status)
                                    }
                                }

                                Label {
                                    text: modelData.parentOperationId.length > 0 ? "↳" : ""
                                    color: "#758291"
                                    Layout.preferredWidth: 10
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

                                Rectangle {
                                    visible: modelData.errorCode !== 0
                                    width: errCodeLbl.implicitWidth + 10
                                    height: 18
                                    radius: 3
                                    color: "#FF525225"
                                    border.color: "#FF5252"
                                    Label {
                                        id: errCodeLbl
                                        anchors.centerIn: parent
                                        text: "E:" + modelData.errorCode
                                        font.family: "Monospace"
                                        font.pixelSize: 9
                                        font.bold: true
                                        color: "#FF5252"
                                    }
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

                // Detail + related (parent / child) events
                Rectangle {
                    Layout.preferredWidth: 420
                    Layout.fillHeight: true
                    color: "#13161a"
                    border.color: "#242930"
                    radius: 4

                    ScrollView {
                        anchors.fill: parent
                        anchors.margins: 8
                        clip: true
                        contentWidth: availableWidth

                        ColumnLayout {
                            width: 400
                            spacing: 4

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6
                                Label {
                                    text: qsTr("EVENT DETAILS")
                                    font.bold: true; font.pixelSize: 11; color: "#A0AAB2"
                                }
                                Item { Layout.fillWidth: true }
                                Button {
                                    visible: !!logTable.selectedEvent
                                    text: qsTr("Copy OpID")
                                    onClicked: {
                                        EngineController.copyToClipboard(logTable.selectedEvent.operationId)
                                        exportNotice.text = qsTr("Copied operationId: %1").arg(logTable.selectedEvent.operationId)
                                    }
                                }
                                Button {
                                    visible: !!logTable.selectedEvent
                                    text: qsTr("Copy JSON")
                                    onClicked: {
                                        var formatted = logTable.formatEventJson(logTable.selectedEvent)
                                        EngineController.copyToClipboard(formatted)
                                        exportNotice.text = qsTr("Copied event JSON to clipboard")
                                    }
                                }
                            }
                            Label {
                                visible: !logTable.selectedEvent
                                text: qsTr("Select an event to view details and related events.")
                                font.pixelSize: 10; color: "#6c7885"
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }

                            Repeater {
                                model: logTable.selectedEvent ? [
                                    ["operationId", logTable.selectedEvent.operationId],
                                    ["parentOperationId", logTable.selectedEvent.parentOperationId],
                                    ["timestampUtc", logTable.selectedEvent.timestampUtc],
                                    ["localUtcOffset", logTable.selectedEvent.localUtcOffset],
                                    ["targetType", logTable.selectedEvent.targetType],
                                    ["target", logTable.selectedEvent.target],
                                    ["errorCode", String(logTable.selectedEvent.errorCode)],
                                    ["durationMs", logTable.selectedEvent.durationMs.toFixed(2) + " ms"],
                                    ["message", logTable.selectedEvent.message],
                                    ["detailsJson", logTable.formatDetailsJson(logTable.selectedEvent.detailsJson)]
                                ] : []
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Label { text: modelData[0]; font.pixelSize: 9; color: "#78909C" }
                                    Label {
                                        text: modelData[1]
                                        Layout.fillWidth: true
                                        wrapMode: Text.WrapAnywhere
                                        font.pixelSize: 10; font.family: "Monospace"; color: "#E0E6ED"
                                    }
                                }
                            }

                            Rectangle { Layout.fillWidth: true; height: 1; color: "#222730"; visible: logTable.relatedEvents.length > 0 }
                            Label {
                                visible: logTable.relatedEvents.length > 0
                                text: qsTr("RELATED EVENTS (%1)").arg(logTable.relatedEvents.length)
                                font.bold: true; font.pixelSize: 11; color: "#A0AAB2"
                            }
                            Repeater {
                                model: logTable.relatedEvents
                                Rectangle {
                                    Layout.fillWidth: true
                                    height: relRow.implicitHeight + 8
                                    color: (logTable.selectedEvent && logTable.selectedEvent.eventId === modelData.eventId) ? "#1d2a33" : "#111417"
                                    radius: 2
                                    MouseArea { anchors.fill: parent; onClicked: logTable.select(modelData) }
                                    RowLayout {
                                        id: relRow
                                        anchors.fill: parent
                                        anchors.margins: 4
                                        spacing: 6
                                        Label {
                                            text: modelData.parentOperationId.length > 0 ? "  ↳" : "▸"
                                            color: "#758291"
                                        }
                                        Label {
                                            text: modelData.status
                                            font.pixelSize: 9; font.bold: true
                                            color: logTable.statusColor(modelData.status)
                                            Layout.preferredWidth: 60
                                        }
                                        Label {
                                            text: modelData.operation + ": " + modelData.message
                                            Layout.fillWidth: true
                                            elide: Text.ElideRight
                                            font.pixelSize: 10; color: "#E0E6ED"
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

    // Step 1: Lightweight Non-Modal DSP Parameter Preview Panel (Section 5.9 Prior Implementation)
    DspPreviewPanel {
        id: dspPreviewPanel
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.top: parent.top
        anchors.topMargin: 56
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 16
        z: 999
        visible: false
        onCloseRequested: dspPreviewPanel.visible = false
    }

    // Phase 4 Step 3: Export / Render panel
    RenderPanel {
        id: renderPanel
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.top: parent.top
        anchors.topMargin: 56
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 16
        width: 460
        z: 1000
        visible: false
        onCloseRequested: renderPanel.visible = false
        onOpenLogRequested: {
            renderPanel.visible = false
            root.currentView = "log"
            catFilter.currentIndex = catFilter.model.indexOf("Render")
            statusFilter.currentIndex = 0
            searchField.text = ""
            logTable.refresh()
        }
    }

    // v3.8.1 Dedicated Workspace Panel (startup selection, import, tags, saves, diff, rollback)
    WorkspacePanel {
        id: workspacePanel
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.top: parent.top
        anchors.topMargin: 56
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 16
        width: 540
        z: 1001
        visible: !WorkspaceManager.selectionFinalized
        onCloseRequested: {
            if (!WorkspaceManager.selectionFinalized) {
                WorkspaceManager.continueInReadOnlyMode()
            }
            workspacePanel.visible = false
        }
    }

    // Audio Output Device Enumeration & Selection Panel (§8.1 / §8.2)
    OutputDevicePanel {
        id: outputDevicePanel
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.top: parent.top
        anchors.topMargin: 56
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 16
        width: 480
        z: 1002
        visible: false
        onCloseRequested: outputDevicePanel.visible = false
    }

    Connections {
        target: WorkspaceManager
        function onWorkspaceErrorOccurred(op, msg) {
            if (op === "WorkspaceRootMissing") {
                workspaceRootMissingLabel.text = msg
                workspaceRootMissingDialog.open()
            }
        }
    }

    Dialog {
        id: workspaceRootMissingDialog
        title: qsTr("Workspace Unavailable — Switched to Read-Only")
        modal: true
        anchors.centerIn: Overlay.overlay
        width: 440
        standardButtons: Dialog.Ok
        contentItem: ColumnLayout {
            spacing: 8
            Label {
                id: workspaceRootMissingLabel
                Layout.fillWidth: true
                wrapMode: Text.WrapAnywhere
                font.pixelSize: 11
                color: "#FFB300"
                text: ""
            }
        }
    }

    // L1 / L2 / L3 Bit-Perfect Verification Detail Popup
    Popup {
        id: bitPerfectDetailPopup
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: 560
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        padding: 16

        property var report: EngineController.getBitPerfectVerificationReport()

        function refreshReport() {
            report = EngineController.getBitPerfectVerificationReport()
        }

        function statusBadgeColor(st) {
            if (st === "Verified" || st === "Direct") return "#00E676"
            if (st === "NotVerified") return "#FF5252"
            return "#FFB300"
        }

        Connections {
            target: EngineController
            function onDiagnosticsChanged() {
                if (bitPerfectDetailPopup.visible) {
                    bitPerfectDetailPopup.refreshReport()
                }
            }
        }

        background: Rectangle {
            color: "#12151a"
            border.color: "#2b323d"
            border.width: 1
            radius: 6
        }

        contentItem: ColumnLayout {
            spacing: 10

            // Header
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Rectangle {
                    width: 8
                    height: 8
                    radius: 4
                    color: bitPerfectDetailPopup.statusBadgeColor(bitPerfectDetailPopup.report.modeStateText)
                }

                Label {
                    text: qsTr("BIT-PERFECT VERIFICATION REPORT (L1 / L2 / L3)")
                    font.bold: true
                    font.pixelSize: 12
                    font.family: "Monospace"
                    color: "#00E5FF"
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    implicitWidth: modeStateLbl.implicitWidth + 12
                    height: 20
                    radius: 3
                    color: "#1b2028"
                    border.color: bitPerfectDetailPopup.statusBadgeColor(bitPerfectDetailPopup.report.modeStateText)
                    Label {
                        id: modeStateLbl
                        anchors.centerIn: parent
                        text: bitPerfectDetailPopup.report.modeStateText || "Indeterminate"
                        font.pixelSize: 10
                        font.bold: true
                        font.family: "Monospace"
                        color: bitPerfectDetailPopup.statusBadgeColor(bitPerfectDetailPopup.report.modeStateText)
                    }
                }

                Button {
                    text: qsTr("Copy JSON")
                    font.pixelSize: 10
                    Layout.preferredHeight: 22
                    onClicked: EngineController.copyToClipboard(JSON.stringify(bitPerfectDetailPopup.report, null, 2))
                }

                Button {
                    text: "✕"
                    font.pixelSize: 11
                    font.bold: true
                    Layout.preferredWidth: 26
                    Layout.preferredHeight: 22
                    onClicked: bitPerfectDetailPopup.close()
                }
            }

            // L1: Software Pipeline Verification
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: l1Col.implicitHeight + 16
                color: "#0d1014"
                border.color: "#222934"
                radius: 4

                ColumnLayout {
                    id: l1Col
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 4

                    property var l1: bitPerfectDetailPopup.report.level1 || ({})

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: qsTr("L1 // SOFTWARE PIPELINE VERIFICATION")
                            font.bold: true
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: "#00E5FF"
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            text: l1Col.l1.status || "Indeterminate"
                            font.bold: true
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: bitPerfectDetailPopup.statusBadgeColor(l1Col.l1.status)
                        }
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 12
                        rowSpacing: 3

                        Label { text: qsTr("maxAbsDiff (Float64):"); font.pixelSize: 10; color: "#8d99a6" }
                        Label {
                            property double d: l1Col.l1.maxAbsDiff !== undefined ? l1Col.l1.maxAbsDiff : 0.0
                            text: (d === 0.0 ? "0.0" : d.toExponential(4)) + "  (post-PCM: " + ((l1Col.l1.postPcmMaxAbsDiff !== undefined && l1Col.l1.postPcmMaxAbsDiff !== 0.0) ? l1Col.l1.postPcmMaxAbsDiff.toExponential(4) : "0.0") + ")"
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: d === 0.0 ? "#00E676" : "#FFB300"
                        }

                        Label { text: qsTr("Verified / Total Blocks:"); font.pixelSize: 10; color: "#8d99a6" }
                        Label {
                            text: (l1Col.l1.verifiedBlocks !== undefined ? l1Col.l1.verifiedBlocks : 0) + " / " + (l1Col.l1.totalBlocks !== undefined ? l1Col.l1.totalBlocks : 0)
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: "#E0E6ED"
                        }

                        Label { text: qsTr("Hash Match (Input vs Output):"); font.pixelSize: 10; color: "#8d99a6" }
                        Label {
                            text: (l1Col.l1.hashMatched ? qsTr("MATCHED") : qsTr("MISMATCH")) + "  (" + (l1Col.l1.inputHashHex || "0x0") + " / " + (l1Col.l1.outputHashHex || "0x0") + ")"
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: l1Col.l1.hashMatched ? "#00E676" : "#FF5252"
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }
                    }
                }
            }

            // L2: Backend / PipeWire Verification
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: l2Col.implicitHeight + 16
                color: "#0d1014"
                border.color: "#222934"
                radius: 4

                ColumnLayout {
                    id: l2Col
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 4

                    property var l2: bitPerfectDetailPopup.report.level2 || ({})

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: qsTr("L2 // BACKEND & TRANSPORT VERIFICATION")
                            font.bold: true
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: "#00E5FF"
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            text: l2Col.l2.status || "Indeterminate"
                            font.bold: true
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: bitPerfectDetailPopup.statusBadgeColor(l2Col.l2.status)
                        }
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 12
                        rowSpacing: 3

                        Label { text: qsTr("Backend State:"); font.pixelSize: 10; color: "#8d99a6" }
                        Label {
                            text: (l2Col.l2.backendStatus || EngineController.backendStatusText) + " (" + (l2Col.l2.backendParamsVerified ? qsTr("Direct Params OK") : qsTr("Non-Direct / Unverified")) + ")"
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: l2Col.l2.backendParamsVerified ? "#00E676" : "#FFB300"
                        }

                        Label { text: qsTr("Sample Rate Match:"); font.pixelSize: 10; color: "#8d99a6" }
                        Label {
                            property int trkRate: Math.round(l2Col.l2.trackSampleRate || 0)
                            property int reqRate: l2Col.l2.requestedRate || 0
                            property int actRate: l2Col.l2.actualRate || 0
                            text: (l2Col.l2.graphRateMatchesTrack ? qsTr("MATCHED") : qsTr("MISMATCH / RESAMPLED")) + "  (Track: " + (trkRate > 0 ? trkRate + " Hz" : "N/A") + ", Req: " + reqRate + " Hz, Actual: " + actRate + " Hz)"
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: l2Col.l2.graphRateMatchesTrack ? "#00E676" : "#FFB300"
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        Label { text: qsTr("negotiatedFormat:"); font.pixelSize: 10; color: "#8d99a6" }
                        Label {
                            text: (l2Col.l2.negotiatedFormat || "F32LE") + " (" + (l2Col.l2.outputBitDepth || 32) + "-bit)"
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: "#E0E6ED"
                        }

                        Label { text: qsTr("Lossless Output Format:"); font.pixelSize: 10; color: "#8d99a6" }
                        Label {
                            text: (l2Col.l2.outputFormatLossless ? qsTr("LOSSLESS (YES)") : qsTr("LOSSY / TRUNCATED (NO)")) + "  |  Dither: " + (l2Col.l2.ditherActive ? qsTr("Active") : qsTr("Off"))
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: (l2Col.l2.outputFormatLossless && !l2Col.l2.ditherActive) ? "#00E676" : "#FFB300"
                        }
                    }
                }
            }

            // L3: External Hardware / DAC Output (Explicitly Unverifiable)
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: l3Col.implicitHeight + 16
                color: "#1b160c"
                border.color: "#FFB300"
                radius: 4

                ColumnLayout {
                    id: l3Col
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 4

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: qsTr("L3 // HARDWARE / DAC OUTPUT VERIFICATION")
                            font.bold: true
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: "#FFB300"
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            text: qsTr("UNVERIFIABLE (EXT CAPTURE REQ)")
                            font.bold: true
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: "#FFB300"
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Unverifiable (External hardware capture required): Host software cannot observe DAC physical output after PipeWire/driver stages, so L1/L2 verification alone does not prove end-to-end hardware bit-perfectness.")
                        font.pixelSize: 10
                        color: "#FFE082"
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }
}
