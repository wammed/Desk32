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
        if (state === 4) return "#00E5FF"
        if (state === 3) return "#00E676"
        if (state === 2) return "#F5B841"
        if (state === 1) return "#8A98A8"
        return "#FF5252"
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

    // Waveform, Live FFT & Interactive PEQ Curve State
    property var cachedWaveData: ({})
    property var cachedSpectrumBins: []
    property var spectrumPeakHold: []
    property bool spectrumHasSignal: false
    property bool showGeqOverlayOnPeq: false
    property int draggedPeqBand: -1
    property bool draggedPeqIsRight: false
    property int hoveredPeqBand: -1
    property bool hoveredPeqIsRight: false
    property var cachedPeqNodes: []

    function refreshWaveform() {
        var w = (typeof waveCanvas !== "undefined" && waveCanvas.width > 64)
                ? Math.min(1024, Math.max(200, Math.round(waveCanvas.width / 2.0)))
                : 360
        cachedWaveData = EngineController.getDeckWaveformPeaks(EngineController.activeDeckId, w)
        if (typeof waveCanvas !== "undefined") waveCanvas.requestPaint()
    }

    function pollSpectrum() {
        var bins = EngineController.getSpectrumBins(monitorChannelMode, 160, peqRightChannel)
        if (!cachedSpectrumBins || cachedSpectrumBins.length !== bins.length) {
            cachedSpectrumBins = bins.slice(0)
        } else {
            var nextBins = cachedSpectrumBins.slice(0)
            for (var k = 0; k < bins.length; ++k) {
                nextBins[k] = (bins[k] >= nextBins[k]) ? bins[k] : Math.max(bins[k], nextBins[k] - 3.0)
            }
            cachedSpectrumBins = nextBins
        }
        if (!spectrumPeakHold || spectrumPeakHold.length !== bins.length) {
            spectrumPeakHold = bins.slice(0)
        } else {
            var nextHold = spectrumPeakHold.slice(0)
            for (var i = 0; i < bins.length; ++i) {
                var decayStep = (bins[i] <= -88.0) ? 1.4 : 0.65
                nextHold[i] = Math.max(bins[i], nextHold[i] - decayStep)
            }
            spectrumPeakHold = nextHold
        }
        var maxV = -90.0
        for (var j = 0; j < spectrumPeakHold.length; ++j) {
            if (spectrumPeakHold[j] > maxV) maxV = spectrumPeakHold[j]
            if (cachedSpectrumBins[j] > maxV) maxV = cachedSpectrumBins[j]
        }
        if (maxV > -86.0) {
            spectrumHasSignal = true
        } else {
            spectrumHasSignal = false
            for (var z = 0; z < cachedSpectrumBins.length; ++z) {
                cachedSpectrumBins[z] = -90.0
                spectrumPeakHold[z] = -90.0
            }
        }
        if (typeof monitorCanvas !== "undefined") monitorCanvas.requestPaint()
    }

    function computeVisiblePeqNodes(plotW, plotH, padL, padT) {
        var nodes = []
        if (currentOpTab !== 1 || !showEqOverlay) return nodes
        var channels = []
        if (monitorChannelMode === 0) channels = [false, true]
        else if (monitorChannelMode === 1) channels = [false]
        else if (monitorChannelMode === 2) channels = [true]
        else channels = [peqRightChannel]

        var logMin = Math.log(20.0)
        var logSpan = Math.log(20000.0) - logMin
        for (var ci = 0; ci < channels.length; ++ci) {
            var isR = channels[ci]
            for (var b = 0; b < 16; ++b) {
                var bd = peqEditListening
                         ? EngineController.getListeningBand(isR, b)
                         : EngineController.getPeqBand(isR, b)
                if (!bd || !bd.enabled) continue
                var f = Math.max(20.0, Math.min(20000.0, bd.frequency))
                var g = Math.max(-18.0, Math.min(18.0, bd.gainDb))
                var tx = padL + ((Math.log(f) - logMin) / logSpan) * plotW
                var ty = padT + ((18.0 - g) / 36.0) * plotH
                var ox = tx
                var oy = ty
                for (var p = 0; p < nodes.length; ++p) {
                    var dx = ox - nodes[p].drawX
                    var dy = oy - nodes[p].drawY
                    if ((dx * dx + dy * dy) < 169.0) {
                        var ang = (nodes.length % 6) * (Math.PI / 3.0)
                        ox = tx + Math.cos(ang) * 7.5
                        oy = ty + Math.sin(ang) * 7.5
                    }
                }
                nodes.push({
                    isRight: isR,
                    band: b,
                    frequency: f,
                    gainDb: g,
                    q: bd.q !== undefined ? bd.q : 1.0,
                    type: bd.type !== undefined ? bd.type : 0,
                    trueX: tx,
                    trueY: ty,
                    drawX: ox,
                    drawY: oy
                })
            }
        }
        return nodes
    }

    function refreshExternalPorts() {
        var list = EngineController.discoverExternalInsertPorts()
        portDiscoveryList = list ? list : []
        var curClient = EngineController.externalInsertClientName
        var matched = 0
        for (var i = 0; i < portDiscoveryList.length; ++i) {
            if (portDiscoveryList[i].clientName === curClient) {
                matched = i
                break
            }
        }
        selectedTargetClientIdx = matched
        if (typeof patchBayCanvas !== "undefined") patchBayCanvas.requestPaint()
    }

    function connectSelectedExternalHost() {
        if (!portDiscoveryList || portDiscoveryList.length === 0) {
            refreshExternalPorts()
        }
        if (!portDiscoveryList || portDiscoveryList.length === 0) return false
        var idx = Math.max(0, Math.min(portDiscoveryList.length - 1, selectedTargetClientIdx))
        var item = portDiscoveryList[idx]
        if (!item || !item.clientName) return false
        var ok = EngineController.configureExternalInsertWithMetadata(
            item.clientName,
            item.sendPortL,
            item.sendPortR,
            item.returnPortL,
            item.returnPortR,
            item.sampleRate > 0 ? item.sampleRate : EngineController.graphSampleRate,
            item.bufferSize > 0 ? item.bufferSize : 256,
            item.reportedLatencyFrames !== undefined ? item.reportedLatencyFrames : -1,
            !!item.latencyReported,
            item.clientName,
            "",
            ""
        )
        if (typeof patchBayCanvas !== "undefined") patchBayCanvas.requestPaint()
        return ok
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
            if (!peqFreqKnob.isDragging) peqFreqKnob.value = b.frequency
            if (!peqGainKnob.isDragging) peqGainKnob.value = b.gainDb
            if (!peqQKnob.isDragging) peqQKnob.value = b.q
            peqBandTypeIndex = b.type !== undefined ? b.type : 0
            peqBandEnabledState = !!b.enabled
        }
        if (!peqInTrimKnob.isDragging)
            peqInTrimKnob.value = consoleRoot.peqEditListening ? 0.0 : EngineController.getDspTrim("peq", false)
        if (!peqOutTrimKnob.isDragging)
            peqOutTrimKnob.value = consoleRoot.peqEditListening ? 0.0 : EngineController.getDspTrim("peq", true)
        peqGridTick++
        peqMiniCanvas.requestPaint()
    }

    function refreshGeqFaders() {
        for (var i = 0; i < 16; ++i) {
            var lf = geqLeftRepeater.itemAt(i)
            if (lf) {
                if (!lf.isDragging) lf.value = EngineController.getGeqBandGain(i)
                lf.bandEnabled = EngineController.isGeqBandEnabled(i)
            }
            var rf = geqRightRepeater.itemAt(i)
            if (rf) {
                if (!rf.isDragging) rf.value = EngineController.getGeqBandGain(i + 16)
                rf.bandEnabled = EngineController.isGeqBandEnabled(i + 16)
            }
        }
    }

    function refreshDspRack() {
        var fp = EngineController.getFilterParams()
        if (fp) {
            if (!dspHpfKnob.isDragging) dspHpfKnob.value = fp.hpfFrequency !== undefined ? fp.hpfFrequency : 20.0
            if (!dspLpfKnob.isDragging) dspLpfKnob.value = fp.lpfFrequency !== undefined ? fp.lpfFrequency : 20000.0
            if (!dspHpfQKnob.isDragging) dspHpfQKnob.value = fp.hpfQ !== undefined ? fp.hpfQ : 0.71
            if (!dspLpfQKnob.isDragging) dspLpfQKnob.value = fp.lpfQ !== undefined ? fp.lpfQ : 0.71
            dspHpfEnabled = !!fp.hpfEnabled
            dspLpfEnabled = !!fp.lpfEnabled
            dspHpfSlope = fp.hpfSlope !== undefined ? fp.hpfSlope : 0
            dspLpfSlope = fp.lpfSlope !== undefined ? fp.lpfSlope : 0
            dspFilterBypassed = !!fp.bypassed
        }
        var cp = EngineController.getCompressorParams()
        if (cp) {
            if (!dspCompThreshKnob.isDragging) dspCompThreshKnob.value = cp.thresholdDb !== undefined ? cp.thresholdDb : -12.0
            if (!dspCompRatioKnob.isDragging) dspCompRatioKnob.value = cp.ratio !== undefined ? cp.ratio : 2.0
            if (!dspCompAttackKnob.isDragging) dspCompAttackKnob.value = cp.attackMs !== undefined ? cp.attackMs : 10.0
            if (!dspCompReleaseKnob.isDragging) dspCompReleaseKnob.value = cp.releaseMs !== undefined ? cp.releaseMs : 120.0
            if (!dspCompMakeupKnob.isDragging) dspCompMakeupKnob.value = cp.makeupGainDb !== undefined ? cp.makeupGainDb : 0.0
            if (!dspCompKneeKnob.isDragging) dspCompKneeKnob.value = cp.kneeDb !== undefined ? cp.kneeDb : 2.0
            dspCompGainReductionDb = cp.gainReductionDb !== undefined ? cp.gainReductionDb : 0.0
            dspCompBypassed = !!cp.bypassed
        }
        var rp = EngineController.getReverbParams()
        if (rp) {
            if (!dspRevRoomKnob.isDragging) dspRevRoomKnob.value = rp.roomSize !== undefined ? rp.roomSize : 0.10
            if (!dspRevDampKnob.isDragging) dspRevDampKnob.value = rp.damping !== undefined ? rp.damping : 0.50
            if (!dspRevWetKnob.isDragging) dspRevWetKnob.value = rp.wetLevel !== undefined ? (rp.wetLevel * 100.0) : 0.0
            if (!dspRevPreKnob.isDragging) dspRevPreKnob.value = rp.predelayMs !== undefined ? rp.predelayMs : 0.0
            if (!dspRevDecayKnob.isDragging) dspRevDecayKnob.value = rp.decaySeconds !== undefined ? rp.decaySeconds : 0.2
            dspRevTailPreserving = rp.tailPreserving !== undefined ? !!rp.tailPreserving : true
            dspRevBypassed = !!rp.bypassed
        }
        var lp = EngineController.getLimiterParams()
        if (lp) {
            if (!dspLimCeilKnob.isDragging) dspLimCeilKnob.value = lp.ceilingDb !== undefined ? lp.ceilingDb : -0.3
            if (!dspLimRelKnob.isDragging) dspLimRelKnob.value = lp.releaseMs !== undefined ? lp.releaseMs : 50.0
            if (!dspLimLookKnob.isDragging) dspLimLookKnob.value = lp.lookaheadMs !== undefined ? lp.lookaheadMs : 1.5
            dspLimGainReductionDb = lp.gainReductionDb !== undefined ? lp.gainReductionDb : 0.0
            dspLimBypassed = !!lp.bypassed
        }
        if (!hpfInTrimKnob.isDragging) hpfInTrimKnob.value = EngineController.getDspTrim("hpf", false)
        if (!hpfOutTrimKnob.isDragging) hpfOutTrimKnob.value = EngineController.getDspTrim("hpf", true)
        if (!lpfInTrimKnob.isDragging) lpfInTrimKnob.value = EngineController.getDspTrim("lpf", false)
        if (!lpfOutTrimKnob.isDragging) lpfOutTrimKnob.value = EngineController.getDspTrim("lpf", true)
        if (!compInTrimKnob.isDragging) compInTrimKnob.value = EngineController.getDspTrim("compressor", false)
        if (!compOutTrimKnob.isDragging) compOutTrimKnob.value = EngineController.getDspTrim("compressor", true)
        if (!revInTrimKnob.isDragging) revInTrimKnob.value = EngineController.getDspTrim("reverb", false)
        if (!revOutTrimKnob.isDragging) revOutTrimKnob.value = EngineController.getDspTrim("reverb", true)
        if (!limInTrimKnob.isDragging) limInTrimKnob.value = EngineController.getDspTrim("limiter", false)
        if (!limOutTrimKnob.isDragging) limOutTrimKnob.value = EngineController.getDspTrim("limiter", true)
        filterMiniCanvas.requestPaint()
        reverbMiniCanvas.requestPaint()
    }

    Component.onCompleted: {
        refreshPeqInspector()
        refreshDspRack()
        geqInTrimFader.value = EngineController.getDspTrim("geq", false)
        geqOutTrimFader.value = EngineController.getDspTrim("geq", true)
        refreshWaveform()
        refreshEqCurve()
        if (currentOpTab === 3) refreshExternalPorts()
    }

    onCurrentOpTabChanged: {
        refreshEqCurve()
        if (currentOpTab === 3) refreshExternalPorts()
    }
    onMonitorViewModeChanged: refreshEqCurve()
    onMonitorChannelModeChanged: {
        refreshEqCurve()
        if (typeof waveCanvas !== "undefined") waveCanvas.requestPaint()
        if (spectrumHasSignal) pollSpectrum()
    }
    onShowGeqOverlayOnPeqChanged: refreshEqCurve()
    onSelectedPeqBandChanged: monitorCanvas.requestPaint()
    onPeqEditListeningChanged: {
        refreshPeqInspector()
        refreshEqCurve()
    }
    onPeqRightChannelChanged: {
        refreshPeqInspector()
        refreshEqCurve()
        if (typeof waveCanvas !== "undefined") waveCanvas.requestPaint()
    }

    Timer {
        id: playheadTimer
        interval: 50
        repeat: true
        readonly property var actSt: {
            var _t = consoleRoot.activeDeckPollTick
            return EngineController.getDeckState(EngineController.activeDeckId)
        }
        running: consoleRoot.visible && !!(actSt && actSt.state === 1)
        onTriggered: {
            consoleRoot.activeDeckPollTick++
            if (typeof waveCanvas !== "undefined") waveCanvas.requestPaint()
        }
    }

    Timer {
        id: spectrumTimer
        interval: 33
        repeat: true
        readonly property var actSt: {
            var _t = consoleRoot.activeDeckPollTick
            return EngineController.getDeckState(EngineController.activeDeckId)
        }
        running: consoleRoot.visible && consoleRoot.showSpectrum && (((actSt && actSt.state === 1) && !EngineController.masterMute) || consoleRoot.spectrumHasSignal)
        onTriggered: consoleRoot.pollSpectrum()
    }

    Connections {
        target: EngineController
        function onGeqParametersChanged() {
            consoleRoot.refreshGeqFaders()
            consoleRoot.refreshEqCurve()
        }
        function onPeqParametersChanged() {
            consoleRoot.refreshPeqInspector()
            consoleRoot.refreshEqCurve()
        }
        function onListeningPresetChanged() {
            consoleRoot.refreshGeqFaders()
            consoleRoot.refreshPeqInspector()
            consoleRoot.refreshEqCurve()
        }
        function onFilterParametersChanged() { consoleRoot.refreshDspRack() }
        function onCompressorParametersChanged() { consoleRoot.refreshDspRack() }
        function onReverbParametersChanged() { consoleRoot.refreshDspRack() }
        function onLimiterParametersChanged() { consoleRoot.refreshDspRack() }
        function onDspPresetStateChanged() {
            consoleRoot.refreshGeqFaders()
            consoleRoot.refreshPeqInspector()
            consoleRoot.refreshDspRack()
            consoleRoot.refreshEqCurve()
        }
        function onDspTelemetryChanged() {
            consoleRoot.dspCompGainReductionDb = EngineController.compressorGainReductionDb
            consoleRoot.dspLimGainReductionDb = EngineController.limiterGainReductionDb
        }
        function onDspTrimsChanged() {
            if (!geqInTrimFader.isDragging)
                geqInTrimFader.value = EngineController.getDspTrim("geq", false)
            if (!geqOutTrimFader.isDragging)
                geqOutTrimFader.value = EngineController.getDspTrim("geq", true)
            consoleRoot.refreshPeqInspector()
            consoleRoot.refreshDspRack()
        }
        function onActiveDeckChanged() {
            consoleRoot.activeDeckPollTick++
            consoleRoot.refreshWaveform()
        }
        function onDeckStateChanged(deckId) {
            consoleRoot.activeDeckPollTick++
            if (deckId === "" || deckId === "ALL" || deckId.toUpperCase() === EngineController.activeDeckId) {
                consoleRoot.refreshWaveform()
            }
        }
        function onDeckWaveformReady(deckId) {
            if (deckId === "" || deckId === "ALL" || deckId.toUpperCase() === EngineController.activeDeckId) {
                consoleRoot.refreshWaveform()
            }
        }
        function onExternalInsertStatusChanged() {
            if (typeof patchBayCanvas !== "undefined") patchBayCanvas.requestPaint()
        }
    }

    // Dark recessed steel console chassis background (deep valley contrast for mounted modules)
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#06080b" }
            GradientStop { position: 0.5; color: "#090c11" }
            GradientStop { position: 1.0; color: "#05070a" }
        }
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
            // REGION 1: LEFT WING — REFERENCE DECK (~488px)
            // =================================================================
            BrushedMetalPanel {
                Layout.preferredWidth: 488
                Layout.fillHeight: true
                baseTint: "#13171d"
                borderColor: "#2f3743"
                showScrews: true
                cornerRadius: 6
                brushedOpacity: 0.32
                surfaceShaderEnabled: true
                surfaceNoiseStrength: 0.038
                surfaceInnerShadowStrength: 0.32

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6

                    // Centered Engraved Panel Header (—— REFERENCE DECK ——)
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Column {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 2
                            Layout.alignment: Qt.AlignVCenter
                            Rectangle { width: parent.width; height: 1; color: "#07090c" }
                            Rectangle { width: parent.width; height: 1; color: "#28ffffff" }
                        }
                        Label {
                            text: qsTr("REFERENCE DECK")
                            font.family: "Monospace"
                            font.pixelSize: 12
                            font.bold: true
                            font.letterSpacing: 1.4
                            color: "#C4D0DD"
                            style: Text.Raised
                            styleColor: "#cc000000"
                        }
                        Column {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 2
                            Layout.alignment: Qt.AlignVCenter
                            Rectangle { width: parent.width; height: 1; color: "#07090c" }
                            Rectangle { width: parent.width; height: 1; color: "#28ffffff" }
                        }
                    }

                    // Switching Mode Bar: Large A<->B Quick Switch + NORMAL | COMPARE
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        ConsoleButton {
                            Layout.fillWidth: true
                            Layout.preferredWidth: 140
                            implicitHeight: 42
                            text: qsTr("A ⇄ B")
                            fontSize: 13
                            font.bold: true
                            accentColor: "#00E5FF"
                            toolTipText: qsTr("Quick Switch Active Deck between A and B")
                            onClicked: EngineController.quickSwitchAB()
                        }

                        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 28; width: 1; height: 28; color: "#2D3642" }

                        ConsoleButton {
                            Layout.fillWidth: true
                            Layout.preferredWidth: 140
                            implicitHeight: 42
                            text: qsTr("NORMAL")
                            fontSize: 13
                            highlighted: EngineController.switchingMode === 0
                            onClicked: EngineController.setSwitchingMode(0)
                        }

                        ConsoleButton {
                            Layout.fillWidth: true
                            Layout.preferredWidth: 140
                            implicitHeight: 42
                            text: qsTr("COMPARE")
                            fontSize: 13
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
                surfaceShaderEnabled: true
                surfaceNoiseStrength: 0.035
                surfaceInnerShadowStrength: 0.30

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

                        RecessedScreenOverlay {
                            cornerRadius: 6
                            topShadowSize: 10
                            sideShadowSize: 6
                            bottomShadowSize: 4
                            topShadowOpacity: 0.60
                            sideShadowOpacity: 0.35
                            showGlassSheen: false
                        }

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
                                    Layout.preferredHeight: 28
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
                                    Layout.preferredHeight: 28
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

                                ConsoleButton {
                                    visible: consoleRoot.currentOpTab === 1 && consoleRoot.showEqOverlay
                                    text: qsTr("+GEQ")
                                    implicitWidth: 54
                                    implicitHeight: 26
                                    fontSize: 9
                                    cornerRadius: 3
                                    accentColor: "#78D0FF"
                                    highlighted: consoleRoot.showGeqOverlayOnPeq
                                    toolTipText: qsTr("Overlay faint 32-GEQ composite curve in Main Monitor")
                                    onClicked: consoleRoot.showGeqOverlayOnPeq = !consoleRoot.showGeqOverlayOnPeq
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
                                             + (parent.actSt.title && parent.actSt.title.length > 0
                                                ? parent.actSt.title
                                                : (parent.actSt.fileName && parent.actSt.fileName.length > 0
                                                   ? parent.actSt.fileName
                                                   : parent.actSt.path.split('/').pop())))
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

                            // Upper Waveform Timeline Region (Real Extracted Peaks + Seek Scrubbing)
                            Rectangle {
                                id: waveTimelineBox
                                visible: consoleRoot.showWaveform
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.preferredHeight: consoleRoot.showSpectrum ? 150 : 400
                                radius: 3
                                color: "#06090e"
                                border.color: "#151d26"

                                RecessedScreenOverlay {
                                    cornerRadius: 3
                                    topShadowSize: 10
                                    sideShadowSize: 6
                                    bottomShadowSize: 5
                                    glassSheenOpacity: 0.04
                                }

                                readonly property var actSt: {
                                    var _t = consoleRoot.activeDeckPollTick
                                    return EngineController.getDeckState(EngineController.activeDeckId)
                                }
                                property bool isScrubbing: false
                                property real scrubRatio: 0.0
                                readonly property real playRatio: isScrubbing
                                                                  ? scrubRatio
                                                                  : ((actSt && actSt.totalFrames > 0)
                                                                     ? Math.max(0.0, Math.min(1.0, (actSt.playhead || 0) / actSt.totalFrames))
                                                                     : 0.0)
                                onPlayRatioChanged: waveCanvas.requestPaint()

                                // Dynamic Time Markings from Track Duration
                                RowLayout {
                                    anchors.top: parent.top
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 8
                                    anchors.topMargin: 4
                                    Repeater {
                                        model: 6
                                        Label {
                                            Layout.fillWidth: index < 5
                                            text: {
                                                var st = waveTimelineBox.actSt
                                                var dur = (st && st.totalFrames > 0 && st.sampleRate > 0) ? (st.totalFrames / st.sampleRate) : 0.0
                                                var sec = Math.max(0, Math.round(dur * (index / 5.0)))
                                                var m = Math.floor(sec / 60)
                                                var s = sec % 60
                                                return (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
                                            }
                                            font.family: "Monospace"
                                            font.pixelSize: 9
                                            color: "#4B5B6C"
                                        }
                                    }
                                }

                                Canvas {
                                    id: waveCanvas
                                    anchors.fill: parent
                                    anchors.leftMargin: 6
                                    anchors.rightMargin: 6
                                    anchors.topMargin: 18
                                    anchors.bottomMargin: 8
                                    onWidthChanged: consoleRoot.refreshWaveform()
                                    onHeightChanged: requestPaint()
                                    onPaint: {
                                        var ctx = getContext("2d")
                                        if (!ctx || width <= 4 || height <= 4) return
                                        ctx.clearRect(0, 0, width, height)

                                        var midY = height * 0.5
                                        var halfH = height * 0.34

                                        // Subtle -6 dB reference lines and exact center 0-axis line
                                        ctx.strokeStyle = "rgba(32, 48, 64, 0.55)"
                                        ctx.lineWidth = 1
                                        ctx.beginPath()
                                        ctx.moveTo(0, midY - halfH * 0.5)
                                        ctx.lineTo(width, midY - halfH * 0.5)
                                        ctx.moveTo(0, midY + halfH * 0.5)
                                        ctx.lineTo(width, midY + halfH * 0.5)
                                        ctx.stroke()

                                        ctx.strokeStyle = "#1B2836"
                                        ctx.beginPath()
                                        ctx.moveTo(0, midY)
                                        ctx.lineTo(width, midY)
                                        ctx.stroke()

                                        var wd = consoleRoot.cachedWaveData
                                        if (!wd || !wd.ready || !wd.bucketCount || wd.bucketCount <= 0) return

                                        var n = wd.bucketCount
                                        var minL = wd.minL
                                        var maxL = wd.maxL
                                        var minR = wd.minR
                                        var maxR = wd.maxR
                                        var rmsL = wd.rmsL
                                        var rmsR = wd.rmsR
                                        var hasRms = !!(rmsL && rmsR && rmsL.length === n && rmsR.length === n)
                                        var chMode = (consoleRoot.monitorChannelMode === 3)
                                                     ? (consoleRoot.peqRightChannel ? 2 : 0)
                                                     : consoleRoot.monitorChannelMode
                                        var playX = waveTimelineBox.playRatio * width

                                        var peaksPos = new Array(n)
                                        var peaksNeg = new Array(n)
                                        var rmsVals = new Array(n)
                                        for (var i = 0; i < n; ++i) {
                                            var mn, mx, rv
                                            if (chMode === 1) {
                                                mn = minL[i]
                                                mx = maxL[i]
                                                rv = hasRms ? rmsL[i] : (0.35 * Math.max(Math.abs(mn), Math.abs(mx)))
                                            } else if (chMode === 2) {
                                                mn = minR[i]
                                                mx = maxR[i]
                                                rv = hasRms ? rmsR[i] : (0.35 * Math.max(Math.abs(mn), Math.abs(mx)))
                                            } else {
                                                mn = Math.min(minL[i], minR[i])
                                                mx = Math.max(maxL[i], maxR[i])
                                                if (hasRms) {
                                                    rv = Math.sqrt(0.5 * (rmsL[i] * rmsL[i] + rmsR[i] * rmsR[i]))
                                                } else {
                                                    rv = 0.35 * Math.max(Math.abs(mn), Math.abs(mx))
                                                }
                                            }
                                            var pos = Math.max(0.0, Math.min(1.0, mx))
                                            var neg = Math.max(0.0, Math.min(1.0, -mn))
                                            peaksPos[i] = pos
                                            peaksNeg[i] = neg
                                            rmsVals[i] = Math.min(Math.max(pos, neg), Math.max(0.0, Math.min(1.0, rv)))
                                        }

                                        // Layer 1: Translucent outer peak envelope + crisp 1px contour
                                        function drawPeakEnvelope(fillColor, strokeColor) {
                                            ctx.beginPath()
                                            for (var k = 0; k < n; ++k) {
                                                var x = ((k + 0.5) / n) * width
                                                var yTop = midY - peaksPos[k] * halfH
                                                if (k === 0) ctx.moveTo(x, yTop)
                                                else ctx.lineTo(x, yTop)
                                            }
                                            for (var j = n - 1; j >= 0; --j) {
                                                var xb = ((j + 0.5) / n) * width
                                                var yBot = midY + peaksNeg[j] * halfH
                                                ctx.lineTo(xb, yBot)
                                            }
                                            ctx.closePath()
                                            ctx.fillStyle = fillColor
                                            ctx.fill()
                                            ctx.strokeStyle = strokeColor
                                            ctx.lineWidth = 1.0
                                            ctx.stroke()
                                        }

                                        if (playX > 0.5) {
                                            ctx.save()
                                            ctx.beginPath()
                                            ctx.rect(0, 0, playX, height)
                                            ctx.clip()
                                            drawPeakEnvelope("rgba(54, 227, 255, 0.20)", "rgba(64, 224, 255, 0.62)")
                                            ctx.restore()
                                        }
                                        if (playX < width - 0.5) {
                                            ctx.save()
                                            ctx.beginPath()
                                            ctx.rect(playX, 0, width - playX, height)
                                            ctx.clip()
                                            drawPeakEnvelope("rgba(28, 110, 133, 0.18)", "rgba(42, 148, 176, 0.46)")
                                            ctx.restore()
                                        }

                                        // Layer 2: True RMS energy core (slender spaced bars for clear dynamics)
                                        var step = width / n
                                        var barW = Math.max(1.0, step * 0.60)
                                        for (var m = 0; m < n; ++m) {
                                            var xc = ((m + 0.5) / n) * width
                                            var rmsPx = rmsVals[m] * halfH
                                            if (rmsPx < 0.6 && peaksPos[m] < 0.005 && peaksNeg[m] < 0.005) continue
                                            var hHalf = Math.max(0.75, rmsPx)
                                            ctx.fillStyle = (xc <= playX) ? "#32D8F5" : "#1D7E9A"
                                            ctx.fillRect(xc - barW * 0.5, midY - hHalf, barW, hHalf * 2.0)
                                        }
                                    }
                                }

                                // Real Playhead Line + Cyan Top Handle Diamond
                                Item {
                                    visible: waveTimelineBox.actSt && waveTimelineBox.actSt.totalFrames > 0
                                    x: Math.round(waveTimelineBox.playRatio * (parent.width - 12)) + 6
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
                                        color: "#992CD6F2"
                                    }
                                }

                                Label {
                                    anchors.centerIn: parent
                                    visible: !consoleRoot.cachedWaveData || !consoleRoot.cachedWaveData.ready
                                    text: {
                                        var st = waveTimelineBox.actSt
                                        if (!st || !st.path || st.path.length === 0)
                                            return qsTr("OVERVIEW WAVEFORM TIMELINE — NO TRACK LOADED")
                                        if (consoleRoot.cachedWaveData && consoleRoot.cachedWaveData.extracting)
                                            return qsTr("EXTRACTING WAVEFORM...")
                                        return qsTr("WAVEFORM READY ON LOAD")
                                    }
                                    font.family: "Monospace"
                                    font.pixelSize: 11
                                    font.bold: true
                                    color: "#4E5F70"
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    acceptedButtons: Qt.LeftButton
                                    cursorShape: (waveTimelineBox.actSt && waveTimelineBox.actSt.totalFrames > 0) ? Qt.PointingHandCursor : Qt.ArrowCursor
                                    function ratioAt(mx) {
                                        return Math.max(0.0, Math.min(1.0, (mx - 6.0) / Math.max(1.0, parent.width - 12.0)))
                                    }
                                    onPressed: function(mouse) {
                                        var st = waveTimelineBox.actSt
                                        if (!st || !st.totalFrames || st.totalFrames <= 0) return
                                        waveTimelineBox.isScrubbing = true
                                        waveTimelineBox.scrubRatio = ratioAt(mouse.x)
                                        waveCanvas.requestPaint()
                                    }
                                    onPositionChanged: function(mouse) {
                                        if (pressed && waveTimelineBox.isScrubbing) {
                                            waveTimelineBox.scrubRatio = ratioAt(mouse.x)
                                            waveCanvas.requestPaint()
                                        }
                                    }
                                    onReleased: function(mouse) {
                                        if (!waveTimelineBox.isScrubbing) return
                                        var st = waveTimelineBox.actSt
                                        var r = ratioAt(mouse.x)
                                        waveTimelineBox.isScrubbing = false
                                        if (st && st.totalFrames > 0) {
                                            EngineController.seek(EngineController.activeDeckId, Math.round(r * st.totalFrames))
                                            consoleRoot.activeDeckPollTick++
                                        }
                                        waveCanvas.requestPaint()
                                    }
                                    onCanceled: {
                                        waveTimelineBox.isScrubbing = false
                                        waveCanvas.requestPaint()
                                    }
                                }
                            }

                            // Lower Spectrum + Composite EQ Curve Canvas (Real FFT Analyzer + Draggable 32-PEQ Curve)
                            Rectangle {
                                visible: consoleRoot.showSpectrum
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.preferredHeight: consoleRoot.showWaveform ? 260 : 420
                                radius: 3
                                color: "#06080d"
                                border.color: "#151d26"

                                RecessedScreenOverlay {
                                    cornerRadius: 3
                                    topShadowSize: 12
                                    sideShadowSize: 7
                                    bottomShadowSize: 5
                                    glassSheenOpacity: 0.035
                                }

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
                                        var logMin = Math.log(20.0)
                                        var logSpan = Math.log(20000.0) - logMin

                                        // Log frequency grid (20 Hz .. 20 kHz)
                                        var freqs = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]
                                        var labels = ["20Hz", "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20kHz"]
                                        ctx.font = "10px Monospace"
                                        for (var i = 0; i < freqs.length; ++i) {
                                            var xn = (Math.log(freqs[i]) - logMin) / logSpan
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

                                        // 1. Real-Time 2048-pt FFT Spectrum Fill + Peak Hold Trace
                                        if (consoleRoot.spectrumHasSignal && consoleRoot.cachedSpectrumBins && consoleRoot.cachedSpectrumBins.length > 2) {
                                            var sbins = consoleRoot.cachedSpectrumBins
                                            var nSpec = sbins.length
                                            var specBot = padT + plotH
                                            var sGrad = ctx.createLinearGradient(0, padT, 0, specBot)
                                            sGrad.addColorStop(0.0, "rgba(0, 212, 255, 0.30)")
                                            sGrad.addColorStop(1.0, "rgba(0, 120, 175, 0.03)")

                                            ctx.beginPath()
                                            for (var si = 0; si < nSpec; ++si) {
                                                var sx = padL + (si / (nSpec - 1)) * plotW
                                                var sdb = Math.max(-90.0, Math.min(0.0, sbins[si]))
                                                var sy = padT + ((-sdb) / 90.0) * plotH
                                                if (si === 0) ctx.moveTo(sx, sy)
                                                else ctx.lineTo(sx, sy)
                                            }
                                            ctx.strokeStyle = "rgba(64, 224, 255, 0.72)"
                                            ctx.lineWidth = 1.4
                                            ctx.stroke()

                                            ctx.lineTo(padL + plotW, specBot)
                                            ctx.lineTo(padL, specBot)
                                            ctx.closePath()
                                            ctx.fillStyle = sGrad
                                            ctx.fill()

                                            // Peak-hold hairline
                                            var ph = consoleRoot.spectrumPeakHold
                                            if (ph && ph.length === nSpec) {
                                                ctx.beginPath()
                                                for (var pi = 0; pi < nSpec; ++pi) {
                                                    var pxh = padL + (pi / (nSpec - 1)) * plotW
                                                    var phdb = Math.max(-90.0, Math.min(0.0, ph[pi]))
                                                    var pyh = padT + ((-phdb) / 90.0) * plotH
                                                    if (pi === 0) ctx.moveTo(pxh, pyh)
                                                    else ctx.lineTo(pxh, pyh)
                                                }
                                                ctx.strokeStyle = "rgba(255, 196, 70, 0.45)"
                                                ctx.lineWidth = 1.0
                                                ctx.stroke()
                                            }
                                        }

                                        // 2. Real EQ Response Curve & Draggable Nodes
                                        if (consoleRoot.showEqOverlay) {
                                            var yZero = padT + (18.0 / 36.0) * plotH
                                            function drawEqCurve(pts, strokeHex, fillTopRgba, lw, dashed) {
                                                if (!pts || pts.length <= 1) return
                                                ctx.save()
                                                if (dashed && ctx.setLineDash) ctx.setLineDash([4, 4])
                                                ctx.beginPath()
                                                for (var k = 0; k < pts.length; ++k) {
                                                    var f = pts[k].f
                                                    var gdb = Math.max(-18.0, Math.min(18.0, pts[k].db))
                                                    var px = padL + ((Math.log(f) - logMin) / logSpan) * plotW
                                                    var py = padT + ((18.0 - gdb) / 36.0) * plotH
                                                    if (k === 0) ctx.moveTo(px, py)
                                                    else ctx.lineTo(px, py)
                                                }
                                                ctx.strokeStyle = strokeHex
                                                ctx.lineWidth = lw
                                                ctx.stroke()
                                                if (fillTopRgba) {
                                                    var grad = ctx.createLinearGradient(0, padT, 0, padT + plotH)
                                                    grad.addColorStop(0.0, fillTopRgba)
                                                    grad.addColorStop(1.0, "rgba(0, 0, 0, 0.0)")
                                                    ctx.lineTo(padL + plotW, yZero)
                                                    ctx.lineTo(padL, yZero)
                                                    ctx.closePath()
                                                    ctx.fillStyle = grad
                                                    ctx.fill()
                                                }
                                                ctx.restore()
                                            }

                                            if (consoleRoot.currentOpTab === 0) {
                                                var geqByp = EngineController.geqBypassed || EngineController.geqGlobalBypass || consoleRoot.isDirect
                                                drawEqCurve(EngineController.getGeqCurve(180),
                                                            geqByp ? "rgba(47, 224, 255, 0.50)" : "#2FE0FF",
                                                            geqByp ? null : "rgba(0, 229, 255, 0.22)",
                                                            geqByp ? 1.8 : 2.4, geqByp)
                                                consoleRoot.cachedPeqNodes = []
                                            } else {
                                                if (consoleRoot.showGeqOverlayOnPeq) {
                                                    drawEqCurve(EngineController.getGeqCurve(180), "rgba(120, 208, 255, 0.45)", null, 1.4, true)
                                                }
                                                var peqByp = !consoleRoot.peqEditListening && (EngineController.peqBypassed || consoleRoot.isDirect)
                                                var drawBoth = (consoleRoot.monitorChannelMode === 0)
                                                var showL = drawBoth || consoleRoot.monitorChannelMode === 1 || (consoleRoot.monitorChannelMode === 3 && !consoleRoot.peqRightChannel)
                                                var showR = drawBoth || consoleRoot.monitorChannelMode === 2 || (consoleRoot.monitorChannelMode === 3 && consoleRoot.peqRightChannel)
                                                if (showL) {
                                                    var ptsL = consoleRoot.peqEditListening
                                                               ? EngineController.getListeningCurve(false, 180)
                                                               : EngineController.getPeqCurve(false, 180)
                                                    var colL = consoleRoot.peqEditListening
                                                               ? "#4CE0B3"
                                                               : (peqByp ? "rgba(47, 224, 255, 0.58)" : "#2FE0FF")
                                                    drawEqCurve(ptsL, colL,
                                                                (!peqByp && !consoleRoot.peqRightChannel) ? "rgba(0, 229, 255, 0.16)" : null,
                                                                !consoleRoot.peqRightChannel ? 2.4 : 1.5, peqByp)
                                                }
                                                if (showR) {
                                                    var ptsR = consoleRoot.peqEditListening
                                                               ? EngineController.getListeningCurve(true, 180)
                                                               : EngineController.getPeqCurve(true, 180)
                                                    var colR = peqByp ? "rgba(245, 184, 65, 0.58)" : "#F5B841"
                                                    drawEqCurve(ptsR, colR,
                                                                (!peqByp && consoleRoot.peqRightChannel) ? "rgba(245, 184, 65, 0.16)" : null,
                                                                consoleRoot.peqRightChannel ? 2.4 : 1.5, peqByp)
                                                }

                                                // Draw enabled PEQ band nodes (unused hidden; slight radial offset when overlapping)
                                                var nodes = consoleRoot.computeVisiblePeqNodes(plotW, plotH, padL, padT)
                                                consoleRoot.cachedPeqNodes = nodes
                                                for (var ni = 0; ni < nodes.length; ++ni) {
                                                    var nd = nodes[ni]
                                                    var isSel = (nd.isRight === consoleRoot.peqRightChannel && nd.band === consoleRoot.selectedPeqBand)
                                                    var isHov = (nd.isRight === consoleRoot.hoveredPeqIsRight && nd.band === consoleRoot.hoveredPeqBand)
                                                    var col = nd.isRight ? "#F5B841" : (consoleRoot.peqEditListening ? "#4CE0B3" : "#2FE0FF")

                                                    if (Math.abs(nd.drawX - nd.trueX) > 0.5 || Math.abs(nd.drawY - nd.trueY) > 0.5) {
                                                        ctx.strokeStyle = "rgba(255,255,255,0.28)"
                                                        ctx.lineWidth = 1
                                                        ctx.beginPath()
                                                        ctx.moveTo(nd.trueX, nd.trueY)
                                                        ctx.lineTo(nd.drawX, nd.drawY)
                                                        ctx.stroke()
                                                    }

                                                    if (isSel || isHov) {
                                                        ctx.strokeStyle = isSel ? "#FFFFFF" : col
                                                        ctx.lineWidth = 1.6
                                                        ctx.beginPath()
                                                        ctx.arc(nd.drawX, nd.drawY, isSel ? 9 : 7.5, 0, Math.PI * 2)
                                                        ctx.stroke()
                                                    }

                                                    ctx.fillStyle = isSel ? "#FFFFFF" : "#0A1017"
                                                    ctx.strokeStyle = col
                                                    ctx.lineWidth = 2.0
                                                    ctx.beginPath()
                                                    ctx.arc(nd.drawX, nd.drawY, 5.0, 0, Math.PI * 2)
                                                    ctx.fill()
                                                    ctx.stroke()

                                                    if (isSel) {
                                                        var tag = (nd.isRight ? "R" : "L") + (nd.band + 1) + " " + consoleRoot.formatHz(nd.frequency) + " " + (nd.gainDb >= 0 ? "+" : "") + nd.gainDb.toFixed(1) + "dB"
                                                        ctx.font = "bold 9px Monospace"
                                                        var tagW = Math.max(142, ctx.measureText(tag).width + 10)
                                                        var txLbl = Math.min(padL + plotW - tagW - 2, Math.max(padL + 4, nd.drawX + 10))
                                                        var tyLbl = Math.max(padT + 14, nd.drawY - 10)
                                                        ctx.fillStyle = "rgba(9, 13, 19, 0.88)"
                                                        ctx.fillRect(txLbl - 3, tyLbl - 10, tagW, 14)
                                                        ctx.fillStyle = col
                                                        ctx.fillText(tag, txLbl + 2, tyLbl)
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        enabled: consoleRoot.currentOpTab === 1 && consoleRoot.showEqOverlay && !consoleRoot.isDirect
                                        hoverEnabled: true
                                        cursorShape: (consoleRoot.draggedPeqBand >= 0 || consoleRoot.hoveredPeqBand >= 0) ? Qt.CrossCursor : Qt.ArrowCursor

                                        function findHitNode(mx, my) {
                                            var nodes = consoleRoot.cachedPeqNodes
                                            var best = null
                                            var bestDist = 196.0 // 14px radius
                                            for (var i = 0; i < nodes.length; ++i) {
                                                var dx = mx - nodes[i].drawX
                                                var dy = my - nodes[i].drawY
                                                var d2 = dx * dx + dy * dy
                                                if (d2 <= bestDist) {
                                                    bestDist = d2
                                                    best = nodes[i]
                                                }
                                            }
                                            return best
                                        }

                                        function applyCoords(mx, my, isR, bandIdx) {
                                            var padL = 28, padR = 28, padT = 10, padB = 20
                                            var plotW = Math.max(1.0, parent.width - padL - padR)
                                            var plotH = Math.max(1.0, parent.height - padT - padB)
                                            var xn = Math.max(0.0, Math.min(1.0, (mx - padL) / plotW))
                                            var yn = Math.max(0.0, Math.min(1.0, (my - padT) / plotH))
                                            var freq = 20.0 * Math.pow(1000.0, xn)
                                            var gain = Math.max(-18.0, Math.min(18.0, 18.0 - yn * 36.0))
                                            var bd = consoleRoot.peqEditListening
                                                     ? EngineController.getListeningBand(isR, bandIdx)
                                                     : EngineController.getPeqBand(isR, bandIdx)
                                            var qVal = (bd && bd.q !== undefined) ? bd.q : peqQKnob.value
                                            var tVal = (bd && bd.type !== undefined) ? bd.type : consoleRoot.peqBandTypeIndex
                                            if (consoleRoot.peqEditListening) {
                                                EngineController.setListeningBand(isR, bandIdx, freq, gain, qVal, tVal, true)
                                            } else {
                                                EngineController.setPeqBand(isR, bandIdx, freq, gain, qVal, tVal, true)
                                            }
                                            consoleRoot.peqGridTick++
                                        }

                                        onPressed: function(mouse) {
                                            var hit = findHitNode(mouse.x, mouse.y)
                                            if (hit) {
                                                consoleRoot.peqRightChannel = hit.isRight
                                                consoleRoot.selectedPeqBand = hit.band
                                                consoleRoot.draggedPeqBand = hit.band
                                                consoleRoot.draggedPeqIsRight = hit.isRight
                                                consoleRoot.refreshPeqInspector()
                                                monitorCanvas.requestPaint()
                                            } else {
                                                consoleRoot.draggedPeqBand = -1
                                            }
                                        }
                                        onPositionChanged: function(mouse) {
                                            if (pressed && consoleRoot.draggedPeqBand >= 0) {
                                                applyCoords(mouse.x, mouse.y, consoleRoot.draggedPeqIsRight, consoleRoot.draggedPeqBand)
                                            } else {
                                                var hit = findHitNode(mouse.x, mouse.y)
                                                var nb = hit ? hit.band : -1
                                                var nr = hit ? hit.isRight : false
                                                if (nb !== consoleRoot.hoveredPeqBand || nr !== consoleRoot.hoveredPeqIsRight) {
                                                    consoleRoot.hoveredPeqBand = nb
                                                    consoleRoot.hoveredPeqIsRight = nr
                                                    monitorCanvas.requestPaint()
                                                }
                                            }
                                        }
                                        onReleased: {
                                            consoleRoot.draggedPeqBand = -1
                                            monitorCanvas.requestPaint()
                                        }
                                        onDoubleClicked: function(mouse) {
                                            var hit = findHitNode(mouse.x, mouse.y)
                                            if (hit) {
                                                var bd = consoleRoot.peqEditListening
                                                         ? EngineController.getListeningBand(hit.isRight, hit.band)
                                                         : EngineController.getPeqBand(hit.isRight, hit.band)
                                                var qVal = (bd && bd.q !== undefined) ? bd.q : 1.0
                                                var tVal = (bd && bd.type !== undefined) ? bd.type : 0
                                                if (consoleRoot.peqEditListening) {
                                                    EngineController.setListeningBand(hit.isRight, hit.band, hit.frequency, 0.0, qVal, tVal, true)
                                                } else {
                                                    EngineController.setPeqBand(hit.isRight, hit.band, hit.frequency, 0.0, qVal, tVal, true)
                                                }
                                            }
                                        }
                                        onWheel: function(wheel) {
                                            var hit = findHitNode(wheel.x, wheel.y)
                                            var isR = hit ? hit.isRight : consoleRoot.peqRightChannel
                                            var bandIdx = hit ? hit.band : consoleRoot.selectedPeqBand
                                            var bd = consoleRoot.peqEditListening
                                                     ? EngineController.getListeningBand(isR, bandIdx)
                                                     : EngineController.getPeqBand(isR, bandIdx)
                                            if (!bd || !bd.enabled) return
                                            var factor = wheel.angleDelta.y > 0 ? 1.12 : (1.0 / 1.12)
                                            var nextQ = Math.max(0.1, Math.min(18.0, bd.q * factor))
                                            if (consoleRoot.peqEditListening) {
                                                EngineController.setListeningBand(isR, bandIdx, bd.frequency, bd.gainDb, nextQ, bd.type, true)
                                            } else {
                                                EngineController.setPeqBand(isR, bandIdx, bd.frequency, bd.gainDb, nextQ, bd.type, true)
                                            }
                                            consoleRoot.peqGridTick++
                                        }
                                    }
                                }

                                Label {
                                    anchors.top: parent.top
                                    anchors.right: parent.right
                                    anchors.margins: 8
                                    text: {
                                        if (consoleRoot.spectrumHasSignal) {
                                            var chTxt = consoleRoot.monitorChannelMode === 0 ? "L+R"
                                                        : (consoleRoot.monitorChannelMode === 1 ? "L"
                                                           : (consoleRoot.monitorChannelMode === 2 ? "R" : "AUTO"))
                                            return qsTr("2048-PT HANN FFT (%1)").arg(chTxt)
                                        }
                                        return consoleRoot.showEqOverlay
                                               ? qsTr("EQ RESPONSE ACTIVE  •  FFT STANDBY")
                                               : qsTr("REAL-TIME FFT SPECTRUM STANDBY")
                                    }
                                    font.family: "Monospace"
                                    font.pixelSize: 10
                                    color: consoleRoot.spectrumHasSignal ? "#5ED2EB" : "#526374"
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
                surfaceShaderEnabled: true
                surfaceNoiseStrength: 0.038
                surfaceInnerShadowStrength: 0.32

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 9

                    // Centered Engraved Panel Header (—— ROUTING ——)
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Column {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 2
                            Layout.alignment: Qt.AlignVCenter
                            Rectangle { width: parent.width; height: 1; color: "#07090c" }
                            Rectangle { width: parent.width; height: 1; color: "#28ffffff" }
                        }
                        Label {
                            text: qsTr("ROUTING")
                            font.family: "Monospace"
                            font.pixelSize: 12
                            font.bold: true
                            font.letterSpacing: 1.4
                            color: "#C4D0DD"
                            style: Text.Raised
                            styleColor: "#cc000000"
                        }
                        Column {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 2
                            Layout.alignment: Qt.AlignVCenter
                            Rectangle { width: parent.width; height: 1; color: "#07090c" }
                            Rectangle { width: parent.width; height: 1; color: "#28ffffff" }
                        }
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
                            ConsoleComboBox {
                                id: routingPresetCombo
                                implicitWidth: 168
                                implicitHeight: 24
                                font.pixelSize: 10
                                font.bold: true
                                model: EngineController.availableListeningPresets()
                                currentIndex: Math.max(0, model.indexOf(EngineController.listeningPresetName))
                                enabled: EngineController.outputRoutingMode === 0 && !consoleRoot.isDirect
                                onActivated: function(idx) {
                                    EngineController.setListeningPreset(model[idx])
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
                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; height: 1; color: "#2d3541" }
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
                            surfaceShaderEnabled: true
                            surfaceNoiseStrength: 0.030
                            surfaceInnerShadowStrength: 0.40

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

                                    Rectangle {
                                        id: balPill
                                        implicitWidth: 92
                                        implicitHeight: 25
                                        radius: 4
                                        color: "#090D12"
                                        border.color: Math.abs(EngineController.masterBalance) > 0.01 ? "#00E5FF" : "#2E3744"
                                        border.width: 1
                                        opacity: !consoleRoot.isDirect ? 1.0 : 0.45

                                        // Center detent marker
                                        Rectangle {
                                            x: parent.width * 0.5 - 0.5
                                            y: 2
                                            width: 1
                                            height: parent.height - 4
                                            color: "#2A3340"
                                        }

                                        // Bipolar L/R attenuation bar
                                        Rectangle {
                                            readonly property double balVal: EngineController.masterBalance
                                            y: parent.height - 4
                                            height: 2
                                            radius: 1
                                            x: balVal < 0 ? (parent.width * 0.5 + balVal * (parent.width * 0.44)) : (parent.width * 0.5)
                                            width: Math.abs(balVal) * (parent.width * 0.44)
                                            color: "#00E5FF"
                                            visible: Math.abs(balVal) > 0.01
                                        }

                                        Text {
                                            anchors.centerIn: parent
                                            readonly property double b: EngineController.masterBalance
                                            readonly property int pct: Math.round(Math.abs(b) * 100)
                                            text: pct < 1 ? qsTr("BAL: C") : (b < 0 ? qsTr("BAL: L%1").arg(pct) : qsTr("BAL: R%1").arg(pct))
                                            font.family: "Monospace"
                                            font.pixelSize: 9
                                            font.bold: true
                                            color: pct >= 1 ? "#00E5FF" : "#C4CCD6"
                                        }

                                        MouseArea {
                                            id: balMouse
                                            anchors.fill: parent
                                            enabled: !consoleRoot.isDirect
                                            hoverEnabled: true
                                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                                            property double dragStartX: 0.0
                                            property double dragStartBal: 0.0
                                            onPressed: function(mouse) {
                                                if (mouse.button === Qt.RightButton) {
                                                    EngineController.setMasterBalance(0.0);
                                                    return;
                                                }
                                                dragStartX = mouse.x;
                                                dragStartBal = EngineController.masterBalance;
                                            }
                                            onPositionChanged: function(mouse) {
                                                if (pressed && (mouse.buttons & Qt.LeftButton)) {
                                                    let next = Math.max(-1.0, Math.min(1.0, dragStartBal + (mouse.x - dragStartX) / 60.0));
                                                    if (Math.abs(next) < 0.03) next = 0.0;
                                                    EngineController.setMasterBalance(next);
                                                }
                                            }
                                            onDoubleClicked: EngineController.setMasterBalance(0.0)
                                            onWheel: function(wheel) {
                                                const step = wheel.angleDelta.y > 0 ? 0.05 : -0.05;
                                                let next = Math.max(-1.0, Math.min(1.0, EngineController.masterBalance + step));
                                                if (Math.abs(next) < 0.02) next = 0.0;
                                                EngineController.setMasterBalance(next);
                                            }
                                        }

                                        ToolTip.visible: balMouse.containsMouse
                                        ToolTip.delay: 450
                                        ToolTip.text: {
                                            const b = EngineController.masterBalance;
                                            const lGain = b > 0 ? Math.max(0, 1 - b) : 1;
                                            const rGain = b < 0 ? Math.max(0, 1 + b) : 1;
                                            const fmt = function(g) { return g <= 0.0001 ? "-∞ dB" : (20 * Math.log(g) / Math.LN10).toFixed(1) + " dB"; };
                                            return qsTr("Balance (drag L/R, double-click or right-click to Center)\nL: %1  |  R: %2").arg(fmt(lGain)).arg(fmt(rGain));
                                        }
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
                                        consoleTaper: true
                                        showHeaderLed: false
                                        showReadout: true
                                        accentColor: "#00E5FF"
                                        enabled: !consoleRoot.isDirect
                                        Component.onCompleted: value = consoleRoot.gainToDb(EngineController.masterGainL)
                                        onValueEdited: function(dbVal) {
                                            var g = consoleRoot.dbToGain(dbVal)
                                            if (consoleRoot.linkMasterLR) EngineController.setMasterGain(g)
                                            else EngineController.setMasterGainL(g)
                                        }
                                        Connections {
                                            target: EngineController
                                            function onMasterGainLChanged() {
                                                if (!masterFaderL.isDragging)
                                                    masterFaderL.value = consoleRoot.gainToDb(EngineController.masterGainL)
                                            }
                                        }
                                    }

                                    // Engraved White Silkscreen Center Scale aligned pixel-for-pixel with consoleTaper ticks
                                    Item {
                                        id: masterScaleCol
                                        Layout.preferredWidth: 32
                                        Layout.fillHeight: true
                                        Layout.bottomMargin: 24   // match ConsoleFader readout + spacing height
                                        readonly property real padTop: 31
                                        readonly property real travelH: Math.max(20, height - padTop * 2)

                                        Repeater {
                                            model: [
                                                { db: 6.0,   lbl: "+6" },
                                                { db: 3.0,   lbl: "+3" },
                                                { db: 0.0,   lbl: "0" },
                                                { db: -5.0,  lbl: "-5" },
                                                { db: -10.0, lbl: "-10" },
                                                { db: -20.0, lbl: "-20" },
                                                { db: -30.0, lbl: "-30" },
                                                { db: -36.0, lbl: "-36" },
                                                { db: -60.0, lbl: "-∞" }
                                            ]
                                            Label {
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                y: masterScaleCol.padTop + (1.0 - masterFaderL.valueToNormalized(modelData.db)) * masterScaleCol.travelH - height * 0.5
                                                text: modelData.lbl
                                                font.family: "Monospace"
                                                font.pixelSize: modelData.db === 0.0 ? 11 : 9
                                                font.bold: modelData.db === 0.0 || modelData.db === 6.0
                                                color: modelData.db === 0.0 ? "#FFFFFF" : (modelData.db > 0 ? "#4CD6F0" : "#CFD9E4")
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
                                        consoleTaper: true
                                        showHeaderLed: false
                                        showReadout: true
                                        accentColor: "#00E5FF"
                                        enabled: !consoleRoot.isDirect
                                        Component.onCompleted: value = consoleRoot.gainToDb(EngineController.masterGainR)
                                        onValueEdited: function(dbVal) {
                                            var g = consoleRoot.dbToGain(dbVal)
                                            if (consoleRoot.linkMasterLR) EngineController.setMasterGain(g)
                                            else EngineController.setMasterGainR(g)
                                        }
                                        Connections {
                                            target: EngineController
                                            function onMasterGainRChanged() {
                                                if (!masterFaderR.isDragging)
                                                    masterFaderR.value = consoleRoot.gainToDb(EngineController.masterGainR)
                                            }
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
                                        text: EngineController.masterMuteL ? qsTr("L MUTED") : qsTr("L MUTE")
                                        fontSize: 9
                                        anodizedFill: EngineController.masterMuteL
                                        anodizedTop: "#B33339"
                                        anodizedBottom: "#61171B"
                                        anodizedText: "#FFE8EA"
                                        accentColor: "#FF5252"
                                        highlighted: EngineController.masterMuteL
                                        toolTipText: qsTr("Independent Left Mute (auto-clears Direct Mode)")
                                        onClicked: EngineController.setMasterMuteL(!EngineController.masterMuteL)
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
                                        text: EngineController.masterMuteR ? qsTr("R MUTED") : qsTr("R MUTE")
                                        fontSize: 9
                                        anodizedFill: EngineController.masterMuteR
                                        anodizedTop: "#B33339"
                                        anodizedBottom: "#61171B"
                                        anodizedText: "#FFE8EA"
                                        accentColor: "#FF5252"
                                        highlighted: EngineController.masterMuteR
                                        toolTipText: qsTr("Independent Right Mute (auto-clears Direct Mode)")
                                        onClicked: EngineController.setMasterMuteR(!EngineController.masterMuteR)
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
                                    text: EngineController.monitorDimEnabled ? qsTr("DIM\nON -20dB") : qsTr("DIM\n-20dB")
                                    fontSize: 10
                                    showLed: true
                                    ledColor: EngineController.monitorDimEnabled ? "#FFB300" : "#444E5C"
                                    accentColor: "#FFB300"
                                    highlighted: EngineController.monitorDimEnabled
                                    enabled: !consoleRoot.isDirect
                                    toolTipText: qsTr("Fixed -20 dB Monitor-Only Dim (Listening & Render Monitor only; never affects Render-File)")
                                    onClicked: EngineController.setMonitorDimEnabled(!EngineController.monitorDimEnabled)
                                }

                                ConsoleButton {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    leftPadding: 4; rightPadding: 4
                                    text: qsTr("LEVEL\nMATCH")
                                    fontSize: 10
                                    accentColor: "#00E5FF"
                                    toolTipText: qsTr("Batch Auto Level Match across loaded decks")
                                    onClicked: EngineController.startAutoLevelMatch(EngineController.activeDeckId, ["A", "B", "C", "D"])
                                }
                            }

                            // 4. GLOBAL BYPASS Toggle Button (Dark gunmetal when OFF, Vivid Crimson-Red when ON)
                            ConsoleButton {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.preferredHeight: 84
                                text: EngineController.globalDspBypass ? qsTr("GLOBAL BYPASS\nON") : qsTr("GLOBAL BYPASS\nOFF")
                                fontSize: 12
                                showLed: true
                                ledColor: EngineController.globalDspBypass ? "#FF8A80" : "#444E5C"
                                anodizedFill: EngineController.globalDspBypass
                                anodizedTop: "#E8434A"
                                anodizedBottom: "#8A191F"
                                anodizedText: "#FFFFFF"
                                accentColor: "#FF5252"
                                highlighted: EngineController.globalDspBypass
                                enabled: !consoleRoot.isDirect
                                toolTipText: qsTr("Bypass all Master DSP nodes (Toggle ON/OFF)")
                                onClicked: EngineController.setGlobalDspBypass(!EngineController.globalDspBypass)
                            }
                        }
                    }

                    // Single DSP Preset Switch (Spec v3.8.3 §11.1 / P6: Global Name + Modified + Modal)
                    ConsoleButton {
                        id: globalPresetSwitchBtn
                        Layout.fillWidth: true
                        implicitHeight: 32
                        text: qsTr("DSP PRESET: %1%2")
                              .arg(EngineController.activeGlobalPresetName)
                              .arg(EngineController.globalPresetModified ? qsTr("  [MODIFIED]") : "")
                        fontSize: 10
                        monospace: true
                        showLed: true
                        ledColor: EngineController.globalPresetModified ? "#F5B841" : "#00E5FF"
                        accentColor: EngineController.globalPresetModified ? "#F5B841" : "#00E5FF"
                        highlighted: EngineController.globalPresetModified
                        toolTipText: qsTr("Open layered DSP Preset Manager (Global, Per-DSP, and Listening Correction)")
                        onClicked: dspPresetModal.open()
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
                                enabled: !consoleRoot.isDirect && !EngineController.geqBypassed && !EngineController.geqGlobalBypass
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
                                anchors.leftMargin: -3
                                width: 9
                                radius: 4
                                color: "#2232E6FF"
                            }
                            Rectangle {
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                anchors.right: parent.right
                                anchors.rightMargin: -3
                                width: 9
                                radius: 4
                                color: "#2232E6FF"
                            }
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
                                gradient: Gradient {
                                    GradientStop { position: 0.0; color: "#151d27" }
                                    GradientStop { position: 1.0; color: "#0d1218" }
                                }
                            }

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                anchors.topMargin: 2
                                anchors.bottomMargin: 2
                                spacing: 4

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 4

                                    // GEQ Preset-Applied Indicator ONLY (§11.1)
                                    Rectangle {
                                        readonly property bool presetOn: !!(EngineController.dspPresetIndicators && EngineController.dspPresetIndicators["geq"])
                                        implicitWidth: 10
                                        implicitHeight: 10
                                        radius: 5
                                        color: presetOn ? "#00E5FF" : "#2D3743"
                                        border.color: presetOn ? "#8DF2FF" : "#445160"
                                        ToolTip.visible: geqIndMa.containsMouse
                                        ToolTip.text: presetOn ? qsTr("GEQ Preset Applied") : qsTr("GEQ Preset Indicator: OFF")
                                        MouseArea { id: geqIndMa; anchors.fill: parent; hoverEnabled: true }
                                    }

                                    // GEQ DSP Node ON/OFF (included in Global preset)
                                    ConsoleButton {
                                        Layout.fillWidth: true
                                        implicitHeight: 24
                                        text: EngineController.geqBypassed ? qsTr("NODE OFF") : qsTr("NODE ON")
                                        fontSize: 9
                                        accentColor: "#32E6FF"
                                        highlighted: !EngineController.geqBypassed
                                        enabled: !consoleRoot.isDirect
                                        toolTipText: qsTr("GEQ DSP Node ON / OFF (saved in Global presets)")
                                        onClicked: EngineController.setDspNodeBypass("GEQ", !EngineController.geqBypassed)
                                    }

                                    // GEQ Temporary Global Bypass (excluded from presets per §11.1)
                                    ConsoleButton {
                                        implicitWidth: 56
                                        implicitHeight: 24
                                        text: EngineController.geqGlobalBypass ? qsTr("BYP ON") : qsTr("BYP")
                                        fontSize: 8
                                        accentColor: "#F5B841"
                                        highlighted: EngineController.geqGlobalBypass
                                        enabled: !consoleRoot.isDirect
                                        toolTipText: qsTr("Temporary GEQ Global Bypass (excluded from presets and modified state)")
                                        onClicked: EngineController.setGeqGlobalBypass(!EngineController.geqGlobalBypass)
                                    }
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
                                enabled: !consoleRoot.isDirect && !EngineController.geqBypassed && !EngineController.geqGlobalBypass
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

                        // LEFT CHANNEL 4x4 Symmetrical Grid (Unified Recessed Glass Display Window)
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

                            // Unified Recessed Display Window for 4x4 Grid
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                radius: 4
                                color: "#06090d"
                                border.color: "#151d26"
                                border.width: 1
                                clip: true

                                RecessedScreenOverlay {
                                    cornerRadius: 4
                                    topShadowSize: 8
                                    sideShadowSize: 6
                                    bottomShadowSize: 5
                                    glassSheenOpacity: 0.06
                                }

                                GridLayout {
                                    anchors.fill: parent
                                    anchors.margins: 4
                                    columns: 4
                                    rowSpacing: 1
                                    columnSpacing: 1

                                    Repeater {
                                        model: 16
                                        Item {
                                            Layout.fillWidth: true
                                            Layout.fillHeight: true

                                            property var bandData: {
                                                var _t = consoleRoot.peqGridTick
                                                return consoleRoot.peqEditListening
                                                       ? EngineController.getListeningBand(false, index)
                                                       : EngineController.getPeqBand(false, index)
                                            }
                                            property bool isOn: !!(bandData && bandData.enabled)
                                            property bool isSelected: !consoleRoot.peqRightChannel && consoleRoot.selectedPeqBand === index

                                            // Cell background: very subtle darkening or active tint only for selected band
                                            Rectangle {
                                                anchors.fill: parent
                                                radius: 2
                                                color: isSelected ? "#143a49" : (isOn ? "#090d12" : "transparent")
                                                border.color: isSelected ? "#00E5FF" : "transparent"
                                                border.width: isSelected ? 1 : 0
                                            }

                                            // Engraved Grooves between cells: Dark 1px line + 1px subtle bottom/right highlight
                                            Rectangle {
                                                visible: (index % 4) < 3
                                                anchors.right: parent.right
                                                anchors.top: parent.top
                                                anchors.bottom: parent.bottom
                                                width: 1
                                                color: "#05070a"
                                            }
                                            Rectangle {
                                                visible: (index % 4) < 3
                                                anchors.right: parent.right
                                                anchors.rightMargin: -1
                                                anchors.top: parent.top
                                                anchors.bottom: parent.bottom
                                                width: 1
                                                color: "#12ffffff"
                                            }
                                            Rectangle {
                                                visible: Math.floor(index / 4) < 3
                                                anchors.bottom: parent.bottom
                                                anchors.left: parent.left
                                                anchors.right: parent.right
                                                height: 1
                                                color: "#05070a"
                                            }
                                            Rectangle {
                                                visible: Math.floor(index / 4) < 3
                                                anchors.bottom: parent.bottom
                                                anchors.bottomMargin: -1
                                                anchors.left: parent.left
                                                anchors.right: parent.right
                                                height: 1
                                                color: "#12ffffff"
                                            }

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
                                                anchors.margins: 4
                                                spacing: 2

                                                // Top row: Amber LED indicator + Band Name + Q factor
                                                RowLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 4

                                                    // Small Amber LED (glows amber when band is enabled)
                                                    Rectangle {
                                                        Layout.preferredWidth: 6; Layout.preferredHeight: 6
                                                        width: 6; height: 6; radius: 3
                                                        color: isOn ? "#F5B841" : "#222830"
                                                        border.color: isOn ? "#FFE6A3" : "#14171d"
                                                        border.width: 1

                                                        // Subtle LED halo
                                                        Rectangle {
                                                            visible: isOn
                                                            anchors.centerIn: parent
                                                            width: 10; height: 10; radius: 5
                                                            color: "#30F5B841"
                                                            z: -1
                                                        }
                                                    }

                                                    Label {
                                                        text: "L-" + (index < 9 ? "0" : "") + (index + 1)
                                                        font.family: "Monospace"
                                                        font.pixelSize: 11
                                                        font.bold: isSelected
                                                        color: isSelected ? "#00E5FF" : (isOn ? "#8896A4" : "#4A5460")
                                                    }

                                                    Item { Layout.fillWidth: true }

                                                    Label {
                                                        text: bandData && bandData.q !== undefined ? ("Q" + bandData.q.toFixed(1)) : ""
                                                        font.family: "Monospace"
                                                        font.pixelSize: 10
                                                        color: isSelected ? "#70DDF0" : (isOn ? "#687684" : "#38414B")
                                                    }
                                                }

                                                // Data Row: FREQ and GAIN in muted gray (cyan only when selected)
                                                RowLayout {
                                                    Layout.fillWidth: true
                                                    Layout.fillHeight: true
                                                    spacing: 3

                                                    Column {
                                                        Layout.fillWidth: true
                                                        spacing: 1
                                                        Label {
                                                            text: "FREQ"
                                                            font.family: "Monospace"
                                                            font.pixelSize: 7
                                                            color: isSelected ? "#4A9AB0" : "#3E4752"
                                                        }
                                                        Label {
                                                            text: bandData && bandData.frequency ? (bandData.frequency >= 1000 ? (bandData.frequency / 1000).toFixed(1) + "k" : Math.round(bandData.frequency) + "Hz") : "--"
                                                            font.family: "Monospace"
                                                            font.pixelSize: 11
                                                            font.bold: isSelected
                                                            color: isSelected ? "#00E5FF" : (isOn ? "#7E8C9A" : "#454E57")
                                                        }
                                                    }

                                                    Column {
                                                        Layout.fillWidth: true
                                                        spacing: 1
                                                        Label {
                                                            text: "GAIN"
                                                            font.family: "Monospace"
                                                            font.pixelSize: 7
                                                            color: isSelected ? "#4A9AB0" : "#3E4752"
                                                        }
                                                        Label {
                                                            text: bandData && bandData.gainDb !== undefined
                                                                  ? ((bandData.gainDb > 0 ? "+" : "") + bandData.gainDb.toFixed(1))
                                                                  : "--"
                                                            font.family: "Monospace"
                                                            font.pixelSize: 11
                                                            font.bold: isSelected
                                                            color: isSelected ? "#00E5FF" : (isOn ? "#7E8C9A" : "#454E57")
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
                                        clip: true

                                        RecessedScreenOverlay {
                                            cornerRadius: 4
                                            topShadowSize: 9
                                            sideShadowSize: 6
                                            bottomShadowSize: 4
                                            topShadowOpacity: 0.68
                                            sideShadowOpacity: 0.42
                                            bottomShadowOpacity: 0.30
                                            glassSheenOpacity: 0.065
                                        }
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

                                                var logMin = Math.log(20.0)
                                                var logSpan = Math.log(20000.0) - logMin

                                                // Subtle frequency & dB guide lines
                                                ctx.strokeStyle = "#14202C"
                                                ctx.lineWidth = 1
                                                var guideFreqs = [100, 1000, 10000]
                                                for (var gi = 0; gi < guideFreqs.length; ++gi) {
                                                    var gx = ((Math.log(guideFreqs[gi]) - logMin) / logSpan) * width
                                                    ctx.beginPath()
                                                    ctx.moveTo(gx, 0)
                                                    ctx.lineTo(gx, height)
                                                    ctx.stroke()
                                                }
                                                ctx.strokeStyle = "#223446"
                                                ctx.beginPath()
                                                ctx.moveTo(0, height * 0.5)
                                                ctx.lineTo(width, height * 0.5)
                                                ctx.stroke()

                                                var pts = consoleRoot.peqEditListening
                                                          ? EngineController.getListeningCurve(consoleRoot.peqRightChannel, 120)
                                                          : EngineController.getPeqCurve(consoleRoot.peqRightChannel, 120)
                                                var byp = !consoleRoot.peqEditListening && (EngineController.peqBypassed || consoleRoot.isDirect)
                                                var strokeCol = consoleRoot.peqEditListening
                                                                ? "#4CE0B3"
                                                                : (consoleRoot.peqRightChannel ? "#F2C97D" : "#33E3FF")

                                                if (pts && pts.length > 1) {
                                                    ctx.save()
                                                    if (byp && ctx.setLineDash) ctx.setLineDash([4, 3])
                                                    ctx.strokeStyle = strokeCol
                                                    ctx.globalAlpha = byp ? 0.65 : 1.0
                                                    ctx.lineWidth = 2.2
                                                    ctx.beginPath()
                                                    for (var k = 0; k < pts.length; ++k) {
                                                        var x = ((Math.log(Math.max(20.0, pts[k].f)) - logMin) / logSpan) * width
                                                        var db = Math.max(-18.0, Math.min(18.0, pts[k].db))
                                                        var y = ((18.0 - db) / 36.0) * height
                                                        if (k === 0) ctx.moveTo(x, y)
                                                        else ctx.lineTo(x, y)
                                                    }
                                                    ctx.stroke()
                                                    if (!byp) {
                                                        ctx.lineTo(width, height * 0.5)
                                                        ctx.lineTo(0, height * 0.5)
                                                        ctx.closePath()
                                                        ctx.fillStyle = consoleRoot.peqRightChannel
                                                                        ? "rgba(242, 201, 125, 0.14)"
                                                                        : (consoleRoot.peqEditListening ? "rgba(76, 224, 179, 0.14)" : "rgba(51, 227, 255, 0.14)")
                                                        ctx.fill()
                                                    }
                                                    ctx.restore()
                                                }

                                                // Highlight currently selected band crosshair & dot
                                                var bf = Math.max(20.0, Math.min(20000.0, peqFreqKnob.value))
                                                var bg = Math.max(-18.0, Math.min(18.0, peqGainKnob.value))
                                                var bx = ((Math.log(bf) - logMin) / logSpan) * width
                                                var by = ((18.0 - bg) / 36.0) * height
                                                ctx.strokeStyle = "rgba(255, 255, 255, 0.20)"
                                                ctx.lineWidth = 1
                                                ctx.beginPath()
                                                ctx.moveTo(bx, 0)
                                                ctx.lineTo(bx, height)
                                                ctx.stroke()

                                                ctx.fillStyle = consoleRoot.peqBandEnabledState ? "#FFFFFF" : "#3A4654"
                                                ctx.strokeStyle = strokeCol
                                                ctx.lineWidth = 1.8
                                                ctx.beginPath()
                                                ctx.arc(bx, by, 4.0, 0, Math.PI * 2)
                                                ctx.fill()
                                                ctx.stroke()
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
                                        color: "#06090e"
                                        border.color: "#294658"
                                        border.width: 1
                                        clip: true

                                        RecessedScreenOverlay {
                                            cornerRadius: 5
                                            topShadowSize: 8
                                            sideShadowSize: 5
                                            bottomShadowSize: 4
                                            topShadowOpacity: 0.68
                                            sideShadowOpacity: 0.42
                                            bottomShadowOpacity: 0.30
                                            glassSheenOpacity: 0.065
                                        }

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
                                                    toolTipText: qsTr("Switch between Master PEQ and Listening Correction PEQ")
                                                    onClicked: consoleRoot.peqEditListening = !consoleRoot.peqEditListening
                                                }
                                                // Mastering PEQ Preset-Applied Indicator ONLY (§11.1)
                                                Rectangle {
                                                    visible: !consoleRoot.peqEditListening
                                                    readonly property bool presetOn: !!(EngineController.dspPresetIndicators && EngineController.dspPresetIndicators["peq"])
                                                    implicitWidth: 10
                                                    implicitHeight: 10
                                                    radius: 5
                                                    color: presetOn ? "#00E5FF" : "#2D3743"
                                                    border.color: presetOn ? "#8DF2FF" : "#445160"
                                                    ToolTip.visible: peqIndMa.containsMouse
                                                    ToolTip.text: presetOn ? qsTr("PEQ Preset Applied") : qsTr("PEQ Preset Indicator: OFF")
                                                    MouseArea { id: peqIndMa; anchors.fill: parent; hoverEnabled: true }
                                                }
                                                ConsoleButton {
                                                    text: EngineController.peqBypassed ? qsTr("EQ BYPASSED") : qsTr("EQ ACTIVE")
                                                    implicitWidth: 88
                                                    implicitHeight: 24
                                                    fontSize: 9
                                                    visible: !consoleRoot.peqEditListening
                                                    highlighted: !EngineController.peqBypassed
                                                    enabled: !consoleRoot.isDirect
                                                    onClicked: EngineController.setDspNodeBypass("PEQ", !EngineController.peqBypassed)
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
                                        enabled: !consoleRoot.isDirect && !consoleRoot.peqEditListening
                                        onValueEdited: function(v) { value = v; EngineController.setDspTrim("peq", false, v) }
                                    }
                                    ConsoleKnob {
                                        id: peqOutTrimKnob
                                        Layout.alignment: Qt.AlignHCenter
                                        labelText: qsTr("OUT")
                                        minValue: -12.0; maxValue: 12.0; defaultValue: 0.0; logarithmic: false; bipolar: true
                                        unitSuffix: "dB"; decimals: 1; knobDiameter: 94; arcColor: "#33E3FF"
                                        enabled: !consoleRoot.isDirect && !consoleRoot.peqEditListening
                                        onValueEdited: function(v) { value = v; EngineController.setDspTrim("peq", true, v) }
                                    }

                                    Item { Layout.fillHeight: true }
                                }
                            }
                        }

                        // RIGHT CHANNEL 4x4 Symmetrical Grid (Unified Recessed Glass Display Window)
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

                            // Unified Recessed Display Window for 4x4 Grid
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                radius: 4
                                color: "#06090d"
                                border.color: "#151d26"
                                border.width: 1
                                clip: true

                                RecessedScreenOverlay {
                                    cornerRadius: 4
                                    topShadowSize: 8
                                    sideShadowSize: 6
                                    bottomShadowSize: 5
                                    glassSheenOpacity: 0.06
                                }

                                GridLayout {
                                    anchors.fill: parent
                                    anchors.margins: 4
                                    columns: 4
                                    rowSpacing: 1
                                    columnSpacing: 1

                                    Repeater {
                                        model: 16
                                        Item {
                                            Layout.fillWidth: true
                                            Layout.fillHeight: true

                                            property var bandData: {
                                                var _t = consoleRoot.peqGridTick
                                                return consoleRoot.peqEditListening
                                                       ? EngineController.getListeningBand(true, index)
                                                       : EngineController.getPeqBand(true, index)
                                            }
                                            property bool isOn: !!(bandData && bandData.enabled)
                                            property bool isSelected: consoleRoot.peqRightChannel && consoleRoot.selectedPeqBand === index

                                            // Cell background: very subtle darkening or active tint only for selected band
                                            Rectangle {
                                                anchors.fill: parent
                                                radius: 2
                                                color: isSelected ? "#382c16" : (isOn ? "#090d12" : "transparent")
                                                border.color: isSelected ? "#F5B841" : "transparent"
                                                border.width: isSelected ? 1 : 0
                                            }

                                            // Engraved Grooves between cells: Dark 1px line + 1px subtle bottom/right highlight
                                            Rectangle {
                                                visible: (index % 4) < 3
                                                anchors.right: parent.right
                                                anchors.top: parent.top
                                                anchors.bottom: parent.bottom
                                                width: 1
                                                color: "#05070a"
                                            }
                                            Rectangle {
                                                visible: (index % 4) < 3
                                                anchors.right: parent.right
                                                anchors.rightMargin: -1
                                                anchors.top: parent.top
                                                anchors.bottom: parent.bottom
                                                width: 1
                                                color: "#12ffffff"
                                            }
                                            Rectangle {
                                                visible: Math.floor(index / 4) < 3
                                                anchors.bottom: parent.bottom
                                                anchors.left: parent.left
                                                anchors.right: parent.right
                                                height: 1
                                                color: "#05070a"
                                            }
                                            Rectangle {
                                                visible: Math.floor(index / 4) < 3
                                                anchors.bottom: parent.bottom
                                                anchors.bottomMargin: -1
                                                anchors.left: parent.left
                                                anchors.right: parent.right
                                                height: 1
                                                color: "#12ffffff"
                                            }

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
                                                anchors.margins: 4
                                                spacing: 2

                                                // Top row: Amber LED indicator + Band Name + Q factor
                                                RowLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 4

                                                    // Small Amber LED (glows amber when band is enabled)
                                                    Rectangle {
                                                        Layout.preferredWidth: 6; Layout.preferredHeight: 6
                                                        width: 6; height: 6; radius: 3
                                                        color: isOn ? "#F5B841" : "#222830"
                                                        border.color: isOn ? "#FFE6A3" : "#14171d"
                                                        border.width: 1

                                                        // Subtle LED halo
                                                        Rectangle {
                                                            visible: isOn
                                                            anchors.centerIn: parent
                                                            width: 10; height: 10; radius: 5
                                                            color: "#30F5B841"
                                                            z: -1
                                                        }
                                                    }

                                                    Label {
                                                        text: "R-" + (index < 9 ? "0" : "") + (index + 1)
                                                        font.family: "Monospace"
                                                        font.pixelSize: 11
                                                        font.bold: isSelected
                                                        color: isSelected ? "#F5B841" : (isOn ? "#8896A4" : "#4A5460")
                                                    }

                                                    Item { Layout.fillWidth: true }

                                                    Label {
                                                        text: bandData && bandData.q !== undefined ? ("Q" + bandData.q.toFixed(1)) : ""
                                                        font.family: "Monospace"
                                                        font.pixelSize: 10
                                                        color: isSelected ? "#FFDF8A" : (isOn ? "#687684" : "#38414B")
                                                    }
                                                }

                                                // Data Row: FREQ and GAIN in muted gray (champagne gold only when selected)
                                                RowLayout {
                                                    Layout.fillWidth: true
                                                    Layout.fillHeight: true
                                                    spacing: 3

                                                    Column {
                                                        Layout.fillWidth: true
                                                        spacing: 1
                                                        Label {
                                                            text: "FREQ"
                                                            font.family: "Monospace"
                                                            font.pixelSize: 7
                                                            color: isSelected ? "#AD8C47" : "#3E4752"
                                                        }
                                                        Label {
                                                            text: bandData && bandData.frequency ? (bandData.frequency >= 1000 ? (bandData.frequency / 1000).toFixed(1) + "k" : Math.round(bandData.frequency) + "Hz") : "--"
                                                            font.family: "Monospace"
                                                            font.pixelSize: 11
                                                            font.bold: isSelected
                                                            color: isSelected ? "#F5B841" : (isOn ? "#7E8C9A" : "#454E57")
                                                        }
                                                    }

                                                    Column {
                                                        Layout.fillWidth: true
                                                        spacing: 1
                                                        Label {
                                                            text: "GAIN"
                                                            font.family: "Monospace"
                                                            font.pixelSize: 7
                                                            color: isSelected ? "#AD8C47" : "#3E4752"
                                                        }
                                                        Label {
                                                            text: bandData && bandData.gainDb !== undefined
                                                                  ? ((bandData.gainDb > 0 ? "+" : "") + bandData.gainDb.toFixed(1))
                                                                  : "--"
                                                            font.family: "Monospace"
                                                            font.pixelSize: 11
                                                            font.bold: isSelected
                                                            color: isSelected ? "#F5B841" : (isOn ? "#7E8C9A" : "#454E57")
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
                                        text: consoleRoot.dspFilterBypassed ? qsTr("OFF") : qsTr("ON")
                                        implicitWidth: 68; implicitHeight: 26; fontSize: 10
                                        showLed: true; ledColor: consoleRoot.dspFilterBypassed ? "#4A5462" : "#00E676"
                                        accentColor: "#48D8FF"
                                        highlighted: !consoleRoot.dspFilterBypassed
                                        enabled: !consoleRoot.isDirect
                                        toolTipText: qsTr("Toggle HPF/LPF Module ON / OFF")
                                        onClicked: EngineController.setDspNodeBypass("Filter", !consoleRoot.dspFilterBypassed)
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
                                    clip: true

                                    RecessedScreenOverlay {
                                        cornerRadius: 4
                                        topShadowSize: 8
                                        sideShadowSize: 5
                                        bottomShadowSize: 4
                                        topShadowOpacity: 0.68
                                        sideShadowOpacity: 0.42
                                        bottomShadowOpacity: 0.30
                                        glassSheenOpacity: 0.065
                                    }

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

                                // Row 3: HPF (with independent HPF IN/OUT Trim) & LPF (with independent LPF IN/OUT Trim)
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    spacing: 6

                                    // HPF Strip
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        spacing: 4

                                        ColumnLayout {
                                            Layout.preferredWidth: 46
                                            spacing: 3
                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 3
                                                Rectangle {
                                                    readonly property bool presetOn: !!(EngineController.dspPresetIndicators && EngineController.dspPresetIndicators["hpf"])
                                                    implicitWidth: 8; implicitHeight: 8; radius: 4
                                                    color: presetOn ? "#00E5FF" : "#2D3743"
                                                    border.color: presetOn ? "#8DF2FF" : "#445160"
                                                    ToolTip.visible: hpfIndMa.containsMouse
                                                    ToolTip.text: presetOn ? qsTr("HPF Preset Applied") : qsTr("HPF Preset Indicator: OFF")
                                                    MouseArea { id: hpfIndMa; anchors.fill: parent; hoverEnabled: true }
                                                }
                                                Label {
                                                    text: "HPF"
                                                    font.family: "Monospace"; font.pixelSize: 8; font.bold: true; color: "#8EC3D6"
                                                }
                                            }
                                            ConsoleButton {
                                                Layout.fillWidth: true
                                                implicitHeight: 30
                                                text: consoleRoot.dspHpfEnabled ? qsTr("ON") : qsTr("OFF")
                                                fontSize: 9
                                                highlighted: consoleRoot.dspHpfEnabled
                                                enabled: !consoleRoot.isDirect
                                                onClicked: EngineController.setHpfNodeEnabled(!consoleRoot.dspHpfEnabled)
                                            }
                                            ConsoleButton {
                                                Layout.fillWidth: true
                                                implicitHeight: 22
                                                text: consoleRoot.dspHpfSlope === 1 ? "24dB" : (consoleRoot.dspHpfSlope === 2 ? "18dB" : "12dB")
                                                fontSize: 9
                                                enabled: !consoleRoot.isDirect
                                                onClicked: {
                                                    var nextSlope = consoleRoot.dspHpfSlope === 0 ? 2 : (consoleRoot.dspHpfSlope === 2 ? 1 : 0)
                                                    consoleRoot.dspHpfSlope = nextSlope
                                                    EngineController.setFilterParams(dspHpfKnob.value, consoleRoot.dspHpfEnabled, consoleRoot.dspHpfSlope, dspLpfKnob.value, consoleRoot.dspLpfEnabled, consoleRoot.dspLpfSlope, dspHpfQKnob.value, dspLpfQKnob.value)
                                                }
                                            }
                                        }

                                        ConsoleKnob {
                                            id: dspHpfKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("HPF Freq")
                                            minValue: 10.0; maxValue: 1000.0; defaultValue: 20.0; logarithmic: true
                                            unitSuffix: " Hz"; decimals: 0; knobDiameter: 40; arcColor: "#48D8FF"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setFilterParams(v, consoleRoot.dspHpfEnabled, consoleRoot.dspHpfSlope, dspLpfKnob.value, consoleRoot.dspLpfEnabled, consoleRoot.dspLpfSlope, dspHpfQKnob.value, dspLpfQKnob.value) }
                                        }

                                        ConsoleKnob {
                                            id: dspHpfQKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("HPF Q")
                                            minValue: 0.1; maxValue: 10.0; defaultValue: 0.71; logarithmic: true
                                            unitSuffix: ""; decimals: 2; knobDiameter: 40; arcColor: "#48D8FF"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setFilterParams(dspHpfKnob.value, consoleRoot.dspHpfEnabled, consoleRoot.dspHpfSlope, dspLpfKnob.value, consoleRoot.dspLpfEnabled, consoleRoot.dspLpfSlope, v, dspLpfQKnob.value) }
                                        }

                                        Rectangle { Layout.preferredWidth: 1; width: 1; Layout.fillHeight: true; color: "#2D526E" }

                                        ConsoleKnob {
                                            id: hpfInTrimKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("IN")
                                            minValue: -12.0; maxValue: 12.0; defaultValue: 0.0; logarithmic: false; bipolar: true
                                            unitSuffix: "dB"; decimals: 1; knobDiameter: 38; arcColor: "#48D8FF"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setDspTrim("hpf", false, v) }
                                        }
                                        ConsoleKnob {
                                            id: hpfOutTrimKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("OUT")
                                            minValue: -12.0; maxValue: 12.0; defaultValue: 0.0; logarithmic: false; bipolar: true
                                            unitSuffix: "dB"; decimals: 1; knobDiameter: 38; arcColor: "#48D8FF"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setDspTrim("hpf", true, v) }
                                        }
                                    }

                                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; height: 1; color: "#26465E" }

                                    // LPF Strip
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        spacing: 4

                                        ColumnLayout {
                                            Layout.preferredWidth: 46
                                            spacing: 3
                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 3
                                                Rectangle {
                                                    readonly property bool presetOn: !!(EngineController.dspPresetIndicators && EngineController.dspPresetIndicators["lpf"])
                                                    implicitWidth: 8; implicitHeight: 8; radius: 4
                                                    color: presetOn ? "#00E5FF" : "#2D3743"
                                                    border.color: presetOn ? "#8DF2FF" : "#445160"
                                                    ToolTip.visible: lpfIndMa.containsMouse
                                                    ToolTip.text: presetOn ? qsTr("LPF Preset Applied") : qsTr("LPF Preset Indicator: OFF")
                                                    MouseArea { id: lpfIndMa; anchors.fill: parent; hoverEnabled: true }
                                                }
                                                Label {
                                                    text: "LPF"
                                                    font.family: "Monospace"; font.pixelSize: 8; font.bold: true; color: "#8EC3D6"
                                                }
                                            }
                                            ConsoleButton {
                                                Layout.fillWidth: true
                                                implicitHeight: 30
                                                text: consoleRoot.dspLpfEnabled ? qsTr("ON") : qsTr("OFF")
                                                fontSize: 9
                                                highlighted: consoleRoot.dspLpfEnabled
                                                enabled: !consoleRoot.isDirect
                                                onClicked: EngineController.setLpfNodeEnabled(!consoleRoot.dspLpfEnabled)
                                            }
                                            ConsoleButton {
                                                Layout.fillWidth: true
                                                implicitHeight: 22
                                                text: consoleRoot.dspLpfSlope === 1 ? "24dB" : (consoleRoot.dspLpfSlope === 2 ? "18dB" : "12dB")
                                                fontSize: 9
                                                enabled: !consoleRoot.isDirect
                                                onClicked: {
                                                    var nextSlope = consoleRoot.dspLpfSlope === 0 ? 2 : (consoleRoot.dspLpfSlope === 2 ? 1 : 0)
                                                    consoleRoot.dspLpfSlope = nextSlope
                                                    EngineController.setFilterParams(dspHpfKnob.value, consoleRoot.dspHpfEnabled, consoleRoot.dspHpfSlope, dspLpfKnob.value, consoleRoot.dspLpfEnabled, consoleRoot.dspLpfSlope, dspHpfQKnob.value, dspLpfQKnob.value)
                                                }
                                            }
                                        }

                                        ConsoleKnob {
                                            id: dspLpfKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("LPF Freq")
                                            minValue: 1000.0; maxValue: 22000.0; defaultValue: 20000.0; logarithmic: true
                                            unitSuffix: " Hz"; decimals: 0; knobDiameter: 40; arcColor: "#48D8FF"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setFilterParams(dspHpfKnob.value, consoleRoot.dspHpfEnabled, consoleRoot.dspHpfSlope, v, consoleRoot.dspLpfEnabled, consoleRoot.dspLpfSlope, dspHpfQKnob.value, dspLpfQKnob.value) }
                                        }

                                        ConsoleKnob {
                                            id: dspLpfQKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("LPF Q")
                                            minValue: 0.1; maxValue: 10.0; defaultValue: 0.71; logarithmic: true
                                            unitSuffix: ""; decimals: 2; knobDiameter: 40; arcColor: "#48D8FF"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setFilterParams(dspHpfKnob.value, consoleRoot.dspHpfEnabled, consoleRoot.dspHpfSlope, dspLpfKnob.value, consoleRoot.dspLpfEnabled, consoleRoot.dspLpfSlope, dspHpfQKnob.value, v) }
                                        }

                                        Rectangle { Layout.preferredWidth: 1; width: 1; Layout.fillHeight: true; color: "#2D526E" }

                                        ConsoleKnob {
                                            id: lpfInTrimKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("IN")
                                            minValue: -12.0; maxValue: 12.0; defaultValue: 0.0; logarithmic: false; bipolar: true
                                            unitSuffix: "dB"; decimals: 1; knobDiameter: 38; arcColor: "#48D8FF"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setDspTrim("lpf", false, v) }
                                        }
                                        ConsoleKnob {
                                            id: lpfOutTrimKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("OUT")
                                            minValue: -12.0; maxValue: 12.0; defaultValue: 0.0; logarithmic: false; bipolar: true
                                            unitSuffix: "dB"; decimals: 1; knobDiameter: 38; arcColor: "#48D8FF"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setDspTrim("lpf", true, v) }
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
                                    Rectangle {
                                        readonly property bool presetOn: !!(EngineController.dspPresetIndicators && EngineController.dspPresetIndicators["compressor"])
                                        implicitWidth: 10; implicitHeight: 10; radius: 5
                                        color: presetOn ? "#00E5FF" : "#2D3743"
                                        border.color: presetOn ? "#8DF2FF" : "#445160"
                                        ToolTip.visible: compIndMa.containsMouse
                                        ToolTip.text: presetOn ? qsTr("Compressor Preset Applied") : qsTr("Compressor Preset Indicator: OFF")
                                        MouseArea { id: compIndMa; anchors.fill: parent; hoverEnabled: true }
                                    }
                                    Item { Layout.fillWidth: true }
                                    ConsoleButton {
                                        text: consoleRoot.dspCompBypassed ? qsTr("OFF") : qsTr("ON")
                                        implicitWidth: 68; implicitHeight: 26; fontSize: 10
                                        showLed: true; ledColor: consoleRoot.dspCompBypassed ? "#4A5462" : "#00E676"
                                        accentColor: "#F5B841"
                                        highlighted: !consoleRoot.dspCompBypassed
                                        enabled: !consoleRoot.isDirect
                                        toolTipText: qsTr("Toggle Compressor ON / OFF")
                                        onClicked: EngineController.setDspNodeBypass("Compressor", !consoleRoot.dspCompBypassed)
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
                                    clip: true

                                    RecessedScreenOverlay {
                                        cornerRadius: 4
                                        topShadowSize: 8
                                        sideShadowSize: 5
                                        bottomShadowSize: 4
                                        topShadowOpacity: 0.68
                                        sideShadowOpacity: 0.42
                                        bottomShadowOpacity: 0.30
                                        glassSheenOpacity: 0.065
                                    }

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
                                            linearScale: true
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
                                            minValue: -60.0; maxValue: 0.0; defaultValue: 0.0; logarithmic: false
                                            unitSuffix: " dB"; decimals: 1; knobDiameter: 44; arcColor: "#F5B841"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setCompressorParams(v, dspCompRatioKnob.value, dspCompAttackKnob.value, dspCompReleaseKnob.value, dspCompMakeupKnob.value, dspCompKneeKnob.value) }
                                        }
                                        ConsoleKnob {
                                            id: dspCompRatioKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("Ratio")
                                            minValue: 1.0; maxValue: 20.0; defaultValue: 1.0; logarithmic: true
                                            unitSuffix: ":1"; decimals: 1; knobDiameter: 44; arcColor: "#F5B841"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setCompressorParams(dspCompThreshKnob.value, v, dspCompAttackKnob.value, dspCompReleaseKnob.value, dspCompMakeupKnob.value, dspCompKneeKnob.value) }
                                        }
                                        ConsoleKnob {
                                            id: dspCompKneeKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("Knee")
                                            minValue: 0.0; maxValue: 20.0; defaultValue: 0.0; logarithmic: false
                                            unitSuffix: " dB"; decimals: 1; knobDiameter: 44; arcColor: "#F5B841"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setCompressorParams(dspCompThreshKnob.value, dspCompRatioKnob.value, dspCompAttackKnob.value, dspCompReleaseKnob.value, dspCompMakeupKnob.value, v) }
                                        }
                                        ConsoleKnob {
                                            id: dspCompAttackKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("Attack")
                                            minValue: 0.1; maxValue: 500.0; defaultValue: 10.0; logarithmic: true
                                            unitSuffix: " ms"; decimals: 1; knobDiameter: 44; arcColor: "#F5B841"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setCompressorParams(dspCompThreshKnob.value, dspCompRatioKnob.value, v, dspCompReleaseKnob.value, dspCompMakeupKnob.value, dspCompKneeKnob.value) }
                                        }
                                        ConsoleKnob {
                                            id: dspCompReleaseKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("Release")
                                            minValue: 1.0; maxValue: 1000.0; defaultValue: 100.0; logarithmic: true
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

                                    Rectangle { Layout.preferredWidth: 1; width: 1; Layout.fillHeight: true; color: "#363E4A" }
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
                                        Layout.preferredHeight: 18
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
                                    Rectangle {
                                        readonly property bool presetOn: !!(EngineController.dspPresetIndicators && EngineController.dspPresetIndicators["limiter"])
                                        implicitWidth: 10; implicitHeight: 10; radius: 5
                                        color: presetOn ? "#00E5FF" : "#2D3743"
                                        border.color: presetOn ? "#8DF2FF" : "#445160"
                                        ToolTip.visible: limIndMa.containsMouse
                                        ToolTip.text: presetOn ? qsTr("Limiter Preset Applied") : qsTr("Limiter Preset Indicator: OFF")
                                        MouseArea { id: limIndMa; anchors.fill: parent; hoverEnabled: true }
                                    }
                                    Item { Layout.fillWidth: true }
                                    ConsoleButton {
                                        text: consoleRoot.dspLimBypassed ? qsTr("OFF") : qsTr("ON")
                                        implicitWidth: 68; implicitHeight: 26; fontSize: 10
                                        showLed: true; ledColor: consoleRoot.dspLimBypassed ? "#4A5462" : "#00E676"
                                        accentColor: "#F2C044"
                                        highlighted: !consoleRoot.dspLimBypassed
                                        enabled: !consoleRoot.isDirect
                                        toolTipText: qsTr("Toggle Peak Limiter ON / OFF")
                                        onClicked: EngineController.setDspNodeBypass("Limiter", !consoleRoot.dspLimBypassed)
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
                                    clip: true

                                    RecessedScreenOverlay {
                                        cornerRadius: 4
                                        topShadowSize: 8
                                        sideShadowSize: 5
                                        bottomShadowSize: 4
                                        topShadowOpacity: 0.68
                                        sideShadowOpacity: 0.42
                                        bottomShadowOpacity: 0.30
                                        glassSheenOpacity: 0.065
                                    }

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
                                            linearScale: true
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
                                            minValue: -12.0; maxValue: 0.0; defaultValue: 0.0; logarithmic: false
                                            unitSuffix: " dBFS"; decimals: 1; knobDiameter: 66; arcColor: "#F2C044"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setLimiterParams(v, dspLimRelKnob.value, dspLimLookKnob.value) }
                                        }
                                        ConsoleKnob {
                                            id: dspLimRelKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("Release")
                                            minValue: 10.0; maxValue: 500.0; defaultValue: 50.0; logarithmic: true
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

                                    Rectangle { Layout.preferredWidth: 1; width: 1; Layout.fillHeight: true; color: "#594718" }
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
                                    Rectangle {
                                        readonly property bool presetOn: !!(EngineController.dspPresetIndicators && EngineController.dspPresetIndicators["reverb"])
                                        implicitWidth: 10; implicitHeight: 10; radius: 5
                                        color: presetOn ? "#00E5FF" : "#2D3743"
                                        border.color: presetOn ? "#8DF2FF" : "#445160"
                                        ToolTip.visible: revIndMa.containsMouse
                                        ToolTip.text: presetOn ? qsTr("Reverb Preset Applied") : qsTr("Reverb Preset Indicator: OFF")
                                        MouseArea { id: revIndMa; anchors.fill: parent; hoverEnabled: true }
                                    }
                                    Item { Layout.fillWidth: true }
                                    ConsoleButton {
                                        text: consoleRoot.dspRevBypassed ? qsTr("OFF") : qsTr("ON")
                                        implicitWidth: 68; implicitHeight: 26; fontSize: 10
                                        showLed: true; ledColor: consoleRoot.dspRevBypassed ? "#4A5462" : "#00E676"
                                        accentColor: "#48D8FF"
                                        highlighted: !consoleRoot.dspRevBypassed
                                        enabled: !consoleRoot.isDirect
                                        toolTipText: qsTr("Toggle Reverb ON / OFF")
                                        onClicked: EngineController.setDspNodeBypass("Reverb", !consoleRoot.dspRevBypassed)
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
                                    clip: true

                                    RecessedScreenOverlay {
                                        cornerRadius: 4
                                        topShadowSize: 8
                                        sideShadowSize: 5
                                        bottomShadowSize: 4
                                        topShadowOpacity: 0.68
                                        sideShadowOpacity: 0.42
                                        bottomShadowOpacity: 0.30
                                        glassSheenOpacity: 0.065
                                    }

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
                                            var preX = Math.min(width * 0.25, (dspRevPreKnob.value / 200.0) * width * 0.25)
                                            var decayRate = 6.9 / Math.max(0.1, dspRevDecayKnob.value)
                                            for (var x = preX; x <= width; x += 2) {
                                                var tSec = ((x - preX) / Math.max(1, width - preX)) * 5.0
                                                var amp = Math.exp(-tSec * decayRate * 0.5)
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
                                            minValue: 0.0; maxValue: 200.0; defaultValue: 0.0; logarithmic: false
                                            unitSuffix: " ms"; decimals: 0; knobDiameter: 44; arcColor: "#48D8FF"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setReverbParams(dspRevRoomKnob.value, dspRevDampKnob.value, dspRevWetKnob.value / 100.0, v, consoleRoot.dspRevTailPreserving, dspRevDecayKnob.value); reverbMiniCanvas.requestPaint() }
                                        }
                                        ConsoleKnob {
                                            id: dspRevRoomKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("Size")
                                            minValue: 0.0; maxValue: 1.0; defaultValue: 0.10; logarithmic: false
                                            unitSuffix: ""; decimals: 2; knobDiameter: 44; arcColor: "#48D8FF"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setReverbParams(v, dspRevDampKnob.value, dspRevWetKnob.value / 100.0, dspRevPreKnob.value, consoleRoot.dspRevTailPreserving, dspRevDecayKnob.value); reverbMiniCanvas.requestPaint() }
                                        }
                                        ConsoleKnob {
                                            id: dspRevDampKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("Damping")
                                            minValue: 0.0; maxValue: 1.0; defaultValue: 0.50; logarithmic: false
                                            unitSuffix: ""; decimals: 2; knobDiameter: 44; arcColor: "#48D8FF"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setReverbParams(dspRevRoomKnob.value, v, dspRevWetKnob.value / 100.0, dspRevPreKnob.value, consoleRoot.dspRevTailPreserving, dspRevDecayKnob.value) }
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
                                                EngineController.setReverbParams(dspRevRoomKnob.value, dspRevDampKnob.value, dspRevWetKnob.value / 100.0, dspRevPreKnob.value, consoleRoot.dspRevTailPreserving, dspRevDecayKnob.value)
                                            }
                                        }
                                        ConsoleKnob {
                                            id: dspRevWetKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("Wet/Dry")
                                            minValue: 0.0; maxValue: 100.0; defaultValue: 0.0; logarithmic: false
                                            unitSuffix: " %"; decimals: 0; knobDiameter: 44; arcColor: "#48D8FF"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setReverbParams(dspRevRoomKnob.value, dspRevDampKnob.value, v / 100.0, dspRevPreKnob.value, consoleRoot.dspRevTailPreserving, dspRevDecayKnob.value) }
                                        }
                                        ConsoleKnob {
                                            id: dspRevDecayKnob
                                            Layout.fillWidth: true
                                            labelText: qsTr("Decay")
                                            minValue: 0.1; maxValue: 10.0; defaultValue: 0.2; logarithmic: true
                                            unitSuffix: " s"; decimals: 1; knobDiameter: 44; arcColor: "#48D8FF"
                                            enabled: !consoleRoot.isDirect
                                            onValueEdited: function(v) { value = v; EngineController.setReverbParams(dspRevRoomKnob.value, dspRevDampKnob.value, dspRevWetKnob.value / 100.0, dspRevPreKnob.value, consoleRoot.dspRevTailPreserving, v); reverbMiniCanvas.requestPaint() }
                                        }
                                    }

                                    Rectangle { Layout.preferredWidth: 1; width: 1; Layout.fillHeight: true; color: "#244D5A" }
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
                            baseTint: "#13171d"
                            borderColor: "#2f3743"
                            showScrews: true
                            cornerRadius: 5
                            brushedOpacity: 0.32

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 12
                                spacing: 7

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6
                                    Label { text: qsTr("GRAPH:"); font.family: "Monospace"; font.pixelSize: 11; color: "#9AA9B8" }
                                    Rectangle {
                                        Layout.fillWidth: true; Layout.preferredHeight: 26; height: 26; radius: 3; color: "#0B0E12"; border.color: "#2E3744"
                                        Label {
                                            anchors.verticalCenter: parent.verticalCenter
                                            anchors.left: parent.left; anchors.leftMargin: 8
                                            text: qsTr("LIVE %1").arg((EngineController.audioBackend || "PipeWire").toUpperCase())
                                            font.family: "Monospace"
                                            font.pixelSize: 11; color: "#DCE6F0"
                                        }
                                    }
                                    ConsoleButton {
                                        text: qsTr("SCAN PORTS")
                                        implicitWidth: 96; implicitHeight: 26; fontSize: 10
                                        onClicked: consoleRoot.refreshExternalPorts()
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6
                                    Label { text: qsTr("TARGET CLIENT:"); font.family: "Monospace"; font.pixelSize: 10; color: "#9AA9B8" }
                                    ConsoleComboBox {
                                        id: extClientCombo
                                        Layout.fillWidth: true
                                        implicitHeight: 26
                                        font.pixelSize: 11
                                        textRole: "clientName"
                                        model: consoleRoot.portDiscoveryList.length > 0
                                               ? consoleRoot.portDiscoveryList
                                               : [{ clientName: qsTr("No External Insert Ports Discovered") }]
                                        currentIndex: consoleRoot.portDiscoveryList.length > 0
                                                      ? Math.max(0, Math.min(consoleRoot.portDiscoveryList.length - 1, consoleRoot.selectedTargetClientIdx))
                                                      : 0
                                        onActivated: function(index) {
                                            consoleRoot.selectedTargetClientIdx = index
                                            patchBayCanvas.requestPaint()
                                        }
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
                                            var st = EngineController.externalInsertState
                                            var connected = (st === 3 || st === 4)
                                            var sel = (consoleRoot.portDiscoveryList && consoleRoot.portDiscoveryList.length > 0)
                                                      ? consoleRoot.portDiscoveryList[Math.max(0, Math.min(consoleRoot.portDiscoveryList.length - 1, consoleRoot.selectedTargetClientIdx))]
                                                      : null
                                            function shortPort(p, fb) {
                                                if (!p || p.length === 0) return fb
                                                return p.length > 20 ? ("…" + p.slice(p.length - 19)) : p
                                            }
                                            var rightLabels = connected
                                                              ? [
                                                                  shortPort(EngineController.externalInsertSendPortL, "ext:in_1"),
                                                                  shortPort(EngineController.externalInsertSendPortR, "ext:in_2"),
                                                                  shortPort(EngineController.externalInsertReturnPortL, "ext:out_1"),
                                                                  shortPort(EngineController.externalInsertReturnPortR, "ext:out_2")
                                                              ]
                                                              : (sel ? [
                                                                  shortPort(sel.sendPortL, qsTr("(candidate)")),
                                                                  shortPort(sel.sendPortR, qsTr("(candidate)")),
                                                                  shortPort(sel.returnPortL, qsTr("(candidate)")),
                                                                  shortPort(sel.returnPortR, qsTr("(candidate)"))
                                                              ] : [qsTr("(unpatched)"), qsTr("(unpatched)"), qsTr("(unpatched)"), qsTr("(unpatched)")])
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

                        // COLUMN 2: SIGNAL FLOW & INSERT STATE (2:1 Status/Bit-Perfect + Slot 0-6 + Prominent Send/Return Meters)
                        BrushedMetalPanel {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            baseTint: "#13171d"
                            borderColor: "#2f3743"
                            showScrews: true
                            cornerRadius: 5
                            brushedOpacity: 0.32

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 12
                                spacing: 8

                                Label {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: qsTr("SIGNAL FLOW & INSERT STATE")
                                    font.family: "Monospace"
                                    font.pixelSize: 12
                                    font.bold: true
                                    color: "#E8BA6B"
                                }

                                // Row 1: Prominent STATUS (2) + BIT-PERFECT EXCLUDED (1) side-by-side at 2:1 ratio
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: false
                                    Layout.preferredHeight: 74
                                    Layout.maximumHeight: 78
                                    spacing: 8

                                    // Left 2/3: Larger STATUS Box
                                    Rectangle {
                                        readonly property int st: EngineController.externalInsertState
                                        readonly property bool isLive: (st === 3 || st === 4)
                                        readonly property bool isWarn: (st === 7 || st === 9)
                                        readonly property bool isErr: (st === 5 || st === 6 || st === 10)
                                        Layout.fillWidth: true
                                        Layout.preferredWidth: 520
                                        Layout.fillHeight: true
                                        radius: 4
                                        color: isErr ? "#291215" : (isWarn ? "#2C220D" : (isLive ? "#0A1E15" : "#0A1116"))
                                        border.color: isErr ? "#A83B45" : (isWarn ? "#D49B2A" : (isLive ? "#257852" : "#2D3B48"))
                                        border.width: 1

                                        Column {
                                            anchors.centerIn: parent
                                            spacing: 3
                                            Label {
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                text: qsTr("INSERT STATUS")
                                                font.family: "Monospace"
                                                font.pixelSize: 10
                                                color: "#7E94A6"
                                            }
                                            Row {
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                spacing: 8
                                                Rectangle {
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    width: 9; height: 9; radius: 4.5
                                                    color: parent.parent.parent.isErr ? "#FF5252"
                                                           : (parent.parent.parent.isWarn ? "#FFB300"
                                                              : (parent.parent.parent.isLive ? "#00E676" : "#5B6B7A"))
                                                }
                                                Label {
                                                    text: EngineController.externalInsertStateText.toUpperCase()
                                                    font.family: "Monospace"
                                                    font.pixelSize: 17
                                                    font.bold: true
                                                    color: parent.parent.parent.isErr ? "#FF8A8A"
                                                           : (parent.parent.parent.isWarn ? "#FFD166"
                                                              : (parent.parent.parent.isLive ? "#00E676" : "#D2E0EC"))
                                                }
                                            }
                                        }
                                        ToolTip.visible: extStatusHover.containsMouse && EngineController.externalInsertLastError.length > 0
                                        ToolTip.text: EngineController.externalInsertLastError
                                        MouseArea {
                                            id: extStatusHover
                                            anchors.fill: parent
                                            hoverEnabled: true
                                        }
                                    }

                                    // Right 1/3: BIT-PERFECT EXCLUDED Box
                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredWidth: 260
                                        Layout.fillHeight: true
                                        radius: 4
                                        color: "#291215"
                                        border.color: "#94333B"
                                        border.width: 1

                                        Column {
                                            anchors.centerIn: parent
                                            spacing: 3
                                            Label {
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                text: qsTr("BIT-PERFECT")
                                                font.family: "Monospace"
                                                font.pixelSize: 10
                                                color: "#D4888E"
                                            }
                                            Label {
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                text: qsTr("EXCLUDED")
                                                font.family: "Monospace"
                                                font.pixelSize: 15
                                                font.bold: true
                                                color: "#FFA6AC"
                                            }
                                        }
                                    }
                                }

                                // Row 2: Master Chain & Clickable Insert Slot 0..6 Buttons
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: false
                                    Layout.preferredHeight: 74
                                    Layout.maximumHeight: 78
                                    radius: 4; color: "#06090D"; border.color: "#222B35"

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 8
                                        spacing: 6
                                        Label {
                                            Layout.alignment: Qt.AlignHCenter
                                            text: qsTr("PEQ → GEQ → HPF/LPF → COMP → REV → LIM   [INSERT @ SLOT %1]").arg(EngineController.externalInsertPosition)
                                            font.family: "Monospace"; font.pixelSize: 10; font.bold: true; color: "#B8C8D8"
                                        }
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 4
                                            Repeater {
                                                model: ["Slot 0", "Slot 1", "Slot 2", "Slot 3", "Slot 4", "Slot 5", "Slot 6"]
                                                ConsoleButton {
                                                    Layout.fillWidth: true
                                                    implicitHeight: 28
                                                    text: modelData
                                                    fontSize: 9
                                                    leftPadding: 3; rightPadding: 3
                                                    accentColor: "#00E5FF"
                                                    highlighted: EngineController.externalInsertPosition === index
                                                    enabled: !consoleRoot.isDirect
                                                    onClicked: EngineController.setExternalInsertPosition(index)
                                                }
                                            }
                                        }
                                    }
                                }

                                // Row 3: Prominent Send & Return LED Level Meters directly under Insert Slot buttons
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    Layout.minimumHeight: 80
                                    Layout.preferredHeight: 112
                                    implicitHeight: 96
                                    radius: 4
                                    color: "#070A0F"
                                    border.color: "#232C37"

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 10
                                        spacing: 8

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.fillHeight: true
                                            spacing: 10
                                            Label {
                                                text: qsTr("SEND LEVEL")
                                                font.family: "Monospace"
                                                font.pixelSize: 11
                                                font.bold: true
                                                color: "#8ECAE6"
                                                Layout.preferredWidth: 104
                                            }
                                            ConsoleLedMeter {
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 20
                                                horizontal: true
                                                segmentCount: 32
                                                peakValue: EngineController.externalInsertSendPeak
                                                rmsValue: EngineController.externalInsertSendPeak * 0.72
                                            }
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.fillHeight: true
                                            spacing: 10
                                            Label {
                                                text: qsTr("RETURN LEVEL")
                                                font.family: "Monospace"
                                                font.pixelSize: 11
                                                font.bold: true
                                                color: "#00E676"
                                                Layout.preferredWidth: 104
                                            }
                                            ConsoleLedMeter {
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 20
                                                horizontal: true
                                                segmentCount: 32
                                                peakValue: EngineController.externalInsertReturnPeak
                                                rmsValue: EngineController.externalInsertReturnPeak * 0.72
                                            }
                                        }
                                    }
                                }

                                // Row 4: Action Buttons
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: false
                                    Layout.preferredHeight: 38
                                    spacing: 8
                                    ConsoleButton {
                                        Layout.fillWidth: true
                                        implicitHeight: 38
                                        text: (EngineController.externalInsertState === 3 || EngineController.externalInsertState === 4)
                                              ? qsTr("RECONNECT HOST")
                                              : qsTr("CONNECT HOST")
                                        fontSize: 11
                                        accentColor: "#00E676"
                                        enabled: !consoleRoot.isDirect
                                        onClicked: consoleRoot.connectSelectedExternalHost()
                                    }
                                    ConsoleButton {
                                        Layout.fillWidth: true
                                        implicitHeight: 38
                                        text: EngineController.externalInsertBypassed ? qsTr("INSERT BYPASS: ON") : qsTr("INSERT BYPASS (DRY)")
                                        fontSize: 11
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
                            baseTint: "#13171d"
                            borderColor: "#2f3743"
                            showScrews: true
                            cornerRadius: 5
                            brushedOpacity: 0.32

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
                                        onClicked: EngineController.confirmRecommendedInsertLatency(false)
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

    PresetModalPopup {
        id: dspPresetModal
    }
}
