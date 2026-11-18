import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Desk32.Core

Rectangle {
    id: dspPreviewRoot

    implicitWidth: 420
    implicitHeight: 700
    color: "#121519"
    border.color: "#262b32"
    border.width: 1
    radius: 6
    clip: true

    signal closeRequested()

    // Internal flags to prevent feedback loops between UI updates and engine signals
    property bool updatingFromEngine: false

    function reloadParameters() {
        updatingFromEngine = true;

        // 1. Filter
        var flt = EngineController.getFilterParams();
        if (flt) {
            hpfFreqSlider.value = flt.hpfFrequency !== undefined ? flt.hpfFrequency : 20.0;
            hpfEnableCheck.checked = flt.hpfEnabled !== undefined ? flt.hpfEnabled : false;
            hpfSlopeCombo.currentIndex = flt.hpfSlope === 1 ? 1 : 0;
            hpfQSlider.value = flt.hpfQ !== undefined ? flt.hpfQ : 0.70710678;

            lpfFreqSlider.value = flt.lpfFrequency !== undefined ? flt.lpfFrequency : 20000.0;
            lpfEnableCheck.checked = flt.lpfEnabled !== undefined ? flt.lpfEnabled : false;
            lpfSlopeCombo.currentIndex = flt.lpfSlope === 1 ? 1 : 0;
            lpfQSlider.value = flt.lpfQ !== undefined ? flt.lpfQ : 0.70710678;
        }

        // 2. Compressor
        var comp = EngineController.getCompressorParams();
        if (comp) {
            compThreshSlider.value = comp.thresholdDb !== undefined ? comp.thresholdDb : 0.0;
            compRatioSlider.value = comp.ratio !== undefined ? comp.ratio : 1.0;
            compAttackSlider.value = comp.attackMs !== undefined ? comp.attackMs : 10.0;
            compReleaseSlider.value = comp.releaseMs !== undefined ? comp.releaseMs : 100.0;
            compMakeupSlider.value = comp.makeupGainDb !== undefined ? comp.makeupGainDb : 0.0;
            compKneeSlider.value = comp.kneeDb !== undefined ? comp.kneeDb : 0.0;
        }

        // 3. Limiter
        var lim = EngineController.getLimiterParams();
        if (lim) {
            limCeilingSlider.value = lim.ceilingDb !== undefined ? lim.ceilingDb : 0.0;
            limReleaseSlider.value = lim.releaseMs !== undefined ? lim.releaseMs : 50.0;
            limLookaheadSlider.value = lim.lookaheadMs !== undefined ? lim.lookaheadMs : 2.0;
        }

        // 4. Reverb
        var rev = EngineController.getReverbParams();
        if (rev) {
            revRoomSizeSlider.value = rev.roomSize !== undefined ? rev.roomSize : 0.5;
            revDampingSlider.value = rev.damping !== undefined ? rev.damping : 0.5;
            revWetSlider.value = rev.wetLevel !== undefined ? rev.wetLevel : 0.2;
            revPredelaySlider.value = rev.predelayMs !== undefined ? rev.predelayMs : 20.0;
            revTailCheck.checked = rev.tailPreserving !== undefined ? rev.tailPreserving : true;
        }

        updatingFromEngine = false;
    }

    Component.onCompleted: reloadParameters()

    Connections {
        target: EngineController
        function onFilterParametersChanged() { if (!updatingFromEngine) reloadParameters(); }
        function onCompressorParametersChanged() { if (!updatingFromEngine) reloadParameters(); }
        function onLimiterParametersChanged() { if (!updatingFromEngine) reloadParameters(); }
        function onReverbParametersChanged() { if (!updatingFromEngine) reloadParameters(); }
    }

    function applyFilter() {
        if (updatingFromEngine) return;
        EngineController.setFilterParams(
            hpfFreqSlider.value,
            hpfEnableCheck.checked,
            hpfSlopeCombo.currentIndex,
            lpfFreqSlider.value,
            lpfEnableCheck.checked,
            lpfSlopeCombo.currentIndex,
            hpfQSlider.value,
            lpfQSlider.value
        );
    }

    function applyCompressor() {
        if (updatingFromEngine) return;
        EngineController.setCompressorParams(
            compThreshSlider.value,
            compRatioSlider.value,
            compAttackSlider.value,
            compReleaseSlider.value,
            compMakeupSlider.value,
            compKneeSlider.value
        );
    }

    function applyLimiter() {
        if (updatingFromEngine) return;
        EngineController.setLimiterParams(
            limCeilingSlider.value,
            limReleaseSlider.value,
            limLookaheadSlider.value
        );
    }

    function applyReverb() {
        if (updatingFromEngine) return;
        EngineController.setReverbParams(
            revRoomSizeSlider.value,
            revDampingSlider.value,
            revWetSlider.value,
            revPredelaySlider.value,
            revTailCheck.checked
        );
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        // Header Strip
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Rectangle {
                width: 8
                height: 8
                radius: 4
                color: "#00E5FF"
            }

            Label {
                text: qsTr("DSP PARAMETERS // REAL-TIME AUDITION")
                font.bold: true
                font.pixelSize: 11
                font.family: "Monospace"
                color: "#00E5FF"
            }

            Item { Layout.fillWidth: true }

            Button {
                text: "✕"
                font.bold: true
                font.pixelSize: 11
                Layout.preferredWidth: 26
                Layout.preferredHeight: 22
                onClicked: dspPreviewRoot.closeRequested()
            }
        }

        // Sub-tabs for switching between DSP nodes cleanly
        TabBar {
            id: dspTabBar
            Layout.fillWidth: true
            Layout.preferredHeight: 26

            TabButton {
                text: "Filter"
                font.pixelSize: 10
                font.bold: true
            }
            TabButton {
                text: "Comp"
                font.pixelSize: 10
                font.bold: true
            }
            TabButton {
                text: "Limiter"
                font.pixelSize: 10
                font.bold: true
            }
            TabButton {
                text: "Reverb"
                font.pixelSize: 10
                font.bold: true
            }
        }

        // Stacked DSP Parameter Views
        StackLayout {
            id: dspStack
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: dspTabBar.currentIndex

            // ================= 1. FILTER (HPF / LPF) =================
            ScrollView {
                clip: true
                ColumnLayout {
                    width: dspStack.width - 12
                    spacing: 10

                    // Filter Node Master Bypass & Reset
                    RowLayout {
                        Layout.fillWidth: true
                        CheckBox {
                            text: qsTr("Filter Active")
                            checked: !EngineController.isDspNodeBypassed("Filter")
                            onToggled: EngineController.setDspNodeBypass("Filter", !checked)
                        }
                        Item { Layout.fillWidth: true }
                        Button {
                            text: qsTr("Reset Defaults")
                            font.pixelSize: 9
                            Layout.preferredHeight: 22
                            onClicked: {
                                hpfFreqSlider.value = 20.0;
                                hpfEnableCheck.checked = false;
                                hpfSlopeCombo.currentIndex = 0;
                                hpfQSlider.value = 0.70710678;
                                lpfFreqSlider.value = 20000.0;
                                lpfEnableCheck.checked = false;
                                lpfSlopeCombo.currentIndex = 0;
                                lpfQSlider.value = 0.70710678;
                                applyFilter();
                            }
                        }
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: "#222730" }

                    // High Pass Filter (HPF)
                    GroupBox {
                        Layout.fillWidth: true
                        title: qsTr("HIGH-PASS FILTER (HPF)")
                        font.pixelSize: 10
                        font.bold: true

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 6

                            RowLayout {
                                Layout.fillWidth: true
                                CheckBox {
                                    id: hpfEnableCheck
                                    text: qsTr("Enable HPF")
                                    font.pixelSize: 10
                                    onToggled: applyFilter()
                                }
                                Item { Layout.fillWidth: true }
                                Label { text: qsTr("Slope:"); font.pixelSize: 9; color: "#78909C" }
                                ComboBox {
                                    id: hpfSlopeCombo
                                    model: ["12 dB/oct", "24 dB/oct"]
                                    Layout.preferredHeight: 22
                                    Layout.preferredWidth: 95
                                    font.pixelSize: 9
                                    onActivated: applyFilter()
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Label { text: qsTr("Cutoff:"); font.pixelSize: 10; color: "#A0AAB2" }
                                Slider {
                                    id: hpfFreqSlider
                                    Layout.fillWidth: true
                                    from: 20
                                    to: 2000
                                    stepSize: 1
                                    onMoved: applyFilter()
                                }
                                Label {
                                    text: Math.round(hpfFreqSlider.value) + " Hz"
                                    font.pixelSize: 10
                                    font.family: "Monospace"
                                    color: "#00E5FF"
                                    Layout.preferredWidth: 60
                                    horizontalAlignment: Text.AlignRight
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Label { text: qsTr("HPF Q:"); font.pixelSize: 10; color: "#A0AAB2" }
                                Slider {
                                    id: hpfQSlider
                                    Layout.fillWidth: true
                                    from: 0.1
                                    to: 10.0
                                    stepSize: 0.01
                                    value: 0.70710678
                                    onMoved: applyFilter()
                                }
                                Label {
                                    text: hpfQSlider.value.toFixed(2)
                                    font.pixelSize: 10; font.family: "Monospace"; color: "#00E5FF"
                                    Layout.preferredWidth: 60; horizontalAlignment: Text.AlignRight
                                }
                            }
                        }
                    }

                    // Low Pass Filter (LPF)
                    GroupBox {
                        Layout.fillWidth: true
                        title: qsTr("LOW-PASS FILTER (LPF)")
                        font.pixelSize: 10
                        font.bold: true

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 6

                            RowLayout {
                                Layout.fillWidth: true
                                CheckBox {
                                    id: lpfEnableCheck
                                    text: qsTr("Enable LPF")
                                    font.pixelSize: 10
                                    onToggled: applyFilter()
                                }
                                Item { Layout.fillWidth: true }
                                Label { text: qsTr("Slope:"); font.pixelSize: 9; color: "#78909C" }
                                ComboBox {
                                    id: lpfSlopeCombo
                                    model: ["12 dB/oct", "24 dB/oct"]
                                    Layout.preferredHeight: 22
                                    Layout.preferredWidth: 95
                                    font.pixelSize: 9
                                    onActivated: applyFilter()
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Label { text: qsTr("Cutoff:"); font.pixelSize: 10; color: "#A0AAB2" }
                                Slider {
                                    id: lpfFreqSlider
                                    Layout.fillWidth: true
                                    from: 1000
                                    to: 20000
                                    stepSize: 10
                                    onMoved: applyFilter()
                                }
                                Label {
                                    text: (lpfFreqSlider.value >= 10000 ? (lpfFreqSlider.value / 1000.0).toFixed(1) + " kHz" : Math.round(lpfFreqSlider.value) + " Hz")
                                    font.pixelSize: 10
                                    font.family: "Monospace"
                                    color: "#00E5FF"
                                    Layout.preferredWidth: 60
                                    horizontalAlignment: Text.AlignRight
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Label { text: qsTr("LPF Q:"); font.pixelSize: 10; color: "#A0AAB2" }
                                Slider {
                                    id: lpfQSlider
                                    Layout.fillWidth: true
                                    from: 0.1
                                    to: 10.0
                                    stepSize: 0.01
                                    value: 0.70710678
                                    onMoved: applyFilter()
                                }
                                Label {
                                    text: lpfQSlider.value.toFixed(2)
                                    font.pixelSize: 10; font.family: "Monospace"; color: "#00E5FF"
                                    Layout.preferredWidth: 60; horizontalAlignment: Text.AlignRight
                                }
                            }
                        }
                    }
                }
            }

            // ================= 2. COMPRESSOR =================
            ScrollView {
                clip: true
                ColumnLayout {
                    width: dspStack.width - 12
                    spacing: 8

                    // Bypass & Realtime GR Meter
                    RowLayout {
                        Layout.fillWidth: true
                        CheckBox {
                            text: qsTr("Compressor Active")
                            checked: !EngineController.isDspNodeBypassed("Compressor")
                            onToggled: EngineController.setDspNodeBypass("Compressor", !checked)
                        }
                        Item { Layout.fillWidth: true }
                        // GR Meter Badge
                        Rectangle {
                            height: 20
                            radius: 3
                            color: "#181c22"
                            border.color: EngineController.compressorGainReductionDb > 0.1 ? "#FFB300" : "#2a313b"
                            implicitWidth: grCompLabel.implicitWidth + 10
                            Label {
                                id: grCompLabel
                                anchors.centerIn: parent
                                text: "GR -" + EngineController.compressorGainReductionDb.toFixed(1) + " dB"
                                font.pixelSize: 9
                                font.family: "Monospace"
                                font.bold: true
                                color: EngineController.compressorGainReductionDb > 0.1 ? "#FFB300" : "#78909C"
                            }
                        }
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: "#222730" }

                    // Threshold
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Threshold:"); font.pixelSize: 10; color: "#A0AAB2"; Layout.preferredWidth: 70 }
                        Slider {
                            id: compThreshSlider
                            Layout.fillWidth: true
                            from: -60.0
                            to: 0.0
                            stepSize: 0.5
                            onMoved: applyCompressor()
                        }
                        Label {
                            text: compThreshSlider.value.toFixed(1) + " dB"
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: "#00E5FF"
                            Layout.preferredWidth: 55
                            horizontalAlignment: Text.AlignRight
                        }
                    }

                    // Ratio
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Ratio:"); font.pixelSize: 10; color: "#A0AAB2"; Layout.preferredWidth: 70 }
                        Slider {
                            id: compRatioSlider
                            Layout.fillWidth: true
                            from: 1.0
                            to: 20.0
                            stepSize: 0.1
                            onMoved: applyCompressor()
                        }
                        Label {
                            text: compRatioSlider.value.toFixed(1) + ":1"
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: "#00E5FF"
                            Layout.preferredWidth: 55
                            horizontalAlignment: Text.AlignRight
                        }
                    }

                    // Attack
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Attack:"); font.pixelSize: 10; color: "#A0AAB2"; Layout.preferredWidth: 70 }
                        Slider {
                            id: compAttackSlider
                            Layout.fillWidth: true
                            from: 0.1
                            to: 200.0
                            stepSize: 0.5
                            onMoved: applyCompressor()
                        }
                        Label {
                            text: compAttackSlider.value.toFixed(1) + " ms"
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: "#00E5FF"
                            Layout.preferredWidth: 55
                            horizontalAlignment: Text.AlignRight
                        }
                    }

                    // Release
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Release:"); font.pixelSize: 10; color: "#A0AAB2"; Layout.preferredWidth: 70 }
                        Slider {
                            id: compReleaseSlider
                            Layout.fillWidth: true
                            from: 10.0
                            to: 1000.0
                            stepSize: 5.0
                            onMoved: applyCompressor()
                        }
                        Label {
                            text: Math.round(compReleaseSlider.value) + " ms"
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: "#00E5FF"
                            Layout.preferredWidth: 55
                            horizontalAlignment: Text.AlignRight
                        }
                    }

                    // Makeup Gain
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Makeup:"); font.pixelSize: 10; color: "#A0AAB2"; Layout.preferredWidth: 70 }
                        Slider {
                            id: compMakeupSlider
                            Layout.fillWidth: true
                            from: 0.0
                            to: 24.0
                            stepSize: 0.5
                            onMoved: applyCompressor()
                        }
                        Label {
                            text: "+" + compMakeupSlider.value.toFixed(1) + " dB"
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: "#00E5FF"
                            Layout.preferredWidth: 55
                            horizontalAlignment: Text.AlignRight
                        }
                    }

                    // Knee
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Knee:"); font.pixelSize: 10; color: "#A0AAB2"; Layout.preferredWidth: 70 }
                        Slider {
                            id: compKneeSlider
                            Layout.fillWidth: true
                            from: 0.0
                            to: 12.0
                            stepSize: 0.5
                            onMoved: applyCompressor()
                        }
                        Label {
                            text: compKneeSlider.value.toFixed(1) + " dB"
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: "#00E5FF"
                            Layout.preferredWidth: 55
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                }
            }

            // ================= 3. PEAK LIMITER =================
            ScrollView {
                clip: true
                ColumnLayout {
                    width: dspStack.width - 12
                    spacing: 8

                    // Notice: Sample-Peak Only per Spec Section 5.5
                    Rectangle {
                        Layout.fillWidth: true
                        height: 22
                        color: "#2a1515"
                        border.color: "#FF5252"
                        radius: 3
                        Label {
                            anchors.centerIn: parent
                            text: qsTr("SAMPLE PEAK LIMITER (NON-TRUE-PEAK, §5.5)")
                            font.pixelSize: 9
                            font.bold: true
                            color: "#FF8A80"
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        CheckBox {
                            text: qsTr("Limiter Active")
                            checked: !EngineController.isDspNodeBypassed("Limiter")
                            onToggled: EngineController.setDspNodeBypass("Limiter", !checked)
                        }
                        Item { Layout.fillWidth: true }
                        // GR & Latency
                        Label {
                            text: "GR -" + EngineController.limiterGainReductionDb.toFixed(1) + " dB | Lat: " + EngineController.limiterLatencyFrames + " smp"
                            font.pixelSize: 9
                            font.family: "Monospace"
                            color: EngineController.limiterGainReductionDb > 0.1 ? "#FF5252" : "#78909C"
                        }
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: "#222730" }

                    // Ceiling
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Ceiling:"); font.pixelSize: 10; color: "#A0AAB2"; Layout.preferredWidth: 70 }
                        Slider {
                            id: limCeilingSlider
                            Layout.fillWidth: true
                            from: -12.0
                            to: 0.0
                            stepSize: 0.1
                            onMoved: applyLimiter()
                        }
                        Label {
                            text: limCeilingSlider.value.toFixed(1) + " dB"
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: "#00E5FF"
                            Layout.preferredWidth: 55
                            horizontalAlignment: Text.AlignRight
                        }
                    }

                    // Release
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Release:"); font.pixelSize: 10; color: "#A0AAB2"; Layout.preferredWidth: 70 }
                        Slider {
                            id: limReleaseSlider
                            Layout.fillWidth: true
                            from: 5.0
                            to: 500.0
                            stepSize: 1.0
                            onMoved: applyLimiter()
                        }
                        Label {
                            text: Math.round(limReleaseSlider.value) + " ms"
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: "#00E5FF"
                            Layout.preferredWidth: 55
                            horizontalAlignment: Text.AlignRight
                        }
                    }

                    // Lookahead
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Lookahead:"); font.pixelSize: 10; color: "#A0AAB2"; Layout.preferredWidth: 70 }
                        Slider {
                            id: limLookaheadSlider
                            Layout.fillWidth: true
                            from: 0.0
                            to: 10.0
                            stepSize: 0.1
                            onMoved: applyLimiter()
                        }
                        Label {
                            text: limLookaheadSlider.value.toFixed(1) + " ms"
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: "#00E5FF"
                            Layout.preferredWidth: 55
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                }
            }

            // ================= 4. ALGORITHMIC REVERB =================
            ScrollView {
                clip: true
                ColumnLayout {
                    width: dspStack.width - 12
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        CheckBox {
                            text: qsTr("Reverb Active")
                            checked: !EngineController.isDspNodeBypassed("Reverb")
                            onToggled: EngineController.setDspNodeBypass("Reverb", !checked)
                        }
                        Item { Layout.fillWidth: true }
                        CheckBox {
                            id: revTailCheck
                            text: qsTr("Tail-Preserving")
                            font.pixelSize: 9
                            onToggled: applyReverb()
                        }
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: "#222730" }

                    // Room Size
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Room Size:"); font.pixelSize: 10; color: "#A0AAB2"; Layout.preferredWidth: 70 }
                        Slider {
                            id: revRoomSizeSlider
                            Layout.fillWidth: true
                            from: 0.0
                            to: 1.0
                            stepSize: 0.01
                            onMoved: applyReverb()
                        }
                        Label {
                            text: (revRoomSizeSlider.value * 100.0).toFixed(0) + " %"
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: "#00E5FF"
                            Layout.preferredWidth: 55
                            horizontalAlignment: Text.AlignRight
                        }
                    }

                    // Damping
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Damping:"); font.pixelSize: 10; color: "#A0AAB2"; Layout.preferredWidth: 70 }
                        Slider {
                            id: revDampingSlider
                            Layout.fillWidth: true
                            from: 0.0
                            to: 1.0
                            stepSize: 0.01
                            onMoved: applyReverb()
                        }
                        Label {
                            text: (revDampingSlider.value * 100.0).toFixed(0) + " %"
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: "#00E5FF"
                            Layout.preferredWidth: 55
                            horizontalAlignment: Text.AlignRight
                        }
                    }

                    // Wet Level
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Wet Level:"); font.pixelSize: 10; color: "#A0AAB2"; Layout.preferredWidth: 70 }
                        Slider {
                            id: revWetSlider
                            Layout.fillWidth: true
                            from: 0.0
                            to: 1.0
                            stepSize: 0.01
                            onMoved: applyReverb()
                        }
                        Label {
                            text: (revWetSlider.value * 100.0).toFixed(0) + " %"
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: "#00E5FF"
                            Layout.preferredWidth: 55
                            horizontalAlignment: Text.AlignRight
                        }
                    }

                    // Pre-delay
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Pre-delay:"); font.pixelSize: 10; color: "#A0AAB2"; Layout.preferredWidth: 70 }
                        Slider {
                            id: revPredelaySlider
                            Layout.fillWidth: true
                            from: 0.0
                            to: 200.0
                            stepSize: 1.0
                            onMoved: applyReverb()
                        }
                        Label {
                            text: Math.round(revPredelaySlider.value) + " ms"
                            font.pixelSize: 10
                            font.family: "Monospace"
                            color: "#00E5FF"
                            Layout.preferredWidth: 55
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                }
            }
        }
    }
}

