import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Desk32.Core
import "../components"

Item {
    id: consoleRoot

    // Navigation / Modal Triggers
    signal deckLoadFileRequested(string deckId)
    signal deckStartFolderRequested(string deckId)
    signal renderPopupRequested()
    signal bitPerfectReportRequested()

    // Non-Destructive UI States (Startup default: 32-GEQ tab = 0; supports --op-tab=N for verification)
    property int currentOpTab: {
        var args = Qt.application.arguments
        for (var i = 0; i < args.length; ++i) {
            if (args[i].indexOf("--op-tab=") === 0) {
                var parsed = parseInt(args[i].substring(9), 10)
                if (!isNaN(parsed) && parsed >= 0 && parsed <= 3) return parsed
            }
        }
        return 0
    }
    property int monitorViewMode: 0        // 0: AUTO, 1: WAVE+SPEC, 2: WAVE ONLY, 3: SPEC+EQ
    property int monitorChannelMode: 0     // 0: L+R, 1: L, 2: R, 3: AUTO
    property bool linkMasterLR: true

    // 32-PEQ Tab State
    property bool peqEditListening: false  // Q-08: Master PEQ vs Listening Correction PEQ switcher
    property bool peqRightChannel: false
    property int selectedPeqBand: 0
    property int peqBandTypeIndex: 0
    property bool peqBandEnabledState: true
    property int activeDeckPollTick: 0
    property int peqGridTick: 0

    // DSP Rack State
    property bool dspFilterBypassed: false
    property bool dspHpfEnabled: false
    property bool dspLpfEnabled: false
    property int dspHpfSlope: 0
    property int dspLpfSlope: 0
    property bool dspCompBypassed: false
    property double dspCompGainReductionDb: 0.0
    property bool dspRevBypassed: false
    property bool dspRevTailPreserving: true
    property bool dspLimBypassed: false
    property double dspLimGainReductionDb: 0.0

    // External Host State
    property var portDiscoveryList: []
    property int selectedTargetClientIdx: 0

    readonly property bool isDirect: EngineController.directModeEnabled
    readonly property bool showWaveform: monitorViewMode === 0 || monitorViewMode === 1 || monitorViewMode === 2
    readonly property bool showSpectrum: monitorViewMode === 0 || monitorViewMode === 1 || monitorViewMode === 3
    readonly property bool showEqOverlay: showSpectrum && (monitorViewMode === 3 || (monitorViewMode === 0 && (currentOpTab === 0 || currentOpTab === 1)))

    function gainToDb(lin) {
        if (lin <= 0.001) return -60.0
        var db = 20.0 * Math.log10(lin)
        return Math.max(-60.0, Math.min(6.0, db))
    }

    function dbToGain(db) {
        if (db <= -59.9) return 0.0
        return Math.max(0.0, Math.min(2.0, Math.pow(10.0, db / 20.0)))
    }

    function bitPerfectStateColor(state) {
        if (state === 1) return "#00E676"
        if (state === 2) return "#00E5FF"
        if (state === 3) return "#F5B841"
        if (state === 4) return "#FF5252"
        return "#8A98A8"
    }

    function geqBandLabel(idx) {
        var labels = [
            "20", "25", "31.5", "40", "50", "63", "80", "100",
            "125", "160", "200", "250", "315", "400", "500", "630",
            "800", "1k", "1.25k", "1.6k", "2k", "2.5k", "3.15k", "4k",
            "5k", "6.3k", "8k", "10k", "12.5k", "16k", "18k", "20k"
        ]
        return (idx >= 0 && idx < labels.length) ? labels[idx] : String(idx + 1)
    }

    function filterTypeName(t) {
        switch (t) {
        case 0: return qsTr("BELL")
        case 1: return qsTr("LO-SHELF")
        case 2: return qsTr("HI-SHELF")
        case 3: return qsTr("NOTCH")
        case 4: return qsTr("LPF")
        case 5: return qsTr("HPF")
        default: return qsTr("BELL")
        }
    }

    function formatHz(f) {
        if (f >= 1000.0) return (f / 1000.0).toFixed(2) + " kHz"
        return f.toFixed(1) + " Hz"
    }

    function refreshEqCurve() {
        monitorCanvas.requestPaint()
        peqMiniCanvas.requestPaint()
    }

    function refreshPeqInspector() {
        var b = consoleRoot.peqEditListening
                ? EngineController.getListeningBand(consoleRoot.peqRightChannel, consoleRoot.selectedPeqBand)
                : EngineController.getPeqBand(consoleRoot.peqRightChannel, consoleRoot.selectedPeqBand)
        if (b && b.frequency !== undefined) {
            peqFreqKnob.value = b.frequency
            peqGainKnob.value = b.gainDb
            peqQKnob.value = b.q
            peqBandTypeIndex = b.type !== undefined ? b.type : 0
            peqBandEnabledState = !!b.enabled
        }
        peqInTrimKnob.value = EngineController.getDspTrim(consoleRoot.peqEditListening ? "listening_peq" : "peq", false)
        peqOutTrimKnob.value = EngineController.getDspTrim(consoleRoot.peqEditListening ? "listening_peq" : "peq", true)
        peqGridTick++
        peqMiniCanvas.requestPaint()
    }

    function refreshDspRack() {
        var fp = EngineController.getFilterParams()
        if (fp) {
            dspHpfKnob.value = fp.hpfFrequency !== undefined ? fp.hpfFrequency : 20.0
            dspLpfKnob.value = fp.lpfFrequency !== undefined ? fp.lpfFrequency : 20000.0
            dspHpfEnabled = !!fp.hpfEnabled
            dspLpfEnabled = !!fp.lpfEnabled
            dspHpfSlope = fp.hpfSlope !== undefined ? fp.hpfSlope : 0
            dspLpfSlope = fp.lpfSlope !== undefined ? fp.lpfSlope : 0
            dspFilterBypassed = !!fp.bypassed
        }
        var cp = EngineController.getCompressorParams()
        if (cp) {
            dspCompThreshKnob.value = cp.thresholdDb !== undefined ? cp.thresholdDb : -12.0
            dspCompRatioKnob.value = cp.ratio !== undefined ? cp.ratio : 2.0
            dspCompAttackKnob.value = cp.attackMs !== undefined ? cp.attackMs : 10.0
            dspCompReleaseKnob.value = cp.releaseMs !== undefined ? cp.releaseMs : 120.0
            dspCompMakeupKnob.value = cp.makeupGainDb !== undefined ? cp.makeupGainDb : 0.0
            dspCompKneeKnob.value = cp.kneeDb !== undefined ? cp.kneeDb : 2.0
            dspCompGainReductionDb = cp.gainReductionDb !== undefined ? cp.gainReductionDb : 0.0
            dspCompBypassed = !!cp.bypassed
        }
        var rp = EngineController.getReverbParams()
        if (rp) {
            dspRevRoomKnob.value = rp.roomSize !== undefined ? rp.roomSize : 0.35
            dspRevDampKnob.value = rp.damping !== undefined ? rp.damping : 0.5
            dspRevWetKnob.value = rp.wetLevel !== undefined ? rp.wetLevel : 0.15
            dspRevPreKnob.value = rp.predelayMs !== undefined ? rp.predelayMs : 12.0
            dspRevTailPreserving = rp.tailPreserving !== undefined ? !!rp.tailPreserving : true
            dspRevBypassed = !!rp.bypassed
        }
        var lp = EngineController.getLimiterParams()
        if (lp) {
            dspLimCeilKnob.value = lp.ceilingDb !== undefined ? lp.ceilingDb : -0.3
            dspLimRelKnob.value = lp.releaseMs !== undefined ? lp.releaseMs : 50.0
            dspLimLookKnob.value = lp.lookaheadMs !== undefined ? lp.lookaheadMs : 1.5
            dspLimGainReductionDb = lp.gainReductionDb !== undefined ? lp.gainReductionDb : 0.0
            dspLimBypassed = !!lp.bypassed
        }
        filterInTrimKnob.value = EngineController.getDspTrim("filter", false)
        filterOutTrimKnob.value = EngineController.getDspTrim("filter", true)
        compInTrimKnob.value = EngineController.getDspTrim("compressor", false)
        compOutTrimKnob.value = EngineController.getDspTrim("compressor", true)
        revInTrimKnob.value = EngineController.getDspTrim("reverb", false)
        revOutTrimKnob.value = EngineController.getDspTrim("reverb", true)
        limInTrimKnob.value = EngineController.getDspTrim("limiter", false)
        limOutTrimKnob.value = EngineController.getDspTrim("limiter", true)
        filterMiniCanvas.requestPaint()
        reverbMiniCanvas.requestPaint()
    }

    Component.onCompleted: {
        refreshPeqInspector()
        refreshDspRack()
        geqInTrimFader.value = EngineController.getDspTrim("geq", false)
        geqOutTrimFader.value = EngineController.getDspTrim("geq", true)
        refreshEqCurve()
    }

    onCurrentOpTabChanged: refreshEqCurve()
    onMonitorViewModeChanged: refreshEqCurve()
    onMonitorChannelModeChanged: refreshEqCurve()
    onPeqEditListeningChanged: {
        refreshPeqInspector()
        refreshEqCurve()
    }
    onPeqRightChannelChanged: {
        refreshPeqInspector()
        refreshEqCurve()
    }

    Connections {
        target: EngineController
        function onGeqParametersChanged() { consoleRoot.refreshEqCurve() }
        function onPeqParametersChanged() {
            consoleRoot.refreshPeqInspector()
            consoleRoot.refreshEqCurve()
        }
        function onListeningPresetChanged() {
            consoleRoot.refreshPeqInspector()
            consoleRoot.refreshEqCurve()
        }
        function onFilterParametersChanged() { consoleRoot.refreshDspRack() }
        function onCompressorParametersChanged() { consoleRoot.refreshDspRack() }
        function onReverbParametersChanged() { consoleRoot.refreshDspRack() }
        function onLimiterParametersChanged() { consoleRoot.refreshDspRack() }
        function onDspTelemetryChanged() { consoleRoot.refreshDspRack() }
        function onDspTrimsChanged() {
            geqInTrimFader.value = EngineController.getDspTrim("geq", false)
            geqOutTrimFader.value = EngineController.getDspTrim("geq", true)
            consoleRoot.refreshPeqInspector()
            consoleRoot.refreshDspRack()
        }
        function onActiveDeckChanged() { consoleRoot.activeDeckPollTick++ }
        function onDeckStateChanged() { consoleRoot.activeDeckPollTick++ }
    }

    // Dark recessed steel console chassis background
    Rectangle {
        anchors.fill: parent
        color: "#0e1116"
    }

    // =========================================================================
    // MAIN HARDWARE CONSOLE BODY (UPPER 3-WING + LOWER FULL-WIDTH PANEL)
    // =========================================================================
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        // ---------------------------------------------------------------------
        // UPPER SECTION (~880 px): LEFT WING + CENTER MONITOR + RIGHT WING
        // ---------------------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 8

            // =================================================================
            // REGION 1: LEFT WING — REFERENCE DECK (Mockup proportions: ~470px)
            // =================================================================
            BrushedMetalPanel {
                Layout.preferredWidth: 468
                Layout.fillHeight: true
                baseTint: "#13171d"
                borderColor: "#2f3743"
                showScrews: true
                cornerRadius: 6
                brushedOpacity: 0.32

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6

                    // Centered Engraved Panel Header (—— REFERENCE DECK ——)
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Rectangle { Layout.fillWidth: true; height: 1; color: "#323a46" }
                        Label {
                            text: qsTr("REFERENCE DECK")
                            font.family: "Monospace"
                            font.pixelSize: 12
                            font.bold: true
                            font.letterSpacing: 1.4
                            color: "#BDC9D6"
                        }
                        Rectangle { Layout.fillWidth: true; height: 1; color: "#323a46" }
                    }

                    // Switching Mode Bar: A<->B Quick Switch + NORMAL | COMPARE
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 5

                        ConsoleButton {
                            text: qsTr("A ⇄ B")
                            implicitWidth: 74
                            implicitHeight: 24
                            fontSize: 10
                            accentColor: "#00E5FF"
                            toolTipText: qsTr("Quick Switch Active Deck between A and B")
                            onClicked: EngineController.quickSwitchAB()
                        }

                        Item { Layout.fillWidth: true }

                        ConsoleButton {
                            text: qsTr("NORMAL")
                            implicitWidth: 80
                            implicitHeight: 24
                            fontSize: 10
                            highlighted: EngineController.switchingMode === 0
                            onClicked: EngineController.setSwitchingMode(0)
                        }

                        ConsoleButton {
                            text: qsTr("COMPARE")
                            implicitWidth: 84
                            implicitHeight: 24
                            fontSize: 10
                            accentColor: "#F5B841"
                            highlighted: EngineController.switchingMode === 1
                            toolTipText: qsTr("Synchronized A/B position + per-deck ms nudge comparison")
                            onClicked: EngineController.setSwitchingMode(1)
                        }
                    }

                    // 4 Compact Vertical Deck Cards (strictly NO Level Match or dummy knobs)
                    Repeater {
                        model: ["A", "B", "C", "D"]
                        DeckSlotCard {
                            deckId: modelData
                            onLoadFileRequested: function(id) { consoleRoot.deckLoadFileRequested(id) }
                            onStartFolderRequested: function(id) { consoleRoot.deckStartFolderRequested(id) }
                        }
                    }
                }
            }

            // =================================================================
            // REGION 2: CENTER MAIN MONITOR (Metallic Beveled Monitor Frame)
            // =================================================================
            BrushedMetalPanel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                baseTint: "#13171d"
                borderColor: "#343d4a"
                topHighlightColor: "#20ffffff"
                cornerRadius: 8
                showScrews: true
                brushedOpacity: 0.32

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 6

                    // Recessed Deep Glass CRT/OLED Monitor Frame
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: 6
                        color: "#05070b"
                        border.color: "#11151b"
                        border.width: 2
                        clip: true

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 6

                            // Centered Monitor Header Segmented Bar (AUTO | WAVE+SPEC | WAVE ONLY | SPEC+EQ + L+R | L | R | AUTO)
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10

                                Item { Layout.fillWidth: true }

                                // Primary Monitor View Segmented Bar
                                Rectangle {
                                    implicitWidth: modeSegRow.implicitWidth + 4
                                    height: 28
                                    radius: 4
                                    color: "#0b0e13"
                                    border.color: "#36404e"

                                    RowLayout {
                                        id: modeSegRow
                                        anchors.fill: parent
                                        anchors.margins: 2
                                        spacing: 2
                                        Repeater {
                                            model: [
                                                qsTr("AUTO"),
                                                qsTr("WAVE+SPEC"),
                                                qsTr("WAVE ONLY"),
                                                qsTr("SPEC+EQ")
                                            ]
                                            ConsoleButton {
                                                text: modelData
                                                implicitHeight: 24
                                                implicitWidth: index === 0 ? 84 : 96
                                                fontSize: 10
                                                cornerRadius: 3
                                                highlighted: consoleRoot.monitorViewMode === index
                                                onClicked: consoleRoot.monitorViewMode = index
                                            }
                                        }
                                    }
                                }

                                // Channel Scope Strip (L+R | L | R | AUTO)
                                Rectangle {
                                    implicitWidth: chSegRow.implicitWidth + 4
                                    height: 28
                                    radius: 4
                                    color: "#0b0e13"
                                    border.color: "#36404e"

                                    RowLayout {
                                        id: chSegRow
                                        anchors.fill: parent
                                        anchors.margins: 2
                                        spacing: 2
                                        Repeater {
                                            model: ["L+R", "L", "R", "AUTO"]
                                            ConsoleButton {
                                                text: modelData
                                                implicitWidth: 48
                                                implicitHeight: 24
                                                fontSize: 10
                                                cornerRadius: 3
                                                accentColor: "#3CE4FF"
                                                highlighted: consoleRoot.monitorChannelMode === index
                                                onClicked: consoleRoot.monitorChannelMode = index
                                            }
                                        }
                                    }
                                }

                                Item { Layout.fillWidth: true }
                            }

                            // Track Header Status Line above Waveform
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 6
                                Layout.rightMargin: 6
                                property var actSt: {
                                    var _t = consoleRoot.activeDeckPollTick
                                    return EngineController.getDeckState(EngineController.activeDeckId)
                                }
                                Label {
                                    text: parent.actSt && parent.actSt.path && parent.actSt.path.length > 0
                                          ? ("DECK " + EngineController.activeDeckId + ": "
                                             + (parent.actSt.title && parent.actSt.title.length > 0 ? parent.actSt.title : parent.actSt.fileName))
                                          : qsTr("MAIN MONITOR — READY (NO TRACK LOADED)")
                                    font.family: "Monospace"
                                    font.pixelSize: 10
                                    font.bold: true
                                    color: "#73D8EC"
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Label {
                                    text: parent.actSt && parent.actSt.sampleRate
                                          ? (Math.round(parent.actSt.sampleRate / 100) / 10 + " kHz")
                                          : ""
                                    font.family: "Monospace"
                                    font.pixelSize: 10
                                    color: "#9AA7B5"
                                }
                            }

                            // Upper Waveform Timeline Region (Honest Gate G4 pending state — NO Math.sin)
                            Rectangle {
                                visible: consoleRoot.showWaveform
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.preferredHeight: consoleRoot.showSpectrum ? 150 : 400
                                radius: 3
                                color: "#06090e"
                                border.color: "#151d26"

                                // Time tick markings along top
                                Row {
                                    anchors.top: parent.top
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.margins: 6
                                    spacing: 120
                                    Repeater {
                                        model: 8
                                        Label {
                                            text: "0" + index + ":00"
                                            font.family: "Monospace"
                                            font.pixelSize: 9
                                            color: "#435160"
                                        }
                                    }
                                }

                                // Zero axis line
                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    height: 1
                                    color: "#192533"
                                }

                                // Real Playhead Line + Cyan Top Handle Diamond
                                Item {
                                    property var actSt: {
                                        var _t = consoleRoot.activeDeckPollTick
                                        return EngineController.getDeckState(EngineController.activeDeckId)
                                    }
                                    property real ratio: (actSt && actSt.totalFrames > 0)
                                                         ? Math.max(0.0, Math.min(1.0, actSt.positionFrames / actSt.totalFrames))
                                                         : 0.5
                                    x: Math.round(ratio * (parent.width - 12)) + 6
                                    y: 2
                                    width: 1
                                    height: parent.height - 4

                                    Rectangle {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        y: 0
                                        width: 10
                                        height: 12
                                        radius: 2
                                        color: "#2CD6F2"
                                    }
                                    Rectangle {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        y: 12
                                        width: 1.5
                                        height: parent.height - 12
                                        color: "#802CD6F2"
                                    }
                                }

                                Label {
                                    anchors.centerIn: parent
                                    text: qsTr("OVERVIEW WAVEFORM TIMELINE — REAL WAVEFORM EXTRACTOR SCHEDULED IN GATE G4")
                                    font.family: "Monospace"
                                    font.pixelSize: 11
                                    font.bold: true
                                    color: "#4E5F70"
                                }
                            }

                            // Lower Spectrum + Composite EQ Curve Canvas (Real EQ Curve + Honest Gate G4 FFT Grid)
                            Rectangle {
                                visible: consoleRoot.showSpectrum
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.preferredHeight: consoleRoot.showWaveform ? 260 : 420
                                radius: 3
                                color: "#06080d"
                                border.color: "#151d26"

                                Canvas {
                                    id: monitorCanvas
                                    anchors.fill: parent
                                    anchors.margins: 4
                                    antialiasing: true
                                    onWidthChanged: requestPaint()
                                    onHeightChanged: requestPaint()

                                    onPaint: {
                                        var ctx = getContext("2d")
                                        if (!ctx) return
                                        var w = width
                                        var h = height
                                        if (w <= 10 || h <= 10) return
                                        ctx.clearRect(0, 0, w, h)

                                        var padL = 28
                                        var padR = 28
                                        var padT = 10
                                        var padB = 20
                                        var plotW = w - padL - padR
                                        var plotH = h - padT - padB

                                        // Log frequency grid (20 Hz .. 20 kHz)
                                        var freqs = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]
                                        var labels = ["20Hz", "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20kHz"]
                                        ctx.font = "10px Monospace"
                                        for (var i = 0; i < freqs.length; ++i) {
                                            var xn = (Math.log(freqs[i]) - Math.log(20.0)) / (Math.log(20000.0) - Math.log(20.0))
                                            var x = padL + xn * plotW
                                            ctx.strokeStyle = freqs[i] === 1000 ? "#243445" : "#121a24"
                                            ctx.lineWidth = 1
                                            ctx.beginPath()
                                            ctx.moveTo(x, padT)
                                            ctx.lineTo(x, padT + plotH)
                                            ctx.stroke()

                                            ctx.fillStyle = "#6F8092"
                                            ctx.fillText(labels[i], x - 12, h - 5)
                                        }

                                        // dB Horizontal Grid (+12 .. -12 dB centered at 0 dB)
                                        var dbMarks = [12, 6, 0, -6, -12]
                                        for (var j = 0; j < dbMarks.length; ++j) {
                                            var dbVal = dbMarks[j]
                                            var yn = (18.0 - dbVal) / 36.0
                                            var y = padT + yn * plotH
                                            ctx.strokeStyle = dbVal === 0 ? "#2B3C4E" : "#121a24"
                                            ctx.lineWidth = dbVal === 0 ? 1.5 : 1
                                            ctx.beginPath()
                                            ctx.moveTo(padL, y)
                                            ctx.lineTo(padL + plotW, y)
                                            ctx.stroke()

                                            ctx.fillStyle = dbVal === 0 ? "#9EB0C2" : "#5A6B7C"
                                            var txt = (dbVal > 0 ? "+" : "") + dbVal
                                            ctx.fillText(txt, 4, y + 3)
                                            ctx.fillText(txt, padL + plotW + 4, y + 3)
                                        }

                                        // Real EQ Response Curve (only when GEQ or PEQ active in AUTO or SPEC+EQ)
                                        if (consoleRoot.showEqOverlay) {
                                            var pts = consoleRoot.currentOpTab === 0
                                                      ? EngineController.getGeqCurve(180)
                                                      : (consoleRoot.peqEditListening
                                                         ? EngineController.getListeningCurve(consoleRoot.peqRightChannel, 180)
                                                         : EngineController.getPeqCurve(consoleRoot.peqRightChannel, 180))

                                            if (pts && pts.length > 1) {
                                                // Soft under-curve glow fill
                                                var grad = ctx.createLinearGradient(0, padT, 0, padT + plotH)
                                                if (consoleRoot.peqEditListening && consoleRoot.currentOpTab === 1) {
                                                    grad.addColorStop(0.0, "rgba(245, 184, 65, 0.22)")
                                                    grad.addColorStop(1.0, "rgba(245, 184, 65, 0.0)")
                                                    ctx.strokeStyle = "#F5B841"
                                                } else {
                                                    grad.addColorStop(0.0, "rgba(0, 229, 255, 0.22)")
                                                    grad.addColorStop(1.0, "rgba(0, 229, 255, 0.0)")
                                                    ctx.strokeStyle = "#2FE0FF"
                                                }

                                                ctx.beginPath()
                                                var yZero = padT + (18.0 / 36.0) * plotH
                                                for (var k = 0; k < pts.length; ++k) {
                                                    var f = pts[k].f
                                                    var gdb = Math.max(-18.0, Math.min(18.0, pts[k].db))
                                                    var px = padL + ((Math.log(f) - Math.log(20.0)) / (Math.log(20000.0) - Math.log(20.0))) * plotW
                                                    var py = padT + ((18.0 - gdb) / 36.0) * plotH
                                                    if (k === 0) ctx.moveTo(px, py)
                                                    else ctx.lineTo(px, py)
                                                }
                                                ctx.lineWidth = 2.4
                                                ctx.stroke()

                                                ctx.lineTo(padL + plotW, yZero)
                                                ctx.lineTo(padL, yZero)
                                                ctx.closePath()
                                                ctx.fillStyle = grad
                                                ctx.fill()
                                            }
                                        }
                                    }
                                }

                                Label {
                                    anchors.top: parent.top
                                    anchors.right: parent.right
                                    anchors.margins: 8
                                    text: consoleRoot.showEqOverlay
                                          ? qsTr("EQ RESPONSE ACTIVE  •  REAL FFT SPECTRUM PENDING GATE G4")
                                          : qsTr("REAL-TIME FFT SPECTRUM PENDING GATE G4")
                                    font.family: "Monospace"
                                    font.pixelSize: 10
                                    color: "#526374"
                                }
                            }

                            // Bottom 1-Line Compact Diagnostic Bar inside Monitor Bezel
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 24
                                radius: 3
                                color: "#090c11"
                                border.color: "#1e2630"

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 10
                                    spacing: 12

                                    Label {
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        text: qsTr("PATH: %1   |   HEADROOM: %2 dB   |   DSP BYPASS: %3   |   INSERT: %4   |   BIT-PERFECT: %5")
                                              .arg(EngineController.outputRoutingMode === 0 ? qsTr("LISTENING") : qsTr("RENDER MONITOR"))
                                              .arg(EngineController.masterHeadroomDb.toFixed(1))
                                              .arg(EngineController.directModeEnabled ? qsTr("DIRECT") : (EngineController.globalDspBypass ? qsTr("ON") : qsTr("OFF")))
                                              .arg(EngineController.externalInsertStateText)
                                              .arg(EngineController.bitPerfectModeStateText)
                                        font.family: "Monospace"
                                        font.pixelSize: 10
                                        color: "#9AA8B7"
                                        elide: Text.ElideRight
                                    }

                                    Label {
                                        text: "64-bit float"
                                        font.family: "Monospace"
                                        font.pixelSize: 10
                                        font.bold: true
                                        color: "#73D8EC"
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // =================================================================
            // REGION 3: RIGHT WING — ROUTING & MASTER CONTROL (~534px; Faders:Buttons = 2:1)
            // =================================================================
            BrushedMetalPanel {
                Layout.preferredWidth: 534
                Layout.fillHeight: true
                baseTint: "#13171d"
                borderColor: "#2f3743"
                showScrews: true
                cornerRadius: 6
                brushedOpacity: 0.32

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 9

                    // Centered Engraved Panel Header (—— ROUTING ——)
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Rectangle { Layout.fillWidth: true; height: 1; color: "#323a46" }
                        Label {
                            text: qsTr("ROUTING")
                            font.family: "Monospace"
                            font.pixelSize: 12
                            font.bold: true
                            font.letterSpacing: 1.4
                            color: "#BDC9D6"
                        }
                        Rectangle { Layout.fillWidth: true; height: 1; color: "#323a46" }
                    }

                    // Tall Balanced 3-Button Routing Section (Q-05 Monitor Output vs Export Path + Dark Preset Selector)
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 7

                        // Monitor Group Header + Inline Dark Preset Picker
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Label {
                                text: qsTr("MONITOR OUTPUT")
                                font.family: "Monospace"
                                font.pixelSize: 9
                                font.bold: true
                                color: "#52CFEA"
                            }
                            Item { Layout.fillWidth: true }
                            Label {
                                text: qsTr("PRESET:")
                                font.family: "Monospace"
                                font.pixelSize: 9
                                color: "#8A99A8"
                            }
                            ComboBox {
                                id: routingPresetCombo
                                implicitWidth: 168
                                implicitHeight: 24
                                model: EngineController.availableListeningPresets()
                                currentIndex: Math.max(0, model.indexOf(EngineController.listeningPresetName))
                                enabled: EngineController.outputRoutingMode === 0 && !consoleRoot.isDirect
                                onActivated: function(idx) {
                                    EngineController.setListeningPreset(model[idx])
                                }
                                contentItem: Label {
                                    leftPadding: 8
                                    rightPadding: 20
                                    text: routingPresetCombo.displayText
                                    font.pixelSize: 10
                                    font.bold: true
                                    color: routingPresetCombo.enabled ? "#DCE6F0" : "#5D6976"
                                    verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideRight
                                }
                                background: Rectangle {
                                    radius: 3
                                    color: "#0B0E13"
                                    border.color: routingPresetCombo.enabled ? "#384250" : "#242A34"
                                    border.width: 1
                                }
                            }
                        }

                        ConsoleButton {
                            Layout.fillWidth: true
                            implicitHeight: 44
                            text: qsTr("Listening")
                            fontSize: 14
                            highlighted: EngineController.outputRoutingMode === 0
                            activeTopColor: "#273A4A"
                            activeBottomColor: "#14222D"
                            onClicked: EngineController.setOutputRoutingMode(0)
                        }

                        ConsoleButton {
                            Layout.fillWidth: true
                            implicitHeight: 44
                            text: qsTr("Render Monitor")
                            fontSize: 14
                            accentColor: "#F5B841"
                            highlighted: EngineController.outputRoutingMode === 1
                            activeTopColor: "#3D311B"
                            activeBottomColor: "#211A0E"
                            onClicked: EngineController.setOutputRoutingMode(1)
                        }

                        // Export Path Group Separator + Balanced 44px Render-File Bar
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 2
                            spacing: 6
                            Label {
                                text: qsTr("EXPORT PATH")
                                font.family: "Monospace"
                                font.pixelSize: 9
                                font.bold: true
                                color: "#E8BA6B"
                            }
                            Rectangle { Layout.fillWidth: true; height: 1; color: "#2d3541" }
                        }

                        ConsoleButton {
                            Layout.fillWidth: true
                            implicitHeight: 44
                            text: EngineController.isRendering
                                  ? qsTr("Render-File (Exporting %1%)").arg(Math.round(EngineController.renderProgress * 100))
                                  : qsTr("Render-File")
                            fontSize: 14
                            accentColor: "#E8BA6B"
                            highlighted: EngineController.isRendering
                            onClicked: consoleRoot.renderPopupRequested()
                        }
                    }

                    // Recessed Master Gain Bridge (2:1 Width Ratio — Wide Faders:Buttons = 336 : 168)
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: 10

                        // Left 2/3 (336 px): Wide Master Meters + Dual Faders + Center dB Scale + Mute
                        BrushedMetalPanel {
                            Layout.fillWidth: true
                            Layout.preferredWidth: 336
                            Layout.fillHeight: true
                            baseTint: "#11151b"
                            borderColor: "#2c3440"
                            isRecessedWell: true
                            brushedOpacity: 0.24
                            cornerRadius: 5

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 6

                                // Top LINK + BAL + Headroom Strip
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6

                                    ConsoleButton {
                                        text: consoleRoot.linkMasterLR ? qsTr("LINK: ON") : qsTr("LINK: OFF")
                                        implicitWidth: 82
                                        implicitHeight: 25
                                        fontSize: 9
                                        showLed: true
                                        ledColor: consoleRoot.linkMasterLR ? "#00E5FF" : "#444"
                                        highlighted: consoleRoot.linkMasterLR
                                        enabled: !consoleRoot.isDirect
                                        onClicked: consoleRoot.linkMasterLR = !consoleRoot.linkMasterLR
                                    }

                                    ConsoleButton {
                                        text: qsTr("BAL: C (G4)")
                                        implicitWidth: 82
                                        implicitHeight: 25
                                        fontSize: 9
                                        enabled: false
                                        toolTipText: qsTr("Independent Balance attenuation (-∞ dB) — Wired in Gate G4")
                                    }

                                    Item { Layout.fillWidth: true }

                                    Label {
                                        text: qsTr("HR %1").arg(EngineController.masterHeadroomDb.toFixed(1))
                                        font.family: "Monospace"
                                        font.pixelSize: 10
                                        font.bold: true
                                        color: EngineController.masterClipped ? "#FF5252" : "#00E676"
                                    }
                                }

                                // [Left Meter] [Left Fader] [Engraved Center Scale] [Right Fader] [Right Meter]
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    spacing: 6

                                    ColumnLayout {
                                        Layout.fillHeight: true
                                        spacing: 3
                                        Label {
                                            Layout.alignment: Qt.AlignHCenter
                                            text: qsTr("Left")
                                            font.pixelSize: 11
                                            color: "#D4DEE8"
                                        }
                                        ConsoleLedMeter {
                                            Layout.fillHeight: true
                                            Layout.preferredWidth: 20
                                            segmentCount: 30
                                            peakValue: EngineController.masterPeakL
                                            rmsValue: EngineController.masterRmsL
                                            clipped: EngineController.masterClipped
                                        }
                                    }

                                    ConsoleFader {
                                        id: masterFaderL
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        labelText: ""
                                        minValue: -60.0
                                        maxValue: 6.0
                                        defaultValue: 0.0
                                        value: consoleRoot.gainToDb(EngineController.masterGainL)
                                        showHeaderLed: false
                                        showReadout: true
                                        accentColor: "#00E5FF"
                                        enabled: !consoleRoot.isDirect
                                        onValueEdited: function(dbVal) {
                                            var g = consoleRoot.dbToGain(dbVal)
                                            if (consoleRoot.linkMasterLR) EngineController.setMasterGain(g)
                                            else EngineController.setMasterGainL(g)
                                        }
                                    }

                                    // Engraved White Silkscreen Center Scale matching fq83 mockup
                                    ColumnLayout {
                                        Layout.preferredWidth: 30
                                        Layout.fillHeight: true
                                        Layout.topMargin: 22
                                        Layout.bottomMargin: 24
                                        Repeater {
                                            model: ["12", "5", "0", "-5", "-10", "-20", "-30", "-36"]
                                            Item {
                                                Layout.fillWidth: true
                                                Layout.fillHeight: true
                                                Label {
                                                    anchors.centerIn: parent
                                                    text: modelData
                                                    font.family: "Monospace"
                                                    font.pixelSize: 9
                                                    font.bold: modelData === "0"
                                                    color: modelData === "0" ? "#FFFFFF" : "#D4DEE8"
                                                }
                                            }
                                        }
                                    }

                                    ConsoleFader {
                                        id: masterFaderR
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        labelText: ""
                                        minValue: -60.0
                                        maxValue: 6.0
                                        defaultValue: 0.0
                                        value: consoleRoot.gainToDb(EngineController.masterGainR)
                                        showHeaderLed: false
                                        showReadout: true
                                        accentColor: "#00E5FF"
                                        enabled: !consoleRoot.isDirect
                                        onValueEdited: function(dbVal) {
                                            var g = consoleRoot.dbToGain(dbVal)
                                            if (consoleRoot.linkMasterLR) EngineController.setMasterGain(g)
                                            else EngineController.setMasterGainR(g)
                                        }
                                    }

                                    ColumnLayout {
                                        Layout.fillHeight: true
                                        spacing: 3
                                        Label {
                                            Layout.alignment: Qt.AlignHCenter
                                            text: qsTr("Right")
                                            font.pixelSize: 11
                                            color: "#D4DEE8"
                                        }
                                        ConsoleLedMeter {
                                            Layout.fillHeight: true
                                            Layout.preferredWidth: 20
                                            segmentCount: 30
                                            peakValue: EngineController.masterPeakR
                                            rmsValue: EngineController.masterRmsR
                                            clipped: EngineController.masterClipped
                                        }
                                    }
                                }

                                // Bottom MUTE Row directly under Dual Master Faders
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6

                                    ConsoleButton {
                                        Layout.fillWidth: true
                                        implicitHeight: 30
                                        text: qsTr("L MUTE")
                                        fontSize: 9
                                        enabled: false
                                        toolTipText: qsTr("Independent Left Mute (+ Direct Mode override) — Wired in Gate G4")
                                    }

                                    ConsoleButton {
                                        Layout.fillWidth: true
                                        implicitHeight: 30
                                        text: EngineController.masterMute ? qsTr("MUTED") : qsTr("MUTE")
                                        fontSize: 10
                                        anodizedFill: EngineController.masterMute
                                        anodizedTop: "#B33339"
                                        anodizedBottom: "#61171B"
                                        anodizedText: "#FFE8EA"
                                        accentColor: "#FF5252"
                                        highlighted: EngineController.masterMute
                                        onClicked: EngineController.setMasterMute(!EngineController.masterMute)
                                    }

                                    ConsoleButton {
                                        Layout.fillWidth: true
                                        implicitHeight: 30
                                        text: qsTr("R MUTE")
                                        fontSize: 9
                                        enabled: false
                                        toolTipText: qsTr("Independent Right Mute (+ Direct Mode override) — Wired in Gate G4")
                                    }
                                }
                            }
                        }

                        // Right 1/3 (168 px): Exact Requested Stack: RENDER -> DIRECT MODE -> [DIM + LEVEL MATCH] -> GLOBAL BYPASS
                        ColumnLayout {
                            Layout.fillWidth: false
                            Layout.preferredWidth: 168
                            Layout.fillHeight: true
                            spacing: 8

                            // 1. Brushed Champagne-Gold RENDER button
                            ConsoleButton {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.preferredHeight: 92
                                text: EngineController.isRendering ? qsTr("RENDERING") : qsTr("RENDER")
                                fontSize: 15
                                anodizedFill: true
                                anodizedTop: "#EDD38B"
                                anodizedBottom: "#8D6D29"
                                anodizedText: "#161207"
                                accentColor: "#F5D98E"
                                highlighted: EngineController.isRendering
                                onClicked: consoleRoot.renderPopupRequested()
                            }

                            // 2. DIRECT MODE Toggle
                            ConsoleButton {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.preferredHeight: 74
                                text: EngineController.directModeEnabled ? qsTr("DIRECT MODE: ON") : qsTr("DIRECT MODE")
                                fontSize: 11
                                showLed: true
                                ledColor: EngineController.directModeEnabled ? "#FF7043" : "#444E5C"
                                accentColor: "#FF7043"
                                highlighted: EngineController.directModeEnabled
                                onClicked: EngineController.setDirectModeEnabled(!EngineController.directModeEnabled)
                            }

                            // 3. DIM + LEVEL MATCH (2 buttons side-by-side)
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.preferredHeight: 74
                                spacing: 6

                                ConsoleButton {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    leftPadding: 4; rightPadding: 4
                                    text: qsTr("DIM\n-20dB")
                                    fontSize: 10
                                    enabled: false
                                    toolTipText: qsTr("Fixed -20 dB Monitor-Only Dim — Wired in Gate G4")
                                }

                                ConsoleButton {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    leftPadding: 4; rightPadding: 4
                                    text: qsTr("LEVEL\nMATCH")
                                    fontSize: 10
                                    accentColor: "#00E5FF"
                                    toolTipText: qsTr("Q-03: Batch Auto Level Match across loaded decks")
                                    onClicked: EngineController.startAutoLevelMatch(EngineController.activeDeckId, ["A", "B", "C", "D"])
                                }
                            }

                            // 4. Deep Crimson-Red GLOBAL BYPASS Button
                            ConsoleButton {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.preferredHeight: 84
                                text: qsTr("GLOBAL BYPASS")
                                fontSize: 12
                                anodizedFill: true
                                anodizedTop: EngineController.globalDspBypass ? "#DF4349" : "#AC2F35"
                                anodizedBottom: EngineController.globalDspBypass ? "#7C1C21" : "#511317"
                                anodizedText: EngineController.globalDspBypass ? "#FFFFFF" : "#F6D6D8"
                                accentColor: "#FF5252"
                                highlighted: EngineController.globalDspBypass
                                enabled: !consoleRoot.isDirect
                                onClicked: EngineController.setGlobalDspBypass(!EngineController.globalDspBypass)
                            }
                        }
                    }

                    // Bottom Full-Width Clickable 5-State BIT-PERFECT Hardware Badge
                    ConsoleButton {
                        Layout.fillWidth: true
                        implicitHeight: 34
                        text: qsTr("BIT-PERFECT: %1   [L1 / L2 / L3 REPORT]")
                              .arg(EngineController.bitPerfectModeStateText.toUpperCase())
                        fontSize: 11
                        monospace: true
                        showLed: true
                        ledColor: consoleRoot.bitPerfectStateColor(EngineController.bitPerfectModeState)
                        accentColor: consoleRoot.bitPerfectStateColor(EngineController.bitPerfectModeState)
                        highlighted: EngineController.bitPerfectModeState > 0
                        onClicked: consoleRoot.bitPerfectReportRequested()
                    }
                }
            }
        }

        // ---------------------------------------------------------------------
        // LOWER FULL-WIDTH CONSOLE OPERATION PANEL (Centered Docked Tabs!)
        // ---------------------------------------------------------------------
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 468

            // Lower Recessed Chassis Panel (matched to Upper Panel Tone)
            BrushedMetalPanel {
                anchors.fill: parent
                anchors.topMargin: 18
                baseTint: "#12161c"
                borderColor: "#2f3743"
                topHighlightColor: "#1cffffff"
                showScrews: true
                cornerRadius: 6
                brushedOpacity: 0.28

                StackLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    anchors.topMargin: 22
                    currentIndex: consoleRoot.currentOpTab

                    // =========================================================
                    // TAB 0: 32-GEQ (Exact Mockup fq83 Split 16 | CENTER CYAN | 16)
                    // =========================================================
                    RowLayout {
                        spacing: 3

                        // Left 16 Bands (20 .. 630 Hz)
                        Repeater {
                            id: geqLeftRepeater
                            model: 16
                            ConsoleFader {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.minimumWidth: 34
                                labelText: consoleRoot.geqBandLabel(index)
                                minValue: -12.0
                                maxValue: 12.0
                                defaultValue: 0.0
                                value: EngineController.getGeqBandGain(index)
                                bandEnabled: EngineController.isGeqBandEnabled(index)
                                showHeaderLed: true
                                showReadout: false
                                enabled: !consoleRoot.isDirect && !EngineController.geqBypassed
                                onValueEdited: function(v) {
                                    value = v
                                    EngineController.setGeqBandGain(index, v)
                                    consoleRoot.refreshEqCurve()
                                }
                                onEnabledToggled: function(en) {
                                    bandEnabled = en
                                    EngineController.setGeqBandEnabled(index, en)
                                    consoleRoot.refreshEqCurve()
                                }
                            }
                        }

                        // CENTER GLOWING CYAN GEQ MASTER ISLAND (GEQ BYPASS + IN / OUT)
                        Item {
                            Layout.preferredWidth: 162
                            Layout.fillHeight: true
                            Layout.leftMargin: 6
                            Layout.rightMargin: 6

                            // Left & Right Glowing Cyan Rails matching fq83 mockup
                            Rectangle {
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                anchors.left: parent.left
                                width: 3
                                radius: 1.5
                                color: "#32E6FF"
                            }
                            Rectangle {
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                anchors.right: parent.right
                                width: 3
                                radius: 1.5
                                color: "#32E6FF"
                            }

                            Rectangle {
                                anchors.fill: parent
                                anchors.leftMargin: 3
                                anchors.rightMargin: 3
                                color: "#11171f"
                            }

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                anchors.topMargin: 2
                                anchors.bottomMargin: 2
                                spacing: 4

                                ConsoleButton {
                                    Layout.fillWidth: true
                                    implicitHeight: 26
                                    text: EngineController.geqBypassed ? qsTr("GEQ BYPASS: ON") : qsTr("GEQ BYPASS")
                                    fontSize: 10
                                    accentColor: "#32E6FF"
                                    highlighted: EngineController.geqBypassed
                                    enabled: !consoleRoot.isDirect
                                    onClicked: EngineController.setDspNodeBypass("geq", !EngineController.geqBypassed)
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    spacing: 10

                                    ConsoleFader {
                                        id: geqInTrimFader
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        labelText: qsTr("IN")
                                        minValue: -12.0
                                        maxValue: 12.0
                                        defaultValue: 0.0
                                        showHeaderLed: false
                                        showReadout: false
                                        accentColor: "#32E6FF"
                                        enabled: !consoleRoot.isDirect
                                        onValueEdited: function(v) {
                                            value = v
                                            EngineController.setDspTrim("geq", false, v)
                                        }
                                    }

                                    ConsoleFader {
                                        id: geqOutTrimFader
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        labelText: qsTr("OUT")
                                        minValue: -12.0
                                        maxValue: 12.0
                                        defaultValue: 0.0
                                        showHeaderLed: false
                                        showReadout: false
                                        accentColor: "#32E6FF"
                                        enabled: !consoleRoot.isDirect
                                        onValueEdited: function(v) {
                                            value = v
                                            EngineController.setDspTrim("geq", true, v)
                                        }
                                    }
                                }

                                ConsoleButton {
                                    Layout.fillWidth: true
                                    implicitHeight: 22
                                    text: qsTr("FLATTEN ALL")
                                    fontSize: 9
                                    accentColor: "#F5B841"
                                    enabled: !consoleRoot.isDirect
                                    onClicked: {
                                        EngineController.resetAllGeqBands()
                                        for (var l = 0; l < 16; ++l) {
                                            var li = geqLeftRepeater.itemAt(l)
                                            if (li) li.value = 0.0
                                            var ri = geqRightRepeater.itemAt(l)
                                            if (ri) ri.value = 0.0
                                        }
                                        consoleRoot.refreshEqCurve()
                                    }
                                }
                            }
                        }

                        // Right 16 Bands (800 .. 20k Hz)
                        Repeater {
                            id: geqRightRepeater
                            model: 16
                            ConsoleFader {
                                readonly property int bandIdx: index + 16
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.minimumWidth: 34
                                labelText: consoleRoot.geqBandLabel(bandIdx)
                                minValue: -12.0
                                maxValue: 12.0
                                defaultValue: 0.0
                                value: EngineController.getGeqBandGain(bandIdx)
                                bandEnabled: EngineController.isGeqBandEnabled(bandIdx)
                                showHeaderLed: true
                                showReadout: false
                                enabled: !consoleRoot.isDirect && !EngineController.geqBypassed
                                onValueEdited: function(v) {
                                    value = v
                                    EngineController.setGeqBandGain(bandIdx, v)
                                    consoleRoot.refreshEqCurve()
                                }
                                onEnabledToggled: function(en) {
                                    bandEnabled = en
                                    EngineController.setGeqBandEnabled(bandIdx, en)
                                    consoleRoot.refreshEqCurve()
                                }
                            }
                        }
                    }

                    // =========================================================
                    // TAB 1: 32-PEQ (Mockup l91k Symmetric L4x4 | INSPECTOR | R4x4)
                    // =========================================================
                    RowLayout {
                        spacing: 12

                        // LEFT CHANNEL 4x4 Symmetrical Grid (L-01 .. L-16, Cyan)
                        ColumnLayout {
                            Layout.fillWidth: false
                            Layout.preferredWidth: 560
                            Layout.maximumWidth: 568
                            Layout.fillHeight: true
                            spacing: 6

                            Label {
                                Layout.alignment: Qt.AlignHCenter
                                text: qsTr("LEFT CHANNEL")
                                font.family: "Monospace"
                                font.pixelSize: 11
                                font.bold: true
                                color: "#9AA9B8"
                            }

                            GridLayout {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                columns: 4
                                rowSpacing: 5
                                columnSpacing: 5

                                Repeater {
                                    model: 16
                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        radius: 4
                                        property var bandData: {
                                            var _t = consoleRoot.peqGridTick
                                            return consoleRoot.peqEditListening
                                                   ? EngineController.getListeningBand(false, index)
                                                   : EngineController.getPeqBand(false, index)
                                        }
                                        property bool isOn: !!(bandData && bandData.enabled)
                                        property bool isSelected: !consoleRoot.peqRightChannel && consoleRoot.selectedPeqBand === index

                                        color: isOn ? (isSelected ? "#164A5B" : "#10323F") : "#11151B"
                                        border.color: isSelected ? "#52F0FF" : (isOn ? "#26A4BD" : "#28303B")
                                        border.width: isSelected ? 2 : 1

                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                consoleRoot.peqRightChannel = false
                                                consoleRoot.selectedPeqBand = index
                                                consoleRoot.refreshPeqInspector()
                                            }
                                            onDoubleClicked: {
                                                if (!consoleRoot.isDirect && bandData && bandData.frequency !== undefined) {
                                                    var nextOn = !isOn
                                                    if (consoleRoot.peqEditListening) {
                                                        EngineController.setListeningBand(false, index, bandData.frequency, bandData.gainDb, bandData.q, bandData.type, nextOn)
                                                    } else {
                                                        EngineController.setPeqBand(false, index, bandData.frequency, bandData.gainDb, bandData.q, bandData.type, nextOn)
                                                    }
                                                    consoleRoot.peqGridTick++
                                                    consoleRoot.refreshPeqInspector()
                                                }
                                            }
                                        }

                                        ColumnLayout {
                                            anchors.fill: parent
                                            anchors.margins: 6
                                            spacing: 4
                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 5
                                                Rectangle {
                                                    width: 7; height: 7; radius: 3.5
                                                    color: isOn ? "#00E676" : "#2F3743"
                                                    border.color: isOn ? "#99ffffff" : "#161A20"
                                                }
                                                Label {
                                                    text: "L-" + (index < 9 ? "0" : "") + (index + 1)
                                                    font.family: "Monospace"
                                                    font.pixelSize: 13
                                                    font.bold: true
                                                    color: isOn ? "#E8FAFF" : "#96A4B2"
                                                }
                                                Item { Layout.fillWidth: true }
                                                Label {
                                                    text: bandData && bandData.q !== undefined ? ("Q " + bandData.q.toFixed(1)) : ""
                                                    font.family: "Monospace"
                                                    font.pixelSize: 11
                                                    font.bold: isOn
                                                    color: isOn ? "#82EAFC" : "#6B7886"
                                                }
                                            }
                                            RowLayout {
                                                Layout.fillWidth: true
                                                Layout.fillHeight: true
                                                spacing: 4
                                                Rectangle {
                                                    Layout.fillWidth: true
                                                    Layout.fillHeight: true
                                                    radius: 3
                                                    color: isOn ? "#091F28" : "#0A0D11"
                                                    border.color: isOn ? "#267287" : "#1E2530"
                                                    Column {
                                                        anchors.centerIn: parent
                                                        spacing: 1
                                                        Label {
                                                            anchors.horizontalCenter: parent.horizontalCenter
                                                            text: "FREQ"
                                                            font.family: "Monospace"
                                                            font.pixelSize: 8
                                                            color: isOn ? "#5FB8CE" : "#4E5966"
                                                        }
                                                        Label {
                                                            anchors.horizontalCenter: parent.horizontalCenter
                                                            text: bandData && bandData.frequency ? (bandData.frequency >= 1000 ? (bandData.frequency / 1000).toFixed(1) + "k" : Math.round(bandData.frequency) + "Hz") : "--"
                                                            font.family: "Monospace"
                                                            font.pixelSize: 13
                                                            font.bold: true
                                                            color: isOn ? "#F0FCFF" : "#8794A2"
                                                        }
                                                    }
                                                }
                                                Rectangle {
                                                    Layout.fillWidth: true
                                                    Layout.fillHeight: true
                                                    radius: 3
                                                    color: isOn ? "#091F28" : "#0A0D11"
                                                    border.color: isOn ? "#267287" : "#1E2530"
                                                    Column {
                                                        anchors.centerIn: parent
                                                        spacing: 1
                                                        Label {
                                                            anchors.horizontalCenter: parent.horizontalCenter
                                                            text: "GAIN"
                                                            font.family: "Monospace"
                                                            font.pixelSize: 8
                                                            color: isOn ? "#5FB8CE" : "#4E5966"
                                                        }
                                                        Label {
                                                            anchors.horizontalCenter: parent.horizontalCenter
                                                            text: bandData && bandData.gainDb !== undefined
                                                                  ? ((bandData.gainDb > 0 ? "+" : "") + bandData.gainDb.toFixed(1))
                                                                  : "--"
                                                            font.family: "Monospace"
                                                            font.pixelSize: 13
                                                            font.bold: true
                                                            color: isOn ? "#F0FCFF" : "#8794A2"
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // CENTER SELECTED BAND INSPECTOR (Glowing Cyan Frame matching l91k mockup)
                        BrushedMetalPanel {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 1020
                            Layout.fillHeight: true
                            baseTint: "#151d24"
                            borderColor: "#33E3FF"
                            accentColor: "#33E3FF"
                            accentWidth: 2
                            cornerRadius: 6

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 12
                                spacing: 14

                                // 1. Left Wide Mini Monitor + 2x3 Filter Type Buttons
                                ColumnLayout {
                                    Layout.fillWidth: false
                                    Layout.preferredWidth: 388
                                    Layout.fillHeight: true
                                    spacing: 8

                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        radius: 4
                                        color: "#06090e"
                                        border.color: "#223442"
                                        Canvas {
                                            id: peqMiniCanvas
                                            anchors.fill: parent
                                            anchors.margins: 4
                                            onWidthChanged: requestPaint()
                                            onHeightChanged: requestPaint()
                                            onPaint: {
                                                var ctx = getContext("2d")
                                                if (!ctx || width <= 4 || height <= 4) return
                                                ctx.clearRect(0, 0, width, height)
                                                ctx.strokeStyle = "#182430"
                                                ctx.lineWidth = 1
                                                ctx.beginPath()
                                                ctx.moveTo(0, height * 0.5)
                                                ctx.lineTo(width, height * 0.5)
                                                ctx.stroke()

                                                var pts = consoleRoot.peqEditListening
                                                          ? EngineController.getListeningCurve(consoleRoot.peqRightChannel, 120)
                                                          : EngineController.getPeqCurve(consoleRoot.peqRightChannel, 120)
                                                if (pts && pts.length > 1) {
                                                    ctx.strokeStyle = consoleRoot.peqRightChannel ? "#F2C97D" : "#33E3FF"
                                                    ctx.lineWidth = 2.2
                                                    ctx.beginPath()
                                                    for (var k = 0; k < pts.length; ++k) {
                                                        var x = (k / (pts.length - 1)) * width
                                                        var db = Math.max(-18.0, Math.min(18.0, pts[k].db))
                                                        var y = ((18.0 - db) / 36.0) * height
                                                        if (k === 0) ctx.moveTo(x, y)
                                                        else ctx.lineTo(x, y)
                                                    }
                                                    ctx.stroke()
                                                }
                                            }
                                        }
                                    }

                                    GridLayout {
                                        Layout.fillWidth: true
                                        columns: 3
                                        rowSpacing: 6
                                        columnSpacing: 6
                                        Repeater {
                                            model: 6
                                            ConsoleButton {
                                                Layout.fillWidth: true
                                                implicitHeight: 34
                                                text: consoleRoot.filterTypeName(index)
                                                fontSize: 10
                                                highlighted: consoleRoot.peqBandTypeIndex === index
                                                enabled: !consoleRoot.isDirect
                                                onClicked: {
                                                    consoleRoot.peqBandTypeIndex = index
                                                    if (consoleRoot.peqEditListening) {
                                                        EngineController.setListeningBand(consoleRoot.peqRightChannel, consoleRoot.selectedPeqBand, peqFreqKnob.value, peqGainKnob.value, peqQKnob.value, index, consoleRoot.peqBandEnabledState)
                                                    } else {
                                                        EngineController.setPeqBand(consoleRoot.peqRightChannel, consoleRoot.selectedPeqBand, peqFreqKnob.value, peqGainKnob.value, peqQKnob.value, index, consoleRoot.peqBandEnabledState)
                                                    }
                                                    consoleRoot.peqGridTick++
                                                }
                                            }
                                        }
                                    }
                                }

                                // 2. Center-Right Column: Prominent SELECTED BAND INSPECTOR Header directly above 3 Large Knobs
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    spacing: 10

                                    // Prominent Inspector Header Block directly above the 3 knobs
                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 74
                                        radius: 5
                                        color: "#0A0E14"
                                        border.color: "#294658"
                                        border.width: 1

                                        ColumnLayout {
                                            anchors.fill: parent
                                            anchors.margins: 8
                                            spacing: 4

                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 8
                                                Label {
                                                    text: qsTr("SELECTED BAND INSPECTOR")
                                                    font.family: "Monospace"
                                                    font.pixelSize: 11
                                                    font.bold: true
                                                    font.letterSpacing: 1.4
                                                    color: "#8EC3D6"
                                                }
                                                Item { Layout.fillWidth: true }
                                                ConsoleButton {
                                                    text: consoleRoot.peqEditListening ? qsTr("LISTENING PEQ") : qsTr("MASTER PEQ")
                                                    implicitWidth: 118
                                                    implicitHeight: 24
                                                    fontSize: 9
                                                    accentColor: consoleRoot.peqEditListening ? "#F5B841" : "#33E3FF"
                                                    highlighted: true
                                                    toolTipText: qsTr("Q-08: Switch between Master PEQ and Listening Correction PEQ")
                                                    onClicked: consoleRoot.peqEditListening = !consoleRoot.peqEditListening
                                                }
                                                ConsoleButton {
                                                    text: EngineController.peqBypassed ? qsTr("EQ BYPASSED") : qsTr("EQ ACTIVE")
                                                    implicitWidth: 88
                                                    implicitHeight: 24
                                                    fontSize: 9
                                                    visible: !consoleRoot.peqEditListening
                                                    highlighted: !EngineController.peqBypassed
                                                    enabled: !consoleRoot.isDirect
                                                    onClicked: EngineController.setDspNodeBypass("peq", !EngineController.peqBypassed)
                                                }
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 8

                                                Label {
                                                    text: (consoleRoot.peqRightChannel ? "[R-CH] " : "[L-CH] ")
                                                          + qsTr("BAND ") + (consoleRoot.selectedPeqBand < 9 ? "0" : "") + (consoleRoot.selectedPeqBand + 1) + "/16"
                                                    font.family: "Monospace"
                                                    font.pixelSize: 18
                                                    font.bold: true
                                                    color: consoleRoot.peqRightChannel ? "#F2C97D" : "#5BE7FF"
                                                }

                                                Item { Layout.fillWidth: true }

                                                ConsoleButton {
                                                    text: "<"
                                                    implicitWidth: 32; implicitHeight: 30; fontSize: 12
                                                    onClicked: {
                                                        consoleRoot.selectedPeqBand = (consoleRoot.selectedPeqBand + 15) % 16
                                                        consoleRoot.refreshPeqInspector()
                                                    }
                                                }
                                                ConsoleButton {
                                                    text: ">"
                                                    implicitWidth: 32; implicitHeight: 30; fontSize: 12
                                                    onClicked: {
                                                        consoleRoot.selectedPeqBand = (consoleRoot.selectedPeqBand + 1) % 16
                                                        consoleRoot.refreshPeqInspector()
                                                    }
                                                }
                                                ConsoleButton {
                                                    text: consoleRoot.peqBandEnabledState ? qsTr("ACTIVE") : qsTr("OFF")
                                                    implicitWidth: 96; implicitHeight: 30; fontSize: 11
                                                    showLed: true
                                                    ledColor: consoleRoot.peqBandEnabledState ? "#00E676" : "#444"
                                                    highlighted: consoleRoot.peqBandEnabledState
                                                    enabled: !consoleRoot.isDirect
                                                    onClicked: {
                                                        consoleRoot.peqBandEnabledState = !consoleRoot.peqBandEnabledState
                                                        if (consoleRoot.peqEditListening) {
                                                            EngineController.setListeningBand(consoleRoot.peqRightChannel, consoleRoot.selectedPeqBand, peqFreqKnob.value, peqGainKnob.value, peqQKnob.value, consoleRoot.peqBandTypeIndex, consoleRoot.peqBandEnabledState)
                                                        } else {
                                                            EngineController.setPeqBand(consoleRoot.peqRightChannel, consoleRoot.selectedPeqBand, peqFreqKnob.value, peqGainKnob.value, peqQKnob.value, consoleRoot.peqBandTypeIndex, consoleRoot.peqBandEnabledState)
                                                        }
                                                        consoleRoot.peqGridTick++
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    // 3 Large Knobs spread evenly across full width under SELECTED BAND INSPECTOR
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        spacing: 16

                                        ConsoleKnob {
                                            id: peqFreqKnob
                                            Layout.fillWidth: true
                                            Layout.alignment: Qt.AlignVCenter
                                            labelText: qsTr("FREQUENCY")
                                            minValue: 20.0; maxValue: 20000.0; defaultValue: 1000.0; logarithmic: true
                                            unitSuffix: " Hz"; decimals: 1; knobDiameter: 94; arcColor: "#33E3FF"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) {
                                                value = v
                                                if (consoleRoot.peqEditListening) EngineController.setListeningBand(consoleRoot.peqRightChannel, consoleRoot.selectedPeqBand, v, peqGainKnob.value, peqQKnob.value, consoleRoot.peqBandTypeIndex, consoleRoot.peqBandEnabledState)
                                                else EngineController.setPeqBand(consoleRoot.peqRightChannel, consoleRoot.selectedPeqBand, v, peqGainKnob.value, peqQKnob.value, consoleRoot.peqBandTypeIndex, consoleRoot.peqBandEnabledState)
                                                consoleRoot.peqGridTick++
                                            }
                                        }

                                        ConsoleKnob {
                                            id: peqGainKnob
                                            Layout.fillWidth: true
                                            Layout.alignment: Qt.AlignVCenter
                                            labelText: qsTr("GAIN")
                                            minValue: -18.0; maxValue: 18.0; defaultValue: 0.0; logarithmic: false; bipolar: true
                                            unitSuffix: " dB"; decimals: 1; knobDiameter: 94; arcColor: "#E8BA6B"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) {
                                                value = v
                                                if (consoleRoot.peqEditListening) EngineController.setListeningBand(consoleRoot.peqRightChannel, consoleRoot.selectedPeqBand, peqFreqKnob.value, v, peqQKnob.value, consoleRoot.peqBandTypeIndex, consoleRoot.peqBandEnabledState)
                                                else EngineController.setPeqBand(consoleRoot.peqRightChannel, consoleRoot.selectedPeqBand, peqFreqKnob.value, v, peqQKnob.value, consoleRoot.peqBandTypeIndex, consoleRoot.peqBandEnabledState)
                                                consoleRoot.peqGridTick++
                                            }
                                        }

                                        ConsoleKnob {
                                            id: peqQKnob
                                            Layout.fillWidth: true
                                            Layout.alignment: Qt.AlignVCenter
                                            labelText: qsTr("Q")
                                            minValue: 0.1; maxValue: 18.0; defaultValue: 1.0; logarithmic: true
                                            unitSuffix: ""; decimals: 2; knobDiameter: 94; arcColor: "#E8BA6B"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) {
                                                value = v
                                                if (consoleRoot.peqEditListening) EngineController.setListeningBand(consoleRoot.peqRightChannel, consoleRoot.selectedPeqBand, peqFreqKnob.value, peqGainKnob.value, v, consoleRoot.peqBandTypeIndex, consoleRoot.peqBandEnabledState)
                                                else EngineController.setPeqBand(consoleRoot.peqRightChannel, consoleRoot.selectedPeqBand, peqFreqKnob.value, peqGainKnob.value, v, consoleRoot.peqBandTypeIndex, consoleRoot.peqBandEnabledState)
                                                consoleRoot.peqGridTick++
                                            }
                                        }
                                    }
                                }

                                // 3. Right Trim Column inside Inspector (IN / OUT matched to 94px large knob size)
                                ColumnLayout {
                                    Layout.fillWidth: false
                                    Layout.preferredWidth: 124
                                    Layout.fillHeight: true
                                    spacing: 6

                                    Item { Layout.fillHeight: true }

                                    ConsoleKnob {
                                        id: peqInTrimKnob
                                        Layout.alignment: Qt.AlignHCenter
                                        labelText: qsTr("IN")
                                        minValue: -12.0; maxValue: 12.0; defaultValue: 0.0; logarithmic: false; bipolar: true
                                        unitSuffix: "dB"; decimals: 1; knobDiameter: 94; arcColor: "#33E3FF"
                                        enabled: !consoleRoot.isDirect
                                        onValueEdited: function(v) { value = v; EngineController.setDspTrim(consoleRoot.peqEditListening ? "listening_peq" : "peq", false, v) }
                                    }
                                    ConsoleKnob {
                                        id: peqOutTrimKnob
                                        Layout.alignment: Qt.AlignHCenter
                                        labelText: qsTr("OUT")
                                        minValue: -12.0; maxValue: 12.0; defaultValue: 0.0; logarithmic: false; bipolar: true
                                        unitSuffix: "dB"; decimals: 1; knobDiameter: 94; arcColor: "#33E3FF"
                                        enabled: !consoleRoot.isDirect
                                        onValueEdited: function(v) { value = v; EngineController.setDspTrim(consoleRoot.peqEditListening ? "listening_peq" : "peq", true, v) }
                                    }

                                    Item { Layout.fillHeight: true }
                                }
                            }
                        }

                        // RIGHT CHANNEL 4x4 Symmetrical Grid (R-01 .. R-16, Champagne Gold)
                        ColumnLayout {
                            Layout.fillWidth: false
                            Layout.preferredWidth: 560
                            Layout.maximumWidth: 568
                            Layout.fillHeight: true
                            spacing: 6

                            Label {
                                Layout.alignment: Qt.AlignHCenter
                                text: qsTr("RIGHT CHANNEL")
                                font.family: "Monospace"
                                font.pixelSize: 11
                                font.bold: true
                                color: "#9AA9B8"
                            }

                            GridLayout {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                columns: 4
                                rowSpacing: 5
                                columnSpacing: 5

                                Repeater {
                                    model: 16
                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        radius: 4
                                        property var bandData: {
                                            var _t = consoleRoot.peqGridTick
                                            return consoleRoot.peqEditListening
                                                   ? EngineController.getListeningBand(true, index)
                                                   : EngineController.getPeqBand(true, index)
                                        }
                                        property bool isOn: !!(bandData && bandData.enabled)
                                        property bool isSelected: consoleRoot.peqRightChannel && consoleRoot.selectedPeqBand === index

                                        color: isOn ? (isSelected ? "#4E3C19" : "#382A12") : "#11151B"
                                        border.color: isSelected ? "#FFE099" : (isOn ? "#C99D4A" : "#28303B")
                                        border.width: isSelected ? 2 : 1

                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                consoleRoot.peqRightChannel = true
                                                consoleRoot.selectedPeqBand = index
                                                consoleRoot.refreshPeqInspector()
                                            }
                                            onDoubleClicked: {
                                                if (!consoleRoot.isDirect && bandData && bandData.frequency !== undefined) {
                                                    var nextOn = !isOn
                                                    if (consoleRoot.peqEditListening) {
                                                        EngineController.setListeningBand(true, index, bandData.frequency, bandData.gainDb, bandData.q, bandData.type, nextOn)
                                                    } else {
                                                        EngineController.setPeqBand(true, index, bandData.frequency, bandData.gainDb, bandData.q, bandData.type, nextOn)
                                                    }
                                                    consoleRoot.peqGridTick++
                                                    consoleRoot.refreshPeqInspector()
                                                }
                                            }
                                        }

                                        ColumnLayout {
                                            anchors.fill: parent
                                            anchors.margins: 6
                                            spacing: 4
                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 5
                                                Rectangle {
                                                    width: 7; height: 7; radius: 3.5
                                                    color: isOn ? "#00E676" : "#2F3743"
                                                    border.color: isOn ? "#99ffffff" : "#161A20"
                                                }
                                                Label {
                                                    text: "R-" + (index < 9 ? "0" : "") + (index + 1)
                                                    font.family: "Monospace"
                                                    font.pixelSize: 13
                                                    font.bold: true
                                                    color: isOn ? "#FFF2D4" : "#96A4B2"
                                                }
                                                Item { Layout.fillWidth: true }
                                                Label {
                                                    text: bandData && bandData.q !== undefined ? ("Q " + bandData.q.toFixed(1)) : ""
                                                    font.family: "Monospace"
                                                    font.pixelSize: 11
                                                    font.bold: isOn
                                                    color: isOn ? "#E8BA6B" : "#6B7886"
                                                }
                                            }
                                            RowLayout {
                                                Layout.fillWidth: true
                                                Layout.fillHeight: true
                                                spacing: 4
                                                Rectangle {
                                                    Layout.fillWidth: true
                                                    Layout.fillHeight: true
                                                    radius: 3
                                                    color: isOn ? "#251C0C" : "#0A0D11"
                                                    border.color: isOn ? "#8F6E33" : "#1E2530"
                                                    Column {
                                                        anchors.centerIn: parent
                                                        spacing: 1
                                                        Label {
                                                            anchors.horizontalCenter: parent.horizontalCenter
                                                            text: "FREQ"
                                                            font.family: "Monospace"
                                                            font.pixelSize: 8
                                                            color: isOn ? "#D4AA61" : "#4E5966"
                                                        }
                                                        Label {
                                                            anchors.horizontalCenter: parent.horizontalCenter
                                                            text: bandData && bandData.frequency ? (bandData.frequency >= 1000 ? (bandData.frequency / 1000).toFixed(1) + "k" : Math.round(bandData.frequency) + "Hz") : "--"
                                                            font.family: "Monospace"
                                                            font.pixelSize: 13
                                                            font.bold: true
                                                            color: isOn ? "#FFF6E3" : "#8794A2"
                                                        }
                                                    }
                                                }
                                                Rectangle {
                                                    Layout.fillWidth: true
                                                    Layout.fillHeight: true
                                                    radius: 3
                                                    color: isOn ? "#251C0C" : "#0A0D11"
                                                    border.color: isOn ? "#8F6E33" : "#1E2530"
                                                    Column {
                                                        anchors.centerIn: parent
                                                        spacing: 1
                                                        Label {
                                                            anchors.horizontalCenter: parent.horizontalCenter
                                                            text: "GAIN"
                                                            font.family: "Monospace"
                                                            font.pixelSize: 8
                                                            color: isOn ? "#D4AA61" : "#4E5966"
                                                        }
                                                        Label {
                                                            anchors.horizontalCenter: parent.horizontalCenter
                                                            text: bandData && bandData.gainDb !== undefined
                                                                  ? ((bandData.gainDb > 0 ? "+" : "") + bandData.gainDb.toFixed(1))
                                                                  : "--"
                                                            font.family: "Monospace"
                                                            font.pixelSize: 13
                                                            font.bold: true
                                                            color: isOn ? "#FFF6E3" : "#8794A2"
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

                    // =========================================================
                    // TAB 2: DSP — 4 Color-Coded Hardware Rack Units (Uniform 68px Displays)
                    // =========================================================
                    RowLayout {
                        spacing: 10

                        // MODULE 1: STEEL-BLUE HPF / LPF
                        BrushedMetalPanel {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            baseTint: "#162A3A"
                            borderColor: "#3D6C90"
                            showScrews: true
                            cornerRadius: 5
                            brushedOpacity: 0.34

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 12
                                spacing: 8

                                // Row 1: Uniform Title Header
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 26
                                    spacing: 8
                                    Label {
                                        text: qsTr("HPF / LPF")
                                        font.pixelSize: 15
                                        font.bold: true
                                        color: "#6BE0FF"
                                    }
                                    Item { Layout.fillWidth: true }
                                    ConsoleButton {
                                        text: consoleRoot.dspFilterBypassed ? qsTr("BYP") : qsTr("ON")
                                        implicitWidth: 48; implicitHeight: 24; fontSize: 10
                                        showLed: true; ledColor: consoleRoot.dspFilterBypassed ? "#555" : "#00E676"
                                        highlighted: !consoleRoot.dspFilterBypassed
                                        enabled: !consoleRoot.isDirect
                                        onClicked: EngineController.setDspNodeBypass("filter", !consoleRoot.dspFilterBypassed)
                                    }
                                }

                                // Row 2: Uniform 68px OLED Filter Response Display Window
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 68
                                    radius: 4
                                    color: "#060B10"
                                    border.color: "#2C4D66"
                                    border.width: 1

                                    Canvas {
                                        id: filterMiniCanvas
                                        anchors.fill: parent
                                        anchors.margins: 5
                                        onWidthChanged: requestPaint()
                                        onHeightChanged: requestPaint()
                                        onPaint: {
                                            var ctx = getContext("2d")
                                            if (!ctx || width <= 4 || height <= 4) return
                                            ctx.clearRect(0, 0, width, height)

                                            // Reference lines (0 dB, -12 dB)
                                            ctx.strokeStyle = "#162A3A"
                                            ctx.lineWidth = 1
                                            var y0 = (6.0 / 30.0) * height
                                            var y12 = (18.0 / 30.0) * height
                                            ctx.beginPath()
                                            ctx.moveTo(0, y0); ctx.lineTo(width, y0)
                                            ctx.moveTo(0, y12); ctx.lineTo(width, y12)
                                            ctx.stroke()

                                            var pts = EngineController.getFilterCurve(96)
                                            if (pts && pts.length > 1) {
                                                ctx.strokeStyle = "#48D8FF"
                                                ctx.lineWidth = 2.0
                                                ctx.beginPath()
                                                for (var i = 0; i < pts.length; ++i) {
                                                    var x = (i / (pts.length - 1)) * width
                                                    var db = Math.max(-24.0, Math.min(6.0, pts[i].db))
                                                    var y = ((6.0 - db) / 30.0) * height
                                                    if (i === 0) ctx.moveTo(x, y)
                                                    else ctx.lineTo(x, y)
                                                }
                                                ctx.stroke()
                                            }
                                        }
                                    }

                                    Label {
                                        anchors.top: parent.top
                                        anchors.right: parent.right
                                        anchors.margins: 5
                                        text: qsTr("FILTER RESPONSE (20Hz–20kHz)")
                                        font.family: "Monospace"
                                        font.pixelSize: 8
                                        color: "#5286A8"
                                    }
                                }

                                // Row 3: Parameters + IN/OUT Trim
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    spacing: 8

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        spacing: 4
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 6
                                            ConsoleButton {
                                                text: consoleRoot.dspHpfEnabled ? qsTr("HPF\nON") : qsTr("HPF\nOFF")
                                                implicitWidth: 48; implicitHeight: 46; fontSize: 9
                                                highlighted: consoleRoot.dspHpfEnabled
                                                enabled: !consoleRoot.isDirect
                                                onClicked: {
                                                    consoleRoot.dspHpfEnabled = !consoleRoot.dspHpfEnabled
                                                    EngineController.setFilterParams(dspHpfKnob.value, consoleRoot.dspHpfEnabled, consoleRoot.dspHpfSlope, dspLpfKnob.value, consoleRoot.dspLpfEnabled, consoleRoot.dspLpfSlope, 0.707, 0.707)
                                                }
                                            }
                                            ConsoleKnob {
                                                id: dspHpfKnob
                                                Layout.fillWidth: true
                                                labelText: qsTr("HPF Cutoff")
                                                minValue: 20.0; maxValue: 2000.0; defaultValue: 20.0; logarithmic: true
                                                unitSuffix: " Hz"; decimals: 0; knobDiameter: 48; arcColor: "#48D8FF"
                                                enabled: !consoleRoot.isDirect
                                                onValueEdited: function(v) { value = v; EngineController.setFilterParams(v, consoleRoot.dspHpfEnabled, consoleRoot.dspHpfSlope, dspLpfKnob.value, consoleRoot.dspLpfEnabled, consoleRoot.dspLpfSlope, 0.707, 0.707) }
                                            }
                                            ConsoleButton {
                                                text: consoleRoot.dspHpfSlope === 1 ? "24dB" : "12dB"
                                                implicitWidth: 44; implicitHeight: 28; fontSize: 9
                                                enabled: !consoleRoot.isDirect
                                                onClicked: {
                                                    consoleRoot.dspHpfSlope = consoleRoot.dspHpfSlope === 1 ? 0 : 1
                                                    EngineController.setFilterParams(dspHpfKnob.value, consoleRoot.dspHpfEnabled, consoleRoot.dspHpfSlope, dspLpfKnob.value, consoleRoot.dspLpfEnabled, consoleRoot.dspLpfSlope, 0.707, 0.707)
                                                }
                                            }
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 6
                                            ConsoleButton {
                                                text: consoleRoot.dspLpfEnabled ? qsTr("LPF\nON") : qsTr("LPF\nOFF")
                                                implicitWidth: 48; implicitHeight: 46; fontSize: 9
                                                highlighted: consoleRoot.dspLpfEnabled
                                                enabled: !consoleRoot.isDirect
                                                onClicked: {
                                                    consoleRoot.dspLpfEnabled = !consoleRoot.dspLpfEnabled
                                                    EngineController.setFilterParams(dspHpfKnob.value, consoleRoot.dspHpfEnabled, consoleRoot.dspHpfSlope, dspLpfKnob.value, consoleRoot.dspLpfEnabled, consoleRoot.dspLpfSlope, 0.707, 0.707)
                                                }
                                            }
                                            ConsoleKnob {
                                                id: dspLpfKnob
                                                Layout.fillWidth: true
                                                labelText: qsTr("LPF Cutoff")
                                                minValue: 1000.0; maxValue: 20000.0; defaultValue: 20000.0; logarithmic: true
                                                unitSuffix: " Hz"; decimals: 0; knobDiameter: 48; arcColor: "#48D8FF"
                                                enabled: !consoleRoot.isDirect
                                                onValueEdited: function(v) { value = v; EngineController.setFilterParams(dspHpfKnob.value, consoleRoot.dspHpfEnabled, consoleRoot.dspHpfSlope, v, consoleRoot.dspLpfEnabled, consoleRoot.dspLpfSlope, 0.707, 0.707) }
                                            }
                                            ConsoleButton {
                                                text: consoleRoot.dspLpfSlope === 1 ? "24dB" : "12dB"
                                                implicitWidth: 44; implicitHeight: 28; fontSize: 9
                                                enabled: !consoleRoot.isDirect
                                                onClicked: {
                                                    consoleRoot.dspLpfSlope = consoleRoot.dspLpfSlope === 1 ? 0 : 1
                                                    EngineController.setFilterParams(dspHpfKnob.value, consoleRoot.dspHpfEnabled, consoleRoot.dspHpfSlope, dspLpfKnob.value, consoleRoot.dspLpfEnabled, consoleRoot.dspLpfSlope, 0.707, 0.707)
                                                }
                                            }
                                        }
                                    }

                                    Rectangle { width: 1; Layout.fillHeight: true; color: "#2D526E" }
                                    ColumnLayout {
                                        Layout.preferredWidth: 102
                                        Layout.fillHeight: true
                                        spacing: 4
                                        ConsoleKnob {
                                            id: filterInTrimKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("IN")
                                            minValue: -12.0; maxValue: 12.0; defaultValue: 0.0; logarithmic: false; bipolar: true
                                            unitSuffix: "dB"; decimals: 1; knobDiameter: 44; arcColor: "#48D8FF"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setDspTrim("filter", false, v) }
                                        }
                                        ConsoleKnob {
                                            id: filterOutTrimKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("OUT")
                                            minValue: -12.0; maxValue: 12.0; defaultValue: 0.0; logarithmic: false; bipolar: true
                                            unitSuffix: "dB"; decimals: 1; knobDiameter: 44; arcColor: "#48D8FF"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setDspTrim("filter", true, v) }
                                        }
                                    }
                                }
                            }
                        }

                        // MODULE 2: CHARCOAL / AMBER COMPRESSOR
                        BrushedMetalPanel {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            baseTint: "#1B2028"
                            borderColor: "#444D5A"
                            showScrews: true
                            cornerRadius: 5
                            brushedOpacity: 0.34

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 12
                                spacing: 8

                                // Row 1: Uniform Title Header
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 26
                                    spacing: 8
                                    Label {
                                        text: qsTr("COMPRESSOR")
                                        font.pixelSize: 15
                                        font.bold: true
                                        color: "#E8BA6B"
                                    }
                                    Item { Layout.fillWidth: true }
                                    ConsoleButton {
                                        text: consoleRoot.dspCompBypassed ? qsTr("BYP") : qsTr("ON")
                                        implicitWidth: 48; implicitHeight: 24; fontSize: 10
                                        showLed: true; ledColor: consoleRoot.dspCompBypassed ? "#555" : "#00E676"
                                        highlighted: !consoleRoot.dspCompBypassed
                                        enabled: !consoleRoot.isDirect
                                        onClicked: EngineController.setDspNodeBypass("compressor", !consoleRoot.dspCompBypassed)
                                    }
                                }

                                // Row 2: Uniform 68px OLED Gain Reduction Window
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 68
                                    radius: 4
                                    color: "#090C11"
                                    border.color: "#38414E"
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 7
                                        spacing: 4

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Label { text: qsTr("GAIN REDUCTION"); font.family: "Monospace"; font.pixelSize: 9; font.bold: true; color: "#D6AE66" }
                                            Item { Layout.fillWidth: true }
                                            Label { text: consoleRoot.dspCompGainReductionDb.toFixed(1) + " dB"; font.family: "Monospace"; font.pixelSize: 11; font.bold: true; color: "#F5B841" }
                                        }

                                        ConsoleLedMeter {
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 22
                                            horizontal: true
                                            segmentCount: 24
                                            peakValue: Math.min(1.0, Math.abs(consoleRoot.dspCompGainReductionDb) / 18.0)
                                            rmsValue: peakValue * 0.75
                                            greenColor: "#F5B841"
                                            amberColor: "#FF8F00"
                                            redColor: "#FF5252"
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Label { text: "0"; font.family: "Monospace"; font.pixelSize: 8; color: "#687584" }
                                            Item { Layout.fillWidth: true }
                                            Label { text: "-3"; font.family: "Monospace"; font.pixelSize: 8; color: "#687584" }
                                            Item { Layout.fillWidth: true }
                                            Label { text: "-6"; font.family: "Monospace"; font.pixelSize: 8; color: "#687584" }
                                            Item { Layout.fillWidth: true }
                                            Label { text: "-12"; font.family: "Monospace"; font.pixelSize: 8; color: "#687584" }
                                            Item { Layout.fillWidth: true }
                                            Label { text: "-18 dB"; font.family: "Monospace"; font.pixelSize: 8; color: "#687584" }
                                        }
                                    }
                                }

                                // Row 3: Parameters + IN/OUT Trim
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    spacing: 6

                                    GridLayout {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        columns: 3
                                        rowSpacing: 2
                                        columnSpacing: 2

                                        ConsoleKnob {
                                            id: dspCompThreshKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("Threshold")
                                            minValue: -48.0; maxValue: 0.0; defaultValue: -12.0; logarithmic: false
                                            unitSuffix: " dB"; decimals: 1; knobDiameter: 44; arcColor: "#F5B841"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setCompressorParams(v, dspCompRatioKnob.value, dspCompAttackKnob.value, dspCompReleaseKnob.value, dspCompMakeupKnob.value, dspCompKneeKnob.value) }
                                        }
                                        ConsoleKnob {
                                            id: dspCompRatioKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("Ratio")
                                            minValue: 1.0; maxValue: 20.0; defaultValue: 2.0; logarithmic: true
                                            unitSuffix: ":1"; decimals: 1; knobDiameter: 44; arcColor: "#F5B841"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setCompressorParams(dspCompThreshKnob.value, v, dspCompAttackKnob.value, dspCompReleaseKnob.value, dspCompMakeupKnob.value, dspCompKneeKnob.value) }
                                        }
                                        ConsoleKnob {
                                            id: dspCompKneeKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("Knee")
                                            minValue: 0.0; maxValue: 24.0; defaultValue: 2.0; logarithmic: false
                                            unitSuffix: " dB"; decimals: 1; knobDiameter: 44; arcColor: "#F5B841"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setCompressorParams(dspCompThreshKnob.value, dspCompRatioKnob.value, dspCompAttackKnob.value, dspCompReleaseKnob.value, dspCompMakeupKnob.value, v) }
                                        }
                                        ConsoleKnob {
                                            id: dspCompAttackKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("Attack")
                                            minValue: 0.5; maxValue: 150.0; defaultValue: 10.0; logarithmic: true
                                            unitSuffix: " ms"; decimals: 1; knobDiameter: 44; arcColor: "#F5B841"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setCompressorParams(dspCompThreshKnob.value, dspCompRatioKnob.value, v, dspCompReleaseKnob.value, dspCompMakeupKnob.value, dspCompKneeKnob.value) }
                                        }
                                        ConsoleKnob {
                                            id: dspCompReleaseKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("Release")
                                            minValue: 10.0; maxValue: 1000.0; defaultValue: 120.0; logarithmic: true
                                            unitSuffix: " ms"; decimals: 0; knobDiameter: 44; arcColor: "#F5B841"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setCompressorParams(dspCompThreshKnob.value, dspCompRatioKnob.value, dspCompAttackKnob.value, v, dspCompMakeupKnob.value, dspCompKneeKnob.value) }
                                        }
                                        ConsoleKnob {
                                            id: dspCompMakeupKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("Makeup")
                                            minValue: -12.0; maxValue: 24.0; defaultValue: 0.0; logarithmic: false; bipolar: true
                                            unitSuffix: " dB"; decimals: 1; knobDiameter: 44; arcColor: "#F5B841"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setCompressorParams(dspCompThreshKnob.value, dspCompRatioKnob.value, dspCompAttackKnob.value, dspCompReleaseKnob.value, v, dspCompKneeKnob.value) }
                                        }
                                    }

                                    Rectangle { width: 1; Layout.fillHeight: true; color: "#363E4A" }
                                    ColumnLayout {
                                        Layout.preferredWidth: 102
                                        Layout.fillHeight: true
                                        spacing: 4
                                        ConsoleKnob {
                                            id: compInTrimKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("IN")
                                            minValue: -12.0; maxValue: 12.0; defaultValue: 0.0; logarithmic: false; bipolar: true
                                            unitSuffix: "dB"; decimals: 1; knobDiameter: 44; arcColor: "#F5B841"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setDspTrim("compressor", false, v) }
                                        }
                                        ConsoleKnob {
                                            id: compOutTrimKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("OUT")
                                            minValue: -12.0; maxValue: 12.0; defaultValue: 0.0; logarithmic: false; bipolar: true
                                            unitSuffix: "dB"; decimals: 1; knobDiameter: 44; arcColor: "#F5B841"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setDspTrim("compressor", true, v) }
                                        }
                                    }
                                }
                            }
                        }

                        // MODULE 3: DARK AMBER-YELLOW LIMITER [SAMPLE-PEAK]
                        BrushedMetalPanel {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            baseTint: "#2C230E"
                            borderColor: "#8A6D22"
                            showScrews: true
                            cornerRadius: 5
                            brushedOpacity: 0.34

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 12
                                spacing: 8

                                // Row 1: Uniform Title Header
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 26
                                    spacing: 6
                                    Label {
                                        text: qsTr("LIMITER")
                                        font.pixelSize: 15
                                        font.bold: true
                                        color: "#F5D270"
                                    }
                                    Rectangle {
                                        implicitWidth: spLbl.implicitWidth + 10
                                        height: 18
                                        radius: 2
                                        color: "#1B1508"
                                        border.color: "#A88628"
                                        Label {
                                            id: spLbl
                                            anchors.centerIn: parent
                                            text: "SAMPLE-PEAK"
                                            font.family: "Monospace"
                                            font.pixelSize: 9
                                            font.bold: true
                                            color: "#FCE8A6"
                                        }
                                    }
                                    Item { Layout.fillWidth: true }
                                    ConsoleButton {
                                        text: consoleRoot.dspLimBypassed ? qsTr("BYP") : qsTr("ON")
                                        implicitWidth: 48; implicitHeight: 24; fontSize: 10
                                        showLed: true; ledColor: consoleRoot.dspLimBypassed ? "#555" : "#00E676"
                                        highlighted: !consoleRoot.dspLimBypassed
                                        enabled: !consoleRoot.isDirect
                                        onClicked: EngineController.setDspNodeBypass("limiter", !consoleRoot.dspLimBypassed)
                                    }
                                }

                                // Row 2: Uniform 68px OLED Peak Limiting Display Window
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 68
                                    radius: 4
                                    color: "#110E06"
                                    border.color: "#6B541B"
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 7
                                        spacing: 4

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Label { text: qsTr("LIMITING ATTENUATION"); font.family: "Monospace"; font.pixelSize: 9; font.bold: true; color: "#E2BE5B" }
                                            Item { Layout.fillWidth: true }
                                            Label { text: consoleRoot.dspLimGainReductionDb.toFixed(1) + " dB"; font.family: "Monospace"; font.pixelSize: 11; font.bold: true; color: "#FFD966" }
                                        }

                                        ConsoleLedMeter {
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 22
                                            horizontal: true
                                            segmentCount: 24
                                            peakValue: Math.min(1.0, Math.abs(consoleRoot.dspLimGainReductionDb) / 12.0)
                                            rmsValue: peakValue * 0.8
                                            greenColor: "#F2C044"
                                            amberColor: "#FF9800"
                                            redColor: "#FF5252"
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Label { text: "0"; font.family: "Monospace"; font.pixelSize: 8; color: "#8C7437" }
                                            Item { Layout.fillWidth: true }
                                            Label { text: "-3"; font.family: "Monospace"; font.pixelSize: 8; color: "#8C7437" }
                                            Item { Layout.fillWidth: true }
                                            Label { text: "-6"; font.family: "Monospace"; font.pixelSize: 8; color: "#8C7437" }
                                            Item { Layout.fillWidth: true }
                                            Label { text: "-9"; font.family: "Monospace"; font.pixelSize: 8; color: "#8C7437" }
                                            Item { Layout.fillWidth: true }
                                            Label { text: "-12 dB"; font.family: "Monospace"; font.pixelSize: 8; color: "#8C7437" }
                                        }
                                    }
                                }

                                // Row 3: Parameters + IN/OUT Trim
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    spacing: 8

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        spacing: 6
                                        ConsoleKnob {
                                            id: dspLimCeilKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("Ceiling")
                                            minValue: -12.0; maxValue: 0.0; defaultValue: -0.3; logarithmic: false
                                            unitSuffix: " dB"; decimals: 1; knobDiameter: 66; arcColor: "#F2C044"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setLimiterParams(v, dspLimRelKnob.value, dspLimLookKnob.value) }
                                        }
                                        ConsoleKnob {
                                            id: dspLimRelKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("Release")
                                            minValue: 5.0; maxValue: 500.0; defaultValue: 50.0; logarithmic: true
                                            unitSuffix: " ms"; decimals: 0; knobDiameter: 66; arcColor: "#F2C044"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setLimiterParams(dspLimCeilKnob.value, v, dspLimLookKnob.value) }
                                        }
                                        ConsoleKnob {
                                            id: dspLimLookKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("Lookahead")
                                            minValue: 0.0; maxValue: 10.0; defaultValue: 1.5; logarithmic: false
                                            unitSuffix: " ms"; decimals: 1; knobDiameter: 54; arcColor: "#F2C044"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setLimiterParams(dspLimCeilKnob.value, dspLimRelKnob.value, v) }
                                        }
                                    }

                                    Rectangle { width: 1; Layout.fillHeight: true; color: "#594718" }
                                    ColumnLayout {
                                        Layout.preferredWidth: 102
                                        Layout.fillHeight: true
                                        spacing: 4
                                        ConsoleKnob {
                                            id: limInTrimKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("IN")
                                            minValue: -12.0; maxValue: 12.0; defaultValue: 0.0; logarithmic: false; bipolar: true
                                            unitSuffix: "dB"; decimals: 1; knobDiameter: 44; arcColor: "#F2C044"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setDspTrim("limiter", false, v) }
                                        }
                                        ConsoleKnob {
                                            id: limOutTrimKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("OUT")
                                            minValue: -12.0; maxValue: 12.0; defaultValue: 0.0; logarithmic: false; bipolar: true
                                            unitSuffix: "dB"; decimals: 1; knobDiameter: 44; arcColor: "#F2C044"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setDspTrim("limiter", true, v) }
                                        }
                                    }
                                }
                            }
                        }

                        // MODULE 4: DEEP TEAL REVERB
                        BrushedMetalPanel {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            baseTint: "#112931"
                            borderColor: "#2C6373"
                            showScrews: true
                            cornerRadius: 5
                            brushedOpacity: 0.34

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 12
                                spacing: 8

                                // Row 1: Uniform Title Header
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 26
                                    spacing: 8
                                    Label {
                                        text: qsTr("REVERB")
                                        font.pixelSize: 15
                                        font.bold: true
                                        color: "#6BE0FF"
                                    }
                                    Item { Layout.fillWidth: true }
                                    ConsoleButton {
                                        text: consoleRoot.dspRevBypassed ? qsTr("BYP") : qsTr("ON")
                                        implicitWidth: 48; implicitHeight: 24; fontSize: 10
                                        showLed: true; ledColor: consoleRoot.dspRevBypassed ? "#555" : "#00E676"
                                        highlighted: !consoleRoot.dspRevBypassed
                                        enabled: !consoleRoot.isDirect
                                        onClicked: EngineController.setDspNodeBypass("reverb", !consoleRoot.dspRevBypassed)
                                    }
                                }

                                // Row 2: Uniform 68px OLED Reverb Decay Envelope Window
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 68
                                    radius: 4
                                    color: "#050B0E"
                                    border.color: "#224E5B"
                                    border.width: 1

                                    Canvas {
                                        id: reverbMiniCanvas
                                        anchors.fill: parent
                                        anchors.margins: 5
                                        onWidthChanged: requestPaint()
                                        onHeightChanged: requestPaint()
                                        onPaint: {
                                            var ctx = getContext("2d")
                                            if (!ctx || width <= 4 || height <= 4) return
                                            ctx.clearRect(0, 0, width, height)

                                            ctx.strokeStyle = "#122B33"
                                            ctx.lineWidth = 1
                                            ctx.beginPath()
                                            ctx.moveTo(0, height * 0.5); ctx.lineTo(width, height * 0.5)
                                            ctx.moveTo(width * 0.5, 0); ctx.lineTo(width * 0.5, height)
                                            ctx.stroke()

                                            ctx.strokeStyle = "#5CE0FF"
                                            ctx.fillStyle = "rgba(92, 224, 255, 0.22)"
                                            ctx.lineWidth = 2.0
                                            ctx.beginPath()
                                            ctx.moveTo(0, height)
                                            var preX = Math.min(width * 0.25, (dspRevPreKnob.value / 100.0) * width * 0.25)
                                            var decay = 1.5 + (1.0 - dspRevRoomKnob.value) * 4.5
                                            for (var x = preX; x <= width; x += 2) {
                                                var t = (x - preX) / Math.max(1, width - preX)
                                                var amp = Math.exp(-t * decay)
                                                var y = height - amp * (height - 6)
                                                ctx.lineTo(x, y)
                                            }
                                            ctx.lineTo(width, height)
                                            ctx.closePath()
                                            ctx.fill()
                                            ctx.stroke()
                                        }
                                    }

                                    Label {
                                        anchors.top: parent.top
                                        anchors.right: parent.right
                                        anchors.margins: 5
                                        text: qsTr("TAIL DECAY ENVELOPE")
                                        font.family: "Monospace"
                                        font.pixelSize: 8
                                        color: "#4F8C9E"
                                    }
                                }

                                // Row 3: Parameters + IN/OUT Trim
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    spacing: 6

                                    GridLayout {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        columns: 3
                                        rowSpacing: 2
                                        columnSpacing: 2

                                        ConsoleKnob {
                                            id: dspRevPreKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("Pre-Delay")
                                            minValue: 0.0; maxValue: 100.0; defaultValue: 12.0; logarithmic: false
                                            unitSuffix: " ms"; decimals: 0; knobDiameter: 44; arcColor: "#48D8FF"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setReverbParams(dspRevRoomKnob.value, dspRevDampKnob.value, dspRevWetKnob.value, v, consoleRoot.dspRevTailPreserving); reverbMiniCanvas.requestPaint() }
                                        }
                                        ConsoleKnob {
                                            id: dspRevRoomKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("Size")
                                            minValue: 0.0; maxValue: 1.0; defaultValue: 0.35; logarithmic: false
                                            unitSuffix: ""; decimals: 2; knobDiameter: 44; arcColor: "#48D8FF"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setReverbParams(v, dspRevDampKnob.value, dspRevWetKnob.value, dspRevPreKnob.value, consoleRoot.dspRevTailPreserving); reverbMiniCanvas.requestPaint() }
                                        }
                                        ConsoleKnob {
                                            id: dspRevDampKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("Damping")
                                            minValue: 0.0; maxValue: 1.0; defaultValue: 0.5; logarithmic: false
                                            unitSuffix: ""; decimals: 2; knobDiameter: 44; arcColor: "#48D8FF"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setReverbParams(dspRevRoomKnob.value, v, dspRevWetKnob.value, dspRevPreKnob.value, consoleRoot.dspRevTailPreserving) }
                                        }
                                        ConsoleButton {
                                            Layout.fillWidth: true
                                            implicitHeight: 36
                                            text: consoleRoot.dspRevTailPreserving ? qsTr("TAIL-\nPRESERVE") : qsTr("IMMEDIATE")
                                            fontSize: 8
                                            highlighted: consoleRoot.dspRevTailPreserving
                                            accentColor: "#48D8FF"
                                            enabled: !consoleRoot.isDirect
                                            onClicked: {
                                                consoleRoot.dspRevTailPreserving = !consoleRoot.dspRevTailPreserving
                                                EngineController.setReverbParams(dspRevRoomKnob.value, dspRevDampKnob.value, dspRevWetKnob.value, dspRevPreKnob.value, consoleRoot.dspRevTailPreserving)
                                            }
                                        }
                                        ConsoleKnob {
                                            id: dspRevWetKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("Wet/Dry")
                                            minValue: 0.0; maxValue: 1.0; defaultValue: 0.15; logarithmic: false
                                            unitSuffix: ""; decimals: 2; knobDiameter: 44; arcColor: "#48D8FF"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setReverbParams(dspRevRoomKnob.value, dspRevDampKnob.value, v, dspRevPreKnob.value, consoleRoot.dspRevTailPreserving) }
                                        }
                                        Item { Layout.fillWidth: true }
                                    }

                                    Rectangle { width: 1; Layout.fillHeight: true; color: "#244D5A" }
                                    ColumnLayout {
                                        Layout.preferredWidth: 102
                                        Layout.fillHeight: true
                                        spacing: 4
                                        ConsoleKnob {
                                            id: revInTrimKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("IN")
                                            minValue: -12.0; maxValue: 12.0; defaultValue: 0.0; logarithmic: false; bipolar: true
                                            unitSuffix: "dB"; decimals: 1; knobDiameter: 44; arcColor: "#48D8FF"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setDspTrim("reverb", false, v) }
                                        }
                                        ConsoleKnob {
                                            id: revOutTrimKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("OUT")
                                            minValue: -12.0; maxValue: 12.0; defaultValue: 0.0; logarithmic: false; bipolar: true
                                            unitSuffix: "dB"; decimals: 1; knobDiameter: 44; arcColor: "#48D8FF"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setDspTrim("reverb", true, v) }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // =========================================================
                    // TAB 3: EXTERNAL HOST — 3 Studio Matrix Columns (Mockup w58k)
                    // =========================================================
                    RowLayout {
                        spacing: 10

                        // COLUMN 1: Ports & Visual Bezier Patch Matrix
                        BrushedMetalPanel {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            baseTint: "#161B22"
                            borderColor: "#333D4A"
                            showScrews: true
                            cornerRadius: 5

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 12
                                spacing: 7

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6
                                    Label { text: qsTr("GRAPH:"); font.family: "Monospace"; font.pixelSize: 11; color: "#9AA9B8" }
                                    Rectangle {
                                        Layout.fillWidth: true; height: 26; radius: 3; color: "#0B0E12"; border.color: "#2E3744"
                                        Label {
                                            anchors.verticalCenter: parent.verticalCenter
                                            anchors.left: parent.left; anchors.leftMargin: 8
                                            text: (EngineController.audioBackend || "PipeWire") + " (Live)"
                                            font.pixelSize: 11; color: "#DCE6F0"
                                        }
                                    }
                                    ConsoleButton {
                                        text: qsTr("SCAN PORTS")
                                        implicitWidth: 96; implicitHeight: 26; fontSize: 10
                                        onClicked: {
                                            consoleRoot.portDiscoveryList = EngineController.discoverExternalInsertPorts()
                                            patchBayCanvas.requestPaint()
                                        }
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6
                                    Label { text: qsTr("TARGET CLIENT:"); font.family: "Monospace"; font.pixelSize: 10; color: "#9AA9B8" }
                                    ComboBox {
                                        Layout.fillWidth: true
                                        implicitHeight: 26
                                        model: consoleRoot.portDiscoveryList.length > 0
                                               ? consoleRoot.portDiscoveryList
                                               : [qsTr("No External Insert Ports Discovered")]
                                    }
                                }

                                // Visual Patch-Bay Cable Screen
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    radius: 4
                                    color: "#06090D"
                                    border.color: "#222B35"

                                    Canvas {
                                        id: patchBayCanvas
                                        anchors.fill: parent
                                        anchors.margins: 8
                                        onWidthChanged: requestPaint()
                                        onHeightChanged: requestPaint()
                                        onPaint: {
                                            var ctx = getContext("2d")
                                            if (!ctx || width <= 20 || height <= 20) return
                                            ctx.clearRect(0, 0, width, height)

                                            var leftLabels = ["SEND L", "SEND R", "RETURN L", "RETURN R"]
                                            var connected = EngineController.externalInsertState >= 2
                                            var rightLabels = connected
                                                              ? ["ext:in_1", "ext:in_2", "ext:out_1", "ext:out_2"]
                                                              : [qsTr("(unpatched)"), qsTr("(unpatched)"), qsTr("(unpatched)"), qsTr("(unpatched)")]
                                            ctx.font = "11px Monospace"
                                            for (var i = 0; i < 4; ++i) {
                                                var y = 16 + (i / 3.0) * (height - 32)
                                                ctx.fillStyle = "#B8C6D4"
                                                ctx.fillText(leftLabels[i], 4, y + 4)

                                                ctx.fillStyle = connected ? "#00E676" : "#44515E"
                                                ctx.beginPath()
                                                ctx.arc(82, y, 4.5, 0, Math.PI * 2)
                                                ctx.fill()

                                                ctx.fillStyle = connected ? "#00E676" : "#44515E"
                                                ctx.beginPath()
                                                ctx.arc(width - 135, y, 4.5, 0, Math.PI * 2)
                                                ctx.fill()

                                                ctx.fillStyle = connected ? "#DCE6F0" : "#5F6C7A"
                                                ctx.fillText(rightLabels[i], width - 125, y + 4)

                                                // Bezier Patch Line (glowing green when connected, dim dashed when unpatched)
                                                ctx.strokeStyle = connected ? "rgba(0, 230, 118, 0.75)" : "rgba(95, 108, 122, 0.35)"
                                                ctx.lineWidth = connected ? 2.4 : 1.2
                                                ctx.beginPath()
                                                ctx.moveTo(86, y)
                                                ctx.bezierCurveTo(width * 0.42, y, width * 0.56, y, width - 139, y)
                                                ctx.stroke()
                                            }
                                        }
                                    }
                                }

                                Label {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: qsTr("DISCONNECT POLICY: SAFE BYPASS (Dry)")
                                    font.family: "Monospace"
                                    font.pixelSize: 10
                                    color: "#9AA9B8"
                                }
                            }
                        }

                        // COLUMN 2: SIGNAL FLOW & INSERT STATE (Strict Master Chain Order!)
                        BrushedMetalPanel {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            baseTint: "#161B22"
                            borderColor: "#333D4A"
                            showScrews: true
                            cornerRadius: 5

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 12
                                spacing: 7

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6
                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("SIGNAL FLOW & INSERT STATE")
                                        font.family: "Monospace"
                                        font.pixelSize: 12
                                        font.bold: true
                                        color: "#E8BA6B"
                                    }
                                    Rectangle {
                                        implicitWidth: bpExclLbl.implicitWidth + 12
                                        height: 22; radius: 3; color: "#2D1416"; border.color: "#913238"
                                        Label {
                                            id: bpExclLbl
                                            anchors.centerIn: parent
                                            text: qsTr("BIT-PERFECT: EXCLUDED")
                                            font.family: "Monospace"; font.pixelSize: 9; font.bold: true; color: "#FF9AA0"
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true; height: 24; radius: 3; color: "#0B1914"; border.color: "#1F573F"
                                    Label {
                                        anchors.centerIn: parent
                                        text: qsTr("STATUS: %1").arg(EngineController.externalInsertStateText.toUpperCase())
                                        font.family: "Monospace"; font.pixelSize: 10; font.bold: true
                                        color: EngineController.externalInsertState >= 2 ? "#00E676" : "#8FA0B0"
                                    }
                                }

                                // Real Master Chain Slot Selector (Slot 0..6 around PEQ -> GEQ -> HPF/LPF -> COMP -> REV -> LIM)
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 54
                                    radius: 4; color: "#06090D"; border.color: "#222B35"

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 6
                                        spacing: 4
                                        Label {
                                            Layout.alignment: Qt.AlignHCenter
                                            text: qsTr("PEQ → GEQ → HPF/LPF → COMP → REV → LIM  (Insert @ Slot %1)").arg(EngineController.externalInsertPosition)
                                            font.family: "Monospace"; font.pixelSize: 9; color: "#9AA9B8"
                                        }
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 3
                                            Repeater {
                                                model: ["S0", "S1", "S2", "S3", "S4", "S5", "S6"]
                                                ConsoleButton {
                                                    Layout.fillWidth: true
                                                    implicitHeight: 20
                                                    text: modelData
                                                    fontSize: 8
                                                    accentColor: "#00E5FF"
                                                    highlighted: EngineController.externalInsertPosition === index
                                                    enabled: !consoleRoot.isDirect
                                                    onClicked: EngineController.setExternalInsertPosition(index)
                                                }
                                            }
                                        }
                                    }
                                }

                                // Send & Return LED Meters
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 4
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label { text: qsTr("SEND LEVEL"); font.family: "Monospace"; font.pixelSize: 9; color: "#9AA9B8"; Layout.preferredWidth: 88 }
                                        ConsoleLedMeter { Layout.fillWidth: true; Layout.preferredHeight: 10; horizontal: true; segmentCount: 24; peakValue: EngineController.masterPeakL }
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label { text: qsTr("RETURN LEVEL"); font.family: "Monospace"; font.pixelSize: 9; color: "#9AA9B8"; Layout.preferredWidth: 88 }
                                        ConsoleLedMeter { Layout.fillWidth: true; Layout.preferredHeight: 10; horizontal: true; segmentCount: 24; peakValue: EngineController.masterPeakR }
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    ConsoleButton {
                                        Layout.fillWidth: true
                                        implicitHeight: 34
                                        text: qsTr("CONNECT / RECONNECT")
                                        fontSize: 10
                                        accentColor: "#00E676"
                                        onClicked: consoleRoot.portDiscoveryList = EngineController.discoverExternalInsertPorts()
                                    }
                                    ConsoleButton {
                                        Layout.fillWidth: true
                                        implicitHeight: 34
                                        text: EngineController.externalInsertBypassed ? qsTr("INSERT BYPASS: ON") : qsTr("INSERT BYPASS (DRY)")
                                        fontSize: 10
                                        accentColor: "#F5B841"
                                        highlighted: EngineController.externalInsertBypassed
                                        enabled: !consoleRoot.isDirect
                                        onClicked: EngineController.setExternalInsertBypass(!EngineController.externalInsertBypassed)
                                    }
                                }
                            }
                        }

                        // COLUMN 3: LATENCY & DIAGNOSTICS (Mockup w58k)
                        BrushedMetalPanel {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            baseTint: "#161B22"
                            borderColor: "#333D4A"
                            showScrews: true
                            cornerRadius: 5

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 12
                                spacing: 8

                                Label {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: qsTr("LATENCY & DIAGNOSTICS")
                                    font.family: "Monospace"
                                    font.pixelSize: 12
                                    font.bold: true
                                    color: "#BDC9D6"
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    spacing: 6

                                    Rectangle {
                                        Layout.fillWidth: true; Layout.fillHeight: true; radius: 4; color: "#090D12"; border.color: "#28323E"
                                        Column {
                                            anchors.centerIn: parent; spacing: 4
                                            Label { anchors.horizontalCenter: parent.horizontalCenter; text: qsTr("REPORTED LATENCY"); font.family: "Monospace"; font.pixelSize: 9; color: "#7E8E9E" }
                                            Label {
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                text: EngineController.externalInsertReportedLatency > 0
                                                      ? (EngineController.externalInsertReportedLatency + " frames")
                                                      : qsTr("Not Reported")
                                                font.pixelSize: 13; font.bold: true; color: "#E2ECF5"
                                            }
                                        }
                                    }

                                    Rectangle {
                                        Layout.fillWidth: true; Layout.fillHeight: true; radius: 4; color: "#090D12"; border.color: "#28323E"
                                        Column {
                                            anchors.centerIn: parent; spacing: 4
                                            Label { anchors.horizontalCenter: parent.horizontalCenter; text: qsTr("MEASURED ROUND-TRIP"); font.family: "Monospace"; font.pixelSize: 9; color: "#7E8E9E" }
                                            Label {
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                text: EngineController.externalInsertMeasuredLatency >= 0
                                                      ? (EngineController.externalInsertMeasuredLatency + " fr / " + EngineController.externalInsertMeasuredLatencyMs.toFixed(2) + " ms")
                                                      : qsTr("Not Measured")
                                                font.pixelSize: 13; font.bold: true; color: EngineController.externalInsertMeasuredLatency >= 0 ? "#E2ECF5" : "#7E8E9E"
                                            }
                                        }
                                    }

                                    Rectangle {
                                        Layout.fillWidth: true; Layout.fillHeight: true; radius: 4; color: "#090D12"; border.color: "#28323E"
                                        Column {
                                            anchors.centerIn: parent; spacing: 4
                                            Label { anchors.horizontalCenter: parent.horizontalCenter; text: qsTr("APPLIED ALIGNMENT"); font.family: "Monospace"; font.pixelSize: 9; color: "#7E8E9E" }
                                            Label {
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                text: EngineController.externalInsertDelayCompensation + " frames"
                                                font.pixelSize: 13; font.bold: true; color: "#E2ECF5"
                                            }
                                        }
                                    }
                                }

                                ProgressBar {
                                    Layout.fillWidth: true
                                    value: EngineController.externalInsertMeasuredLatency >= 0 ? 1.0 : 0.0
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    ConsoleButton {
                                        Layout.fillWidth: true
                                        implicitHeight: 34
                                        text: qsTr("1. MEASURE IMPULSE")
                                        fontSize: 10
                                        accentColor: "#00E5FF"
                                        enabled: !consoleRoot.isDirect
                                        onClicked: EngineController.calibrateInsertLatency()
                                    }
                                    ConsoleButton {
                                        Layout.fillWidth: true
                                        implicitHeight: 34
                                        text: qsTr("2. APPROVE & APPLY")
                                        fontSize: 10
                                        accentColor: "#00E676"
                                        enabled: !consoleRoot.isDirect && EngineController.externalInsertMeasuredLatency >= 0
                                        onClicked: EngineController.confirmRecommendedInsertLatency(EngineController.externalInsertMeasuredLatency)
                                    }
                                }

                                Label {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: qsTr("%1 kHz   |   Measured: %2   |   XRUN: %3   |   Loop Guard: ARMED")
                                          .arg((Math.round(EngineController.graphSampleRate / 100) / 10).toFixed(1))
                                          .arg(EngineController.externalInsertMeasuredLatency >= 0 ? (EngineController.externalInsertMeasuredLatency + " fr") : qsTr("Not Measured"))
                                          .arg(EngineController.externalInsertXRunCount)
                                    font.family: "Monospace"
                                    font.pixelSize: 10
                                    color: "#A3B2C2"
                                }
                            }
                        }
                    }
                }
            }

            // CENTERED DOCKED TAB STRIP PROTRUDING CLEANLY ACROSS LOWER PANEL TOP SEAM (Exact Mockup Architecture!)
            Rectangle {
                id: opTabBarContainer
                anchors.top: parent.top
                anchors.horizontalCenter: parent.horizontalCenter
                width: opTabRow.implicitWidth + 6
                height: 36
                radius: 4
                color: "#090c10"
                border.color: "#434d5c"
                border.width: 1
                z: 10

                RowLayout {
                    id: opTabRow
                    anchors.fill: parent
                    anchors.margins: 2
                    spacing: 2

                    Repeater {
                        model: [
                            qsTr("32-GEQ"),
                            qsTr("32-PEQ"),
                            qsTr("DSP"),
                            qsTr("External Host")
                        ]
                        ConsoleButton {
                            text: modelData
                            implicitWidth: index === 3 ? 132 : 112
                            implicitHeight: 32
                            fontSize: 12
                            cornerRadius: 3
                            highlighted: consoleRoot.currentOpTab === index
                            accentColor: index === 3 ? "#5BE7FF" : "#00E5FF"
                            activeTopColor: "#2C3846"
                            activeBottomColor: "#16222D"
                            normalTopColor: "#242A34"
                            normalBottomColor: "#161A21"
                            onClicked: consoleRoot.currentOpTab = index
                        }
                    }
                }
            }

            // Right-Side Direct Mode Lock Notice Pill
            Rectangle {
                visible: consoleRoot.isDirect
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.rightMargin: 12
                implicitWidth: directWarnLbl.implicitWidth + 18
                height: 28
                radius: 3
                color: "#36180B"
                border.color: "#F5B841"
                z: 10
                Label {
                    id: directWarnLbl
                    anchors.centerIn: parent
                    text: qsTr("BYPASSED BY DIRECT MODE")
                    font.family: "Monospace"
                    font.pixelSize: 10
                    font.bold: true
                    color: "#F5B841"
                }
            }
        }
    }
}
