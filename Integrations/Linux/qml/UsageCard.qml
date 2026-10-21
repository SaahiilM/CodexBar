import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../Shared/Usage.js" as Usage

Frame {
    id: root
    required property var entry
    // A stable colour per provider keeps the dashboard readable when several
    // providers are listed side by side.
    property color accent: Usage.accent(root.entry.provider)
    padding: 16
    Layout.fillWidth: true
    implicitHeight: content.implicitHeight + topPadding + bottomPadding
    background: Rectangle {
        radius: 14
        color: Qt.alpha(root.accent, 0.06)
        border.width: 1
        border.color: Qt.alpha(root.accent, 0.20)
    }
    ColumnLayout {
        id: content
        width: parent.width
        spacing: 14
        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Image {
                source: "image://provider-icon/" + root.entry.provider + "?color=" + root.accent.toString().substring(1)
                sourceSize.width: 28; sourceSize.height: 28
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
                fillMode: Image.PreserveAspectFit
                visible: root.entry.provider !== ""
            }
            Label {
                text: Usage.providerName(root.entry.provider)
                font.bold: true; font.pixelSize: 18
                Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText
            }
            Rectangle {
                visible: root.entry.plan !== "" && root.entry.plan !== undefined
                radius: 9
                Layout.preferredHeight: 18
                Layout.preferredWidth: planLabel.implicitWidth + 14
                color: Qt.alpha(root.accent, 0.18)
                Label {
                    id: planLabel
                    anchors.centerIn: parent
                    text: root.entry.plan || ""
                    font.pixelSize: 11; font.bold: true
                    color: root.accent
                }
            }
        }
        Label {
            text: root.entry.accountLabel || ""
            visible: text !== ""
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            opacity: 0.75
            font.pixelSize: 12
        }
        Label {
            text: root.entry.status || ""
            visible: text !== ""
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            opacity: 0.7
            font.pixelSize: 12
        }
        Label {
            text: root.entry.error || ""
            visible: text !== ""
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: "#d97732"
        }
        Repeater {
            model: root.entry.windows
            ColumnLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: 6
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: modelData.label
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                        font.pixelSize: 13
                        font.bold: true
                        opacity: 0.85
                    }
                    Label {
                        text: Usage.quotaValue(modelData.remaining, desktop.settings.quotaDisplay) + "% " + (desktop.settings.quotaDisplay === "used" ? "used" : "left")
                        font.bold: true
                        font.pixelSize: 14
                    }
                }
                // A rounded meter shows the accent colour and the warning state.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 10
                    radius: 5
                    color: Qt.alpha(root.accent, 0.14)
                    Rectangle {
                        width: parent.width * Math.max(0, Math.min(1, Usage.quotaValue(modelData.remaining, desktop.settings.quotaDisplay) / 100))
                        height: parent.height
                        radius: parent.radius
                        color: desktop.settings.warningColors && modelData.remaining <= desktop.settings.notifyThreshold ? "#d97732" : root.accent
                        Behavior on width { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                    }
                }
                // The working-day tick shows how far through the working days the
                // weekly quota is, the way the macOS app marks it.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 3
                    visible: modelData.workdayPercent !== undefined && modelData.workdayPercent >= 0
                    color: "transparent"
                    Rectangle {
                        width: parent.width * (modelData.workdayPercent / 100)
                        height: parent.height
                        radius: 1.5
                        color: Qt.alpha(root.accent, 0.55)
                        visible: modelData.workdayPercent > 0
                    }
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    Rectangle {
                        visible: Usage.resetText(modelData.resetsAt, clock.now, desktop.settings.resetDisplay) !== ""
                        radius: 9; height: 20; width: resetLabel.implicitWidth + 16
                        color: Qt.alpha(root.accent, 0.14)
                        Label {
                            id: resetLabel
                            anchors.centerIn: parent
                            text: Usage.resetText(modelData.resetsAt, clock.now, desktop.settings.resetDisplay)
                            font.pixelSize: 11
                            color: root.accent
                        }
                    }
                    Rectangle {
                        visible: modelData.projected !== undefined && modelData.projected >= 0
                        radius: 9; height: 20; width: projectedLabel.implicitWidth + 16
                        color: Qt.alpha(modelData.projected >= 100 ? "#d97732" : root.accent, 0.14)
                        Label {
                            id: projectedLabel
                            anchors.centerIn: parent
                            text: modelData.projected >= 0 ?
                                (modelData.projected >= 100 ? "projected to run out" : "projected " + modelData.projected.toFixed(0) + "% at reset") : ""
                            font.pixelSize: 11
                            color: modelData.projected >= 100 ? "#d97732" : root.accent
                        }
                    }
                    Rectangle {
                        visible: desktop.settings.showPace && modelData.pace !== ""
                        radius: 9; height: 20; width: paceLabel.implicitWidth + 16
                        color: Qt.alpha(root.accent, 0.14)
                        Label {
                            id: paceLabel
                            anchors.centerIn: parent
                            text: modelData.pace || ""
                            font.pixelSize: 11
                            color: root.accent
                        }
                    }
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Qt.alpha(root.accent, 0.15)
            visible: root.entry.credits !== null && root.entry.credits !== undefined
        }
        RowLayout {
            Layout.fillWidth: true
            visible: root.entry.credits !== null && root.entry.credits !== undefined
            Label { text: "Credits"; opacity: 0.6; font.pixelSize: 12; Layout.fillWidth: true }
            Label { text: Usage.number(root.entry.credits) !== null ? Usage.number(root.entry.credits).toFixed(1) : ""; font.bold: true; font.pixelSize: 13 }
        }
        Repeater {
            model: root.entry.details || []
            ColumnLayout {
                required property var modelData
                Layout.fillWidth: true
                Label { text: modelData.title; visible: text !== ""; font.bold: true; font.pixelSize: 13 }
                Repeater {
                    model: modelData.rows
                    Label {
                        required property var modelData
                        Layout.fillWidth: true
                        text: modelData.label + ": " + modelData.value + (modelData.secondaryValue ? " · " + modelData.secondaryValue : "")
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        opacity: 0.85
                    }
                }
                UsageChart { Layout.fillWidth: true; chart: modelData.chart; accent: root.accent }
            }
        }
    }
    Timer { id: clock; property double now: Date.now(); interval: 30000; running: root.visible; repeat: true; onTriggered: now = Date.now() }
}
