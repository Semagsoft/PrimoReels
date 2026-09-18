import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import PrimoReels
import "TimeUtils.js" as TimeUtils

Rectangle {
    id: sourceMonitorRoot
    color: "#181818"
    border.color: "#333333"
    border.width: 1

    // Bound from Main.qml to libraryPanel.selectedPath.
    property string sourcePath: ""

    // Look up bin metadata from the compat array (small N; counts/logic).
    function sourceEntry() {
        var list = timelineEngine.mediaList;
        for (var i = 0; i < list.length; ++i) {
            if (list[i].path === sourcePath) {
                return list[i];
            }
        }
        return null;
    }

    // Independent engine source state (own decoder thread; program keeps
    // playing undisturbed). Until the engine reports this path loaded, fall
    // back to bin metadata so selection feedback is instant.
    // Reactive binding: referencing sourcePath + mediaList here establishes
    // dependencies (a bare sourceEntry() call would evaluate once and go
    // stale when the bin updates).
    property var entry: {
        var watchedPath = sourcePath;
        var watchedList = timelineEngine.mediaList;
        return sourceEntry();
    }
    property bool isLoaded: sourcePath.length > 0 && timelineEngine.sourcePath === sourcePath
                            && timelineEngine.sourceDuration > 0
    property double binDuration: entry ? (entry.duration || 0) : 0
    property double displayDuration: isLoaded ? timelineEngine.sourceDuration : binDuration
    property double displayPosition: isLoaded ? timelineEngine.sourcePosition : 0
    property bool displayPlaying: isLoaded && timelineEngine.sourcePlaying
    // Split from the Image source bindings: Image.visible must not read
    // Image.source (self-reference binding loop).
    property bool showLiveFrame: isLoaded && timelineEngine.sourceFrameAvailable
    property bool showThumb: !showLiveFrame && sourcePath.length > 0
                             && timelineEngine.hasThumb(sourcePath)

    onSourcePathChanged: {
        if (sourcePath.length > 0) {
            timelineEngine.requestThumb(sourcePath);
            // Auto-loads (and auto-plays on ready) on the source thread;
            // the program monitor is untouched.
            timelineEngine.loadSource(sourcePath);
        } else {
            timelineEngine.pauseSource();
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        Label {
            text: qsTr("Source Monitor")
            color: "#cccccc"
            font.pointSize: 10
        }

        // Video Display Area: live source frame when loaded, bin thumbnail
        // while opening, placeholder when empty.
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#000000"
            border.color: "#2a2a2a"

            VideoFrameItem {
                id: sourcePreview
                anchors.fill: parent
                engine: timelineEngine
                sourceMode: true
                visible: sourcePath.length > 0
            }

            Image {
                anchors.fill: parent
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                cache: false
                visible: sourceMonitorRoot.showThumb
                source: sourceMonitorRoot.showThumb
                        ? "image://thumbs/" + encodeURIComponent(sourceMonitorRoot.sourcePath)
                          + "/" + timelineEngine.thumbVersion
                        : ""
            }

            Text {
                anchors.centerIn: parent
                text: sourceMonitorRoot.sourcePath.length === 0
                      ? qsTr("No Source Selected\nSelect or double-click a bin item")
                      : (!sourceMonitorRoot.showLiveFrame && !sourceMonitorRoot.showThumb
                         ? qsTr("Loading source…") : "")
                color: "#c7c7c7"
                horizontalAlignment: Text.AlignHCenter
                font.pointSize: 12
                visible: text.length > 0
            }

            Label {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 8
                text: sourceMonitorRoot.entry ? sourceMonitorRoot.entry.name : ""
                color: "#ffffff"
                elide: Text.ElideRight
                font.pointSize: 9
                visible: text.length > 0
            }
        }

        // Playback Controls (source thread only).
        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 10

            Button {
                text: "⏮"
                focusPolicy: Qt.NoFocus
                enabled: sourceMonitorRoot.isLoaded
                Accessible.name: qsTr("Source go to start")
                ToolTip.text: qsTr("Go to start")
                ToolTip.visible: hovered
                onClicked: timelineEngine.seekSource(0)
            }
            Button {
                text: sourceMonitorRoot.displayPlaying ? "⏸" : "▶"
                focusPolicy: Qt.NoFocus
                enabled: sourceMonitorRoot.sourcePath.length > 0
                Accessible.name: sourceMonitorRoot.displayPlaying ? qsTr("Pause source") : qsTr("Play source")
                ToolTip.text: sourceMonitorRoot.displayPlaying ? qsTr("Pause") : qsTr("Preview source")
                ToolTip.visible: hovered
                onClicked: {
                    if (!sourceMonitorRoot.isLoaded) {
                        timelineEngine.loadSource(sourceMonitorRoot.sourcePath);
                    } else if (timelineEngine.sourcePlaying) {
                        timelineEngine.pauseSource();
                    } else {
                        timelineEngine.playSource();
                    }
                }
            }
            Button {
                text: "⏭"
                focusPolicy: Qt.NoFocus
                enabled: sourceMonitorRoot.isLoaded
                Accessible.name: qsTr("Source go to end")
                ToolTip.text: qsTr("Go to end")
                ToolTip.visible: hovered
                onClicked: timelineEngine.seekSource(sourceMonitorRoot.displayDuration)
            }

            Label {
                text: TimeUtils.formatTime(sourceMonitorRoot.displayPosition) + " / " + TimeUtils.formatTime(sourceMonitorRoot.displayDuration)
                color: "#ffffff"
                font.family: "Monospace"
                Accessible.name: qsTr("Source position")
            }
        }

        // Scrub Slider (seeks only when this source is loaded).
        Slider {
            id: sourceScrubSlider
            Layout.fillWidth: true
            from: 0
            to: sourceMonitorRoot.displayDuration > 0 ? sourceMonitorRoot.displayDuration : 1.0
            value: pressed ? value : sourceMonitorRoot.displayPosition
            enabled: sourceMonitorRoot.isLoaded
            Accessible.name: qsTr("Source scrub")
            onMoved: timelineEngine.seekSource(value)
        }
    }
}
