import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import PrimoReels
import "TimeUtils.js" as TimeUtils

Rectangle {
    id: monitorRoot
    color: "#181818"
    border.color: "#333333"
    border.width: 1

    // Index of the timeline clip selected in TimelineView (-1 = none).
    // Bound from Main.qml so duplicate bin paths resolve to the
    // selected clip instead of the first path match.
    property int selectedClipIndex: -1
    // Split from the Image source binding below: Image.visible must not
    // read Image.source (self-reference binding loop).
    property bool showVideo: timelineEngine.frameAvailable
    // Title overlay of the currently previewed timeline clip.
    // Declared before use: a binding that reads this property during the
    // initial creation pass would otherwise see undefined (the script
    // binding below only runs on first evaluation, which can sort after
    // the consumer).
    property string currentTitle: {
        var clips = timelineEngine.timelineClips;
        if (timelineEngine.sequencePlaying && timelineEngine.sequenceIndex >= 0
                && timelineEngine.sequenceIndex < clips.length) {
            return clips[timelineEngine.sequenceIndex].title || "";
        }
        var src = timelineEngine.currentSource;
        // Prefer the selected clip when it matches the loaded source so
        // duplicate bin entries (same path twice) show the right title.
        if (selectedClipIndex >= 0 && selectedClipIndex < clips.length
                && clips[selectedClipIndex].path === src) {
            return clips[selectedClipIndex].title || "";
        }
        for (var i = 0; i < clips.length; ++i) {
            if (clips[i].path === src) {
                return clips[i].title || "";
            }
        }
        return "";
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        Label {
            text: qsTr("Program Monitor")
            color: "#cccccc"
            font.pointSize: 10
        }

        // Video Display Area
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#000000"
            border.color: "#2a2a2a"

                VideoFrameItem {
                    id: videoPreview
                    anchors.fill: parent
                    engine: timelineEngine
                    sourceMode: false
                    rotation: timelineEngine.clipRotation
                    transform: Scale {
                        origin.x: videoPreview.width / 2
                        origin.y: videoPreview.height / 2
                        xScale: timelineEngine.clipScaleX
                        yScale: timelineEngine.clipScaleY
                    }
                }

            Text {
                anchors.centerIn: parent
                text: timelineEngine.currentSource && !timelineEngine.frameAvailable
                      ? qsTr("Loading frame...")
                      : timelineEngine.timelineClips.length > 0 && !timelineEngine.frameAvailable
                        ? qsTr("No Clip Loaded\nClick a timeline clip to preview it")
                        : timelineEngine.timelineClips.length === 0 && !timelineEngine.currentSource
                          ? qsTr("No Media Loaded\nClick 'Import Media'")
                          : ""
                color: "#c7c7c7"
                horizontalAlignment: Text.AlignHCenter
                font.pointSize: 12
                visible: text.length > 0 && !videoPreview.visible
            }

            // Title overlay of the currently previewed timeline clip.
            // Bound property (not a function call) so it re-evaluates when
            // timelineClips or currentSource change.
            Text {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 16
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.WordWrap
                font.pointSize: 18
                font.bold: true
                color: "#ffffff"
                style: Text.Outline
                styleColor: "#000000"
                text: monitorRoot.currentTitle
                visible: timelineEngine.frameAvailable && text.length > 0
            }
        }

        // Playback Controls
        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 15

            Button {
                text: "⏮"
                // NoFocus: Space is the global Play shortcut; a focused
                // transport button would toggle twice (button + menu action).
                focusPolicy: Qt.NoFocus
                Accessible.name: qsTr("Go to start")
                ToolTip.text: qsTr("Go to start")
                ToolTip.visible: hovered
                onClicked: timelineEngine.seek(0)
            }
            Button {
                text: timelineEngine.isPlaying ? "⏸" : "▶"
                focusPolicy: Qt.NoFocus
                Accessible.name: timelineEngine.isPlaying ? qsTr("Pause") : qsTr("Play")
                ToolTip.text: timelineEngine.isPlaying ? qsTr("Pause") : qsTr("Play")
                ToolTip.visible: hovered
                onClicked: {
                    if (timelineEngine.isPlaying) {
                        timelineEngine.pause()
                    } else if (timelineEngine.sequencePlaying) {
                        // Paused mid-sequence: resume in place (trim window kept).
                        timelineEngine.play()
                    } else if (timelineEngine.timelineClips.length > 0) {
                        var idx = timelineEngine.sequenceIndexOf(timelineEngine.currentSource);
                        timelineEngine.playSequenceFrom(idx >= 0 ? idx : 0)
                    } else {
                        timelineEngine.play()
                    }
                }
            }
            Button {
                text: "⏭"
                focusPolicy: Qt.NoFocus
                Accessible.name: qsTr("Go to end")
                ToolTip.text: qsTr("Go to end")
                ToolTip.visible: hovered
                onClicked: timelineEngine.seek(timelineEngine.duration)
            }

            Label {
                text: TimeUtils.formatTime(timelineEngine.position) + " / " + TimeUtils.formatTime(timelineEngine.duration)
                color: "#ffffff"
                font.family: "Monospace"
                Accessible.name: qsTr("Program position")
            }

            Label {
                text: timelineEngine.sequencePlaying
                      ? qsTr("Clip %1/%2").arg(timelineEngine.sequencePosition()).arg(timelineEngine.sequenceCount())
                      : qsTr("Preview")
                color: timelineEngine.sequencePlaying ? "#ffaa00" : "#c7c7c7"
                font.pointSize: 9
            }
        }

        // Scrub Slider (don't fight playback while dragging).
        Slider {
            id: scrubSlider
            Layout.fillWidth: true
            from: 0
            to: timelineEngine.duration > 0 ? timelineEngine.duration : 1.0
            value: pressed ? value : timelineEngine.position
            Accessible.name: qsTr("Program scrub")
            onMoved: timelineEngine.seek(value)
        }
    }
}
