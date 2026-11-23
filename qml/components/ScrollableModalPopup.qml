import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: modalRoot

    property string titleText: ""
    property int preferredWidth: 620
    property int preferredHeight: 520
    property color accentColor: "#00E5FF"
    default property alias bodyContent: bodyContainer.data

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 0

    readonly property int overlayW: Overlay.overlay ? Overlay.overlay.width : 2560
    readonly property int overlayH: Overlay.overlay ? Overlay.overlay.height : 1440
    width: Math.max(320, Math.min(preferredWidth, overlayW - 48))
    height: Math.max(220, Math.min(preferredHeight, overlayH - 48))

    background: BrushedMetalPanel {
        baseTint: "#0e1116"
        borderColor: "#2d3745"
        accentColor: Qt.rgba(modalRoot.accentColor.r, modalRoot.accentColor.g, modalRoot.accentColor.b, 0.45)
        cornerRadius: 6
        showScrews: true
    }

    contentItem: ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Rectangle {
                width: 8
                height: 8
                radius: 4
                color: modalRoot.accentColor
            }

            Label {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: modalRoot.titleText
                font.family: "Monospace"
                font.pixelSize: 13
                font.bold: true
                color: modalRoot.accentColor
                elide: Text.ElideRight
            }

            ConsoleButton {
                text: "✕"
                implicitWidth: 30
                implicitHeight: 26
                onClicked: modalRoot.close()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: "#222933"
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth

            ColumnLayout {
                id: bodyContainer
                width: parent.width
                spacing: 10
            }
        }
    }
}
