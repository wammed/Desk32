import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Desk32.Core

BrushedMetalPanel {
    id: cardRoot

    property string deckId: "A"
    property var deckState: EngineController.getDeckState(deckId)

    signal loadFileRequested(string deckId)
    signal startFolderRequested(string deckId)

    readonly property bool isActive: EngineController.activeDeckId === deckId
    readonly property bool hasTrack: !!(deckState && deckState.path && deckState.path.length > 0)
    readonly property bool isPlaying: !!(deckState && deckState.state === 1)
    readonly property bool isPaused: !!(deckState && deckState.state === 2)
    readonly property bool isLocked: !!(deckState && deckState.isLocked)
    readonly property bool isErrorStopped: !!(deckState && deckState.hasError)
    readonly property bool folderActive: !!(deckState && deckState.folderActive)
    readonly property string folderDisplayName: {
        var fp = (deckState && deckState.folderPath) ? deckState.folderPath : ""
        if (!fp || fp.length === 0) return ""
        var parts = fp.replace(/\/+$/, "").split("/")
        return parts.length > 0 ? parts[parts.length - 1] : fp
    }
    readonly property int trackOrigin: (deckState && deckState.trackOrigin !== undefined) ? deckState.trackOrigin : 0
    property string endedSessionLabel: ""
    readonly property bool canCopyToWorkspace: WorkspaceManager.hasOpenWorkspace
                                               && hasTrack
                                               && !!(deckState && deckState.canCopyToWorkspace !== false)
                                               && !isPathInOpenWs()

    function isPathInOpenWs() {
        if (!WorkspaceManager.hasOpenWorkspace || !hasTrack) return false
        var wsRoot = WorkspaceManager.openWorkspacePath
        if (!wsRoot || wsRoot.length === 0) return false
        return deckState.path.indexOf(wsRoot + "/") === 0
    }

    function refreshState() {
        deckState = EngineController.getDeckState(deckId)
        syncSeekSlider()
    }

    function syncSeekSlider() {
        if (typeof deckSeekSlider === "undefined" || deckSeekSlider.pressed) return
        var pos = (deckState && deckState.playhead !== undefined) ? deckState.playhead : 0
        deckSeekSlider.value = pos
    }

    Component.onCompleted: syncSeekSlider()

    function formatMsTime(frames, sampleRate) {
        var sr = (sampleRate && sampleRate > 0) ? sampleRate : 48000.0
        var totalSec = Math.max(0.0, frames / sr)
        var mins = Math.floor(totalSec / 60)
        var secs = Math.floor(totalSec % 60)
        var ms = Math.floor((totalSec - Math.floor(totalSec)) * 1000)
        return (mins < 10 ? "0" : "") + mins + ":"
             + (secs < 10 ? "0" : "") + secs + "."
             + (ms < 100 ? (ms < 10 ? "00" : "0") : "") + ms
    }

    function statusLabelText() {
        if (isErrorStopped) return qsTr("ERR")
        if (isPlaying) return qsTr("PLAY")
        if (isPaused) return qsTr("PAUSE")
        if (hasTrack) return isActive ? qsTr("ROUTED") : qsTr("STBY")
        return qsTr("EMPTY")
    }

    function statusColor() {
        if (isErrorStopped) return "#FF5252"
        if (isPlaying) return "#00E676"
        if (isPaused) return "#F5B841"
        if (isActive) return "#00E5FF"
        return "#6D7B8A"
    }

    Layout.fillWidth: true
    Layout.fillHeight: true
    baseTint: isActive ? "#172029" : "#12161c"
    borderColor: isErrorStopped ? "#FF5252" : (isActive ? "#2CD6F2" : (folderActive ? "#C88D22" : "#2c3440"))
    accentColor: isActive ? "#402CD6F2" : (folderActive ? "#30F5B841" : "transparent")
    cornerRadius: 6
    brushedOpacity: 0.30

    Connections {
        target: EngineController
        function onDeckStateChanged(id) {
            if (id === cardRoot.deckId || id === "" || id === "ALL") {
                cardRoot.refreshState()
                if (cardRoot.folderActive) {
                    cardRoot.endedSessionLabel = ""
                    endedSessionTimer.stop()
                }
            }
        }
        function onActiveDeckChanged() {
            cardRoot.refreshState()
        }
        function onFolderSessionEnded(id, reason, folderPath, trackIndex, totalTracks) {
            if (id === cardRoot.deckId && reason !== "new_folder_session" && reason !== "app_shutdown") {
                cardRoot.endedSessionLabel = (reason === "track_replaced" || reason === "deck_locked")
                                             ? qsTr("DISCARDED")
                                             : qsTr("ENDED")
                endedSessionTimer.restart()
            }
        }
    }

    Connections {
        target: WorkspaceManager
        function onWorkspaceStateChanged() { cardRoot.refreshState() }
        function onWorkspaceContentsChanged() { cardRoot.refreshState() }
    }

    Timer {
        id: endedSessionTimer
        interval: 8000
        repeat: false
        onTriggered: cardRoot.endedSessionLabel = ""
    }

    Timer {
        interval: 60
        running: cardRoot.isPlaying
        repeat: true
        onTriggered: cardRoot.refreshState()
    }

    // Background click switches active deck
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        z: 0
        onClicked: {
            if (EngineController.activeDeckId !== cardRoot.deckId) {
                EngineController.switchDeck(cardRoot.deckId, EngineController.switchingMode)
            }
        }
    }

    // Compact 3-Column Hardware Deck Card: Left Transport | Center OLED & Actions | Right LEVEL TRIM Knob
    RowLayout {
        anchors.fill: parent
        anchors.margins: 7
        spacing: 7
        z: 1

        // -----------------------------------------------------------------
        // Left Column: DECK Header + State + Folder Steppers / Transport Keys
        // -----------------------------------------------------------------
        ColumnLayout {
            Layout.preferredWidth: 124
            Layout.fillHeight: true
            spacing: 4

            RowLayout {
                Layout.fillWidth: true
                spacing: 4

                Label {
                    text: "DECK " + cardRoot.deckId
                    font.pixelSize: 15
                    font.bold: true
                    color: cardRoot.isActive ? "#38DEFA" : "#AEB9C5"
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    implicitWidth: stLbl.implicitWidth + 8
                    Layout.preferredHeight: 16
                    height: 16
                    radius: 2
                    color: "#0b0f14"
                    border.color: cardRoot.statusColor()
                    Label {
                        id: stLbl
                        anchors.centerIn: parent
                        text: cardRoot.statusLabelText()
                        font.family: "Monospace"
                        font.pixelSize: 8
                        font.bold: true
                        color: cardRoot.statusColor()
                    }
                }
            }

            // Lock + Folder Next/Prev + Folder Step strip
            RowLayout {
                Layout.fillWidth: true
                spacing: 3

                ConsoleButton {
                    text: "◀"
                    fontSize: 9
                    leftPadding: 3; rightPadding: 3
                    implicitWidth: 26; implicitHeight: 22
                    enabled: cardRoot.folderActive
                    toolTipText: qsTr("Folder Session Previous Track")
                    onClicked: EngineController.retreatFolderPrevious(cardRoot.deckId)
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 22
                    height: 22
                    radius: 3
                    color: "#090c10"
                    border.color: cardRoot.folderActive ? "#1F6E80" : (cardRoot.endedSessionLabel.length > 0 ? "#A67C23" : "#2b323e")
                    Label {
                        anchors.centerIn: parent
                        text: cardRoot.folderActive
                              ? "%1/%2".arg((cardRoot.deckState ? (cardRoot.deckState.folderCurrentIndex || 0) : 0) + 1)
                                           .arg(cardRoot.deckState ? (cardRoot.deckState.folderTotalTracks || 0) : 0)
                              : (cardRoot.endedSessionLabel.length > 0 ? cardRoot.endedSessionLabel : "=")
                        font.family: "Monospace"
                        font.pixelSize: cardRoot.endedSessionLabel.length > 0 && !cardRoot.folderActive ? 7 : 9
                        font.bold: cardRoot.folderActive || cardRoot.endedSessionLabel.length > 0
                        color: cardRoot.folderActive ? "#00E5FF" : (cardRoot.endedSessionLabel.length > 0 ? "#F5B841" : "#677584")
                    }
                }

                ConsoleButton {
                    text: "▶"
                    fontSize: 9
                    leftPadding: 3; rightPadding: 3
                    implicitWidth: 26; implicitHeight: 22
                    enabled: cardRoot.folderActive
                    toolTipText: qsTr("Folder Session Next Track")
                    onClicked: EngineController.advanceFolderNext(cardRoot.deckId)
                }

                ConsoleButton {
                    text: cardRoot.isLocked ? "🔒" : "🔓"
                    fontSize: 9
                    leftPadding: 2; rightPadding: 2
                    implicitWidth: 24; implicitHeight: 22
                    highlighted: cardRoot.isLocked
                    accentColor: "#F5B841"
                    toolTipText: cardRoot.isLocked ? qsTr("Deck Locked") : qsTr("Lock Deck Context")
                    onClicked: EngineController.setDeckLocked(cardRoot.deckId, !cardRoot.isLocked)
                }
            }

            // Prominent Folder Session Status Pill in Left Column
            Rectangle {
                visible: cardRoot.folderActive
                Layout.fillWidth: true
                Layout.preferredHeight: 18
                height: 18
                radius: 3
                color: "#241908"
                border.color: "#F5B841"
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 5; anchors.rightMargin: 5
                    spacing: 4
                    Rectangle {
                        width: 6; height: 6; radius: 3
                        color: "#F5B841"
                    }
                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: qsTr("FOLDER SESSION")
                        font.family: "Monospace"
                        font.pixelSize: 8
                        font.bold: true
                        color: "#FFD573"
                    }
                }
            }

            Item { Layout.fillHeight: true }

            // Iconic Hardware Green / Amber / Burnt-Orange Transport Buttons
            RowLayout {
                Layout.fillWidth: true
                spacing: 4

                ConsoleButton {
                    Layout.fillWidth: true
                    implicitHeight: 30
                    text: "▶"
                    fontSize: 13
                    anodizedFill: true
                    anodizedTop: cardRoot.hasTrack ? (cardRoot.isPlaying ? "#28B85C" : "#1D6C3A") : "#163523"
                    anodizedBottom: cardRoot.hasTrack ? (cardRoot.isPlaying ? "#146B31" : "#0F3D20") : "#0D2015"
                    anodizedText: cardRoot.hasTrack ? "#EFFFF4" : "#3E7A56"
                    highlighted: cardRoot.isPlaying
                    accentColor: "#00E676"
                    enabled: cardRoot.hasTrack
                    toolTipText: qsTr("Play Deck %1").arg(cardRoot.deckId)
                    onClicked: {
                        if (EngineController.activeDeckId !== cardRoot.deckId) {
                            EngineController.switchDeck(cardRoot.deckId, EngineController.switchingMode)
                        }
                        EngineController.play(cardRoot.deckId)
                    }
                }

                ConsoleButton {
                    Layout.fillWidth: true
                    implicitHeight: 30
                    text: "⏸"
                    fontSize: 12
                    anodizedFill: true
                    anodizedTop: cardRoot.hasTrack ? (cardRoot.isPaused ? "#D89A2A" : "#6C501C") : "#362B16"
                    anodizedBottom: cardRoot.hasTrack ? (cardRoot.isPaused ? "#7E5612" : "#38290C") : "#1F180B"
                    anodizedText: cardRoot.hasTrack ? "#FFF8E6" : "#7D6940"
                    highlighted: cardRoot.isPaused
                    accentColor: "#F5B841"
                    enabled: cardRoot.hasTrack
                    toolTipText: qsTr("Pause Deck %1").arg(cardRoot.deckId)
                    onClicked: EngineController.pause(cardRoot.deckId)
                }

                ConsoleButton {
                    Layout.fillWidth: true
                    implicitHeight: 30
                    text: "⏹"
                    fontSize: 12
                    anodizedFill: true
                    anodizedTop: cardRoot.hasTrack ? "#B4551E" : "#402414"
                    anodizedBottom: cardRoot.hasTrack ? "#662C0C" : "#24140B"
                    anodizedText: cardRoot.hasTrack ? "#FFF2EB" : "#805338"
                    enabled: cardRoot.hasTrack
                    toolTipText: qsTr("Stop Deck %1").arg(cardRoot.deckId)
                    onClicked: EngineController.stop(cardRoot.deckId)
                }
            }
        }

        // -----------------------------------------------------------------
        // Right Column: Recessed OLED Track Screen + Compact Action / Nudge Strip
        // -----------------------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 5

            // Recessed Dark OLED Window (no artwork)
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 4
                color: "#06090d"
                border.color: cardRoot.isActive && cardRoot.hasTrack ? "#1F6E80" : "#272e39"
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 7
                    spacing: 3

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Label {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: cardRoot.hasTrack
                                  ? ((cardRoot.deckState.title && cardRoot.deckState.title.length > 0)
                                     ? cardRoot.deckState.title
                                     : cardRoot.deckState.path.split('/').pop())
                                  : qsTr("No track loaded")
                            font.pixelSize: 11
                            font.bold: cardRoot.hasTrack
                            color: cardRoot.hasTrack ? "#E9F3FA" : "#566270"
                            elide: Text.ElideMiddle
                        }

                        Label {
                            visible: cardRoot.hasTrack
                            text: {
                                var c = (cardRoot.deckState && cardRoot.deckState.codec) ? cardRoot.deckState.codec : "PCM"
                                var sr = ((cardRoot.deckState && cardRoot.deckState.sampleRate) ? (cardRoot.deckState.sampleRate / 1000.0).toFixed(1).replace(/\.0$/, "") : "48") + "k"
                                return c + " " + sr
                            }
                            font.family: "Monospace"
                            font.pixelSize: 9
                            color: "#52CFEA"
                        }
                    }

                    // Active Folder Session Banner inside OLED (Folder position + folder name + read-ahead tooltip)
                    Rectangle {
                        id: oledFolderBanner
                        visible: cardRoot.folderActive
                        Layout.fillWidth: true
                        Layout.preferredHeight: 18
                        height: 18
                        radius: 3
                        color: "#241908"
                        border.color: "#F5B841"
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 6; anchors.rightMargin: 6
                            spacing: 5
                            Rectangle {
                                width: 6; height: 6; radius: 3
                                color: "#F5B841"
                            }
                            Label {
                                text: qsTr("FOLDER %1/%2")
                                      .arg((cardRoot.deckState ? (cardRoot.deckState.folderCurrentIndex || 0) : 0) + 1)
                                      .arg(cardRoot.deckState ? (cardRoot.deckState.folderTotalTracks || 0) : 0)
                                font.family: "Monospace"
                                font.pixelSize: 8
                                font.bold: true
                                color: "#FFD573"
                            }
                            Label {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                elide: Text.ElideMiddle
                                text: cardRoot.folderDisplayName
                                font.family: "Monospace"
                                font.pixelSize: 8
                                color: "#F0D699"
                            }
                        }
                        MouseArea {
                            id: oledFolderHover
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.NoButton
                        }
                        ToolTip.visible: oledFolderHover.containsMouse
                        ToolTip.text: {
                            var cur = (cardRoot.deckState ? (cardRoot.deckState.folderCurrentIndex || 0) : 0) + 1
                            var tot = cardRoot.deckState ? (cardRoot.deckState.folderTotalTracks || 0) : 0
                            var fp = (cardRoot.deckState && cardRoot.deckState.folderPath) ? cardRoot.deckState.folderPath : ""
                            var next = (cardRoot.deckState && cardRoot.deckState.readAheadFile) ? cardRoot.deckState.readAheadFile : ""
                            var msg = qsTr("Folder Session active: %1 (%2/%3)").arg(fp).arg(cur).arg(tot)
                            if (next.length > 0) {
                                msg += "\n" + qsTr("Next: %1").arg(next)
                            }
                            return msg
                        }
                    }

                    // High-precision seek bar + ms timecode
                    Slider {
                        id: deckSeekSlider
                        Layout.fillWidth: true
                        Layout.preferredHeight: 16
                        enabled: cardRoot.hasTrack && (cardRoot.deckState && cardRoot.deckState.totalFrames > 0)
                        from: 0.0
                        to: (cardRoot.deckState && cardRoot.deckState.totalFrames > 0) ? cardRoot.deckState.totalFrames : 1.0
                        property bool pendingSeek: false

                        onMoved: {
                            if (pressed) {
                                pendingSeek = true
                            } else {
                                pendingSeek = false
                                EngineController.seek(cardRoot.deckId, Math.round(value))
                                cardRoot.refreshState()
                            }
                        }
                        onPressedChanged: {
                            if (!pressed && pendingSeek) {
                                pendingSeek = false
                                EngineController.seek(cardRoot.deckId, Math.round(value))
                                cardRoot.refreshState()
                            } else if (!pressed) {
                                cardRoot.syncSeekSlider()
                            }
                        }

                        background: Rectangle {
                            x: deckSeekSlider.leftPadding
                            y: deckSeekSlider.topPadding + deckSeekSlider.availableHeight / 2 - height / 2
                            width: deckSeekSlider.availableWidth
                            height: 4
                            radius: 2
                            color: "#141b24"
                            Rectangle {
                                width: deckSeekSlider.visualPosition * parent.width
                                height: parent.height
                                radius: 2
                                color: cardRoot.hasTrack ? "#00E5FF" : "#2d3642"
                            }
                        }

                        handle: Rectangle {
                            x: deckSeekSlider.leftPadding + deckSeekSlider.visualPosition * (deckSeekSlider.availableWidth - width)
                            y: deckSeekSlider.topPadding + deckSeekSlider.availableHeight / 2 - height / 2
                            width: 10
                            height: 10
                            radius: 5
                            color: deckSeekSlider.enabled ? "#E9F4FA" : "#404954"
                            border.color: "#00E5FF"
                            border.width: deckSeekSlider.enabled ? 1 : 0
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Label {
                            text: cardRoot.hasTrack
                                  ? cardRoot.formatMsTime(
                                        deckSeekSlider.pressed ? Math.round(deckSeekSlider.value) : (cardRoot.deckState.playhead || 0),
                                        cardRoot.deckState.sampleRate)
                                    + " / " + cardRoot.formatMsTime(cardRoot.deckState.totalFrames || 0, cardRoot.deckState.sampleRate)
                                  : "00:00.000 / 00:00.000"
                            font.family: "Monospace"
                            font.pixelSize: 9
                            color: cardRoot.isActive ? "#3CD8F5" : "#8090A0"
                        }
                        Item { Layout.fillWidth: true }
                        Rectangle {
                            id: originBadge
                            visible: cardRoot.hasTrack
                            implicitWidth: originBadgeLbl.implicitWidth + 8
                            Layout.preferredHeight: 15
                            height: 15
                            radius: 2
                            color: cardRoot.trackOrigin === 1 ? "#092418"
                                   : (cardRoot.trackOrigin === 2 ? "#2A1F0A" : "#141A24")
                            border.color: cardRoot.trackOrigin === 1 ? "#00E676"
                                          : (cardRoot.trackOrigin === 2 ? "#F5B841" : "#6C8299")
                            border.width: 1
                            Label {
                                id: originBadgeLbl
                                anchors.centerIn: parent
                                text: cardRoot.trackOrigin === 1 ? qsTr("WS: APP")
                                      : (cardRoot.trackOrigin === 2 ? qsTr("WS: EXT") : qsTr("OUTSIDE WS"))
                                font.family: "Monospace"
                                font.pixelSize: 8
                                font.bold: true
                                color: cardRoot.trackOrigin === 1 ? "#69F0AE"
                                       : (cardRoot.trackOrigin === 2 ? "#FFD573" : "#B6C7D9")
                            }
                            MouseArea {
                                id: originBadgeHover
                                anchors.fill: parent
                                hoverEnabled: true
                                acceptedButtons: Qt.NoButton
                            }
                            ToolTip.visible: originBadgeHover.containsMouse
                            ToolTip.text: cardRoot.trackOrigin === 1
                                          ? qsTr("Workspace (App-Copied): Copied into the open workspace by Desk32")
                                          : (cardRoot.trackOrigin === 2
                                             ? qsTr("Workspace (External): Copied or moved into the workspace externally")
                                             : qsTr("Outside Workspace: Located outside the open workspace directory (Read-Only)"))
                        }
                        Label {
                            visible: cardRoot.isErrorStopped
                            text: qsTr("ERROR")
                            font.family: "Monospace"
                            font.pixelSize: 8
                            font.bold: true
                            color: "#FF5252"
                        }
                    }
                }
            }

            // Bottom Action Row inside Center Column: Load | Folder | [->WS] | Nudge -10/+10
            RowLayout {
                Layout.fillWidth: true
                spacing: 3

                ConsoleButton {
                    text: qsTr("Load")
                    fontSize: 9
                    leftPadding: 4; rightPadding: 4
                    implicitWidth: 38; implicitHeight: 22
                    enabled: !cardRoot.isLocked
                    toolTipText: qsTr("Load audio file into Deck %1").arg(cardRoot.deckId)
                    onClicked: cardRoot.loadFileRequested(cardRoot.deckId)
                }

                ConsoleButton {
                    text: qsTr("Folder")
                    fontSize: 9
                    leftPadding: 4; rightPadding: 4
                    implicitWidth: 42; implicitHeight: 22
                    enabled: !cardRoot.isLocked
                    highlighted: cardRoot.folderActive
                    accentColor: "#F5B841"
                    toolTipText: cardRoot.folderActive
                                 ? qsTr("Folder Session active (%1/%2) — click to restart from track 1 (re-scans folder)")
                                   .arg((cardRoot.deckState ? (cardRoot.deckState.folderCurrentIndex || 0) : 0) + 1)
                                   .arg(cardRoot.deckState ? (cardRoot.deckState.folderTotalTracks || 0) : 0)
                                 : qsTr("Start Folder Session from first track")
                    onClicked: cardRoot.startFolderRequested(cardRoot.deckId)
                }

                ConsoleButton {
                    visible: cardRoot.canCopyToWorkspace
                    text: "→WS"
                    fontSize: 8
                    leftPadding: 3; rightPadding: 3
                    implicitWidth: 34; implicitHeight: 22
                    accentColor: "#00E676"
                    toolTipText: qsTr("Copy track into open Workspace")
                    onClicked: EngineController.copyDeckTrackToWorkspace(cardRoot.deckId)
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    implicitWidth: nudgeLbl.implicitWidth + 6
                    Layout.preferredHeight: 22
                    height: 22
                    radius: 3
                    color: "#090c10"
                    border.color: "#29303b"
                    Label {
                        id: nudgeLbl
                        anchors.centerIn: parent
                        property int offFrames: cardRoot.deckState ? (cardRoot.deckState.offsetFrames || 0) : 0
                        property double sr: (cardRoot.deckState && cardRoot.deckState.sampleRate > 0) ? cardRoot.deckState.sampleRate : 48000.0
                        text: ((offFrames * 1000.0) / sr).toFixed(1) + "ms"
                        font.family: "Monospace"
                        font.pixelSize: 8
                        color: offFrames !== 0 ? "#F5B841" : "#7E8D9B"
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onDoubleClicked: EngineController.setDeckOffsetFrames(cardRoot.deckId, 0)
                    }
                }

                ConsoleButton {
                    text: "-10"
                    fontSize: 8
                    leftPadding: 3; rightPadding: 3
                    implicitWidth: 26; implicitHeight: 22
                    toolTipText: qsTr("Compare Nudge -10 ms (double-click ms box to reset)")
                    onClicked: {
                        var sr = (cardRoot.deckState && cardRoot.deckState.sampleRate > 0) ? cardRoot.deckState.sampleRate : 48000.0
                        var cur = cardRoot.deckState ? (cardRoot.deckState.offsetFrames || 0) : 0
                        EngineController.setDeckOffsetFrames(cardRoot.deckId, cur - Math.round(sr * 0.01))
                    }
                }

                ConsoleButton {
                    text: "+10"
                    fontSize: 8
                    leftPadding: 3; rightPadding: 3
                    implicitWidth: 26; implicitHeight: 22
                    toolTipText: qsTr("Compare Nudge +10 ms (double-click ms box to reset)")
                    onClicked: {
                        var sr = (cardRoot.deckState && cardRoot.deckState.sampleRate > 0) ? cardRoot.deckState.sampleRate : 48000.0
                        var cur = cardRoot.deckState ? (cardRoot.deckState.offsetFrames || 0) : 0
                        EngineController.setDeckOffsetFrames(cardRoot.deckId, cur + Math.round(sr * 0.01))
                    }
                }
            }
        }

        // -----------------------------------------------------------------
        // Right Column: Per-Deck Monitor LEVEL TRIM Knob (-24 .. +24 dB)
        // -----------------------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: false
            Layout.preferredWidth: 78
            Layout.fillHeight: true

            Item { Layout.fillHeight: true }

            ConsoleKnob {
                id: deckLevelTrimKnob
                Layout.alignment: Qt.AlignHCenter
                labelText: qsTr("LEVEL TRIM")
                minValue: -24.0
                maxValue: 24.0
                defaultValue: 0.0
                logarithmic: false
                bipolar: true
                unitSuffix: " dB"
                decimals: 1
                knobDiameter: 44
                arcColor: cardRoot.isActive ? "#38DEFA" : "#E8BA6B"
                enabled: !EngineController.directModeEnabled
                Component.onCompleted: value = (cardRoot.deckState && cardRoot.deckState.levelMatchDb !== undefined)
                                               ? cardRoot.deckState.levelMatchDb : 0.0
                onValueEdited: function(v) {
                    EngineController.setDeckLevelMatchDb(cardRoot.deckId, v)
                }
                Connections {
                    target: cardRoot
                    function onDeckStateChanged() {
                        if (deckLevelTrimKnob.isDragging) return
                        deckLevelTrimKnob.value = (cardRoot.deckState && cardRoot.deckState.levelMatchDb !== undefined)
                                                  ? cardRoot.deckState.levelMatchDb : 0.0
                    }
                }
            }

            Item { Layout.fillHeight: true }
        }
    }
}
