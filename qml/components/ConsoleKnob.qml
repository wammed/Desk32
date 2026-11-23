import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: knobRoot

    // Public API
    property string labelText: "FREQUENCY"
    property double value: 45.2
    property double minValue: 20.0
    property double maxValue: 20000.0
    property double defaultValue: 1000.0
    property bool logarithmic: true
    property bool bipolar: false        // true for Gain (-18..+18 dB, arc starts at 0)
    property double bipolarOrigin: 0.0
    property string unitSuffix: " Hz"
    property int decimals: 1
    property color arcColor: "#00E5FF"
    property real knobDiameter: 112

    signal valueEdited(double newValue)

    readonly property bool isDragging: knobMouse.pressed
    readonly property bool isCompactKnob: knobDiameter < 70
    readonly property int dialPad: isCompactKnob ? 18 : 26
    readonly property int readoutHeight: isCompactKnob ? 20 : 26

    implicitWidth: Math.max(isCompactKnob ? 78 : 112, knobDiameter + dialPad)
    implicitHeight: (isCompactKnob ? 14 : 16) + (knobDiameter + dialPad) + readoutHeight + (isCompactKnob ? 6 : 12)

    function valueToNorm(v) {
        var clamped = Math.max(minValue, Math.min(maxValue, v))
        if (logarithmic && minValue > 0 && maxValue > minValue) {
            return (Math.log(clamped) - Math.log(minValue)) / (Math.log(maxValue) - Math.log(minValue))
        }
        if (maxValue <= minValue) return 0.0
        return (clamped - minValue) / (maxValue - minValue)
    }

    function normToValue(norm) {
        var n = Math.max(0.0, Math.min(1.0, norm))
        if (logarithmic && minValue > 0 && maxValue > minValue) {
            return Math.exp(Math.log(minValue) + n * (Math.log(maxValue) - Math.log(minValue)))
        }
        return minValue + n * (maxValue - minValue)
    }

    function formatDisplay(v) {
        if (bipolar) {
            var clean = Math.abs(v) < 0.049 ? 0.0 : v
            return (clean > 0 ? "+" : "") + clean.toFixed(decimals) + unitSuffix
        }
        if (unitSuffix === " Hz" && v >= 1000.0) {
            return (v / 1000.0).toFixed(2) + " kHz"
        }
        return v.toFixed(decimals) + unitSuffix
    }

    readonly property real normVal: valueToNorm(value)
    readonly property real pointerAngleDeg: -135.0 + normVal * 270.0

    onValueChanged: arcCanvas.requestPaint()
    onArcColorChanged: arcCanvas.requestPaint()
    onBipolarChanged: arcCanvas.requestPaint()

    ColumnLayout {
        anchors.fill: parent
        spacing: knobRoot.isCompactKnob ? 3 : 6

        // 1. Real text parameter label
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: knobRoot.labelText
            font.pixelSize: knobRoot.isCompactKnob ? 10 : 11
            font.bold: true
            font.letterSpacing: 0.6
            color: "#D0DAE4"
        }

        // 2. Knob well + dynamic glowing arc + hybrid metallic bitmap layers
        Item {
            id: dialContainer
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: knobRoot.knobDiameter + knobRoot.dialPad
            Layout.preferredHeight: knobRoot.knobDiameter + knobRoot.dialPad

            // Dynamic LED ring / value arc + outer ticks (Scene Graph Canvas, never baked)
            Canvas {
                id: arcCanvas
                anchors.fill: parent
                antialiasing: true
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
                    var cy = h * 0.5
                    var ringR = knobRoot.knobDiameter * 0.425
                    // Convert knob rotation (-135..+135 around top=-pi/2) to Canvas radians
                    var startRad = (-135.0 - 90.0) * Math.PI / 180.0
                    var endRad = (135.0 - 90.0) * Math.PI / 180.0
                    var curRad = (knobRoot.pointerAngleDeg - 90.0) * Math.PI / 180.0

                    // Outer perimeter ticks
                    var tickCount = 21
                    for (var i = 0; i < tickCount; ++i) {
                        var tn = i / (tickCount - 1)
                        var ta = startRad + tn * (endRad - startRad)
                        var isMajor = (i === 0 || i === 10 || i === 20)
                        var rIn = ringR + 6
                        var rOut = ringR + (isMajor ? 11 : 8)
                        ctx.beginPath()
                        ctx.strokeStyle = (i === 10 && knobRoot.bipolar) ? "#FFFFFF" : (isMajor ? "#E6EEF6" : "#A2B0BE")
                        ctx.lineWidth = isMajor ? 1.6 : 1.1
                        ctx.moveTo(cx + Math.cos(ta) * rIn, cy + Math.sin(ta) * rIn)
                        ctx.lineTo(cx + Math.cos(ta) * rOut, cy + Math.sin(ta) * rOut)
                        ctx.stroke()
                    }

                    // Recessed unlit arc track
                    ctx.beginPath()
                    ctx.strokeStyle = "#141920"
                    ctx.lineWidth = 4.5
                    ctx.lineCap = "round"
                    ctx.arc(cx, cy, ringR, startRad, endRad, false)
                    ctx.stroke()

                    // Active glowing arc
                    var originRad = knobRoot.bipolar
                        ? ((-135.0 + knobRoot.valueToNorm(knobRoot.bipolarOrigin) * 270.0) - 90.0) * Math.PI / 180.0
                        : startRad

                    if (Math.abs(curRad - originRad) > 0.005) {
                        var a0 = Math.min(originRad, curRad)
                        var a1 = Math.max(originRad, curRad)

                        // Soft outer glow pass
                        ctx.beginPath()
                        ctx.strokeStyle = Qt.rgba(knobRoot.arcColor.r, knobRoot.arcColor.g, knobRoot.arcColor.b, 0.30)
                        ctx.lineWidth = 8.0
                        ctx.lineCap = "round"
                        ctx.arc(cx, cy, ringR, a0, a1, false)
                        ctx.stroke()

                        // Core bright arc
                        ctx.beginPath()
                        ctx.strokeStyle = knobRoot.arcColor
                        ctx.lineWidth = 3.5
                        ctx.lineCap = "round"
                        ctx.arc(cx, cy, ringR, a0, a1, false)
                        ctx.stroke()
                    }
                }
            }

            // Layer 1: Machined well & directional cast shadow
            Image {
                anchors.centerIn: parent
                width: knobRoot.knobDiameter + 12
                height: knobRoot.knobDiameter + 12
                source: "qrc:/Desk32/resources/ui/knob_metal_base.png"
                smooth: true
                mipmap: true
            }

            // Layer 2: Fixed-lighting anisotropic brushed aluminum cap (light source never spins)
            Image {
                anchors.centerIn: parent
                width: knobRoot.knobDiameter
                height: knobRoot.knobDiameter
                source: "qrc:/Desk32/resources/ui/knob_metal_cap.png"
                smooth: true
                mipmap: true
            }

            // Layer 3: Rotating engraved pointer notch
            Image {
                anchors.centerIn: parent
                width: knobRoot.knobDiameter
                height: knobRoot.knobDiameter
                source: "qrc:/Desk32/resources/ui/knob_pointer.png"
                smooth: true
                mipmap: true
                rotation: knobRoot.pointerAngleDeg
            }

            MouseArea {
                id: knobMouse
                anchors.fill: parent
                cursorShape: Qt.SizeVerCursor
                property real startY: 0
                property real startNorm: 0

                onPressed: function(mouse) {
                    startY = mouse.y
                    startNorm = knobRoot.valueToNorm(knobRoot.value)
                }

                onPositionChanged: function(mouse) {
                    if (!(mouse.buttons & Qt.LeftButton)) return
                    var fine = !!(mouse.modifiers & Qt.ShiftModifier)
                    var sensitivity = fine ? 640.0 : 180.0
                    var nextNorm = Math.max(0.0, Math.min(1.0, startNorm + (startY - mouse.y) / sensitivity))
                    var nextVal = knobRoot.normToValue(nextNorm)
                    if (!fine && knobRoot.bipolar && Math.abs(nextVal - knobRoot.bipolarOrigin) < 0.08) {
                        nextVal = knobRoot.bipolarOrigin
                    }
                    knobRoot.value = nextVal
                    knobRoot.valueEdited(nextVal)
                }

                onDoubleClicked: {
                    knobRoot.value = knobRoot.defaultValue
                    knobRoot.valueEdited(knobRoot.defaultValue)
                }

                onWheel: function(wheel) {
                    var fine = !!(wheel.modifiers & Qt.ShiftModifier)
                    var stepNorm = fine ? 0.004 : 0.02
                    var dir = wheel.angleDelta.y > 0 ? 1.0 : -1.0
                    var nextNorm = Math.max(0.0, Math.min(1.0, knobRoot.valueToNorm(knobRoot.value) + dir * stepNorm))
                    var nextVal = knobRoot.normToValue(nextNorm)
                    knobRoot.value = nextVal
                    knobRoot.valueEdited(nextVal)
                    wheel.accepted = true
                }
            }
        }

        // 3. Recessed digital readout window (real text, crisp & editable/hoverable)
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            implicitWidth: Math.max(knobRoot.isCompactKnob ? 72 : 102, readoutText.implicitWidth + (knobRoot.isCompactKnob ? 12 : 18))
            implicitHeight: knobRoot.readoutHeight
            radius: 3
            color: "#090c10"
            border.color: "#2b3440"
            border.width: 1

            Label {
                id: readoutText
                anchors.centerIn: parent
                text: knobRoot.formatDisplay(knobRoot.value)
                font.family: "Monospace"
                font.pixelSize: knobRoot.isCompactKnob ? 10 : 12
                font.bold: true
                color: "#EBF2F7"
            }
        }
    }
}
