import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Desk32.Core

// Spec v3.8.3 §11.1 / P6: Layered DSP Preset Modal (Qt 6.4 compatible)
// Consolidates Global Presets, Per-DSP Presets (all 7 DSPs), and separated Listening Correction list.
ScrollableModalPopup {
    id: presetModal

    titleText: qsTr("DSP PRESET MANAGER")
    preferredWidth: 820
    preferredHeight: 680
    accentColor: "#00E5FF"

    // 0 = Global, 1 = Per-DSP, 2 = Listening Correction (separate axis per §11.1)
    property int activeLayer: 0
    property string selectedDspTarget: "peq"
    property var globalPresets: []
    property var perDspPresets: []
    property var listeningPresets: []

    property string pendingOverwriteName: ""
    property string pendingOverwriteKind: "" // "global" | "per-dsp" | "listening"
    property string renamingPresetId: ""
    property string renamingPresetName: ""
    property string statusMessage: ""
    property bool statusIsError: false

    readonly property var dspTargets: [
        { key: "peq",        label: "PEQ" },
        { key: "geq",        label: "GEQ" },
        { key: "hpf",        label: "HPF" },
        { key: "lpf",        label: "LPF" },
        { key: "compressor", label: "COMP" },
        { key: "limiter",    label: "LIM" },
        { key: "reverb",     label: "REV" }
    ]

    function refreshLists() {
        globalPresets = EngineController.listGlobalDspPresets()
        perDspPresets = EngineController.listPerDspPresets(selectedDspTarget)
        listeningPresets = EngineController.availableListeningPresets()
    }

    function updateStatusFromReport(fallbackOkText) {
        var rep = EngineController.lastDspPresetReport()
        if (rep && rep.ok === false) {
            statusIsError = true
            statusMessage = rep.errorMessage ? rep.errorMessage : qsTr("Preset operation failed.")
            return
        }
        if (rep && rep.hasWarnings) {
            statusIsError = false
            var parts = []
            if (rep.clampedFields && rep.clampedFields.length > 0)
                parts.push(qsTr("Clamped: %1").arg(rep.clampedFields.length))
            if (rep.peqOverflowLeft > 0 || rep.peqOverflowRight > 0)
                parts.push(qsTr("PEQ overflow: L=%1 R=%2").arg(rep.peqOverflowLeft).arg(rep.peqOverflowRight))
            if (rep.unknownEnumFields && rep.unknownEnumFields.length > 0)
                parts.push(qsTr("Unknown enums defaulted: %1").arg(rep.unknownEnumFields.length))
            if (rep.unknownKeys && rep.unknownKeys.length > 0)
                parts.push(qsTr("Unknown keys preserved: %1").arg(rep.unknownKeys.length))
            statusMessage = fallbackOkText + (parts.length > 0 ? ("  [" + parts.join(" | ") + "]") : "")
            return
        }
        statusIsError = false
        statusMessage = fallbackOkText
    }

    function requestSave(kindStr, nameStr) {
        var trimmed = nameStr.trim()
        if (trimmed.length === 0) {
            statusIsError = true
            statusMessage = qsTr("Enter a preset name before saving.")
            return
        }
        if (kindStr === "listening") {
            var data = EngineController.captureCurrentStateAsPreset("listening_correction")
            var okLc = EngineController.savePreset("listening_correction", trimmed, data)
            statusIsError = !okLc
            statusMessage = okLc ? qsTr("Saved Listening Correction preset '%1'.").arg(trimmed)
                                 : qsTr("Could not save Listening Correction preset '%1' (read-only factory name?).").arg(trimmed)
            if (okLc) saveNameInput.text = ""
            refreshLists()
            return
        }

        var tgt = (kindStr === "global") ? "all" : selectedDspTarget
        if (EngineController.isBuiltInDspPreset(kindStr, tgt, trimmed)) {
            statusIsError = true
            statusMessage = qsTr("Cannot overwrite read-only built-in preset '%1'.").arg(trimmed)
            return
        }
        if (EngineController.hasUserDspPresetNamed(kindStr, tgt, trimmed)) {
            pendingOverwriteKind = kindStr
            pendingOverwriteName = trimmed
            return
        }
        executeSave(kindStr, trimmed, false)
    }

    function executeSave(kindStr, trimmed, overwrite) {
        var ok = (kindStr === "global")
                 ? EngineController.saveGlobalDspPreset(trimmed, overwrite)
                 : EngineController.savePerDspPreset(selectedDspTarget, trimmed, overwrite)
        pendingOverwriteKind = ""
        pendingOverwriteName = ""
        if (ok) {
            saveNameInput.text = ""
            updateStatusFromReport(qsTr("Saved preset '%1'.").arg(trimmed))
        } else {
            updateStatusFromReport(qsTr("Save failed."))
        }
        refreshLists()
    }

    onOpened: {
        pendingOverwriteKind = ""
        pendingOverwriteName = ""
        renamingPresetId = ""
        renamingPresetName = ""
        statusMessage = ""
        statusIsError = false
        refreshLists()
    }

    Connections {
        target: EngineController
        function onDspPresetStateChanged() { presetModal.refreshLists() }
        function onPresetManagementChanged() { presetModal.refreshLists() }
    }

    // -------------------------------------------------------------------------
    // 1. Active Global Summary + 7-DSP Preset-Applied Indicator Breakdown (§11.1)
    // -------------------------------------------------------------------------
    Rectangle {
        Layout.fillWidth: true
        implicitHeight: activeSummaryCol.implicitHeight + 18
        radius: 4
        color: "#090d12"
        border.color: EngineController.globalPresetModified ? "#A87424" : "#223444"
        border.width: 1

        ColumnLayout {
            id: activeSummaryCol
            anchors.fill: parent
            anchors.margins: 9
            spacing: 7

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    text: qsTr("ACTIVE GLOBAL:")
                    font.family: "Monospace"
                    font.pixelSize: 10
                    font.bold: true
                    color: "#7E93A6"
                }

                Label {
                    Layout.fillWidth: true
                    text: EngineController.activeGlobalPresetName
                    font.family: "Monospace"
                    font.pixelSize: 12
                    font.bold: true
                    color: "#E8F4FF"
                    elide: Text.ElideRight
                }

                Rectangle {
                    visible: EngineController.globalPresetModified
                    implicitWidth: modLbl.implicitWidth + 12
                    implicitHeight: 20
                    radius: 3
                    color: "#3A280E"
                    border.color: "#F5B841"
                    Label {
                        id: modLbl
                        anchors.centerIn: parent
                        text: qsTr("MODIFIED")
                        font.family: "Monospace"
                        font.pixelSize: 9
                        font.bold: true
                        color: "#FFD470"
                    }
                }
            }

            // 7-DSP Preset Applied Breakdown (independent of DSP Node ON/OFF)
            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                Label {
                    text: qsTr("APPLIED:")
                    font.family: "Monospace"
                    font.pixelSize: 9
                    color: "#6C8094"
                }

                Repeater {
                    model: presetModal.dspTargets
                    Rectangle {
                        readonly property bool isApplied: !!(EngineController.dspPresetIndicators && EngineController.dspPresetIndicators[modelData.key])
                        Layout.fillWidth: true
                        implicitHeight: 22
                        radius: 3
                        color: isApplied ? "#102834" : "#11161D"
                        border.color: isApplied ? "#2AD6F2" : "#26303C"

                        RowLayout {
                            anchors.centerIn: parent
                            spacing: 5
                            Rectangle {
                                width: 6; height: 6; radius: 3
                                color: isApplied ? "#00E5FF" : "#394452"
                            }
                            Label {
                                text: modelData.label
                                font.family: "Monospace"
                                font.pixelSize: 9
                                font.bold: true
                                color: isApplied ? "#D8F6FF" : "#6F7F90"
                            }
                        }
                    }
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // 2. Layer Switcher: Global | Per-DSP | Listening Correction (separate)
    // -------------------------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        ConsoleButton {
            Layout.fillWidth: true
            implicitHeight: 32
            text: qsTr("GLOBAL PRESETS")
            fontSize: 11
            highlighted: presetModal.activeLayer === 0
            accentColor: "#00E5FF"
            onClicked: {
                presetModal.activeLayer = 0
                presetModal.pendingOverwriteName = ""
                presetModal.renamingPresetId = ""
                presetModal.refreshLists()
            }
        }

        ConsoleButton {
            Layout.fillWidth: true
            implicitHeight: 32
            text: qsTr("PER-DSP PRESETS")
            fontSize: 11
            highlighted: presetModal.activeLayer === 1
            accentColor: "#48D8FF"
            onClicked: {
                presetModal.activeLayer = 1
                presetModal.pendingOverwriteName = ""
                presetModal.renamingPresetId = ""
                presetModal.refreshLists()
            }
        }

        ConsoleButton {
            Layout.fillWidth: true
            implicitHeight: 32
            text: qsTr("LISTENING CORRECTION")
            fontSize: 11
            highlighted: presetModal.activeLayer === 2
            accentColor: "#F5B841"
            onClicked: {
                presetModal.activeLayer = 2
                presetModal.pendingOverwriteName = ""
                presetModal.renamingPresetId = ""
                presetModal.refreshLists()
            }
        }
    }

    // Per-DSP Sub-Layer Target Selector (7 DSPs)
    RowLayout {
        Layout.fillWidth: true
        visible: presetModal.activeLayer === 1
        spacing: 6

        Repeater {
            model: presetModal.dspTargets
            ConsoleButton {
                Layout.fillWidth: true
                implicitHeight: 28
                text: modelData.label
                fontSize: 10
                showLed: true
                ledColor: !!(EngineController.dspPresetIndicators && EngineController.dspPresetIndicators[modelData.key]) ? "#00E5FF" : "#3A4554"
                highlighted: presetModal.selectedDspTarget === modelData.key
                accentColor: "#48D8FF"
                onClicked: {
                    presetModal.selectedDspTarget = modelData.key
                    presetModal.pendingOverwriteName = ""
                    presetModal.renamingPresetId = ""
                    presetModal.refreshLists()
                }
            }
        }
    }

    // Scope notice banner
    Label {
        Layout.fillWidth: true
        font.family: "Monospace"
        font.pixelSize: 9
        color: presetModal.activeLayer === 2 ? "#E8BA6B" : "#7B93A8"
        wrapMode: Text.WordWrap
        text: {
            if (presetModal.activeLayer === 0)
                return qsTr("Global presets apply all 7 Mastering Chain DSPs and node ON/OFF states. Trim, Master, External Insert, GEQ Bypass, and Listening Correction are preserved.")
            if (presetModal.activeLayer === 1 && presetModal.selectedDspTarget === "peq")
                return qsTr("Per-DSP PEQ presets apply strictly to Mastering Chain PEQ (never shared with Listening Correction). Node ON/OFF is preserved.")
            if (presetModal.activeLayer === 1)
                return qsTr("Per-DSP presets replace only the selected DSP parameters. DSP node ON/OFF and all other DSPs are preserved.")
            return qsTr("Listening Correction profiles apply strictly to Listening Path PEQ and are completely isolated from Mastering Chain presets and Render output.")
        }
    }

    // -------------------------------------------------------------------------
    // 3. Save Current State Bar + Overwrite Confirmation
    // -------------------------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        TextField {
            id: saveNameInput
            Layout.fillWidth: true
            implicitHeight: 32
            placeholderText: presetModal.activeLayer === 0
                             ? qsTr("Save current chain as Global preset...")
                             : (presetModal.activeLayer === 1
                                ? qsTr("Save current %1 as Per-DSP preset...").arg(presetModal.selectedDspTarget.toUpperCase())
                                : qsTr("Save current Listening Correction profile..."))
            color: "#E8F2FA"
            placeholderTextColor: "#5E7184"
            font.family: "Monospace"
            font.pixelSize: 11
            background: Rectangle {
                radius: 4
                color: "#090D12"
                border.color: saveNameInput.activeFocus ? "#00E5FF" : "#2A3543"
            }
            onAccepted: {
                var k = presetModal.activeLayer === 0 ? "global" : (presetModal.activeLayer === 1 ? "per-dsp" : "listening")
                presetModal.requestSave(k, text)
            }
        }

        ConsoleButton {
            implicitWidth: 118
            implicitHeight: 32
            text: qsTr("SAVE PRESET")
            fontSize: 10
            accentColor: "#00E676"
            highlighted: saveNameInput.text.trim().length > 0
            onClicked: {
                var k = presetModal.activeLayer === 0 ? "global" : (presetModal.activeLayer === 1 ? "per-dsp" : "listening")
                presetModal.requestSave(k, saveNameInput.text)
            }
        }
    }

    // Inline Overwrite Confirmation (never silently overwrites §11.1)
    Rectangle {
        Layout.fillWidth: true
        visible: presetModal.pendingOverwriteName.length > 0
        implicitHeight: owRow.implicitHeight + 14
        radius: 4
        color: "#2C1E0B"
        border.color: "#F5B841"

        RowLayout {
            id: owRow
            anchors.fill: parent
            anchors.margins: 7
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: qsTr("Preset '%1' already exists. Overwrite it?").arg(presetModal.pendingOverwriteName)
                font.family: "Monospace"
                font.pixelSize: 10
                color: "#FFE2A0"
                wrapMode: Text.WordWrap
            }
            ConsoleButton {
                implicitWidth: 94
                implicitHeight: 26
                text: qsTr("OVERWRITE")
                fontSize: 9
                accentColor: "#F5B841"
                highlighted: true
                onClicked: presetModal.executeSave(presetModal.pendingOverwriteKind, presetModal.pendingOverwriteName, true)
            }
            ConsoleButton {
                implicitWidth: 74
                implicitHeight: 26
                text: qsTr("CANCEL")
                fontSize: 9
                onClicked: {
                    presetModal.pendingOverwriteKind = ""
                    presetModal.pendingOverwriteName = ""
                }
            }
        }
    }

    // Inline Rename Bar for User Presets (preserves immutable `id` §11.1)
    Rectangle {
        Layout.fillWidth: true
        visible: presetModal.renamingPresetId.length > 0
        implicitHeight: renRow.implicitHeight + 14
        radius: 4
        color: "#0F1E2A"
        border.color: "#00E5FF"

        RowLayout {
            id: renRow
            anchors.fill: parent
            anchors.margins: 7
            spacing: 8

            Label {
                text: qsTr("RENAME:")
                font.family: "Monospace"
                font.pixelSize: 10
                font.bold: true
                color: "#7BDFF2"
            }

            TextField {
                id: renameField
                Layout.fillWidth: true
                implicitHeight: 26
                text: presetModal.renamingPresetName
                color: "#EAF5FF"
                font.family: "Monospace"
                font.pixelSize: 10
                background: Rectangle {
                    radius: 3
                    color: "#080C10"
                    border.color: "#2F5068"
                }
            }

            ConsoleButton {
                implicitWidth: 78
                implicitHeight: 26
                text: qsTr("APPLY")
                fontSize: 9
                accentColor: "#00E5FF"
                highlighted: true
                onClicked: {
                    var k = presetModal.activeLayer === 0 ? "global" : "per-dsp"
                    var ok = EngineController.renameUserDspPreset(k, presetModal.selectedDspTarget,
                                                                  presetModal.renamingPresetId, renameField.text, false)
                    presetModal.renamingPresetId = ""
                    presetModal.renamingPresetName = ""
                    presetModal.updateStatusFromReport(ok ? qsTr("Renamed preset.") : qsTr("Rename failed."))
                    presetModal.refreshLists()
                }
            }

            ConsoleButton {
                implicitWidth: 70
                implicitHeight: 26
                text: qsTr("CANCEL")
                fontSize: 9
                onClicked: {
                    presetModal.renamingPresetId = ""
                    presetModal.renamingPresetName = ""
                }
            }
        }
    }

    // Status / Validation Report Banner
    Rectangle {
        Layout.fillWidth: true
        visible: presetModal.statusMessage.length > 0
        implicitHeight: statusLbl.implicitHeight + 12
        radius: 4
        color: presetModal.statusIsError ? "#2E1216" : "#0D232A"
        border.color: presetModal.statusIsError ? "#FF5252" : "#2D7A8E"

        Label {
            id: statusLbl
            anchors.fill: parent
            anchors.margins: 6
            text: presetModal.statusMessage
            font.family: "Monospace"
            font.pixelSize: 10
            color: presetModal.statusIsError ? "#FFB3B8" : "#AEE8F5"
            wrapMode: Text.WordWrap
        }
    }

    // -------------------------------------------------------------------------
    // 4. Preset Lists (Global / Per-DSP / Listening Correction)
    // -------------------------------------------------------------------------
    ColumnLayout {
        Layout.fillWidth: true
        visible: presetModal.activeLayer === 0 || presetModal.activeLayer === 1
        spacing: 5

        Repeater {
            model: presetModal.activeLayer === 0 ? presetModal.globalPresets : presetModal.perDspPresets

            Rectangle {
                readonly property bool isCurrentGlobal: presetModal.activeLayer === 0 && modelData.id === EngineController.activeGlobalPresetId
                Layout.fillWidth: true
                implicitHeight: 38
                radius: 4
                color: isCurrentGlobal ? "#132938" : "#11161D"
                border.color: isCurrentGlobal ? "#00E5FF" : "#242E3A"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 8
                    spacing: 8

                    Rectangle {
                        implicitWidth: badgeText.implicitWidth + 10
                        implicitHeight: 18
                        radius: 2
                        color: modelData.isFactory ? "#1B2836" : "#1D2B1F"
                        border.color: modelData.isFactory ? "#3B5B78" : "#3E7A4B"
                        Label {
                            id: badgeText
                            anchors.centerIn: parent
                            text: modelData.isFactory ? qsTr("BUILT-IN") : qsTr("USER")
                            font.family: "Monospace"
                            font.pixelSize: 8
                            font.bold: true
                            color: modelData.isFactory ? "#8EC8EB" : "#8EE8A2"
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: modelData.name
                        font.family: "Monospace"
                        font.pixelSize: 11
                        font.bold: isCurrentGlobal
                        color: isCurrentGlobal ? "#FFFFFF" : "#D5E1EC"
                        elide: Text.ElideRight
                    }

                    ConsoleButton {
                        implicitWidth: 74
                        implicitHeight: 26
                        text: qsTr("LOAD")
                        fontSize: 9
                        accentColor: "#00E5FF"
                        highlighted: isCurrentGlobal
                        onClicked: {
                            var ok = (presetModal.activeLayer === 0)
                                     ? EngineController.applyGlobalDspPreset(modelData.id)
                                     : EngineController.applyPerDspPreset(presetModal.selectedDspTarget, modelData.id)
                            presetModal.updateStatusFromReport(
                                ok ? qsTr("Applied '%1'.").arg(modelData.name)
                                   : qsTr("Failed to apply '%1'.").arg(modelData.name))
                            presetModal.refreshLists()
                        }
                    }

                    ConsoleButton {
                        visible: !modelData.isFactory
                        implicitWidth: 68
                        implicitHeight: 26
                        text: qsTr("RENAME")
                        fontSize: 9
                        onClicked: {
                            presetModal.renamingPresetId = modelData.id
                            presetModal.renamingPresetName = modelData.name
                            renameField.text = modelData.name
                            renameField.forceActiveFocus()
                        }
                    }

                    ConsoleButton {
                        visible: !modelData.isFactory
                        implicitWidth: 64
                        implicitHeight: 26
                        text: qsTr("DEL")
                        fontSize: 9
                        accentColor: "#FF5252"
                        onClicked: {
                            var k = presetModal.activeLayer === 0 ? "global" : "per-dsp"
                            var ok = EngineController.deleteUserDspPreset(k, presetModal.selectedDspTarget, modelData.id)
                            presetModal.updateStatusFromReport(
                                ok ? qsTr("Deleted '%1'.").arg(modelData.name)
                                   : qsTr("Delete failed."))
                            presetModal.refreshLists()
                        }
                    }
                }
            }
        }
    }

    // Separate Listening Correction List (Layer 2)
    ColumnLayout {
        Layout.fillWidth: true
        visible: presetModal.activeLayer === 2
        spacing: 5

        Repeater {
            model: presetModal.listeningPresets

            Rectangle {
                readonly property bool isActiveLc: modelData === EngineController.listeningPresetName
                Layout.fillWidth: true
                implicitHeight: 36
                radius: 4
                color: isActiveLc ? "#2A2112" : "#11161D"
                border.color: isActiveLc ? "#F5B841" : "#242E3A"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 8
                    spacing: 8

                    Label {
                        Layout.fillWidth: true
                        text: modelData
                        font.family: "Monospace"
                        font.pixelSize: 11
                        font.bold: isActiveLc
                        color: isActiveLc ? "#FFE39A" : "#D5E1EC"
                        elide: Text.ElideRight
                    }

                    ConsoleButton {
                        implicitWidth: 74
                        implicitHeight: 26
                        text: qsTr("APPLY")
                        fontSize: 9
                        accentColor: "#F5B841"
                        highlighted: isActiveLc
                        onClicked: {
                            EngineController.setListeningPreset(modelData)
                            presetModal.statusIsError = false
                            presetModal.statusMessage = qsTr("Applied Listening Correction '%1'.").arg(modelData)
                        }
                    }
                }
            }
        }
    }
}
