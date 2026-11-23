import QtQuick

Item {
    id: meterRoot

    property double peakValue: 0.0   // linear 0.0 .. 1.0+
    property double rmsValue: 0.0    // linear 0.0 .. 1.0+
    property bool clipped: false
    property bool horizontal: false
    property bool linearScale: false
    property int segmentCount: 28
    property color greenColor: "#00E676"
    property color amberColor: "#F5B841"
    property color redColor: "#FF5252"

    implicitWidth: horizontal ? 160 : 14
    implicitHeight: horizontal ? 12 : 200

    function linearToNormalized(lin) {
        if (linearScale) {
            return Math.max(0.0, Math.min(1.0, lin))
        }
        if (lin <= 1e-5) return 0.0
        var db = 20.0 * Math.log10(lin)
        // Map -60 dB .. +3 dB into 0.0 .. 1.0
        return Math.max(0.0, Math.min(1.0, (db + 60.0) / 63.0))
    }

    readonly property double normPeak: linearToNormalized(peakValue)
    readonly property double normRms: linearToNormalized(rmsValue)

    onNormPeakChanged: meterCanvas.requestPaint()
    onNormRmsChanged: meterCanvas.requestPaint()
    onClippedChanged: meterCanvas.requestPaint()

    Rectangle {
        anchors.fill: parent
        radius: 2
        color: "#06080a"
        border.color: meterRoot.clipped ? meterRoot.redColor : "#1c222b"
        border.width: 1

        Canvas {
            id: meterCanvas
            anchors.fill: parent
            anchors.margins: 2
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            Component.onCompleted: requestPaint()

            onPaint: {
                var ctx = getContext("2d")
                if (!ctx) return
                var w = width
                var h = height
                if (w <= 0 || h <= 0) return
                ctx.clearRect(0, 0, w, h)

                var count = Math.max(6, meterRoot.segmentCount)
                var gap = 1.5
                var pk = meterRoot.normPeak
                var rms = meterRoot.normRms

                for (var i = 0; i < count; ++i) {
                    var frac = (i + 1) / count
                    var segColor = frac > 0.88
                                   ? meterRoot.redColor
                                   : (frac > 0.68 ? meterRoot.amberColor : meterRoot.greenColor)

                    var isPeakLit = (pk >= frac - (0.5 / count)) || (meterRoot.clipped && i === count - 1)
                    var isRmsLit = (rms >= frac - (0.5 / count))

                    if (isPeakLit) {
                        ctx.fillStyle = segColor
                    } else if (isRmsLit) {
                        ctx.fillStyle = Qt.rgba(segColor.r, segColor.g, segColor.b, 0.55)
                    } else {
                        ctx.fillStyle = "#14181f"
                    }

                    if (meterRoot.horizontal) {
                        var sw = (w - (count - 1) * gap) / count
                        var sx = i * (sw + gap)
                        ctx.fillRect(sx, 0, Math.max(1, sw), h)
                    } else {
                        var sh = (h - (count - 1) * gap) / count
                        var sy = h - (i + 1) * sh - i * gap
                        ctx.fillRect(0, sy, w, Math.max(1, sh))
                    }
                }
            }
        }
    }
}
