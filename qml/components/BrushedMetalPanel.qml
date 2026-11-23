import QtQuick

Item {
    id: panelRoot

    property color baseTint: "#13171d"
    property color borderColor: "#2f3743"
    property color topHighlightColor: "#1cffffff"
    property color bottomShadowColor: "#55000000"
    property color accentColor: "transparent"
    property int accentWidth: 1
    property int cornerRadius: 5
    property bool showScrews: false
    property bool isRecessedWell: false
    property real brushedOpacity: 0.32

    implicitWidth: 200
    implicitHeight: 200

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
                GradientStop { position: 0.0; color: panelRoot.isRecessedWell ? "#38000000" : "#10ffffff" }
                GradientStop { position: 0.18; color: panelRoot.isRecessedWell ? "#10000000" : "#04ffffff" }
                GradientStop { position: 0.78; color: panelRoot.isRecessedWell ? "#05000000" : "#14000000" }
                GradientStop { position: 1.0; color: panelRoot.isRecessedWell ? "#18ffffff" : "#38000000" }
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
            color: panelRoot.isRecessedWell ? "#55000000" : panelRoot.topHighlightColor
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
            color: panelRoot.isRecessedWell ? "#20ffffff" : panelRoot.bottomShadowColor
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

        // Optional corner machine screws
        Repeater {
            model: panelRoot.showScrews ? 4 : 0
            Image {
                width: 11
                height: 11
                source: "qrc:/Desk32/resources/ui/screw_hex_dark.png"
                smooth: true
                x: (index % 2 === 0) ? 7 : (panelFrame.width - width - 7)
                y: (index < 2) ? 7 : (panelFrame.height - height - 7)
                opacity: 0.9
            }
        }
    }
}
