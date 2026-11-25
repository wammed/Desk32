import QtQuick

// Qt 6.4-compatible recessed screen inset shadow + subtle diagonal glass sheen overlay
Item {
    id: overlayRoot

    property int cornerRadius: 4
    property real topShadowOpacity: 0.65
    property real sideShadowOpacity: 0.42
    property real bottomShadowOpacity: 0.30
    property int topShadowSize: 10
    property int sideShadowSize: 7
    property int bottomShadowSize: 5
    property bool showGlassSheen: true
    property real glassSheenOpacity: 0.035

    anchors.fill: parent
    z: 50

    Rectangle {
        id: clipMask
        anchors.fill: parent
        radius: overlayRoot.cornerRadius
        color: "transparent"
        clip: true

        // Top inset cast shadow from outer metal bezel
        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: overlayRoot.topShadowSize
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, overlayRoot.topShadowOpacity) }
                GradientStop { position: 0.45; color: Qt.rgba(0, 0, 0, overlayRoot.topShadowOpacity * 0.35) }
                GradientStop { position: 1.0; color: "transparent" }
            }
        }

        // Left inset cast shadow
        Rectangle {
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            width: overlayRoot.sideShadowSize
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, overlayRoot.sideShadowOpacity) }
                GradientStop { position: 1.0; color: "transparent" }
            }
        }

        // Right inset cast shadow
        Rectangle {
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            width: overlayRoot.sideShadowSize
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, overlayRoot.sideShadowOpacity) }
            }
        }

        // Bottom inner rim shadow + catchlight on lower lip
        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: overlayRoot.bottomShadowSize
            gradient: Gradient {
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, overlayRoot.bottomShadowOpacity) }
            }
        }
        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: overlayRoot.cornerRadius
            anchors.rightMargin: overlayRoot.cornerRadius
            height: 1
            color: "#1effffff"
        }

        // Subtle overhead glass reflection across upper screen surface
        Rectangle {
            visible: overlayRoot.showGlassSheen
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: Math.min(parent.height * 0.42, 160)
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.rgba(0.86, 0.94, 1.0, overlayRoot.glassSheenOpacity) }
                GradientStop { position: 0.55; color: Qt.rgba(0.86, 0.94, 1.0, overlayRoot.glassSheenOpacity * 0.28) }
                GradientStop { position: 1.0; color: "transparent" }
            }
        }
    }
}
