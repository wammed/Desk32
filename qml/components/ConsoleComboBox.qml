import QtQuick
import QtQuick.Controls

ComboBox {
    id: control

    implicitHeight: 30
    font.pixelSize: 11

    contentItem: Label {
        leftPadding: 10
        rightPadding: 26
        text: control.displayText
        font: control.font
        color: control.enabled ? "#DCE6F0" : "#677584"
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Text {
        x: control.width - width - 10
        y: (control.height - height) * 0.5
        text: "▾"
        font.pixelSize: 11
        color: control.enabled ? (control.popup.visible ? "#33E3FF" : "#8FA4B8") : "#495462"
    }

    background: Rectangle {
        radius: 4
        color: control.enabled ? "#090D12" : "#0D1117"
        border.color: (control.activeFocus || control.popup.visible) ? "#33E3FF" : "#2E3846"
        border.width: 1
    }

    delegate: ItemDelegate {
        width: ListView.view ? ListView.view.width : control.width
        height: 28
        highlighted: control.highlightedIndex === index
        contentItem: Label {
            text: {
                if (control.textRole && control.textRole.length > 0) {
                    if (modelData && modelData[control.textRole] !== undefined)
                        return String(modelData[control.textRole])
                    if (model && model[control.textRole] !== undefined)
                        return String(model[control.textRole])
                }
                return modelData !== undefined ? String(modelData) : ""
            }
            color: highlighted ? "#33E3FF" : "#DCE6F0"
            font: control.font
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            color: highlighted ? "#152534" : "#0D1117"
        }
    }

    popup: Popup {
        y: control.height + 2
        width: control.width
        implicitHeight: Math.min(320, listContent.contentHeight + padding * 2)
        padding: 2

        contentItem: ListView {
            id: listContent
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }

        background: Rectangle {
            radius: 4
            color: "#0D1117"
            border.color: "#334050"
            border.width: 1
        }
    }
}
