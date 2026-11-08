import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Export queue view: lists queued export jobs with progress and cancel
// buttons. Driven by timelineEngine.exportQueue / exportQueueCount /
// exportQueueCurrentIndex / addExportJob / cancelExportJob /
// cancelAllExportJobs.
Rectangle {
    id: queueRoot
    color: "#181818"
    border.color: "#333333"
    border.width: 1

    property int currentIndex: -1

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        Label {
            text: qsTr("Export Queue")
            color: "#cccccc"
            font.pointSize: 10
            font.bold: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: qsTr("%1 job(s)").arg(queueRoot.currentIndex >= 0 ? queueRoot.currentIndex + 1 : 0)
                color: "#cccccc"
                font.pointSize: 9
            }

            Item { Layout.fillWidth: true }

            Button {
                text: qsTr("Cancel All")
                font.pointSize: 9
                focusPolicy: Qt.NoFocus
                enabled: timelineEngine.exportQueueCount > 0
                Accessible.name: qsTr("Cancel all export jobs")
                onClicked: timelineEngine.cancelAllExportJobs()
            }
        }

        ListView {
            id: queueList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: timelineEngine.exportQueue
            spacing: 4

            delegate: Item {
                width: ListView.view.width
                height: 60

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 4
                    spacing: 3

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Label {
                            text: modelData.firstPath || qsTr("Untitled")
                            color: "#ffffff"
                            font.pointSize: 9
                            font.bold: true
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                            Accessible.name: modelData.outputPath
                        }

                        Label {
                            text: modelData.state === 0 ? qsTr("Pending")
                                : modelData.state === 1 ? qsTr("Running")
                                : modelData.state === 2 ? qsTr("Completed")
                                : modelData.state === 3 ? qsTr("Failed")
                                : qsTr("Cancelled")
                            color: modelData.state === 1 ? "#ffaa00"
                                : modelData.state === 2 ? "#4caf50"
                                : modelData.state === 3 ? "#ff5555"
                                : "#cccccc"
                            font.pointSize: 8
                            Accessible.name: qsTr("Job state")
                        }

                        Button {
                            text: qsTr("Cancel")
                            font.pointSize: 8
                            focusPolicy: Qt.NoFocus
                            enabled: modelData.state === 0 || modelData.state === 1
                            Accessible.name: qsTr("Cancel export job")
                            onClicked: timelineEngine.cancelExportJob(ListView.index)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        ProgressBar {
                            Layout.fillWidth: true
                            from: 0
                            to: 1
                            value: modelData.progress
                            Accessible.name: qsTr("Export progress")
                            Accessible.value: Math.round(modelData.progress * 100)
                        }

                        Label {
                            text: (modelData.progress * 100).toFixed(0) + "%"
                            color: "#cccccc"
                            font.pointSize: 8
                            font.family: "Monospace"
                            Layout.preferredWidth: 35
                            Accessible.name: qsTr("Export progress percent")
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Label {
                            text: modelData.errorMessage
                            color: "#ff5555"
                            font.pointSize: 8
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                            visible: modelData.errorMessage.length > 0
                            Accessible.name: qsTr("Export error")
                        }

                        Label {
                            text: modelData.warningMessage
                            color: "#ffaa00"
                            font.pointSize: 8
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                            visible: modelData.warningMessage.length > 0
                            Accessible.name: qsTr("Export warning")
                        }
                    }
                }

                Rectangle {
                    anchors.fill: parent
                    anchors.topMargin: 1
                    anchors.bottomMargin: 1
                    color: "#333333"
                    opacity: 0.3
                    visible: ListView.index === queueRoot.currentIndex
                }
            }
        }

        Text {
            text: timelineEngine.exportQueueCount === 0
                  ? qsTr("No queued exports.")
                  : ""
            color: "#888888"
            font.pointSize: 9
            anchors.horizontalCenter: parent.horizontalCenter
            visible: timelineEngine.exportQueueCount === 0
        }
    }

    Connections {
        target: timelineEngine
        function onExportQueueChanged() {
            queueRoot.currentIndex = timelineEngine.exportQueueCurrentIndex;
        }
    }

    Component.onCompleted: {
        queueRoot.currentIndex = timelineEngine.exportQueueCurrentIndex;
    }
}
