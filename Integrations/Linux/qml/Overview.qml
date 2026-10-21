import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../Shared/Usage.js" as Usage

// The summary strip above the provider cards: what is tracked, what is closest
// to its limit and when the next window resets.
Frame {
    id: root
    property var summary: desktop.overview
    property bool spending: false
    Layout.fillWidth: true
    padding: 14
    implicitHeight: content.implicitHeight + topPadding + bottomPadding
    background: Rectangle {
        radius: 14
        color: Qt.alpha(palette.highlight, 0.08)
        border.width: 1
        border.color: Qt.alpha(palette.highlight, 0.18)
    }
    ColumnLayout {
        id: content
        width: parent.width
        spacing: 10
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            font.pixelSize: 15
            font.bold: true
            text: root.summary.providers === 0 ? "Nothing to track yet" :
                root.summary.highest >= 95 ? "A provider is close to its limit" :
                root.summary.highest >= 80 ? "Usage is climbing" : "Everything is within limits"
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            opacity: 0.7
            font.pixelSize: 12
            text: root.summary.nextReset
                ? "Next reset: " + (root.summary.nextLabel ? root.summary.nextLabel.toLowerCase() + " " : "") + Usage.resetLabel(root.summary.nextReset, Date.now())
                : "No reset time reported"
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Repeater {
                model: [
                    {label: "Providers", value: root.summary.providers === 0 ? "—" : String(root.summary.providers)},
                    {label: "Highest usage", value: root.summary.providers === 0 ? "—" : root.summary.highest.toFixed(0) + "%"},
                    {label: "Credits", value: root.summary.credits === null ? "—" : root.summary.credits.toFixed(0)},
                    {label: "Local spend", value: root.summary.spend === null ? "—" : Usage.money(root.summary.spend)}]
                delegate: Rectangle {
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.preferredHeight: 54
                    radius: 10
                    color: Qt.alpha(palette.highlight, 0.10)
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 0
                        Label {
                            Layout.fillWidth: true
                            text: modelData.label
                            font.pixelSize: 11
                            opacity: 0.65
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: modelData.value
                            font.pixelSize: 17
                            font.bold: true
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }
    }
}
