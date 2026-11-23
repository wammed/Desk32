import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Desk32.Core

BrushedMetalPanel {
    id: topBarRoot

    property string currentView: "console"
    signal viewSelected(string viewId)

    implicitHeight: 56
    Layout.fillWidth: true
    baseTint: "#13171d"
    borderColor: "#2c3440"
    topHighlightColor: "#1cffffff"
    cornerRadius: 0
    brushedOpacity: 0.32

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 22
        anchors.rightMargin: 18
        spacing: 16

        // Left: Desk32 Hardware Brand Wordmark with subtle warm/cyan wave bar
        Column {
            Layout.alignment: Qt.AlignVCenter
            spacing: 1

            Row {
                spacing: 0
                Label {
                    text: "Desk"
                    font.pixelSize: 27
                    font.bold: true
                    color: "#F3F6FA"
                }
                Label {
                    text: "32"
                    font.pixelSize: 27
                    font.bold: true
                    color: "#E8BA6B"
                }
            }

            Rectangle {
                width: 96
                height: 2
                radius: 1
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: "#00E5FF" }
                    GradientStop { position: 0.6; color: "#E8BA6B" }
                    GradientStop { position: 1.0; color: "transparent" }
                }
            }
        }

        Item { Layout.fillWidth: true }

        // Center: Flush Segmented 4-Tab Bar matching mockups
        Rectangle {
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: tabRow.implicitWidth + 4
            implicitHeight: 38
            radius: 4
            color: "#090c10"
            border.color: "#363f4d"
            border.width: 1

            RowLayout {
                id: tabRow
                anchors.fill: parent
                anchors.margins: 2
                spacing: 2

                Repeater {
                    model: [
                        { id: "console", label: qsTr("Console") },
                        { id: "workspace", label: qsTr("Workspace") },
                        { id: "log", label: qsTr("Log") },
                        { id: "settings", label: qsTr("Settings") }
                    ]

                    ConsoleButton {
                        text: modelData.label
                        implicitWidth: 132
                        implicitHeight: 34
                        fontSize: 13
                        cornerRadius: 3
                        highlighted: topBarRoot.currentView === modelData.id
                        activeTopColor: "#28333f"
                        activeBottomColor: "#142029"
                        normalTopColor: "#242a34"
                        normalBottomColor: "#161a21"
                        onClicked: topBarRoot.viewSelected(modelData.id)
                    }
                }
            }
        }

        Item { Layout.fillWidth: true }

        // Right: Workspace Badge + 2-Line Hardware Stream Readout Box (matching mockup corner box)
        RowLayout {
            Layout.alignment: Qt.AlignVCenter
            spacing: 8

            Rectangle {
                implicitWidth: wsBadgeLbl.implicitWidth + 14
                Layout.preferredHeight: 36
                height: 36
                radius: 4
                color: WorkspaceManager.hasOpenWorkspace ? "#0d2419" : "#12161c"
                border.color: WorkspaceManager.hasOpenWorkspace ? "#2d7850" : "#2f3744"
                Label {
                    id: wsBadgeLbl
                    anchors.centerIn: parent
                    text: WorkspaceManager.hasOpenWorkspace ? qsTr("WS: OPEN") : qsTr("WS: READ-ONLY")
                    font.family: "Monospace"
                    font.pixelSize: 10
                    font.bold: true
                    color: WorkspaceManager.hasOpenWorkspace ? "#00E676" : "#9AA7B5"
                }
            }

            Rectangle {
                implicitWidth: Math.max(158, backendLbl.implicitWidth + 24)
                Layout.preferredHeight: 38
                height: 38
                radius: 4
                color: "#080b0f"
                border.color: "#323b48"
                border.width: 1

                Column {
                    anchors.centerIn: parent
                    spacing: 1
                    Label {
                        id: backendLbl
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: (EngineController.audioBackend || "PipeWire") + " "
                              + (Math.round(EngineController.graphSampleRate / 100) / 10) + "kHz"
                        font.pixelSize: 12
                        font.bold: true
                        color: "#4CD6F0"
                    }
                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "64 bit float"
                        font.pixelSize: 10
                        color: "#A3B0BE"
                    }
                }
            }
        }
    }
}
