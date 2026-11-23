import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Desk32.Core
import "components"

ApplicationWindow {
    id: protoWin
    visible: true
    visibility: (SettingsManager.fullscreenOnStartup && Qt.application.arguments.indexOf("--screenshot") < 0)
                ? Window.FullScreen : Window.Windowed
    width: 2560
    height: 1440
    title: qsTr("Desk32 — Gate G2 Material & Control Prototype (WQHD)")
    color: "#090b0e"

    Shortcut {
        sequence: "F11"
        onActivated: protoWin.visibility = (protoWin.visibility === Window.FullScreen)
                                            ? Window.Windowed : Window.FullScreen
    }

    Shortcut {
        sequence: "Escape"
        onActivated: Qt.quit()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 28
        spacing: 20

        // Prototype Header
        BrushedMetalPanel {
            Layout.fillWidth: true
            Layout.preferredHeight: 68
            baseTint: "#0e1116"
            borderColor: "#232a34"
            showScrews: true

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 28
                anchors.rightMargin: 28
                spacing: 16

                Label {
                    text: qsTr("DESK32 // GATE G2 HARDWARE TEXTURE PROTOTYPE (WQHD 2560×1440)")
                    font.family: "Monospace"
                    font.pixelSize: 15
                    font.bold: true
                    color: "#00E5FF"
                }

                Item { Layout.fillWidth: true }

                Label {
                    text: qsTr("Drag: Adjust | Shift+Drag/Wheel: Fine | Double-Click: Reset | F11: Fullscreen | Esc: Quit")
                    font.pixelSize: 12
                    color: "#9AA7B5"
                }
            }
        }

        // Main Prototype Comparison Bench (1:1 WQHD scale)
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 24

            // LEFT: 1 GEQ Fader Prototype (Slot + Cap + Ticks + LED + Real Text Readout)
            BrushedMetalPanel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                baseTint: "#0e1116"
                borderColor: "#232a34"
                showScrews: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 28
                    spacing: 16

                    Label {
                        text: qsTr("PROTOTYPE 1 // 32-GEQ FADER UNIT (1:1 WQHD CONSOLE SCALE)")
                        font.family: "Monospace"
                        font.pixelSize: 13
                        font.bold: true
                        color: "#D0DAE4"
                    }

                    Label {
                        text: qsTr("Bitmap metallic cap + recessed slot + vector ticks + amber LED + live text readout")
                        font.pixelSize: 11
                        color: "#808D9B"
                    }

                    Item { Layout.fillHeight: true }

                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        spacing: 48

                        // 1:1 Console Height Fader (~360px high, matching bottom 450px GEQ panel)
                        BrushedMetalPanel {
                            Layout.preferredWidth: 130
                            Layout.preferredHeight: 380
                            baseTint: "#0a0c10"
                            borderColor: "#1d232c"

                            ConsoleFader {
                                id: protoGeqFader
                                anchors.centerIn: parent
                                width: 64
                                height: 356
                                labelText: "1k"
                                value: 3.5
                                bandEnabled: true
                            }
                        }

                        // Live numeric & interaction readout card
                        ColumnLayout {
                            spacing: 10
                            Label {
                                text: qsTr("Fader State (Real Text):")
                                font.pixelSize: 12
                                font.bold: true
                                color: "#B8C4D0"
                            }
                            Label {
                                text: qsTr("Band: %1  |  Gain: %2 dB  |  Active: %3")
                                      .arg(protoGeqFader.labelText)
                                      .arg((protoGeqFader.value > 0 ? "+" : "") + protoGeqFader.value.toFixed(2))
                                      .arg(protoGeqFader.bandEnabled ? "ON" : "OFF")
                                font.family: "Monospace"
                                font.pixelSize: 12
                                color: "#00E5FF"
                            }
                            Button {
                                id: resetBtn
                                text: qsTr("Reset 0.0 dB")
                                onClicked: protoGeqFader.value = 0.0
                                contentItem: Label {
                                    text: resetBtn.text
                                    font.pixelSize: 11
                                    font.bold: true
                                    color: "#D0DAE4"
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                                background: Rectangle {
                                    implicitWidth: 110
                                    implicitHeight: 28
                                    radius: 4
                                    color: resetBtn.down ? "#14181f" : (resetBtn.hovered ? "#252c37" : "#1c222b")
                                    border.color: "#364150"
                                    border.width: 1
                                }
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            // RIGHT: 1 PEQ Large Inspector Knob Prototype (Cyan & Champagne Gold)
            BrushedMetalPanel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                baseTint: "#0e1116"
                borderColor: "#232a34"
                accentColor: "#4000E5FF"
                showScrews: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 28
                    spacing: 16

                    Label {
                        text: qsTr("PROTOTYPE 2 // PEQ INSPECTOR LARGE KNOB (1:1 WQHD SCALE)")
                        font.family: "Monospace"
                        font.pixelSize: 13
                        font.bold: true
                        color: "#D0DAE4"
                    }

                    Label {
                        text: qsTr("Fixed-light brushed aluminum cap + rotating engraved notch + dynamic Scene Graph arc + real text readout")
                        font.pixelSize: 11
                        color: "#808D9B"
                    }

                    Item { Layout.fillHeight: true }

                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        spacing: 56

                        // Unipolar Frequency Knob (L-CH Cyan)
                        ConsoleKnob {
                            id: protoFreqKnob
                            labelText: "FREQUENCY"
                            value: 45.2
                            minValue: 20.0
                            maxValue: 20000.0
                            defaultValue: 1000.0
                            logarithmic: true
                            bipolar: false
                            unitSuffix: " Hz"
                            decimals: 1
                            arcColor: "#00E5FF"
                            knobDiameter: 128
                        }

                        // Bipolar Gain Knob (R-CH Champagne Gold preview)
                        ConsoleKnob {
                            id: protoGainKnob
                            labelText: "GAIN (BIPOLAR)"
                            value: -4.5
                            minValue: -18.0
                            maxValue: 18.0
                            defaultValue: 0.0
                            logarithmic: false
                            bipolar: true
                            bipolarOrigin: 0.0
                            unitSuffix: " dB"
                            decimals: 1
                            arcColor: "#E5B96B"
                            knobDiameter: 128
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }
        }
    }
}
