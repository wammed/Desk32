import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: faderRoot

    // Public API (real text & numeric model — zero baked-in labels)
    property string labelText: "1k"
    property double value: 0.0          // dB
    property double minValue: -12.0
    property double maxValue: 12.0
    property double defaultValue: 0.0
    property double stepSize: 0.1
    property bool bandEnabled: true
    property color ledColor: "#F2A93B"
    property color accentColor: "#00E5FF"
    property bool showHeaderLed: true
    property bool showReadout: true
    property bool consoleTaper: false
    readonly property bool isDragging: dragArea.dragging

    signal valueEdited(double newValue)
    signal enabledToggled(bool newEnabled)

    implicitWidth: 56
    implicitHeight: 360

    function clampAndSnap(val, fine) {
        var clamped = Math.max(minValue, Math.min(maxValue, val))
        var step = fine ? 0.05 : stepSize
        if (step > 0) {
            clamped = Math.round(clamped / step) * step
        }
        if (Math.abs(clamped - defaultValue) < 0.03 && !fine) {
            clamped = defaultValue
        }
        return Math.max(minValue, Math.min(maxValue, clamped))
    }

    function valueToNormalized(v) {
        if (!consoleTaper) {
            if (maxValue <= minValue) return 0.5
            return Math.max(0.0, Math.min(1.0, (v - minValue) / (maxValue - minValue)))
        }
        // Studio console taper: generous positive boost travel (0 dB @ 0.68, +6 dB @ 1.00)
        if (v >= 0.0) {
            var spanPos = Math.max(0.1, maxValue)
            return Math.min(1.0, 0.68 + (v / spanPos) * 0.32)
        }
        if (v >= -12.0) return 0.40 + ((v + 12.0) / 12.0) * 0.28
        if (v >= -30.0) return 0.16 + ((v + 30.0) / 18.0) * 0.24
        var spanBot = Math.max(1.0, -30.0 - minValue)
        return Math.max(0.0, ((v - minValue) / spanBot) * 0.16)
    }

    function normalizedToValue(norm) {
        var n = Math.max(0.0, Math.min(1.0, norm))
        if (!consoleTaper) {
            return minValue + n * (maxValue - minValue)
        }
        if (n >= 0.68) return ((n - 0.68) / 0.32) * maxValue
        if (n >= 0.40) return -12.0 + ((n - 0.40) / 0.28) * 12.0
        if (n >= 0.16) return -30.0 + ((n - 0.16) / 0.24) * 18.0
        return minValue + (n / 0.16) * (-30.0 - minValue)
    }

    function normalizedPos() {
        return valueToNormalized(value)
    }

    function formatDb(v) {
        var clean = Math.abs(v) < 0.049 ? 0.0 : v
        return (clean > 0 ? "+" : "") + clean.toFixed(1) + " dB"
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        // 1. Recessed metallic bezel + glowing amber LED toggle
        Item {
            visible: faderRoot.showHeaderLed
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 20
            Layout.preferredHeight: 20

            Rectangle {
                anchors.fill: parent
                radius: 3
                color: "#0b0d11"
                border.color: "#2b323d"
                border.width: 1

                // Soft outer LED bloom halo
                Rectangle {
                    anchors.centerIn: parent
                    width: 22
                    height: 22
                    radius: 11
                    visible: faderRoot.bandEnabled
                    color: Qt.rgba(faderRoot.ledColor.r, faderRoot.ledColor.g, faderRoot.ledColor.b, 0.14)
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: 15
                    height: 15
                    radius: 4
                    visible: faderRoot.bandEnabled
                    color: Qt.rgba(faderRoot.ledColor.r, faderRoot.ledColor.g, faderRoot.ledColor.b, 0.34)
                }

                // Core LED diode
                Rectangle {
                    anchors.centerIn: parent
                    width: 9
                    height: 9
                    radius: 1.5
                    color: faderRoot.bandEnabled ? faderRoot.ledColor : "#232830"
                    border.color: faderRoot.bandEnabled ? "#FFE2A6" : "#15181d"
                    border.width: 1
                    // Subtle specular hotspot on LED lens
                    Rectangle {
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.margins: 1.5
                        width: 3
                        height: 2
                        radius: 1
                        visible: faderRoot.bandEnabled
                        color: "#90ffffff"
                    }
                }
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    faderRoot.bandEnabled = !faderRoot.bandEnabled
                    faderRoot.enabledToggled(faderRoot.bandEnabled)
                }
            }
        }

        // 2. Vertical slot + calibrated tick marks + bitmap fader cap
        Item {
            id: trackArea
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 160

            readonly property real capWidth: 34
            readonly property real capHeight: 54
            readonly property real travelPadding: capHeight * 0.5 + 4
            readonly property real travelHeight: Math.max(20, height - travelPadding * 2)

            // Precision tick marks on both sides of the slot
            Canvas {
                id: tickCanvas
                anchors.fill: parent
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                Component.onCompleted: requestPaint()

                onPaint: {
                    var ctx = getContext("2d")
                    if (!ctx) return
                    var w = width
                    var h = height
                    ctx.clearRect(0, 0, w, h)

                    var cx = w * 0.5
                    var topY = trackArea.travelPadding
                    var len = trackArea.travelHeight

                    if (faderRoot.consoleTaper) {
                        var marks = [faderRoot.maxValue, 3.0, 0.0, -5.0, -10.0, -20.0, -30.0, -36.0, faderRoot.minValue]
                        for (var m = 0; m < marks.length; ++m) {
                            var dbM = marks[m]
                            var nM = faderRoot.valueToNormalized(dbM)
                            var ym = Math.round(topY + (1.0 - nM) * len) + 0.5
                            var isUnity = (dbM === 0.0)
                            var isMajorM = (dbM === faderRoot.maxValue || dbM === 0.0 || dbM === -10.0 || dbM === -20.0 || dbM === -30.0 || dbM === faderRoot.minValue)
                            var tLen = isUnity ? 13 : (isMajorM ? 9 : 5)
                            ctx.strokeStyle = isUnity ? "#FFFFFF" : (isMajorM ? "#E2EAF2" : "#9AA7B5")
                            ctx.lineWidth = isUnity ? 2.0 : (isMajorM ? 1.3 : 1.0)
                            ctx.beginPath()
                            ctx.moveTo(cx - 6 - tLen, ym); ctx.lineTo(cx - 6, ym)
                            ctx.moveTo(cx + 6, ym); ctx.lineTo(cx + 6 + tLen, ym)
                            ctx.stroke()
                        }
                    } else {
                        // 13 ticks across range (+12 down to -12 in 2 dB steps)
                        var numTicks = 13
                        for (var i = 0; i < numTicks; ++i) {
                            var t = i / (numTicks - 1)
                            var y = Math.round(topY + t * len) + 0.5
                            var isCenter = (i === 6)
                            var isMajor = (i === 0 || i === 3 || i === 6 || i === 9 || i === 12)
                            var tickLen = isCenter ? 12 : (isMajor ? 8 : 5)
                            var gap = 6

                            ctx.strokeStyle = isCenter ? "#FFFFFF" : (isMajor ? "#DDE6F0" : "#9AA7B5")
                            ctx.lineWidth = isCenter ? 1.8 : (isMajor ? 1.3 : 1.0)

                            // Left ticks
                            ctx.beginPath()
                            ctx.moveTo(cx - gap - tickLen, y)
                            ctx.lineTo(cx - gap, y)
                            ctx.stroke()

                            // Right ticks
                            ctx.beginPath()
                            ctx.moveTo(cx + gap, y)
                            ctx.lineTo(cx + gap + tickLen, y)
                            ctx.stroke()
                        }
                    }
                }
            }

            // Deep recessed vertical fader slot
            Rectangle {
                id: slotTrack
                width: 6
                height: trackArea.travelHeight + 8
                anchors.horizontalCenter: parent.horizontalCenter
                y: trackArea.travelPadding - 4
                radius: 3
                color: "#050709"
                border.color: "#1e242c"
                border.width: 1

                // Inner slot depth gradient
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 1
                    radius: 2
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: "#020304" }
                        GradientStop { position: 0.5; color: "#0a0d11" }
                        GradientStop { position: 1.0; color: "#151a22" }
                    }
                }
            }

            // Directional cast shadow underneath floating fader cap
            Image {
                width: trackArea.capWidth + 20
                height: trackArea.capHeight + 22
                source: "qrc:/Desk32/resources/ui/fader_cap_shadow.png"
                smooth: true
                mipmap: true
                x: faderCapImage.x - 8
                y: faderCapImage.y - 5
                opacity: faderRoot.bandEnabled ? 0.88 : 0.45
            }

            // Bitmap metallic fader cap
            Image {
                id: faderCapImage
                width: trackArea.capWidth
                height: trackArea.capHeight
                source: "qrc:/Desk32/resources/ui/fader_cap_console.png"
                smooth: true
                mipmap: true
                anchors.horizontalCenter: parent.horizontalCenter
                y: trackArea.travelPadding + (1.0 - faderRoot.normalizedPos()) * trackArea.travelHeight - height * 0.5
                opacity: faderRoot.bandEnabled ? 1.0 : 0.45
            }

            MouseArea {
                id: dragArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.SizeVerCursor

                property real startY: 0
                property double startVal: 0.0
                property bool dragging: false

                function setFromY(mouseY, fine) {
                    var norm = 1.0 - ((mouseY - trackArea.travelPadding) / trackArea.travelHeight)
                    norm = Math.max(0.0, Math.min(1.0, norm))
                    var nextVal = faderRoot.clampAndSnap(faderRoot.normalizedToValue(norm), fine)
                    if (Math.abs(nextVal - faderRoot.value) > 1e-6) {
                        faderRoot.value = nextVal
                        faderRoot.valueEdited(nextVal)
                    }
                }

                onPressed: function(mouse) {
                    dragging = true
                    startY = mouse.y
                    startVal = faderRoot.value
                    // If Shift is held, use relative fine drag; otherwise allow direct or handle drag
                    var capY = faderCapImage.y
                    var onCap = (mouse.y >= capY && mouse.y <= capY + faderCapImage.height)
                    if (!onCap && !(mouse.modifiers & Qt.ShiftModifier)) {
                        setFromY(mouse.y, false)
                        startVal = faderRoot.value
                    }
                }

                onPositionChanged: function(mouse) {
                    if (!dragging) return
                    var fine = !!(mouse.modifiers & Qt.ShiftModifier)
                    if (fine) {
                        var deltaPx = startY - mouse.y
                        var span = faderRoot.maxValue - faderRoot.minValue
                        var deltaVal = (deltaPx / trackArea.travelHeight) * span * 0.25
                        var nextVal = faderRoot.clampAndSnap(startVal + deltaVal, true)
                        if (Math.abs(nextVal - faderRoot.value) > 1e-6) {
                            faderRoot.value = nextVal
                            faderRoot.valueEdited(nextVal)
                        }
                    } else {
                        setFromY(mouse.y, false)
                    }
                }

                onReleased: dragging = false
                onCanceled: dragging = false

                onDoubleClicked: {
                    faderRoot.value = faderRoot.defaultValue
                    faderRoot.valueEdited(faderRoot.defaultValue)
                }

                onWheel: function(wheel) {
                    var fine = !!(wheel.modifiers & Qt.ShiftModifier)
                    var dir = wheel.angleDelta.y > 0 ? 1 : -1
                    var inc = fine ? 0.1 : 0.5
                    var nextVal = faderRoot.clampAndSnap(faderRoot.value + dir * inc, fine)
                    if (Math.abs(nextVal - faderRoot.value) > 1e-6) {
                        faderRoot.value = nextVal
                        faderRoot.valueEdited(nextVal)
                    }
                    wheel.accepted = true
                }
            }
        }

        // 3. Real text frequency label (e.g., 1k)
        Label {
            visible: faderRoot.labelText.length > 0
            Layout.alignment: Qt.AlignHCenter
            text: faderRoot.labelText
            font.family: "Monospace"
            font.pixelSize: 11
            font.bold: true
            color: faderRoot.bandEnabled ? "#C5D0DC" : "#5D6773"
        }

        // 4. Real text dB readout (always real text, never baked into image)
        Rectangle {
            visible: faderRoot.showReadout
            Layout.alignment: Qt.AlignHCenter
            implicitWidth: Math.max(48, dbReadoutLbl.implicitWidth + 8)
            implicitHeight: 18
            radius: 2
            color: "#090b0f"
            border.color: Math.abs(faderRoot.value) > 0.05 && faderRoot.bandEnabled ? "#2f3d4c" : "#1a2028"

            Label {
                id: dbReadoutLbl
                anchors.centerIn: parent
                text: faderRoot.formatDb(faderRoot.value)
                font.family: "Monospace"
                font.pixelSize: 9
                font.bold: Math.abs(faderRoot.value) > 0.05
                color: !faderRoot.bandEnabled
                       ? "#505963"
                       : (Math.abs(faderRoot.value) > 0.05 ? faderRoot.accentColor : "#8894A2")
            }
        }
    }
}
