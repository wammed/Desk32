import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Desk32.Core

Rectangle {
    id: outputDeviceRoot

    implicitWidth: 480
    implicitHeight: 780
    color: "#121519"
    border.color: "#262b32"
    border.width: 1
    radius: 6
    clip: true

    signal closeRequested()

    property var devicesList: []
    property var audioCaps: ({})
    property string statusNotice: ""
    property bool statusIsError: false
    property bool syncingControls: false

    readonly property string activeBackend: EngineController.audioBackend || "PipeWire"
    readonly property string activeTargetId: (audioCaps && audioCaps.targetDeviceId !== undefined)
                                             ? String(audioCaps.targetDeviceId)
                                             : ""
    readonly property bool isDefaultTargetSelected: activeTargetId === "" || activeTargetId === "default"
    readonly property var formatOptions: ["F32LE", "S24_32LE", "S32LE", "S16LE"]
    readonly property var bufferOptions: [64, 128, 256, 512, 1024, 2048]
    readonly property var sampleRateOptions: [44100, 48000, 88200, 96000, 176400, 192000]

    function syncStreamControls() {
        syncingControls = true

        var curFmt = EngineController.outputPcmFormat || "F32LE"
        var fmtIdx = formatOptions.indexOf(curFmt)
        pcmFormatCombo.currentIndex = fmtIdx >= 0 ? fmtIdx : 0

        var q = (audioCaps && audioCaps.negotiatedQuantum > 0) ? audioCaps.negotiatedQuantum : 256
        var bufIdx = bufferOptions.indexOf(q)
        bufferFramesCombo.currentIndex = bufIdx >= 0 ? bufIdx : 2

        var sr = Math.round(EngineController.graphSampleRate)
        var srIdx = sampleRateOptions.indexOf(sr)
        sampleRateCombo.currentIndex = srIdx >= 0 ? srIdx : 1

        liveDitherCheck.checked = EngineController.liveDitherEnabled
        syncingControls = false
    }

    function refreshCapabilitiesOnly() {
        audioCaps = EngineController.getAudioCapabilities()
        syncStreamControls()
    }

    function refreshDevices() {
        devicesList = EngineController.enumerateOutputDevices()
        refreshCapabilitiesOnly()
    }

    function isDeviceSelected(dev) {
        if (!dev || isDefaultTargetSelected) {
            return false
        }
        return String(dev.name) === activeTargetId || String(dev.nodeId) === activeTargetId
    }

    function activeDeviceDisplayName() {
        if (isDefaultTargetSelected) {
            return activeBackend === "JACK"
                   ? qsTr("System Default (JACK Physical Auto)")
                   : qsTr("System Default (PipeWire Auto)")
        }
        for (var i = 0; i < devicesList.length; ++i) {
            var d = devicesList[i]
            if (String(d.name) === activeTargetId || String(d.nodeId) === activeTargetId) {
                return (d.description && d.description.length > 0) ? d.description : d.name
            }
        }
        return activeTargetId
    }

    function switchBackend(backendName) {
        var ok = EngineController.setAudioBackend(backendName)
        refreshDevices()
        if (ok) {
            statusIsError = false
            statusNotice = qsTr("Switched audio backend to %1.").arg(backendName)
        } else {
            statusIsError = true
            statusNotice = qsTr("Failed to switch audio backend to %1 (check Log Tab / server status).").arg(backendName)
        }
    }

    function selectDevice(idOrName, labelText) {
        var ok = EngineController.selectOutputDevice(idOrName)
        refreshDevices()
        if (ok) {
            statusIsError = false
            statusNotice = qsTr("Switched output device to: %1").arg(labelText)
        } else {
            statusIsError = true
            statusNotice = qsTr("Failed to switch output device to: %1 (check Log Tab for details)").arg(labelText)
        }
    }

    function capColor(stateText) {
        if (stateText === "Supported") return "#00E676"
        if (stateText === "Unavailable") return "#FFB300"
        if (stateText === "Unsupported") return "#546E7A"
        return "#78909C"
    }

    function streamStateColor() {
        var st = EngineController.backendStatusText
        if (st === "Streaming") return "#00E676"
        if (st === "Error") return "#FF5252"
        if (st === "Connecting" || st === "Paused") return "#FFB300"
        return "#78909C"
    }

    Component.onCompleted: refreshDevices()

    onVisibleChanged: {
        if (visible) {
            refreshDevices()
        }
    }

    Connections {
        target: EngineController
        function onDiagnosticsChanged() {
            outputDeviceRoot.refreshCapabilitiesOnly()
        }
        function onGraphSampleRateChanged() {
            outputDeviceRoot.refreshCapabilitiesOnly()
        }
        function onAudioBackendChanged() {
            outputDeviceRoot.refreshDevices()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10

        // Panel Header
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Rectangle {
                width: 8
                height: 8
                radius: 4
                color: outputDeviceRoot.streamStateColor()
            }

            Label {
                text: qsTr("AUDIO OUTPUT DEVICES")
                font.bold: true
                font.pixelSize: 12
                font.family: "Monospace"
                color: "#00E5FF"
            }

            Rectangle {
                implicitWidth: backendBadgeLbl.implicitWidth + 10
                height: 18
                radius: 3
                color: "#1a2028"
                border.color: "#2b3440"
                Label {
                    id: backendBadgeLbl
                    anchors.centerIn: parent
                    text: (outputDeviceRoot.audioCaps.backend || "PipeWire").toUpperCase()
                    font.pixelSize: 9
                    font.bold: true
                    font.family: "Monospace"
                    color: "#B0BEC5"
                }
            }

            Item { Layout.fillWidth: true }

            Button {
                text: qsTr("Scan")
                font.pixelSize: 10
                font.bold: true
                Layout.preferredHeight: 24
                onClicked: {
                    outputDeviceRoot.refreshDevices()
                    outputDeviceRoot.statusIsError = false
                    outputDeviceRoot.statusNotice = qsTr("Enumerated %1 output device(s).").arg(outputDeviceRoot.devicesList.length)
                }
            }

            Button {
                text: "✕"
                font.pixelSize: 11
                font.bold: true
                Layout.preferredWidth: 26
                Layout.preferredHeight: 24
                onClicked: outputDeviceRoot.closeRequested()
            }
        }

        // Status Feedback Banner
        Rectangle {
            Layout.fillWidth: true
            visible: outputDeviceRoot.statusNotice.length > 0
            implicitHeight: statusNoticeLbl.implicitHeight + 12
            radius: 4
            color: outputDeviceRoot.statusIsError ? "#FF525218" : "#00E67618"
            border.color: outputDeviceRoot.statusIsError ? "#FF5252" : "#00E676"

            RowLayout {
                anchors.fill: parent
                anchors.margins: 6
                spacing: 6

                Label {
                    id: statusNoticeLbl
                    Layout.fillWidth: true
                    text: outputDeviceRoot.statusNotice
                    font.pixelSize: 10
                    color: outputDeviceRoot.statusIsError ? "#FF8A80" : "#B9F6CA"
                    wrapMode: Text.WordWrap
                }

                Button {
                    text: "✕"
                    flat: true
                    font.pixelSize: 9
                    Layout.preferredWidth: 20
                    Layout.preferredHeight: 20
                    onClicked: outputDeviceRoot.statusNotice = ""
                }
            }
        }

        // Audio Backend Switcher (PipeWire / PipeWire-JACK)
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: backendRow.implicitHeight + 12
            color: "#0d1014"
            border.color: "#222934"
            radius: 4

            RowLayout {
                id: backendRow
                anchors.fill: parent
                anchors.margins: 6
                spacing: 8

                Label {
                    text: qsTr("Audio Backend:")
                    font.bold: true
                    font.pixelSize: 10
                    color: "#A0AAB2"
                }

                Repeater {
                    model: EngineController.availableAudioBackends()
                    Button {
                        text: modelData === "JACK" ? qsTr("JACK (PipeWire)") : modelData
                        font.pixelSize: 10
                        font.bold: true
                        Layout.preferredHeight: 24
                        highlighted: outputDeviceRoot.activeBackend === modelData
                        onClicked: outputDeviceRoot.switchBackend(modelData)
                    }
                }

                Item { Layout.fillWidth: true }
            }
        }

        // Active Stream Summary Card
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: activeSummaryCol.implicitHeight + 16
            color: "#0d1014"
            border.color: "#222934"
            radius: 4

            ColumnLayout {
                id: activeSummaryCol
                anchors.fill: parent
                anchors.margins: 8
                spacing: 4

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: qsTr("ACTIVE OUTPUT SUMMARY")
                        font.bold: true
                        font.pixelSize: 10
                        font.family: "Monospace"
                        color: "#00E5FF"
                    }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: EngineController.backendStatusText.toUpperCase()
                        font.bold: true
                        font.pixelSize: 10
                        font.family: "Monospace"
                        color: outputDeviceRoot.streamStateColor()
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: outputDeviceRoot.activeDeviceDisplayName()
                    font.bold: true
                    font.pixelSize: 12
                    color: "#ECEFF1"
                    elide: Text.ElideMiddle
                }

                Label {
                    Layout.fillWidth: true
                    property int qVal: (outputDeviceRoot.audioCaps && outputDeviceRoot.audioCaps.negotiatedQuantum > 0)
                                       ? outputDeviceRoot.audioCaps.negotiatedQuantum : 256
                    property double srVal: EngineController.graphSampleRate > 0 ? EngineController.graphSampleRate : 48000.0
                    property double latMs: (qVal * 1000.0) / srVal
                    text: Math.round(srVal) + " Hz  •  " + EngineController.outputPcmFormat + "  •  "
                          + qVal + " smp (" + latMs.toFixed(2) + " ms)  •  XRUN: " + EngineController.xrunCount
                    font.pixelSize: 10
                    font.family: "Monospace"
                    color: "#90A4AE"
                    elide: Text.ElideRight
                }
            }
        }

        // Device List Section Header
        RowLayout {
            Layout.fillWidth: true
            Label {
                text: qsTr("AVAILABLE OUTPUT SINKS (%1)").arg(outputDeviceRoot.devicesList.length)
                font.bold: true
                font.pixelSize: 10
                font.family: "Monospace"
                color: "#A0AAB2"
            }
            Item { Layout.fillWidth: true }
            Label {
                text: qsTr("Click a device to route audio")
                font.pixelSize: 10
                color: "#6c7885"
            }
        }

        // Scrollable Output Device List
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth

            ColumnLayout {
                width: outputDeviceRoot.width - 28
                spacing: 6

                // Pinned Option: System Default (PipeWire Auto)
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: defaultCardCol.implicitHeight + 16
                    radius: 4
                    color: outputDeviceRoot.isDefaultTargetSelected
                           ? "#00E67614"
                           : (defaultMouse.containsMouse ? "#171c22" : "#0f1217")
                    border.color: outputDeviceRoot.isDefaultTargetSelected
                                  ? "#00E676"
                                  : (defaultMouse.containsMouse ? "#324050" : "#222934")
                    border.width: outputDeviceRoot.isDefaultTargetSelected ? 1.5 : 1

                    MouseArea {
                        id: defaultMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: outputDeviceRoot.selectDevice("default", qsTr("System Default (PipeWire Auto)"))
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 8

                        Rectangle {
                            width: 12
                            height: 12
                            radius: 6
                            color: "transparent"
                            border.color: outputDeviceRoot.isDefaultTargetSelected ? "#00E676" : "#546E7A"
                            border.width: 2
                            Rectangle {
                                anchors.centerIn: parent
                                width: 6
                                height: 6
                                radius: 3
                                visible: outputDeviceRoot.isDefaultTargetSelected
                                color: "#00E676"
                            }
                        }

                        ColumnLayout {
                            id: defaultCardCol
                            Layout.fillWidth: true
                            spacing: 2

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6
                                Label {
                                    text: outputDeviceRoot.activeBackend === "JACK"
                                          ? qsTr("System Default (JACK Physical Auto)")
                                          : qsTr("System Default (PipeWire Auto)")
                                    font.bold: true
                                    font.pixelSize: 11
                                    color: "#E0E6ED"
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }
                                Rectangle {
                                    visible: outputDeviceRoot.isDefaultTargetSelected
                                    implicitWidth: defActiveLbl.implicitWidth + 8
                                    height: 16
                                    radius: 3
                                    color: "#00E67622"
                                    border.color: "#00E676"
                                    Label {
                                        id: defActiveLbl
                                        anchors.centerIn: parent
                                        text: qsTr("ACTIVE")
                                        font.pixelSize: 8
                                        font.bold: true
                                        font.family: "Monospace"
                                        color: "#00E676"
                                    }
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                text: outputDeviceRoot.activeBackend === "JACK"
                                      ? qsTr("Connect to default physical JACK playback ports")
                                      : qsTr("Follow PipeWire / WirePlumber default sink routing")
                                font.pixelSize: 10
                                color: "#808c99"
                                elide: Text.ElideRight
                            }
                        }
                    }
                }

                // Enumerated PipeWire / Simulation Sink Cards
                Repeater {
                    model: outputDeviceRoot.devicesList

                    Rectangle {
                        id: devCard
                        Layout.fillWidth: true
                        implicitHeight: devCardCol.implicitHeight + 16
                        radius: 4

                        readonly property bool selected: outputDeviceRoot.isDeviceSelected(modelData)
                        readonly property string displayTitle: (modelData.description && modelData.description.length > 0)
                                                               ? modelData.description
                                                               : modelData.name

                        color: selected
                               ? "#00E67614"
                               : (devMouse.containsMouse ? "#171c22" : "#0f1217")
                        border.color: selected
                                      ? "#00E676"
                                      : (devMouse.containsMouse ? "#324050" : "#222934")
                        border.width: selected ? 1.5 : 1

                        MouseArea {
                            id: devMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: outputDeviceRoot.selectDevice(modelData.name, devCard.displayTitle)
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 8

                            Rectangle {
                                width: 12
                                height: 12
                                radius: 6
                                color: "transparent"
                                border.color: devCard.selected ? "#00E676" : "#546E7A"
                                border.width: 2
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 6
                                    height: 6
                                    radius: 3
                                    visible: devCard.selected
                                    color: "#00E676"
                                }
                            }

                            ColumnLayout {
                                id: devCardCol
                                Layout.fillWidth: true
                                spacing: 3

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6

                                    Label {
                                        text: devCard.displayTitle
                                        font.bold: true
                                        font.pixelSize: 11
                                        color: "#E0E6ED"
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                    }

                                    Rectangle {
                                        visible: !!modelData.isDefault && !devCard.selected
                                        implicitWidth: defTagLbl.implicitWidth + 8
                                        height: 16
                                        radius: 3
                                        color: "#00E5FF18"
                                        border.color: "#00E5FF"
                                        Label {
                                            id: defTagLbl
                                            anchors.centerIn: parent
                                            text: qsTr("DEFAULT")
                                            font.pixelSize: 8
                                            font.bold: true
                                            font.family: "Monospace"
                                            color: "#00E5FF"
                                        }
                                    }

                                    Rectangle {
                                        visible: devCard.selected
                                        implicitWidth: devActLbl.implicitWidth + 8
                                        height: 16
                                        radius: 3
                                        color: "#00E67622"
                                        border.color: "#00E676"
                                        Label {
                                            id: devActLbl
                                            anchors.centerIn: parent
                                            text: qsTr("ACTIVE")
                                            font.pixelSize: 8
                                            font.bold: true
                                            font.family: "Monospace"
                                            color: "#00E676"
                                        }
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: "#" + (modelData.nodeId !== undefined ? modelData.nodeId : 0) + "  •  " + (modelData.name || "")
                                    font.pixelSize: 9
                                    font.family: "Monospace"
                                    color: "#78909C"
                                    elide: Text.ElideMiddle
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6

                                    Label {
                                        text: modelData.mediaClass || "Audio/Sink"
                                        font.pixelSize: 9
                                        font.family: "Monospace"
                                        color: "#808c99"
                                    }
                                    Label {
                                        text: "•"
                                        font.pixelSize: 9
                                        color: "#455A64"
                                    }
                                    Label {
                                        text: (modelData.sampleRate && modelData.sampleRate > 0)
                                              ? (modelData.sampleRate + " Hz")
                                              : qsTr("Graph Rate")
                                        font.pixelSize: 9
                                        font.family: "Monospace"
                                        color: "#00E5FF"
                                    }
                                    Label {
                                        text: "•"
                                        font.pixelSize: 9
                                        color: "#455A64"
                                    }
                                    Label {
                                        text: (modelData.channels || 2) + " ch"
                                        font.pixelSize: 9
                                        font.family: "Monospace"
                                        color: "#90A4AE"
                                    }
                                    Label {
                                        text: "•"
                                        font.pixelSize: 9
                                        color: "#455A64"
                                    }
                                    Label {
                                        text: modelData.nativeFormat || "F32LE"
                                        font.pixelSize: 9
                                        font.family: "Monospace"
                                        color: "#B0BEC5"
                                    }
                                    Item { Layout.fillWidth: true }
                                }
                            }
                        }
                    }
                }

                // Empty State
                Rectangle {
                    Layout.fillWidth: true
                    visible: outputDeviceRoot.devicesList.length === 0
                    implicitHeight: emptyCol.implicitHeight + 20
                    radius: 4
                    color: "#0f1217"
                    border.color: "#222934"

                    ColumnLayout {
                        id: emptyCol
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 6

                        Label {
                            Layout.fillWidth: true
                            text: outputDeviceRoot.activeBackend === "JACK"
                                  ? qsTr("No JACK playback target ports detected.")
                                  : qsTr("No PipeWire Audio/Sink devices detected.")
                            font.bold: true
                            font.pixelSize: 11
                            color: "#FFB300"
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            Layout.fillWidth: true
                            text: outputDeviceRoot.activeBackend === "JACK"
                                  ? qsTr("Ensure PipeWire-JACK is active and target playback ports exist, then click Scan.")
                                  : qsTr("Ensure PipeWire / WirePlumber is running and at least one playback sink is available, then click Scan.")
                            font.pixelSize: 10
                            color: "#808c99"
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }
        }

        // Stream Format, Buffer & Sample Rate Controls
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: streamCfgCol.implicitHeight + 16
            color: "#0d1014"
            border.color: "#222934"
            radius: 4

            ColumnLayout {
                id: streamCfgCol
                anchors.fill: parent
                anchors.margins: 8
                spacing: 6

                Label {
                    text: qsTr("STREAM FORMAT & LATENCY")
                    font.bold: true
                    font.pixelSize: 10
                    font.family: "Monospace"
                    color: "#00E5FF"
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 8
                    rowSpacing: 6

                    Label { text: qsTr("PCM Format:"); font.pixelSize: 10; color: "#A0AAB2" }
                    ComboBox {
                        id: pcmFormatCombo
                        Layout.fillWidth: true
                        Layout.preferredHeight: 26
                        font.pixelSize: 10
                        model: outputDeviceRoot.formatOptions
                        enabled: outputDeviceRoot.audioCaps.formatSelection === "Supported"
                        onActivated: {
                            if (!outputDeviceRoot.syncingControls) {
                                EngineController.setOutputPcmFormat(currentText)
                                outputDeviceRoot.refreshCapabilitiesOnly()
                            }
                        }
                    }

                    Label { text: qsTr("Buffer (Quantum):"); font.pixelSize: 10; color: "#A0AAB2" }
                    ComboBox {
                        id: bufferFramesCombo
                        Layout.fillWidth: true
                        Layout.preferredHeight: 26
                        font.pixelSize: 10
                        model: outputDeviceRoot.bufferOptions
                        enabled: outputDeviceRoot.audioCaps.latencyControl === "Supported"
                        displayText: currentText + " smp"
                        onActivated: {
                            if (!outputDeviceRoot.syncingControls) {
                                var frames = outputDeviceRoot.bufferOptions[currentIndex]
                                EngineController.setOutputBufferFrames(frames)
                                outputDeviceRoot.refreshCapabilitiesOnly()
                            }
                        }
                    }

                    Label { text: qsTr("Fixed Sample Rate:"); font.pixelSize: 10; color: "#A0AAB2" }
                    ComboBox {
                        id: sampleRateCombo
                        Layout.fillWidth: true
                        Layout.preferredHeight: 26
                        font.pixelSize: 10
                        model: outputDeviceRoot.sampleRateOptions
                        enabled: !EngineController.directModeEnabled
                        displayText: EngineController.directModeEnabled
                                     ? (Math.round(EngineController.graphSampleRate) + " Hz (Direct)")
                                     : (currentText + " Hz")
                        onActivated: {
                            if (!outputDeviceRoot.syncingControls) {
                                var sr = outputDeviceRoot.sampleRateOptions[currentIndex]
                                EngineController.setFixedGraphSampleRate(sr)
                                outputDeviceRoot.refreshCapabilitiesOnly()
                            }
                        }
                    }
                }

                CheckBox {
                    id: liveDitherCheck
                    text: qsTr("Enable Live TPDF Dither (for 16/24-bit integer output)")
                    font.pixelSize: 10
                    enabled: !EngineController.directModeEnabled && outputDeviceRoot.audioCaps.formatSelection === "Supported"
                    onToggled: {
                        if (!outputDeviceRoot.syncingControls) {
                            EngineController.setLiveDitherEnabled(checked)
                            outputDeviceRoot.refreshCapabilitiesOnly()
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: outputDeviceRoot.audioCaps.formatSelection !== "Supported" &&
                             !!(outputDeviceRoot.audioCaps.reasons && outputDeviceRoot.audioCaps.reasons.formatSelection)
                    text: (outputDeviceRoot.audioCaps.reasons && outputDeviceRoot.audioCaps.reasons.formatSelection) || ""
                    font.pixelSize: 9
                    color: "#78909C"
                    wrapMode: Text.WordWrap
                }
            }
        }

        // Backend Capabilities (§8.2 4-State Model)
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: capsCol.implicitHeight + 14
            color: "#0d1014"
            border.color: "#222934"
            radius: 4

            ColumnLayout {
                id: capsCol
                anchors.fill: parent
                anchors.margins: 7
                spacing: 4

                Label {
                    text: qsTr("BACKEND CAPABILITIES (SECTION 8.2)")
                    font.bold: true
                    font.pixelSize: 9
                    font.family: "Monospace"
                    color: "#808c99"
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 4
                    columnSpacing: 6
                    rowSpacing: 3

                    Label { text: qsTr("Device Switch:"); font.pixelSize: 9; color: "#78909C" }
                    Label {
                        text: outputDeviceRoot.audioCaps.deviceSwitching || "Unknown"
                        font.pixelSize: 9; font.family: "Monospace"; font.bold: true
                        color: outputDeviceRoot.capColor(outputDeviceRoot.audioCaps.deviceSwitching)
                    }
                    Label { text: qsTr("Direct Mode:"); font.pixelSize: 9; color: "#78909C" }
                    Label {
                        text: outputDeviceRoot.audioCaps.directMode || "Unknown"
                        font.pixelSize: 9; font.family: "Monospace"; font.bold: true
                        color: outputDeviceRoot.capColor(outputDeviceRoot.audioCaps.directMode)
                    }

                    Label { text: qsTr("Rate Follow:"); font.pixelSize: 9; color: "#78909C" }
                    Label {
                        text: outputDeviceRoot.audioCaps.sampleRateFollowing || "Unknown"
                        font.pixelSize: 9; font.family: "Monospace"; font.bold: true
                        color: outputDeviceRoot.capColor(outputDeviceRoot.audioCaps.sampleRateFollowing)
                    }
                    Label { text: qsTr("Exclusive:"); font.pixelSize: 9; color: "#78909C" }
                    Label {
                        text: outputDeviceRoot.audioCaps.exclusiveAccess || "Unknown"
                        font.pixelSize: 9; font.family: "Monospace"; font.bold: true
                        color: outputDeviceRoot.capColor(outputDeviceRoot.audioCaps.exclusiveAccess)
                    }

                    Label { text: qsTr("PCM Format:"); font.pixelSize: 9; color: "#78909C" }
                    Label {
                        text: outputDeviceRoot.audioCaps.formatSelection || "Unknown"
                        font.pixelSize: 9; font.family: "Monospace"; font.bold: true
                        color: outputDeviceRoot.capColor(outputDeviceRoot.audioCaps.formatSelection)
                    }
                    Label { text: qsTr("Latency Ctrl:"); font.pixelSize: 9; color: "#78909C" }
                    Label {
                        text: outputDeviceRoot.audioCaps.latencyControl || "Unknown"
                        font.pixelSize: 9; font.family: "Monospace"; font.bold: true
                        color: outputDeviceRoot.capColor(outputDeviceRoot.audioCaps.latencyControl)
                    }
                }

                Label {
                    Layout.fillWidth: true
                    readonly property var r: outputDeviceRoot.audioCaps.reasons || ({})
                    readonly property string noteText: {
                        var parts = [];
                        if (outputDeviceRoot.audioCaps.sampleRateFollowing !== "Supported" && r.sampleRateFollowing)
                            parts.push("Rate Follow: " + r.sampleRateFollowing);
                        else if (outputDeviceRoot.audioCaps.directMode !== "Supported" && r.directMode)
                            parts.push("Direct Mode: " + r.directMode);
                        if (outputDeviceRoot.audioCaps.latencyControl !== "Supported" && r.latencyControl)
                            parts.push("Latency: " + r.latencyControl);
                        return parts.join(" | ");
                    }
                    visible: noteText.length > 0
                    text: noteText
                    font.pixelSize: 9
                    color: "#90A4AE"
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
