import QtQuick
import QtQuick.Controls

Button {
    id: btnRoot

    // Color & Hardware Finish Overrides
    property color accentColor: "#00E5FF"
    property color activeTopColor: "#244D5E"
    property color activeBottomColor: "#0E222C"
    property color normalTopColor: "#3A4250"
    property color normalBottomColor: "#191E26"
    // Backward-compatible aliases so existing callers still work cleanly
    property color activeBgColor: activeBottomColor
    property color normalBgColor: normalBottomColor
    property color hoverBgColor: "#465061"
    property color borderColor: "#080A0D"
    property color outerFrameColor: "#424C5B"

    // Visual Style Options
    property bool anodizedFill: false   // Solid anodized keycaps (Gold RENDER, Red GLOBAL BYPASS, Transport keys)
    property color anodizedTop: "#4E5766"
    property color anodizedBottom: "#232832"
    property color anodizedText: "#F4F7FA"
    property bool showLed: false
    property color ledColor: "#00E676"
    property string toolTipText: ""
    property int fontSize: 11
    property bool monospace: false
    property int cornerRadius: 5

    implicitHeight: 30
    implicitWidth: Math.max(44, contentLabel.implicitWidth + (showLed ? 26 : 16))
    leftPadding: 7
    rightPadding: 7
    topPadding: 3
    bottomPadding: 5

    readonly property bool isActiveState: highlighted || checked

    ToolTip.visible: hovered && (toolTipText.length > 0 || contentLabel.truncated)
    ToolTip.text: toolTipText.length > 0 ? toolTipText : text
    ToolTip.delay: 350

    contentItem: Item {
        implicitWidth: contentLabel.implicitWidth + (btnRoot.showLed ? 13 : 0)
        implicitHeight: contentLabel.implicitHeight

        Row {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: btnRoot.down ? 1 : -1
            spacing: 5
            width: Math.min(implicitWidth, parent.width)

            Rectangle {
                visible: btnRoot.showLed
                anchors.verticalCenter: parent.verticalCenter
                width: 7
                height: 7
                radius: 3.5
                color: btnRoot.isActiveState ? btnRoot.ledColor : "#363E4A"
                border.color: btnRoot.isActiveState ? "#CCffffff" : "#15191F"
                border.width: 1
            }

            Label {
                id: contentLabel
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, btnRoot.availableWidth - (btnRoot.showLed ? 12 : 0))
                text: btnRoot.text
                font.family: btnRoot.monospace ? "Monospace" : Qt.application.font.family
                font.pixelSize: btnRoot.fontSize
                font.bold: btnRoot.isActiveState || btnRoot.anodizedFill || btnRoot.font.bold
                color: !btnRoot.enabled
                       ? "#606B78"
                       : (btnRoot.anodizedFill
                          ? btnRoot.anodizedText
                          : (btnRoot.isActiveState ? btnRoot.accentColor : "#E4ECF4"))
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }
    }

    background: Item {
        // 1. Recessed Socket Well & Drop Shadow underneath physical keycap
        Rectangle {
            anchors.fill: parent
            radius: btnRoot.cornerRadius
            color: "#06080B"
            border.color: btnRoot.isActiveState ? btnRoot.accentColor : "#2A313C"
            border.width: btnRoot.isActiveState ? 1.5 : 1
        }

        // 2. Keycap 3D Extruded Bottom Chamfer Lip (visible height when unpressed)
        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            radius: Math.max(2, btnRoot.cornerRadius - 1)
            color: "#0A0D12"
        }

        // 3. Raised Sculpted Keycap Top Face (shifts down 1px when pressed)
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.leftMargin: 1.5
            anchors.rightMargin: 1.5
            anchors.topMargin: btnRoot.down ? 2.5 : 1.5
            anchors.bottomMargin: btnRoot.down ? 1.5 : 3.5
            radius: Math.max(2, btnRoot.cornerRadius - 1)
            border.color: !btnRoot.enabled
                          ? "#28303B"
                          : (btnRoot.anodizedFill
                             ? "#30ffffff"
                             : (btnRoot.isActiveState ? Qt.rgba(btnRoot.accentColor.r, btnRoot.accentColor.g, btnRoot.accentColor.b, 0.55) : "#485464"))
            border.width: 1

            gradient: Gradient {
                GradientStop {
                    position: 0.0
                    color: !btnRoot.enabled
                           ? "#252B33"
                           : (btnRoot.down
                              ? "#12161C"
                              : (btnRoot.anodizedFill
                                 ? (btnRoot.hovered ? Qt.lighter(btnRoot.anodizedTop, 1.10) : btnRoot.anodizedTop)
                                 : (btnRoot.isActiveState
                                    ? btnRoot.activeTopColor
                                    : (btnRoot.hovered ? btnRoot.hoverBgColor : btnRoot.normalTopColor))))
                }
                GradientStop {
                    position: 0.22
                    color: !btnRoot.enabled
                           ? "#1E232B"
                           : (btnRoot.down
                              ? "#171C23"
                              : (btnRoot.anodizedFill
                                 ? Qt.darker(btnRoot.anodizedTop, 1.08)
                                 : (btnRoot.isActiveState
                                    ? Qt.darker(btnRoot.activeTopColor, 1.12)
                                    : (btnRoot.hovered ? "#38414E" : "#2D3440"))))
                }
                GradientStop {
                    position: 0.84
                    color: !btnRoot.enabled
                           ? "#161A20"
                           : (btnRoot.down
                              ? "#1E242D"
                              : (btnRoot.anodizedFill
                                 ? btnRoot.anodizedBottom
                                 : (btnRoot.isActiveState
                                    ? btnRoot.activeBottomColor
                                    : btnRoot.normalBottomColor)))
                }
                GradientStop {
                    position: 1.0
                    color: !btnRoot.enabled
                           ? "#101318"
                           : (btnRoot.down
                              ? "#252C36"
                              : (btnRoot.anodizedFill
                                 ? Qt.darker(btnRoot.anodizedBottom, 1.28)
                                 : "#0F1318"))
                }
            }

            // Crisp overhead catchlight along upper bevel edge
            Rectangle {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.topMargin: 1
                anchors.leftMargin: 3
                anchors.rightMargin: 3
                height: 1.5
                radius: 0.5
                color: btnRoot.down
                       ? "#20000000"
                       : (!btnRoot.enabled ? "#16ffffff" : (btnRoot.anodizedFill ? "#60ffffff" : "#44ffffff"))
            }

            // Subtle left & right chamfer catchlights for physical keycap mass
            Rectangle {
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.topMargin: 2
                anchors.bottomMargin: 2
                anchors.leftMargin: 1
                width: 1
                color: btnRoot.enabled ? "#1Effffff" : "#0Affffff"
            }
            Rectangle {
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                anchors.topMargin: 2
                anchors.bottomMargin: 2
                anchors.rightMargin: 1
                width: 1
                color: "#38000000"
            }
        }
    }
}
