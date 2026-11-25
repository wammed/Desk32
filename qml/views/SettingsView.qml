import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Desk32.Core
import "../components"

Item {
    id: settingsRoot

    implicitWidth: 1600
    implicitHeight: 900

    property double selectedUiScale: 1.0
    signal uiScaleChangedRequested(double newScale)

    property int activeCategory: 0
    property var audioCaps: ({})
    property var devicesList: []
    property double cacheBytes: 0
    property int cacheFileCount: 0
    property string cachePathText: ""
    property string feedbackBanner: ""
    property bool resetDefaultsArmed: false

    function formatBytes(bytes) {
        if (!bytes || bytes <= 0) return "0 B"
        if (bytes < 1024) return bytes + " B"
        if (bytes < 1024 * 1024) return (bytes / 1024.0).toFixed(1) + " KB"
        return (bytes / (1024.0 * 1024.0)).toFixed(2) + " MB"
    }

    function capColor(val) {
        if (val === "Supported") return "#00E676"
        if (val === "Simulated") return "#33E3FF"
        return "#6A798A"
    }

    function refresh() {
        audioCaps = EngineController.getAudioCapabilities() || ({})
        devicesList = EngineController.enumerateOutputDevices() || []
        cacheBytes = EngineController.waveformCacheSizeBytes()
        cacheFileCount = EngineController.waveformCacheFileCount()
        cachePathText = EngineController.waveformCachePath()
        resetDefaultsArmed = false
    }

    Component.onCompleted: refresh()

    Connections {
        target: EngineController
        function onWaveformCacheChanged() {
            settingsRoot.cacheBytes = EngineController.waveformCacheSizeBytes()
            settingsRoot.cacheFileCount = EngineController.waveformCacheFileCount()
        }
        function onAudioBackendChanged() { settingsRoot.refresh() }
        function onDiagnosticsChanged() { settingsRoot.audioCaps = EngineController.getAudioCapabilities() || ({}) }
    }

    component DarkSectionCard: Rectangle {
        radius: 4
        color: "#080B10"
        border.color: "#222C38"
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

        // Subtle dark scrim so foreground Settings panel reads cleanly over blurred console
        Rectangle {
            anchors.fill: parent
            color: "#4805070b"
        }
    }

    // Soft cast drop shadow beneath floating Settings frame
    BorderImage {
        anchors.fill: settingsFrame
        anchors.leftMargin: -22
        anchors.rightMargin: -22
        anchors.topMargin: -18
        anchors.bottomMargin: -26
        source: "qrc:/Desk32/resources/ui/panel_shadow_9slice.png"
        border { left: 22; top: 22; right: 22; bottom: 22 }
        opacity: 0.92
    }

    Rectangle {
        id: settingsFrame
        anchors.centerIn: parent
        width: parent && parent.width > 0 ? Math.min(parent.width, 1600) : 1600
        height: parent && parent.height > 0 ? Math.min(parent.height, 900) : 900
        color: "#0e1116"
        border.color: "#252e3a"
        border.width: 1
        radius: 6
        clip: true

        RowLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 10

        // =====================================================================
        // LEFT CATEGORY NAVIGATION PANEL (260px)
        // =====================================================================
        BrushedMetalPanel {
            Layout.preferredWidth: 268
            Layout.fillHeight: true
            baseTint: "#13171d"
            borderColor: "#2f3743"
            showScrews: true
            cornerRadius: 5

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                Label {
                    text: qsTr("CONSOLE SETTINGS")
                    font.family: "Monospace"; font.pixelSize: 12; font.bold: true; color: "#E8BA6B"
                }

                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; height: 1; color: "#27303C" }

                Repeater {
                    model: [
                        { title: qsTr("Audio Engine & Device"), sub: qsTr("Backend, Sample Rate, Quantum") },
                        { title: qsTr("Processing & Safety"), sub: qsTr("Reverb Caps, Insert Policy, Session") },
                        { title: qsTr("Cache & Storage"), sub: qsTr("Waveform Cache, Config & DB Paths") },
                        { title: qsTr("Interface & Language"), sub: qsTr("UI Scale, Bilingual i18n, Display") },
                        { title: qsTr("Graphics, RHI & Telemetry"), sub: qsTr("Vulkan / OpenGL, GPU, Capabilities") }
                    ]
                    ConsoleButton {
                        Layout.fillWidth: true
                        implicitHeight: 48
                        text: modelData.title
                        fontSize: 11
                        showLed: true
                        ledColor: settingsRoot.activeCategory === index ? "#00E5FF" : "#3A4452"
                        accentColor: "#00E5FF"
                        highlighted: settingsRoot.activeCategory === index
                        onClicked: settingsRoot.activeCategory = index
                    }
                }

                Item { Layout.fillHeight: true }

                DarkSectionCard {
                    Layout.fillWidth: true
                    implicitHeight: 68
                    ColumnLayout {
                        anchors.fill: parent; anchors.margins: 8; spacing: 2
                        Label {
                            text: qsTr("ACTIVE GRAPHICS RHI")
                            font.family: "Monospace"; font.pixelSize: 9; font.bold: true; color: "#7F93A6"
                        }
                        Label {
                            text: (EngineController.graphicsBackend || "Unknown") + "  •  " + (EngineController.audioBackend || "PipeWire")
                            font.family: "Monospace"; font.pixelSize: 11; font.bold: true; color: "#00E5FF"
                            elide: Text.ElideRight; Layout.fillWidth: true; Layout.minimumWidth: 0
                        }
                        Label {
                            text: EngineController.gpuRendererName || "-"
                            font.family: "Monospace"; font.pixelSize: 9; color: "#9AA9B8"
                            elide: Text.ElideRight; Layout.fillWidth: true; Layout.minimumWidth: 0
                        }
                    }
                }

                ConsoleButton {
                    Layout.fillWidth: true
                    implicitHeight: 34
                    text: qsTr("SAVE SETTINGS CONFIG")
                    fontSize: 10
                    accentColor: "#00E676"
                    onClicked: {
                        var ok = SettingsManager.save()
                        settingsRoot.feedbackBanner = ok ? qsTr("Saved configuration to %1").arg(SettingsManager.configFilePath) : qsTr("Failed to save configuration")
                    }
                }
            }
        }

        // =====================================================================
        // RIGHT DETAIL PANEL
        // =====================================================================
        BrushedMetalPanel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            baseTint: "#13171d"
            borderColor: "#2f3743"
            showScrews: true
            cornerRadius: 5

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                // Feedback Strip
                Rectangle {
                    visible: settingsRoot.feedbackBanner.length > 0
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    height: 30; radius: 4
                    color: "#092117"; border.color: "#227A54"
                    RowLayout {
                        anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 8
                        Label { Layout.fillWidth: true; Layout.minimumWidth: 0; text: settingsRoot.feedbackBanner; font.family: "Monospace"; font.pixelSize: 10; color: "#B9F6CA"; elide: Text.ElideMiddle }
                        ConsoleButton { text: "✕"; implicitWidth: 24; implicitHeight: 20; fontSize: 9; onClicked: settingsRoot.feedbackBanner = "" }
                    }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: settingsRoot.activeCategory

                    // ---------------------------------------------------------
                    // CATEGORY 0: AUDIO ENGINE & DEVICE
                    // ---------------------------------------------------------
                    ColumnLayout {
                        spacing: 12

                        Label { text: qsTr("AUDIO ENGINE & HARDWARE OUTPUT"); font.family: "Monospace"; font.pixelSize: 13; font.bold: true; color: "#E8BA6B" }

                        DarkSectionCard {
                            Layout.fillWidth: true
                            implicitHeight: 245
                            GridLayout {
                                anchors.fill: parent; anchors.margins: 14
                                columns: 2; columnSpacing: 18; rowSpacing: 12

                                Label { text: qsTr("Audio Backend"); font.pixelSize: 12; color: "#BCC8D4" }
                                ConsoleComboBox {
                                    Layout.fillWidth: true
                                    implicitHeight: 32
                                    model: EngineController.availableAudioBackends()
                                    currentIndex: Math.max(0, model.indexOf(EngineController.audioBackend))
                                    onActivated: {
                                        EngineController.setAudioBackend(currentText)
                                        settingsRoot.refresh()
                                    }
                                }

                                Label { text: qsTr("Target Output Device"); font.pixelSize: 12; color: "#BCC8D4" }
                                RowLayout {
                                    Layout.fillWidth: true; spacing: 6
                                    ConsoleComboBox {
                                        Layout.fillWidth: true
                                        implicitHeight: 32
                                        model: {
                                            var names = []
                                            for (var i = 0; i < settingsRoot.devicesList.length; ++i) {
                                                names.push(settingsRoot.devicesList[i].description || settingsRoot.devicesList[i].name || settingsRoot.devicesList[i].id)
                                            }
                                            return names.length > 0 ? names : [EngineController.currentOutputDevice || qsTr("Default System Output")]
                                        }
                                        currentIndex: {
                                            var cur = EngineController.currentOutputDevice || ""
                                            for (var i = 0; i < settingsRoot.devicesList.length; ++i) {
                                                var d = settingsRoot.devicesList[i]
                                                if ((d.name && d.name === cur) || (d.id && d.id === cur) || (d.description && d.description === cur))
                                                    return i
                                            }
                                            return 0
                                        }
                                        onActivated: function(idx) {
                                            if (idx >= 0 && idx < settingsRoot.devicesList.length) {
                                                var d = settingsRoot.devicesList[idx]
                                                EngineController.selectOutputDevice(d.name || d.id || "")
                                                settingsRoot.refresh()
                                            }
                                        }
                                    }
                                    ConsoleButton {
                                        text: qsTr("RESCAN")
                                        implicitWidth: 74; implicitHeight: 32; fontSize: 10
                                        onClicked: settingsRoot.refresh()
                                    }
                                }

                                Label { text: qsTr("Output PCM Quantization"); font.pixelSize: 12; color: "#BCC8D4" }
                                ConsoleComboBox {
                                    Layout.fillWidth: true
                                    implicitHeight: 32
                                    model: ["F32LE", "S32LE", "S24_32LE", "S16LE"]
                                    currentIndex: Math.max(0, model.indexOf(EngineController.outputPcmFormat))
                                    onActivated: EngineController.setOutputPcmFormat(currentText)
                                }

                                Label { text: qsTr("Buffer Quantum (Frames)"); font.pixelSize: 12; color: "#BCC8D4" }
                                ConsoleComboBox {
                                    Layout.fillWidth: true
                                    implicitHeight: 32
                                    model: ["64", "128", "256", "512", "1024", "2048"]
                                    currentIndex: Math.max(0, model.indexOf(String(SettingsManager.audioBufferFrames)))
                                    onActivated: EngineController.setOutputBufferFrames(parseInt(currentText, 10) || 256)
                                }

                                Label { text: qsTr("Fixed Graph Sample Rate"); font.pixelSize: 12; color: "#BCC8D4" }
                                ConsoleComboBox {
                                    Layout.fillWidth: true
                                    implicitHeight: 32
                                    enabled: !EngineController.directModeEnabled
                                    model: ["44100 Hz", "48000 Hz", "88200 Hz", "96000 Hz", "176400 Hz", "192000 Hz"]
                                    currentIndex: {
                                        var sr = Math.round(EngineController.graphSampleRate)
                                        var idx = model.indexOf(sr + " Hz")
                                        return idx >= 0 ? idx : 1
                                    }
                                    onActivated: {
                                        var v = parseFloat(currentText)
                                        if (isFinite(v) && v > 0) EngineController.setFixedGraphSampleRate(v)
                                    }
                                }
                            }
                        }

                        DarkSectionCard {
                            Layout.fillWidth: true
                            implicitHeight: 90
                            RowLayout {
                                anchors.fill: parent; anchors.margins: 14; spacing: 14
                                ConsoleButton {
                                    Layout.fillWidth: true; implicitHeight: 40
                                    text: SettingsManager.directModeDefault ? qsTr("DIRECT MODE DEFAULT: ON") : qsTr("DIRECT MODE DEFAULT: OFF")
                                    fontSize: 10; showLed: true
                                    ledColor: SettingsManager.directModeDefault ? "#FF7043" : "#444E5C"
                                    accentColor: "#FF7043"; highlighted: SettingsManager.directModeDefault
                                    onClicked: {
                                        SettingsManager.directModeDefault = !SettingsManager.directModeDefault
                                        SettingsManager.save()
                                    }
                                }
                                ConsoleButton {
                                    Layout.fillWidth: true; implicitHeight: 40
                                    text: EngineController.liveDitherEnabled ? qsTr("LIVE TPDF DITHER: ON") : qsTr("LIVE TPDF DITHER: OFF")
                                    fontSize: 10; showLed: true
                                    ledColor: EngineController.liveDitherEnabled ? "#00E5FF" : "#444E5C"
                                    accentColor: "#00E5FF"; highlighted: EngineController.liveDitherEnabled
                                    enabled: !EngineController.directModeEnabled
                                    onClicked: EngineController.setLiveDitherEnabled(!EngineController.liveDitherEnabled)
                                }
                            }
                        }

                        DarkSectionCard {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            ColumnLayout {
                                anchors.fill: parent; anchors.margins: 14; spacing: 8
                                Label { text: qsTr("BACKEND CAPABILITY AVAILABILITY (%1)").arg(settingsRoot.audioCaps.backend || "PipeWire"); font.family: "Monospace"; font.pixelSize: 11; font.bold: true; color: "#78D8F0" }
                                GridLayout {
                                    Layout.fillWidth: true; columns: 3; columnSpacing: 10; rowSpacing: 8
                                    Repeater {
                                        model: [
                                            { k: "directMode", l: qsTr("Direct Mode") },
                                            { k: "sampleRateFollowing", l: qsTr("Sample-Rate Follow") },
                                            { k: "exclusiveAccess", l: qsTr("Exclusive Access") },
                                            { k: "hardwareBitPerfect", l: qsTr("Hardware Bit-Perfect") },
                                            { k: "deviceSwitching", l: qsTr("Device Switching") },
                                            { k: "latencyControl", l: qsTr("Latency Quantum") },
                                            { k: "formatSelection", l: qsTr("Format Selection") },
                                            { k: "externalInsert", l: qsTr("External Insert") },
                                            { k: "offlineRender", l: qsTr("Offline Render") }
                                        ]
                                        Rectangle {
                                            Layout.fillWidth: true; height: 34; radius: 3
                                            color: "#0C1016"; border.color: "#222C38"
                                            readonly property string statusVal: settingsRoot.audioCaps[modelData.k] || "Unknown"
                                            RowLayout {
                                                anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10
                                                Label { Layout.fillWidth: true; text: modelData.l; font.pixelSize: 10; color: "#C8D4E0" }
                                                Label { text: statusVal.toUpperCase(); font.family: "Monospace"; font.pixelSize: 9; font.bold: true; color: settingsRoot.capColor(statusVal) }
                                            }
                                        }
                                    }
                                }
                                Item { Layout.fillHeight: true }
                            }
                        }
                    }

                    // ---------------------------------------------------------
                    // CATEGORY 1: PROCESSING & SAFETY DEFAULTS
                    // ---------------------------------------------------------
                    ColumnLayout {
                        spacing: 12
                        Label { text: qsTr("PROCESSING, EXTERNAL INSERT POLICY & SESSION DEFAULTS"); font.family: "Monospace"; font.pixelSize: 13; font.bold: true; color: "#E8BA6B" }

                        DarkSectionCard {
                            Layout.fillWidth: true
                            implicitHeight: 280
                            GridLayout {
                                anchors.fill: parent; anchors.margins: 16; columns: 2; columnSpacing: 18; rowSpacing: 14

                                Label { text: qsTr("External Insert Disconnect Policy"); font.pixelSize: 12; color: "#BCC8D4" }
                                ConsoleComboBox {
                                    Layout.fillWidth: true
                                    implicitHeight: 32
                                    model: [qsTr("Safe Auto-Bypass (Dry)"), qsTr("Mute Output on Disconnect"), qsTr("Stop Playback & Safe Mute")]
                                    currentIndex: Math.max(0, Math.min(2, EngineController.externalInsertDisconnectPolicy))
                                    onActivated: function(idx) { EngineController.setExternalInsertDisconnectPolicy(idx) }
                                }

                                Label { text: qsTr("Reverb Max Tail Retention (Offline Render)"); font.pixelSize: 12; color: "#BCC8D4" }
                                ConsoleComboBox {
                                    Layout.fillWidth: true
                                    implicitHeight: 32
                                    model: ["2.0 s", "5.0 s", "10.0 s", "15.0 s", "30.0 s"]
                                    currentIndex: {
                                        var t = SettingsManager.reverbMaxTailSeconds
                                        if (t <= 2.5) return 0
                                        if (t <= 6.0) return 1
                                        if (t <= 11.0) return 2
                                        if (t <= 16.0) return 3
                                        return 4
                                    }
                                    onActivated: {
                                        var sec = parseFloat(currentText)
                                        if (isFinite(sec)) {
                                            SettingsManager.reverbMaxTailSeconds = sec
                                            EngineController.setReverbLimits(sec, SettingsManager.reverbMaxProcessingMs)
                                        }
                                    }
                                }

                                Label { text: qsTr("Restore Last Session State on Startup"); font.pixelSize: 12; color: "#BCC8D4" }
                                ConsoleButton {
                                    Layout.fillWidth: true; implicitHeight: 34
                                    text: SettingsManager.restoreSessionOnStartup ? qsTr("RESTORE SESSION ON STARTUP: ON") : qsTr("RESTORE SESSION ON STARTUP: OFF")
                                    fontSize: 10; showLed: true
                                    ledColor: SettingsManager.restoreSessionOnStartup ? "#00E676" : "#444E5C"
                                    accentColor: "#00E676"; highlighted: SettingsManager.restoreSessionOnStartup
                                    onClicked: {
                                        SettingsManager.restoreSessionOnStartup = !SettingsManager.restoreSessionOnStartup
                                        SettingsManager.save()
                                    }
                                }

                                Label { text: qsTr("Sync Master DSP Chain to Offline Render Engine"); font.pixelSize: 12; color: "#BCC8D4" }
                                ConsoleButton {
                                    Layout.fillWidth: true; implicitHeight: 34
                                    text: EngineController.syncMasterDspToRender ? qsTr("SYNC MASTER DSP → RENDER: ON") : qsTr("SYNC MASTER DSP → RENDER: OFF")
                                    fontSize: 10; showLed: true
                                    ledColor: EngineController.syncMasterDspToRender ? "#00E5FF" : "#444E5C"
                                    accentColor: "#00E5FF"; highlighted: EngineController.syncMasterDspToRender
                                    onClicked: EngineController.setSyncMasterDspToRender(!EngineController.syncMasterDspToRender)
                                }

                                Label { text: qsTr("Active Listening Correction Preset"); font.pixelSize: 12; color: "#BCC8D4" }
                                ConsoleComboBox {
                                    Layout.fillWidth: true
                                    implicitHeight: 32
                                    model: EngineController.availableListeningPresets()
                                    currentIndex: Math.max(0, model.indexOf(EngineController.listeningPresetName))
                                    onActivated: EngineController.setListeningPreset(currentText)
                                }
                            }
                        }

                        Item { Layout.fillHeight: true }
                    }

                    // ---------------------------------------------------------
                    // CATEGORY 2: CACHE & STORAGE (WAVEFORM DISK CACHE + CONFIG)
                    // ---------------------------------------------------------
                    ColumnLayout {
                        spacing: 12
                        Label { text: qsTr("CACHE & STORAGE MANAGEMENT"); font.family: "Monospace"; font.pixelSize: 13; font.bold: true; color: "#E8BA6B" }

                        DarkSectionCard {
                            Layout.fillWidth: true
                            implicitHeight: 168
                            ColumnLayout {
                                anchors.fill: parent; anchors.margins: 16; spacing: 10
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label { text: qsTr("WAVEFORM OVERVIEW PEAK CACHE (.wfm)"); font.family: "Monospace"; font.pixelSize: 11; font.bold: true; color: "#78D8F0" }
                                    Item { Layout.fillWidth: true }
                                    Label {
                                        text: qsTr("%1  (%2 cached track(s))").arg(settingsRoot.formatBytes(settingsRoot.cacheBytes)).arg(settingsRoot.cacheFileCount)
                                        font.family: "Monospace"; font.pixelSize: 12; font.bold: true; color: "#00E5FF"
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Directory: %1").arg(settingsRoot.cachePathText)
                                    font.family: "Monospace"; font.pixelSize: 10; color: "#9AA9B8"; elide: Text.ElideMiddle
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Extracted asynchronously off the audio thread using independent FFmpeg decoders and stored under $XDG_CACHE_HOME.")
                                    font.pixelSize: 11; color: "#7F93A6"; wrapMode: Text.WordWrap
                                }
                                RowLayout {
                                    Layout.fillWidth: true; spacing: 10
                                    Item { Layout.fillWidth: true }
                                    ConsoleButton {
                                        text: qsTr("REFRESH CACHE STATS")
                                        implicitWidth: 158; implicitHeight: 34; fontSize: 10
                                        onClicked: settingsRoot.refresh()
                                    }
                                    ConsoleButton {
                                        text: qsTr("CLEAR WAVEFORM CACHE")
                                        implicitWidth: 176; implicitHeight: 34; fontSize: 10
                                        accentColor: "#FF5252"
                                        onClicked: {
                                            var ok = EngineController.clearWaveformCache()
                                            settingsRoot.refresh()
                                            settingsRoot.feedbackBanner = ok ? qsTr("Cleared disk waveform cache.") : qsTr("Failed to remove one or more cache files.")
                                        }
                                    }
                                }
                            }
                        }

                        DarkSectionCard {
                            Layout.fillWidth: true
                            implicitHeight: 172
                            ColumnLayout {
                                anchors.fill: parent; anchors.margins: 16; spacing: 8
                                Label { text: qsTr("APPLICATION CONFIGURATION & AUDIT DATABASE PATHS"); font.family: "Monospace"; font.pixelSize: 11; font.bold: true; color: "#78D8F0" }
                                Label {
                                    Layout.fillWidth: true; Layout.minimumWidth: 0
                                    text: qsTr("Settings TOML/JSON: %1").arg(SettingsManager.configFilePath)
                                    font.family: "Monospace"; font.pixelSize: 10; color: "#DCE6F0"; elide: Text.ElideMiddle
                                }
                                Label {
                                    Layout.fillWidth: true; Layout.minimumWidth: 0
                                    text: qsTr("SQLite Audit DB:    %1").arg(EngineController.logDatabasePath())
                                    font.family: "Monospace"; font.pixelSize: 10; color: "#DCE6F0"; elide: Text.ElideMiddle
                                }
                                Item { Layout.fillHeight: true }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Item { Layout.fillWidth: true }
                                    ConsoleButton {
                                        text: settingsRoot.resetDefaultsArmed ? qsTr("CONFIRM RESET TO DEFAULTS") : qsTr("RESET SETTINGS TO DEFAULTS")
                                        implicitWidth: 210; implicitHeight: 34; fontSize: 10
                                        accentColor: "#FF5252"
                                        anodizedFill: settingsRoot.resetDefaultsArmed
                                        anodizedTop: "#B33339"; anodizedBottom: "#61171B"; anodizedText: "#FFE8EA"
                                        onClicked: {
                                            if (!settingsRoot.resetDefaultsArmed) {
                                                settingsRoot.resetDefaultsArmed = true
                                                return
                                            }
                                            SettingsManager.resetToDefaults(true)
                                            settingsRoot.resetDefaultsArmed = false
                                            settingsRoot.refresh()
                                            settingsRoot.feedbackBanner = qsTr("Settings reset to factory console defaults.")
                                        }
                                    }
                                }
                            }
                        }

                        Item { Layout.fillHeight: true }
                    }

                    // ---------------------------------------------------------
                    // CATEGORY 3: INTERFACE & LOCALIZATION
                    // ---------------------------------------------------------
                    ColumnLayout {
                        spacing: 12
                        Label { text: qsTr("INTERFACE SCALING & BILINGUAL LOCALIZATION"); font.family: "Monospace"; font.pixelSize: 13; font.bold: true; color: "#E8BA6B" }

                        DarkSectionCard {
                            Layout.fillWidth: true
                            implicitHeight: 220
                            GridLayout {
                                anchors.fill: parent; anchors.margins: 16; columns: 2; columnSpacing: 18; rowSpacing: 14

                                Label { text: qsTr("UI Language"); font.pixelSize: 12; color: "#BCC8D4" }
                                RowLayout {
                                    Layout.fillWidth: true; spacing: 8
                                    ConsoleButton {
                                        Layout.fillWidth: true; implicitHeight: 36
                                        text: qsTr("English (en)")
                                        fontSize: 11; accentColor: "#00E5FF"
                                        highlighted: LanguageManager.currentLanguage !== "ja"
                                        onClicked: LanguageManager.setLanguage("en")
                                    }
                                    ConsoleButton {
                                        Layout.fillWidth: true; implicitHeight: 36
                                        text: qsTr("日本語 (ja)")
                                        fontSize: 11; accentColor: "#00E5FF"
                                        highlighted: LanguageManager.currentLanguage === "ja"
                                        onClicked: LanguageManager.setLanguage("ja")
                                    }
                                }

                                Label { text: qsTr("Console UI Scale Factor"); font.pixelSize: 12; color: "#BCC8D4" }
                                RowLayout {
                                    Layout.fillWidth: true; spacing: 6
                                    Repeater {
                                        model: [0.80, 0.90, 1.00, 1.15, 1.25]
                                        ConsoleButton {
                                            Layout.fillWidth: true; implicitHeight: 36
                                            text: Math.round(modelData * 100) + "%"
                                            fontSize: 10; accentColor: "#E8BA6B"
                                            highlighted: Math.abs(settingsRoot.selectedUiScale - modelData) < 0.02
                                            onClicked: settingsRoot.uiScaleChangedRequested(modelData)
                                        }
                                    }
                                }

                                Label { text: qsTr("Fullscreen Mode on Startup (F11 Toggle)"); font.pixelSize: 12; color: "#BCC8D4" }
                                ConsoleButton {
                                    Layout.fillWidth: true; implicitHeight: 36
                                    text: SettingsManager.fullscreenOnStartup ? qsTr("FULLSCREEN ON STARTUP: ON") : qsTr("FULLSCREEN ON STARTUP: OFF")
                                    fontSize: 10; showLed: true
                                    ledColor: SettingsManager.fullscreenOnStartup ? "#00E5FF" : "#444E5C"
                                    accentColor: "#00E5FF"; highlighted: SettingsManager.fullscreenOnStartup
                                    onClicked: {
                                        SettingsManager.fullscreenOnStartup = !SettingsManager.fullscreenOnStartup
                                        SettingsManager.save()
                                    }
                                }
                            }
                        }

                        Item { Layout.fillHeight: true }
                    }

                    // ---------------------------------------------------------
                    // CATEGORY 4: GRAPHICS, RHI & RUNTIME TELEMETRY
                    // ---------------------------------------------------------
                    ColumnLayout {
                        spacing: 12
                        Label { text: qsTr("GRAPHICS RHI PIPELINE & RUNTIME DIAGNOSTICS"); font.family: "Monospace"; font.pixelSize: 13; font.bold: true; color: "#E8BA6B" }

                        DarkSectionCard {
                            Layout.fillWidth: true
                            implicitHeight: 280
                            GridLayout {
                                anchors.fill: parent; anchors.margins: 16; columns: 2; columnSpacing: 18; rowSpacing: 10

                                Label { text: qsTr("Qt SceneGraph RHI Backend:"); font.family: "Monospace"; font.pixelSize: 11; color: "#8E9FAD" }
                                Label { text: EngineController.graphicsBackend || "-"; font.family: "Monospace"; font.pixelSize: 12; font.bold: true; color: "#00E676" }

                                Label { text: qsTr("Detected GPU Renderer:"); font.family: "Monospace"; font.pixelSize: 11; color: "#8E9FAD" }
                                Label { text: EngineController.gpuRendererName || "-"; font.family: "Monospace"; font.pixelSize: 11; color: "#DCE6F0"; Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight }

                                Label { text: qsTr("Audio Driver & Format:"); font.family: "Monospace"; font.pixelSize: 11; color: "#8E9FAD" }
                                Label {
                                    text: qsTr("%1  •  %2 Hz  •  %3").arg(EngineController.audioBackend).arg(Math.round(EngineController.graphSampleRate)).arg(EngineController.outputPcmFormat)
                                    font.family: "Monospace"; font.pixelSize: 11; color: "#33E3FF"
                                }

                                Label { text: qsTr("Direct Mode Status:"); font.family: "Monospace"; font.pixelSize: 11; color: "#8E9FAD" }
                                Label { text: EngineController.directModeEnabled ? qsTr("Enabled (Hardware/Graph Bypass)") : qsTr("Off (Studio DSP Path)"); font.family: "Monospace"; font.pixelSize: 11; color: "#DCE6F0" }

                                Label { text: qsTr("Bit-Perfect Verification:"); font.family: "Monospace"; font.pixelSize: 11; color: "#8E9FAD" }
                                Label { text: EngineController.bitPerfectModeStateText; font.family: "Monospace"; font.pixelSize: 11; font.bold: true; color: "#F5B841" }

                                Label { text: qsTr("XRUN Counter:"); font.family: "Monospace"; font.pixelSize: 11; color: "#8E9FAD" }
                                Label { text: String(EngineController.xrunCount); font.family: "Monospace"; font.pixelSize: 11; font.bold: true; color: EngineController.xrunCount > 0 ? "#FF5252" : "#00E676" }

                                Label { text: qsTr("Active DSP Tail Limit:"); font.family: "Monospace"; font.pixelSize: 11; color: "#8E9FAD" }
                                Label { text: SettingsManager.reverbMaxTailSeconds.toFixed(1) + " s / " + SettingsManager.reverbMaxProcessingMs.toFixed(1) + " ms cap"; font.family: "Monospace"; font.pixelSize: 11; color: "#DCE6F0" }
                            }
                        }

                        Item { Layout.fillHeight: true }
                    }
                }
            }
        }
        }
    }
}
