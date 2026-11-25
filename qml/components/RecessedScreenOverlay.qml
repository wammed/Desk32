import QtQuick

// Qt 6.4-compatible recessed screen inset shadow + subtle diagonal glass sheen overlay
Item {
    id: overlayRoot

    property int cornerRadius: 4
    property real topShadowOpacity: 0.68
    property real sideShadowOpacity: 0.44
    property real bottomShadowOpacity: 0.32
    property int topShadowSize: 10
    property int sideShadowSize: 7
    property int bottomShadowSize: 5
    property bool showGlassSheen: true
    property real glassSheenOpacity: 0.05

    anchors.fill: parent
    z: 50

    Rectangle {
        id: clipMask
        anchors.fill: parent
        radius: overlayRoot.cornerRadius
        color: "transparent"
        clip: true

        // 1. Subtle diagonal glass sheen (rendered behind bezel inset shadows so bezel casts onto glass)
        Canvas {
            id: glassSheenCanvas
            visible: overlayRoot.showGlassSheen && overlayRoot.glassSheenOpacity > 0.001
            anchors.fill: parent
            anchors.margins: 1
            antialiasing: true
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            Component.onCompleted: requestPaint()

            Connections {
                target: overlayRoot
                function onGlassSheenOpacityChanged() { glassSheenCanvas.requestPaint() }
                function onCornerRadiusChanged() { glassSheenCanvas.requestPaint() }
                function onShowGlassSheenChanged() { glassSheenCanvas.requestPaint() }
            }

            onPaint: {
                var ctx = getContext("2d")
                if (!ctx) return
                var w = width
                var h = height
                if (w <= 2 || h <= 2) return
                ctx.clearRect(0, 0, w, h)

                var r = Math.max(0, Math.min(overlayRoot.cornerRadius - 1, Math.min(w, h) * 0.5))
                var op = overlayRoot.glassSheenOpacity

                ctx.save()
                // Rounded-rect clip path matching inner glass perimeter
                ctx.beginPath()
                ctx.moveTo(r, 0)
                ctx.lineTo(w - r, 0)
                ctx.quadraticCurveTo(w, 0, w, r)
                ctx.lineTo(w, h - r)
                ctx.quadraticCurveTo(w, h, w - r, h)
                ctx.lineTo(r, h)
                ctx.quadraticCurveTo(0, h, 0, h - r)
                ctx.lineTo(0, r)
                ctx.quadraticCurveTo(0, 0, r, 0)
                ctx.closePath()
                ctx.clip()

                // Broad soft diagonal ambient sheen (top-left toward bottom-right)
                var baseGrad = ctx.createLinearGradient(0, 0, w * 0.72, h * 0.92)
                baseGrad.addColorStop(0.00, "rgba(210, 236, 255, " + (op * 0.95).toFixed(4) + ")")
                baseGrad.addColorStop(0.28, "rgba(210, 236, 255, " + (op * 0.40).toFixed(4) + ")")
                baseGrad.addColorStop(0.55, "rgba(210, 236, 255, " + (op * 0.08).toFixed(4) + ")")
                baseGrad.addColorStop(1.00, "rgba(210, 236, 255, 0.0)")
                ctx.fillStyle = baseGrad
                ctx.fillRect(0, 0, w, h)

                // Soft diagonal specular reflection band across glass surface
                var bandGrad = ctx.createLinearGradient(w * 0.05, 0, w * 0.58, h * 0.95)
                bandGrad.addColorStop(0.00, "rgba(225, 244, 255, 0.0)")
                bandGrad.addColorStop(0.18, "rgba(225, 244, 255, " + (op * 0.65).toFixed(4) + ")")
                bandGrad.addColorStop(0.32, "rgba(225, 244, 255, " + (op * 0.16).toFixed(4) + ")")
                bandGrad.addColorStop(0.46, "rgba(225, 244, 255, 0.0)")
                bandGrad.addColorStop(0.54, "rgba(225, 244, 255, " + (op * 0.32).toFixed(4) + ")")
                bandGrad.addColorStop(0.64, "rgba(225, 244, 255, 0.0)")
                bandGrad.addColorStop(1.00, "rgba(225, 244, 255, 0.0)")
                ctx.fillStyle = bandGrad
                ctx.fillRect(0, 0, w, h)

                ctx.restore()
            }
        }

        // 2. Top inset cast shadow from outer metal bezel
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

        // 3. Left inset cast shadow
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

        // 4. Right inset cast shadow
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

        // 5. Bottom inner rim shadow + catchlight on lower bevel lip
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
            color: "#22ffffff"
        }
    }
}
