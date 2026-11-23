import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Desk32.Core
import "../components"
import ".."

Item {
    id: settingsViewRoot

    property int activeCategory: 0
    property double selectedUiScale: 1.0

    signal uiScaleChangedRequested(double newScale)

    RowLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12

        // Left Category Sidebar (5 Categories per Brief §7)
        BrushedMetalPanel {
            Layout.preferredWidth: 320
            Layout.fillHeight: true
            baseTint: "#161b22"
            borderColor: "#313945"
            showScrews: true

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 8

                Label {
                    text: qsTr("SETTINGS CATEGORY")
                    font.family: "Monospace"
                    font.pixelSize: 11
                    font.bold: true
                    color: "#7E8C9B"
                }

                Repeater {
                    model: [
                        qsTr("1. Audio Output & Backend"),
                        qsTr("2. DSP & Metering"),
                        qsTr("3. Workspace & Paths"),
                        qsTr("4. Interface & Language"),
                        qsTr("5. Graphics & Diagnostics")
                    ]

                    ConsoleButton {
                        Layout.fillWidth: true
                        implicitHeight: 40
                        text: modelData
                        fontSize: 12
                        highlighted: settingsViewRoot.activeCategory === index
                        onClicked: settingsViewRoot.activeCategory = index
                    }
                }

                Item { Layout.fillHeight: true }
            }
        }

        // Right Wide Scrollable Category Panel
        BrushedMetalPanel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            baseTint: "#141920"
            borderColor: "#313945"

            StackLayout {
                anchors.fill: parent
                anchors.margins: 14
                currentIndex: settingsViewRoot.activeCategory

                // 0. Audio Output & Backend (embeds existing verified OutputDevicePanel)
                OutputDevicePanel {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                }

                // 1. DSP & Metering
                ScrollView {
                    clip: true
                    contentWidth: availableWidth
                    ColumnLayout {
                        width: parent.width
                        spacing: 12
                        BrushedMetalPanel {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 220
                            baseTint: "#0e1217"
                            borderColor: "#252e3a"
                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 18
                                spacing: 10
                                Label {
                                    text: qsTr("DSP & METERING DEFAULTS")
                                    font.family: "Monospace"
                                    font.pixelSize: 13
                                    font.bold: true
                                    color: "#00E5FF"
                                }
                                Label {
                                    text: qsTr("Default Listening Preset: %1\nReverb Max Tail: %2 s\nReverb Max Processing Budget: %3 ms\nExternal Insert Slot: %4")
                                          .arg(SettingsManager.listeningPreset)
                                          .arg(SettingsManager.reverbMaxTailSeconds.toFixed(1))
                                          .arg(SettingsManager.reverbMaxProcessingMs.toFixed(1))
                                          .arg(SettingsManager.externalInsertPosition)
                                    font.family: "Monospace"
                                    font.pixelSize: 12
                                    color: "#CBD6E2"
                                }
                            }
                        }
                    }
                }

                // 2. Workspace & Paths
                ScrollView {
                    clip: true
                    contentWidth: availableWidth
                    ColumnLayout {
                        width: parent.width
                        spacing: 12
                        BrushedMetalPanel {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 240
                            baseTint: "#0e1217"
                            borderColor: "#252e3a"
                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 18
                                spacing: 10
                                Label {
                                    text: qsTr("WORKSPACE & XDG PATHS")
                                    font.family: "Monospace"
                                    font.pixelSize: 13
                                    font.bold: true
                                    color: "#00E5FF"
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WrapAnywhere
                                    text: qsTr("Config File: %1\nOpen Workspace: %2")
                                          .arg(SettingsManager.configFilePath)
                                          .arg(WorkspaceManager.hasOpenWorkspace ? WorkspaceManager.openWorkspacePath : qsTr("None (Read-Only)"))
                                    font.family: "Monospace"
                                    font.pixelSize: 12
                                    color: "#CBD6E2"
                                }
                            }
                        }
                    }
                }

                // 3. Interface & Language
                ScrollView {
                    clip: true
                    contentWidth: availableWidth
                    ColumnLayout {
                        width: parent.width
                        spacing: 12
                        BrushedMetalPanel {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 300
                            baseTint: "#0e1217"
                            borderColor: "#252e3a"
                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 18
                                spacing: 12

                                Label {
                                    text: qsTr("INTERFACE & LANGUAGE")
                                    font.family: "Monospace"
                                    font.pixelSize: 13
                                    font.bold: true
                                    color: "#00E5FF"
                                }

                                RowLayout {
                                    spacing: 10
                                    Label { text: qsTr("Language / 言語:"); color: "#CBD6E2"; font.pixelSize: 12 }
                                    ConsoleButton {
                                        text: "English"
                                        highlighted: LanguageManager.currentLanguage === "en"
                                        onClicked: LanguageManager.setLanguage("en")
                                    }
                                    ConsoleButton {
                                        text: "日本語"
                                        highlighted: LanguageManager.currentLanguage === "ja"
                                        onClicked: LanguageManager.setLanguage("ja")
                                    }
                                }

                                RowLayout {
                                    spacing: 10
                                    Label { text: qsTr("UI Scaling:"); color: "#CBD6E2"; font.pixelSize: 12 }
                                    Repeater {
                                        model: [0.8, 1.0, 1.25, 1.5]
                                        ConsoleButton {
                                            text: Math.round(modelData * 100) + "%" + (modelData === 1.0 ? " (WQHD)" : "")
                                            highlighted: Math.abs(settingsViewRoot.selectedUiScale - modelData) < 0.01
                                            onClicked: {
                                                settingsViewRoot.selectedUiScale = modelData
                                                settingsViewRoot.uiScaleChangedRequested(modelData)
                                            }
                                        }
                                    }
                                }

                                CheckBox {
                                    text: qsTr("Launch Fullscreen on Startup (WQHD Target)")
                                    checked: SettingsManager.fullscreenOnStartup
                                    onToggled: SettingsManager.fullscreenOnStartup = checked
                                }
                            }
                        }
                    }
                }

                // 4. Graphics & Diagnostics
                ScrollView {
                    clip: true
                    contentWidth: availableWidth
                    ColumnLayout {
                        width: parent.width
                        spacing: 12
                        BrushedMetalPanel {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 240
                            baseTint: "#0e1217"
                            borderColor: "#252e3a"
                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 18
                                spacing: 10
                                Label {
                                    text: qsTr("GRAPHICS (RHI) & DIAGNOSTICS")
                                    font.family: "Monospace"
                                    font.pixelSize: 13
                                    font.bold: true
                                    color: "#00E5FF"
                                }
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    text: qsTr("Default RHI Backend: Vulkan (automatic OpenGL fallback if unavailable)\nDirect Mode & Bit-Perfect L1/L2/L3 diagnostics are available in Right Wing Master Control.")
                                    font.pixelSize: 12
                                    color: "#CBD6E2"
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
