import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Desk32.Core
import "../components"
import ".."

Item {
    id: wsViewRoot

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12

        // Top Workspace Status Strip
        BrushedMetalPanel {
            Layout.fillWidth: true
            Layout.preferredHeight: 54
            baseTint: "#161b22"
            borderColor: "#313945"
            showScrews: true

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 20
                spacing: 12

                Label {
                    text: qsTr("WORKSPACE:")
                    font.family: "Monospace"
                    font.pixelSize: 12
                    font.bold: true
                    color: "#8090A0"
                }

                Label {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: WorkspaceManager.hasOpenWorkspace
                          ? WorkspaceManager.openWorkspacePath
                          : qsTr("Read-Only Mode — Select a dedicated Workspace at startup to enable import, tag editing, saves, and Render")
                    font.family: "Monospace"
                    font.pixelSize: 12
                    font.bold: WorkspaceManager.hasOpenWorkspace
                    color: WorkspaceManager.hasOpenWorkspace ? "#00E5FF" : "#F5B841"
                    elide: Text.ElideMiddle
                }

                Label {
                    text: qsTr("Dedicated Workspace & Checkpoint Browser")
                    font.pixelSize: 10
                    color: "#6B7886"
                }
            }
        }

        // Host existing verified WorkspacePanel inside full-view brushed frame (G5 refactors into full 2-pane layout)
        BrushedMetalPanel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            baseTint: "#141920"
            borderColor: "#313945"

            WorkspacePanel {
                id: embeddedWorkspacePanel
                anchors.fill: parent
                anchors.margins: 8
                visible: true
            }
        }
    }
}
