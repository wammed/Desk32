import QtQuick

Item {
    id: panelRoot

    property color baseTint: "#13171d"
    property color borderColor: "#2f3743"
    property color topHighlightColor: "#2cffffff"
    property color bottomShadowColor: "#78000000"
    property color accentColor: "transparent"
    property int accentWidth: 1
    property int cornerRadius: 5
    property bool showScrews: false
    property bool isRecessedWell: false
    property bool dropShadowEnabled: !isRecessedWell
    property real shadowOpacity: 0.72
    property real brushedOpacity: 0.32

    implicitWidth: 200
    implicitHeight: 200

    // Outer cast drop shadow for physical panel Z-depth above chassis
    BorderImage {
        visible: panelRoot.dropShadowEnabled
        z: -1
        anchors.fill: panelFrame
        anchors.leftMargin: -10
        anchors.rightMargin: -10
        anchors.topMargin: -8
        anchors.bottomMargin: -13
        source: "qrc:/Desk32/resources/ui/panel_shadow_9slice.png"
        border { left: 22; top: 22; right: 22; bottom: 22 }
        opacity: panelRoot.shadowOpacity
    }

    Rectangle {
        id: panelFrame
        anchors.fill: parent
        radius: panelRoot.cornerRadius
        color: panelRoot.baseTint
        border.color: panelRoot.borderColor
        border.width: 1
        clip: true

        // Reusable seamless brushed-metal texture tile
        Image {
            anchors.fill: parent
            anchors.margins: 1
            source: "qrc:/Desk32/resources/ui/brushed_metal_tile.png"
            fillMode: Image.Tile
            opacity: panelRoot.isRecessedWell ? 0.14 : panelRoot.brushedOpacity
            smooth: false
        }

        // Machined gunmetal lighting gradient (bright overhead bevel vs recessed dark well)
        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            radius: Math.max(0, panelRoot.cornerRadius - 1)
            gradient: Gradient {
                GradientStop { position: 0.0; color: panelRoot.isRecessedWell ? "#52000000" : "#18ffffff" }
                GradientStop { position: 0.14; color: panelRoot.isRecessedWell ? "#18000000" : "#06ffffff" }
                GradientStop { position: 0.80; color: panelRoot.isRecessedWell ? "#06000000" : "#1c000000" }
                GradientStop { position: 1.0; color: panelRoot.isRecessedWell ? "#1effffff" : "#4c000000" }
            }
        }

        // Inner side bevel shadows for recessed wells
        Rectangle {
            visible: panelRoot.isRecessedWell
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.margins: 1
            width: 6
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: "#48000000" }
                GradientStop { position: 1.0; color: "transparent" }
            }
        }
        Rectangle {
            visible: panelRoot.isRecessedWell
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            anchors.margins: 1
            width: 6
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 1.0; color: "#48000000" }
            }
        }

        // Top chamfer catchlight
        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 1
            anchors.leftMargin: panelRoot.cornerRadius
            anchors.rightMargin: panelRoot.cornerRadius
            height: 1
            color: panelRoot.isRecessedWell ? "#70000000" : panelRoot.topHighlightColor
        }

        // Bottom bevel lip
        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottomMargin: 1
            anchors.leftMargin: panelRoot.cornerRadius
            anchors.rightMargin: panelRoot.cornerRadius
            height: 1
            color: panelRoot.isRecessedWell ? "#28ffffff" : panelRoot.bottomShadowColor
        }

        // Optional glowing inner accent frame (e.g., PEQ Inspector / Routed Deck)
        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            radius: Math.max(0, panelRoot.cornerRadius - 1)
            color: "transparent"
            border.color: panelRoot.accentColor
            border.width: panelRoot.accentColor !== "transparent" ? panelRoot.accentWidth : 0
            visible: panelRoot.accentColor !== "transparent"
        }

        // Optional corner machine screws with countersunk socket well
        Repeater {
            model: panelRoot.showScrews ? 4 : 0
            Item {
                width: 15
                height: 15
                x: (index % 2 === 0) ? 5 : (panelFrame.width - width - 5)
                y: (index < 2) ? 5 : (panelFrame.height - height - 5)

                // Countersunk dark recess + subtle bottom rim highlight
                Rectangle {
                    anchors.fill: parent
                    radius: 7.5
                    color: "#06080c"
                    border.color: "#1effffff"
                    border.width: 1
                    Rectangle {
                        anchors.fill: parent
                        radius: 7.5
                        color: "transparent"
                        border.color: "#aa000000"
                        border.width: 1
                        anchors.topMargin: -1
                    }
                }

                Image {
                    anchors.centerIn: parent
                    width: 11
                    height: 11
                    source: "qrc:/Desk32/resources/ui/screw_hex_dark.png"
                    smooth: true
                    opacity: 0.92
                }
            }
        }
    }
}
