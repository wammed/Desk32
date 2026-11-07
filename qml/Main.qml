import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Desk32.Core

ApplicationWindow {
    id: root
    visible: true
    width: 2560
    height: 1440
    minimumWidth: 1280
    minimumHeight: 720
    title: qsTr("Desk32 - Audiophile Studio Console")

    color: "#121417"

    header: ToolBar {
        background: Rectangle {
            color: "#1a1d21"
            border.color: "#2a2e33"
            border.width: 1
        }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 16

            Label {
                text: "DESK32 // STUDIO CONSOLE MONITOR"
                font.family: "Monospace"
                font.bold: true
                font.pixelSize: 14
                color: "#00E5FF"
            }

            Item { Layout.fillWidth: true }

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

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        // Top 3 Columns: Left Wing (Decks A-D), Center (Main Monitor), Right Wing (Master/DSP)
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 8

            // Left Wing: Reference Decks A - D (approx 480px)
            Rectangle {
                Layout.preferredWidth: 480
                Layout.fillHeight: true
                color: "#181a1e"
                border.color: "#2c3138"
                border.width: 1
                radius: 4

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12

                    Label {
                        text: qsTr("REFERENCE DECKS (A-D)")
                        font.bold: true
                        font.pixelSize: 13
                        color: "#E0E6ED"
                    }

                    Repeater {
                        model: ["DECK A", "DECK B", "DECK C", "DECK D"]
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            color: index === 0 ? "#1e2228" : "#141619"
                            border.color: index === 0 ? "#00E5FF" : "#262b32"
                            border.width: 1
                            radius: 3

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 8

                                RowLayout {
                                    Label {
                                        text: modelData
                                        font.bold: true
                                        color: index === 0 ? "#00E5FF" : "#8A94A0"
                                    }
                                    Item { Layout.fillWidth: true }
                                    Label {
                                        text: index === 0 ? qsTr("[PLAYING]") : qsTr("[STANDBY]")
                                        font.pixelSize: 11
                                        color: index === 0 ? "#00E676" : "#606870"
                                    }
                                }

                                Label {
                                    text: qsTr("No audio loaded")
                                    font.pixelSize: 11
                                    color: "#606870"
                                }
                            }
                        }
                    }
                }
            }

            // Center: Main Monitor (Spectrum / Track Waveform / Diagnostics)
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#16181c"
                border.color: "#2c3138"
                border.width: 1
                radius: 4

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 16

                    RowLayout {
                        Label {
                            text: qsTr("MAIN MONITOR & DIAGNOSTICS")
                            font.bold: true
                            font.pixelSize: 14
                            color: "#E0E6ED"
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            text: "PIPEWIRE // 192.0 kHz // 64-bit float"
                            font.family: "Monospace"
                            font.pixelSize: 12
                            color: "#00E5FF"
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: "#0e1012"
                        border.color: "#22262c"

                        CenterLabel {
                            anchors.centerIn: parent
                            text: qsTr("Phase 0 Execution Foundation PoC Active\nTarget Resolution: WQHD 2560x1440")
                            horizontalAlignment: Text.AlignHCenter
                            color: "#707c8a"
                            font.pixelSize: 16
                        }
                    }
                }
            }

            // Right Wing: Master / DSP & Output Path
            Rectangle {
                Layout.preferredWidth: 420
                Layout.fillHeight: true
                color: "#181a1e"
                border.color: "#2c3138"
                border.width: 1
                radius: 4

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12

                    Label {
                        text: qsTr("MASTER / OUTPUT PATH")
                        font.bold: true
                        font.pixelSize: 13
                        color: "#E0E6ED"
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 110
                        color: "#131518"
                        border.color: "#262b32"
                        radius: 3

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 8
                            Label {
                                text: qsTr("3-PATH OUTPUT ROUTING")
                                font.pixelSize: 11
                                font.bold: true
                                color: "#A0AAB2"
                            }
                            RadioButton {
                                text: qsTr("Listening Path (Correction ON)")
                                checked: true
                            }
                            RadioButton {
                                text: qsTr("Render Monitor (Pure Master)")
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }
        }

        // Bottom: Full-Width EQ Console (Height: approx 450px)
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 450
            color: "#15171a"
            border.color: "#2c3138"
            border.width: 1
            radius: 4

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12

                RowLayout {
                    Label {
                        text: qsTr("EQ CONSOLE // 32-BAND GEQ & 24-BAND PEQ")
                        font.bold: true
                        font.pixelSize: 13
                        color: "#E0E6ED"
                    }

                    Item { Layout.fillWidth: true }

                    Button {
                        text: qsTr("GEQ (32 Faders)")
                        highlighted: true
                    }

                    Button {
                        text: qsTr("PEQ (12+12 Band)")
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: "#0f1113"
                    border.color: "#20242a"

                    Label {
                        anchors.centerIn: parent
                        text: qsTr("32-Band Graphic Equalizer Standard View (Initial Display Mode)")
                        color: "#606870"
                        font.pixelSize: 14
                    }
                }
            }
        }
    }

    component CenterLabel: Label {
        font.family: "Monospace"
    }
}

