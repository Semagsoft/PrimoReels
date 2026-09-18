import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: timelineRoot
    objectName: "timelineRoot"
    color: "#1e1e1e"
    border.color: "#333333"
    border.width: 1

    property int selectedClipIndex: -1
    property int contextClipIndex: -1
    property real pixelsPerSecond: 100
    // Reactive mirrors of selectedClip()/canSplitAtPlayhead(): bindings that
    // call JS functions only re-evaluate when a dependency emits, so expose
    // the same state as plain properties with explicit dependencies.
    property var curClip: selectedClipIndex >= 0
                          && selectedClipIndex < timelineEngine.timelineClips.length
                          ? timelineEngine.timelineClips[selectedClipIndex] : null
    property bool canSplit: {
        if (!curClip) {
            return false;
        }
        if ((curClip.track || 0) === 2) {
            // A1 bed clips live on the absolute output timeline: split when
            // the playhead output time lies strictly inside the clip.
            var a1off = outputTimeAtPlayhead() - (curClip.startTime || 0);
            return a1off >= 0.1 && a1off <= curClip.duration - 0.1;
        }
        if (timelineEngine.sequencePlaying) {
            if (selectedClipIndex !== timelineEngine.sequenceIndex) {
                return false;
            }
        } else if (timelineEngine.currentSource !== curClip.path) {
            return false;
        }
        var offset = timelineEngine.position - (curClip.trimStart || 0);
        return offset >= 0.1 && offset <= curClip.duration - 0.1;
    }

    Menu {
        id: clipContextMenu
        MenuItem {
            text: qsTr("Split at Playhead")
            enabled: canSplitAtPlayhead()
            onTriggered: {
                timelineRoot.selectedClipIndex = contextClipIndex;
                splitSelectedAtPlayhead();
            }
        }
        MenuItem {
            text: (contextClipIndex >= 0 && contextClipIndex < timelineEngine.timelineClips.length && (timelineEngine.timelineClips[contextClipIndex].track || 0) === 1) ? qsTr("Move to Video 1") : qsTr("Move to Overlay (V2)")
            enabled: contextClipIndex >= 0
            onTriggered: {
                var clip = timelineEngine.timelineClips[contextClipIndex];
                var targetTrack = (clip.track || 0) === 1 ? 0 : 1;
                timelineEngine.setClipTrack(contextClipIndex, targetTrack);
            }
        }
        MenuItem {
            text: qsTr("Move to Audio (A1)")
            enabled: contextClipIndex >= 0
            onTriggered: timelineEngine.setClipTrack(contextClipIndex, 2)
        }
        MenuItem {
            text: (contextClipIndex >= 0 && contextClipIndex < timelineEngine.timelineClips.length && timelineEngine.timelineClips[contextClipIndex].muted) ? qsTr("Unmute Clip") : qsTr("Mute Clip")
            enabled: contextClipIndex >= 0
            onTriggered: {
                var c = timelineEngine.timelineClips[contextClipIndex];
                timelineEngine.setClipMuted(contextClipIndex, !(c.muted || false));
            }
        }
        MenuItem {
            text: qsTr("Detach Audio to A1")
            enabled: contextClipIndex >= 0
            onTriggered: timelineEngine.detachAudio(contextClipIndex)
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Remove Clip")
            enabled: contextClipIndex >= 0
            onTriggered: {
                if (timelineRoot.selectedClipIndex === contextClipIndex) {
                    timelineRoot.selectedClipIndex = -1;
                }
                timelineEngine.removeTimelineClip(contextClipIndex);
            }
        }
    }
    property real sequenceEnd: {
        var end = 0.0;
        var clips = timelineEngine.timelineClips;
        for (var i = 0; i < clips.length; ++i) {
            var clipEnd = clips[i].startTime + clips[i].duration;
            if (clipEnd > end) {
                end = clipEnd;
            }
        }
        return end;
    }
    property real timelineDuration: Math.max(timelineEngine.duration, sequenceEnd, 10.0)
    // Adaptive ruler step so segments stay readable at any zoom and long
    // projects don't spawn hundreds of delegates.
    property real rulerStep: {
        var candidates = [1, 2, 5, 10, 15, 30, 60, 120, 300];
        for (var i = 0; i < candidates.length; ++i) {
            if (candidates[i] * pixelsPerSecond >= 80) {
                return candidates[i];
            }
        }
        return 300;
    }
    property int rulerSegments: Math.max(1, Math.ceil(timelineDuration / rulerStep))

    // Track content origin: clips start 6px in so header pills don't overlap.
    property real trackOrigin: 6
    // V1 insert model: returns the flat-list index where a dropped V1
    // clip belongs. V2 overlays use absolute startTime (see
    // overlayDropArea) and are skipped for the time comparison, but the
    // returned index is flat (engine inserts at flat positions; V1 order
    // is the filtered list order, so inserting before/after an overlay
    // preserves the visual V1 sequence).
    function dropIndexAt(x) {
        var time = Math.max(0, (x - trackOrigin) / pixelsPerSecond);
        var clips = timelineEngine.timelineClips;
        for (var i = 0; i < clips.length; ++i) {
            if ((clips[i].track || 0) !== 0) {
                continue;
            }
            if (time < clips[i].startTime + clips[i].duration) {
                return i;
            }
        }
        return clips.length;
    }

    function hasOverlayClips() {
        var clips = timelineEngine.timelineClips;
        for (var i = 0; i < clips.length; ++i) {
            if ((clips[i].track || 0) === 1) {
                return true;
            }
        }
        return false;
    }

    Connections {
        target: timelineEngine
        function onTimelineClipsChanged() {
            if (timelineRoot.selectedClipIndex >= timelineEngine.timelineClips.length) {
                timelineRoot.selectedClipIndex = timelineEngine.timelineClips.length - 1;
            }
        }
    }

    function selectedClip() {
        if (selectedClipIndex < 0 || selectedClipIndex >= timelineEngine.timelineClips.length) {
            return null;
        }
        return timelineEngine.timelineClips[selectedClipIndex];
    }

    function syncTitleField() {
        // Don't clobber in-progress typing: only sync when the field isn't focused.
        if (clipTitleField.activeFocus) {
            return;
        }
        var clip = selectedClip();
        var next = clip ? (clip.title || "") : "";
        if (clipTitleField.text !== next) {
            clipTitleField.text = next;
        }
    }

    onSelectedClipIndexChanged: syncTitleField()
    Connections {
        target: timelineEngine
        function onTimelineClipsChanged() { timelineRoot.syncTitleField() }
    }

    function playheadClipOffset() {
        var clip = selectedClip();
        if (!clip) {
            return -1;
        }
        if ((clip.track || 0) === 2) {
            // A1 bed: offset from the clip's absolute start. splitTimelineClip
            // measures from the playable start, which for absolute tracks is
            // startTime (trims shift the file mapping, not the output one).
            var a1off = outputTimeAtPlayhead() - (clip.startTime || 0);
            if (a1off >= 0.1 && a1off <= clip.duration - 0.1) {
                return a1off;
            }
            return -1;
        }
        // Disambiguate duplicate bin items: in sequence mode the playhead
        // belongs to sequenceIndex, otherwise to the selected preview clip.
        if (timelineEngine.sequencePlaying) {
            if (selectedClipIndex !== timelineEngine.sequenceIndex) {
                return -1;
            }
        } else if (timelineEngine.currentSource !== clip.path) {
            return -1;
        }
        var offset = timelineEngine.position - (clip.trimStart || 0);
        if (offset >= 0.1 && offset <= clip.duration - 0.1) {
            return offset;
        }
        return -1;
    }

    // Output-timeline time at the playhead (drives A1 split/attach).
    // Sequence playback maps through the V1 row window; single preview maps
    // through the selected clip when it is the loaded source, else through
    // the loaded source's V1 row; ad-hoc preview falls back to the playhead
    // time itself (which is also where the playhead line is drawn).
    function outputTimeAtPlayhead() {
        if (timelineEngine.sequencePlaying) {
            var seq = timelineEngine.sequenceIndex >= 0
                      && timelineEngine.sequenceIndex < timelineEngine.timelineClips.length
                      ? timelineEngine.timelineClips[timelineEngine.sequenceIndex] : null;
            if (!seq) {
                return -1;
            }
            return (seq.startTime || 0) + (timelineEngine.position - (seq.trimStart || 0));
        }
        var sel = selectedClip();
        if (sel && timelineEngine.currentSource === sel.path) {
            return (sel.startTime || 0) + (timelineEngine.position - (sel.trimStart || 0));
        }
        var clips = timelineEngine.timelineClips;
        for (var i = 0; i < clips.length; ++i) {
            if ((clips[i].track || 0) === 0 && clips[i].path === timelineEngine.currentSource) {
                return (clips[i].startTime || 0)
                       + (timelineEngine.position - (clips[i].trimStart || 0));
            }
        }
        return timelineEngine.position;
    }

    // Attach an audio file to A1 under the playhead output time. Returns the
    // start time used and selects the new clip.
    function attachAudioAtPlayhead(path) {
        if (!path || path.length === 0) {
            return -1;
        }
        var t = outputTimeAtPlayhead();
        if (!(t >= 0)) {
            t = timelineEngine.position;
        }
        t = Math.max(0, t);
        timelineEngine.appendAudioClip(path, t);
        selectedClipIndex = timelineEngine.timelineClips.length - 1;
        return t;
    }

    function canSplitAtPlayhead() {
        return playheadClipOffset() >= 0;
    }

    function splitSelectedAtPlayhead() {
        var offset = playheadClipOffset();
        if (offset >= 0 && selectedClipIndex >= 0
                && selectedClipIndex < timelineEngine.timelineClips.length) {
            timelineEngine.splitTimelineClip(selectedClipIndex, offset);
            selectedClipIndex = Math.min(selectedClipIndex + 1,
                                         timelineEngine.timelineClips.length - 1);
        }
    }

    function applyTransitionToSelected(name) {
        if (selectedClipIndex < 0 || selectedClipIndex >= timelineEngine.timelineClips.length - 1) {
            return false;
        }
        var clip = timelineEngine.timelineClips[selectedClipIndex];
        timelineEngine.setClipTransition(
            selectedClipIndex, clip.transition === name ? "" : name,
            clip.transitionDuration || 0.5);
        return true;
    }

    function applyEffectToSelected(name) {
        if (selectedClipIndex < 0 || selectedClipIndex >= timelineEngine.timelineClips.length) {
            return false;
        }
        var clip = timelineEngine.timelineClips[selectedClipIndex];
        timelineEngine.setClipEffect(selectedClipIndex, clip.effect === name ? "" : name);
        return true;
    }

    // Split is triggered by the global "S" Shortcut in Main.qml, which
    // guards against text-input focus (search, titles). No local shortcut
    // here to avoid double-triggering or splitting while typing.

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        // Timeline Toolbar (Zoom, Snapping, Track controls)
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            Layout.leftMargin: 4
            Layout.rightMargin: 4
            spacing: 10

            Label {
                text: qsTr("Timeline")
                color: "#cccccc"
                font.pointSize: 10
            }

            Label {
                text: timelineEngine.timelineClips.length === 0
                      ? qsTr("Empty — drag clips here or use + in the Bin")
                      : qsTr("%n clip(s)", "", timelineEngine.timelineClips.length)
                color: "#c7c7c7"
                font.pointSize: 9
            }

            Item { Layout.fillWidth: true }

            Button {
                text: timelineEngine.isPlaying ? qsTr("⏸ Sequence") : qsTr("▶ Sequence")
                font.pointSize: 9
                focusPolicy: Qt.NoFocus
                Accessible.name: timelineEngine.isPlaying ? qsTr("Pause sequence") : qsTr("Play sequence")
                ToolTip.text: qsTr("Play timeline sequence")
                ToolTip.visible: hovered
                enabled: timelineEngine.timelineClips.length > 0
                onClicked: {
                    if (timelineEngine.isPlaying) {
                        timelineEngine.pause()
                    } else if (timelineEngine.sequencePlaying) {
                        // Paused mid-sequence: resume in place (trim window kept).
                        timelineEngine.play()
                    } else {
                        var startIdx = (selectedClipIndex >= 0
                                        && selectedClipIndex < timelineEngine.timelineClips.length)
                                       ? selectedClipIndex : 0;
                        selectedClipIndex = startIdx;
                        timelineEngine.playSequenceFrom(startIdx)
                    }
                }
            }
            Button {
                text: qsTr("Split (S)")
                font.pointSize: 9
                focusPolicy: Qt.NoFocus
                enabled: timelineRoot.canSplit
                Accessible.name: qsTr("Split selected clip at playhead")
                ToolTip.text: qsTr("Split selected clip at playhead")
                ToolTip.visible: hovered
                onClicked: splitSelectedAtPlayhead()
            }
            Button {
                text: qsTr("Clear")
                font.pointSize: 9
                focusPolicy: Qt.NoFocus
                Accessible.name: qsTr("Clear timeline")
                ToolTip.text: qsTr("Remove all timeline clips")
                ToolTip.visible: hovered
                enabled: timelineEngine.timelineClips.length > 0
                onClicked: {
                    timelineEngine.clearTimeline();
                    selectedClipIndex = -1;
                }
            }
            Button {
                text: qsTr("Zoom -")
                font.pointSize: 9
                focusPolicy: Qt.NoFocus
                Accessible.name: qsTr("Zoom out timeline")
                enabled: pixelsPerSecond > 10
                ToolTip.text: qsTr("Zoom out timeline")
                ToolTip.visible: hovered
                onClicked: pixelsPerSecond = Math.max(10, pixelsPerSecond / 1.25)
            }
            Button {
                text: qsTr("Zoom +")
                font.pointSize: 9
                focusPolicy: Qt.NoFocus
                Accessible.name: qsTr("Zoom in timeline")
                enabled: pixelsPerSecond < 800
                ToolTip.text: qsTr("Zoom in timeline")
                ToolTip.visible: hovered
                onClicked: pixelsPerSecond = Math.min(800, pixelsPerSecond * 1.25)
            }
            Button {
                text: qsTr("Fit")
                font.pointSize: 9
                focusPolicy: Qt.NoFocus
                Accessible.name: qsTr("Fit timeline to window")
                ToolTip.text: qsTr("Fit timeline to window")
                ToolTip.visible: hovered
                onClicked: {
                    var dur = Math.max(1.0, timelineDuration);
                    var w = timelineFlick.width - 20;
                    pixelsPerSecond = Math.max(10, Math.min(800, w / dur));
                }
            }
        }

        // Trim Bar (selected clip)
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            Layout.leftMargin: 4
            Layout.rightMargin: 4
            spacing: 8
            visible: timelineRoot.curClip !== null

            Label {
                text: qsTr("Trim: %1").arg(timelineRoot.curClip ? timelineRoot.curClip.name : "")
                color: "#cccccc"
                font.pointSize: 9
                Layout.maximumWidth: 140
                elide: Text.ElideRight
            }
            Button {
                text: timelineRoot.curClip && (timelineRoot.curClip.track || 0) === 1 ? qsTr("V2") : timelineRoot.curClip && (timelineRoot.curClip.track || 0) === 2 ? qsTr("A1") : qsTr("V1")
                font.pointSize: 8
                focusPolicy: Qt.NoFocus
                Accessible.name: qsTr("Move clip between tracks")
                ToolTip.text: timelineRoot.curClip && (timelineRoot.curClip.track || 0) === 2 ? qsTr("Move audio bed clip to Video 1") : qsTr("Move clip between Video 1 and Overlay (Video 2)")
                ToolTip.visible: hovered
                onClicked: timelineEngine.setClipTrack(selectedClipIndex,
                    ((timelineRoot.curClip.track || 0) === 0) ? 1 : 0)
            }
            Label { text: qsTr("Title"); color: "#cccccc"; font.pointSize: 8 }
            TextField {
                id: clipTitleField
                Layout.preferredWidth: 130
                leftPadding: 6
                rightPadding: 6
                font.pointSize: 9
                color: "#ffffff"
                placeholderText: qsTr("Clip title…")
                Accessible.name: qsTr("Clip title")
                Accessible.description: qsTr("Title burned into the clip on export")
                // No text: binding (it would re-fire on any
                // timelineClipsChanged while typing). Synced imperatively
                // via syncTitleField() on selection + timeline changes.
                Component.onCompleted: timelineRoot.syncTitleField()
                background: Rectangle {
                    color: "#252526"
                    border.color: "#333333"
                    radius: 3
                }
                onEditingFinished: {
                    if (selectedClipIndex >= 0) {
                        timelineEngine.setClipTitle(selectedClipIndex, text);
                    }
                }
            }
            Label { text: qsTr("In"); color: "#cccccc"; font.pointSize: 8 }
            Button {
                text: "−"; font.pointSize: 9; focusPolicy: Qt.NoFocus
                implicitWidth: 36; implicitHeight: 32
                Accessible.name: qsTr("Trim start earlier")
                ToolTip.text: qsTr("Trim start earlier")
                ToolTip.visible: hovered
                onClicked: timelineEngine.setClipTrimStart(selectedClipIndex,
                    (timelineRoot.curClip.trimStart || 0) - 0.5)
            }
            Label {
                text: (timelineRoot.curClip ? (timelineRoot.curClip.trimStart || 0) : 0).toFixed(1) + "s"
                color: "#ffffff"; font.pointSize: 9
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("Trim start %1 seconds").arg((timelineRoot.curClip ? (timelineRoot.curClip.trimStart || 0) : 0).toFixed(1))
            }
            Button {
                text: "+"; font.pointSize: 9; focusPolicy: Qt.NoFocus
                implicitWidth: 36; implicitHeight: 32
                Accessible.name: qsTr("Trim start later")
                ToolTip.text: qsTr("Trim start later")
                ToolTip.visible: hovered
                onClicked: timelineEngine.setClipTrimStart(selectedClipIndex,
                    (timelineRoot.curClip.trimStart || 0) + 0.5)
            }
            Label { text: qsTr("Out"); color: "#cccccc"; font.pointSize: 8 }
            Button {
                text: "−"; font.pointSize: 9; focusPolicy: Qt.NoFocus
                implicitWidth: 36; implicitHeight: 32
                Accessible.name: qsTr("Trim end earlier")
                ToolTip.text: qsTr("Trim end earlier")
                ToolTip.visible: hovered
                onClicked: timelineEngine.setClipTrimEnd(selectedClipIndex,
                    (timelineRoot.curClip.trimEnd || 0) - 0.5)
            }
            Label {
                text: (timelineRoot.curClip ? (timelineRoot.curClip.trimEnd || 0) : 0).toFixed(1) + "s"
                color: "#ffffff"; font.pointSize: 9
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("Trim end %1 seconds").arg((timelineRoot.curClip ? (timelineRoot.curClip.trimEnd || 0) : 0).toFixed(1))
            }
            Button {
                text: "+"; font.pointSize: 9; focusPolicy: Qt.NoFocus
                implicitWidth: 36; implicitHeight: 32
                Accessible.name: qsTr("Trim end later")
                ToolTip.text: qsTr("Trim end later")
                ToolTip.visible: hovered
                onClicked: timelineEngine.setClipTrimEnd(selectedClipIndex,
                    (timelineRoot.curClip.trimEnd || 0) + 0.5)
            }
            Label {
                text: timelineRoot.curClip
                      ? qsTr("%1s of %2s").arg(timelineRoot.curClip.duration.toFixed(1))
                        .arg(((timelineRoot.curClip.sourceDuration || timelineRoot.curClip.duration)).toFixed(1))
                      : ""
                color: "#c7c7c7"; font.pointSize: 8
            }
            Label {
                visible: timelineRoot.curClip && (timelineRoot.curClip.transition || "").length > 0
                text: timelineRoot.curClip ? ("◇ " + timelineRoot.curClip.transition) : ""
                color: "#ffcc66"; font.pointSize: 8
            }
            Button {
                visible: timelineRoot.curClip && (timelineRoot.curClip.transition || "").length > 0
                text: "−"; font.pointSize: 9; focusPolicy: Qt.NoFocus
                implicitWidth: 36; implicitHeight: 32
                Accessible.name: qsTr("Shorter transition")
                ToolTip.text: qsTr("Shorter transition")
                ToolTip.visible: hovered
                onClicked: timelineEngine.setClipTransition(selectedClipIndex,
                    timelineRoot.curClip.transition,
                    (timelineRoot.curClip.transitionDuration || 0.5) - 0.25)
            }
            Label {
                visible: timelineRoot.curClip && (timelineRoot.curClip.transition || "").length > 0
                text: timelineRoot.curClip ? (timelineRoot.curClip.transitionDuration || 0.5).toFixed(2) + "s" : ""
                color: "#ffffff"; font.pointSize: 9
            }
            Button {
                visible: timelineRoot.curClip && (timelineRoot.curClip.transition || "").length > 0
                text: "+"; font.pointSize: 9; focusPolicy: Qt.NoFocus
                implicitWidth: 36; implicitHeight: 32
                Accessible.name: qsTr("Longer transition")
                ToolTip.text: qsTr("Longer transition")
                ToolTip.visible: hovered
                onClicked: timelineEngine.setClipTransition(selectedClipIndex,
                    timelineRoot.curClip.transition,
                    (timelineRoot.curClip.transitionDuration || 0.5) + 0.25)
            }
            Item { Layout.fillWidth: true }
        }

        // Tracks Area
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#222222"
            border.color: "#2a2a2a"

            Flickable {
                id: timelineFlick
                anchors.fill: parent
                contentWidth: Math.max(timelineFlick.width, timelineDuration * pixelsPerSecond)
                // Tracks end at y=195 (audio track); allow vertical scroll
                // on short windows instead of clipping.
                contentHeight: Math.max(timelineFlick.height, 205)
                clip: true
                focus: true

                // Ruler / Timecode Header (click to seek)
                Rectangle {
                    width: timelineFlick.contentWidth
                    height: 25
                    color: "#2a2a2a"

                    Row {
                        anchors.fill: parent
                        Repeater {
                            model: rulerSegments
                            Rectangle {
                                width: rulerStep * pixelsPerSecond
                                height: 25
                                color: "transparent"
                                border.color: "#3d3d3d"
                                border.width: 1
                                Text {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: (index * rulerStep) + "s"
                                    color: "#cccccc"
                                    font.pointSize: 9
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Slider
                        Accessible.name: qsTr("Timeline ruler. Left and right arrows seek.")
                        onClicked: function(mouse) {
                            var t = (mouse.x - trackOrigin) / pixelsPerSecond;
                            timelineEngine.seek(Math.max(0, Math.min(t, timelineDuration)));
                        }
                        // Only a subset of keys has Keys.on<Name>Pressed
                        // convenience signals (no Home/End/Insert); handle
                        // navigation keys via the generic handler.
                        Keys.onPressed: function(event) {
                            if (event.key === Qt.Key_Left) {
                                timelineEngine.seek(Math.max(0, timelineEngine.position - 1));
                                event.accepted = true;
                            } else if (event.key === Qt.Key_Right) {
                                timelineEngine.seek(Math.min(timelineDuration, timelineEngine.position + 1));
                                event.accepted = true;
                            } else if (event.key === Qt.Key_Home) {
                                timelineEngine.seek(0);
                                event.accepted = true;
                            } else if (event.key === Qt.Key_End) {
                                timelineEngine.seek(timelineDuration);
                                event.accepted = true;
                            }
                        }
                    }
                }

                // Video Track 1 (drop target for Bin clips)
                Rectangle {
                    id: videoTrack
                    y: 30
                    width: timelineFlick.contentWidth
                    height: 60
                    color: timelineDropArea.containsDrag ? "#3a3a3a" : "#2d2d2d"
                    border.color: timelineDropArea.containsDrag ? "#007acc" : "#3a3a3a"

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.margins: 6
                        width: video1Label.implicitWidth + 12
                        height: video1Label.implicitHeight + 6
                        color: "#1e1e1e"
                        opacity: 0.85
                        radius: 3
                        z: 2
                        Text {
                            id: video1Label
                            anchors.centerIn: parent
                            text: qsTr("Video 1")
                            color: "#cccccc"
                            font.pointSize: 9
                        }
                    }

                    // Sequence clips (V1 only; insert-at-drop model)
                    // Model-backed: roles notify per-row, delegates recycle.
                    Repeater {
                        model: timelineEngine.timelineModel
                        delegate: Rectangle {
                            id: v1ClipDelegate
                            visible: (model.track || 0) === 0
                            x: 6 + model.startTime * pixelsPerSecond
                            y: 18
                            width: Math.max(4, model.duration * pixelsPerSecond - 4)
                            height: 40
                            color: (timelineEngine.sequencePlaying && (model.track || 0) === 0 && timelineEngine.sequenceIndex === index)
                                     ? "#2d8a4e"
                                     : model.path === timelineEngine.currentSource ? "#0098ff" : "#007acc"
                            radius: 4
                            border.color: timelineRoot.selectedClipIndex === index ? "#ffaa00" : "#1c5b88"
                            border.width: (timelineRoot.selectedClipIndex === index
                                           || (timelineEngine.sequencePlaying && (model.track || 0) === 0 && timelineEngine.sequenceIndex === index)) ? 2 : 1
                            Accessible.role: Accessible.Button
                            Accessible.name: model.name + ", " + (model.duration || 0).toFixed(1) + qsTr(" seconds")
                                                + (timelineRoot.selectedClipIndex === index ? qsTr(", selected") : "")

                            Image {
                                anchors.fill: parent
                                anchors.margins: 2
                                fillMode: Image.PreserveAspectCrop
                                opacity: 0.25
                                asynchronous: true
                                cache: false
                                visible: pixelsPerSecond >= 40 && model.thumbTick >= 0
                                         && timelineEngine.hasThumb(model.path)
                                source: visible ? ("image://thumbs/" + encodeURIComponent(model.path) + "/" + model.thumbTick) : ""
                            }

                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                activeFocusOnTab: true
                                Accessible.role: Accessible.Button
                                Accessible.name: model.name
                                onClicked: function(mouse) {
                                    if (mouse.button === Qt.RightButton) {
                                        timelineRoot.contextClipIndex = index;
                                        clipContextMenu.popup();
                                    } else {
                                        timelineFlick.forceActiveFocus();
                                        timelineRoot.selectedClipIndex = index;
                                        timelineEngine.previewTimelineClip(index);
                                    }
                                }
                                // Keyboard alternative to mouse: Enter previews,
                                // Menu key opens the context menu, Delete removes.
                                Keys.onReturnPressed: {
                                    timelineRoot.selectedClipIndex = index;
                                    timelineEngine.previewTimelineClip(index);
                                }
                                Keys.onEnterPressed: {
                                    timelineRoot.selectedClipIndex = index;
                                    timelineEngine.previewTimelineClip(index);
                                }
                                Keys.onMenuPressed: {
                                    timelineRoot.contextClipIndex = index;
                                    clipContextMenu.popup();
                                }
                                Keys.onDeletePressed: {
                                    if (timelineRoot.selectedClipIndex === index) {
                                        timelineRoot.selectedClipIndex = -1;
                                    }
                                    timelineEngine.removeTimelineClip(index);
                                }
                            }

                            Rectangle {
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                anchors.margins: 2
                                width: Math.min(parent.width - 4,
                                    ((model.transitionDuration || 0.5) * pixelsPerSecond) / 2 + 10)
                                visible: (model.transition || "").length > 0
                                         && index < timelineEngine.timelineClips.length - 1
                                color: "#9a6a00"
                                opacity: 0.9
                                radius: 3
                                Text {
                                    anchors.centerIn: parent
                                    text: "◇"
                                    color: "#ffffff"
                                    font.pointSize: 9
                                }
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 6
                                spacing: 6
                                Text {
                                    Layout.fillWidth: true
                                    leftPadding: 2
                                    rightPadding: 2
                                    text: (model.effect ? "✨ " : "") + model.name
                                    color: "#ffffff"
                                    font.pointSize: 9
                                    elide: Text.ElideRight
                                }
                                ToolButton {
                                    text: "◀"
                                    font.pointSize: 8
                                    implicitWidth: 36; implicitHeight: 32
                                    Accessible.name: qsTr("Move clip earlier")
                                    visible: timelineRoot.selectedClipIndex === index && v1ClipDelegate.width > 140
                                    enabled: index > 0
                                    ToolTip.text: qsTr("Move clip earlier")
                                    ToolTip.visible: hovered
                                    onClicked: {
                                        timelineEngine.moveTimelineClip(index, index - 1);
                                        timelineRoot.selectedClipIndex = index - 1;
                                    }
                                }
                                ToolButton {
                                    text: "▶"
                                    font.pointSize: 8
                                    implicitWidth: 36; implicitHeight: 32
                                    Accessible.name: qsTr("Move clip later")
                                    visible: timelineRoot.selectedClipIndex === index && v1ClipDelegate.width > 140
                                    enabled: index < timelineEngine.timelineClips.length - 1
                                    ToolTip.text: qsTr("Move clip later")
                                    ToolTip.visible: hovered
                                    onClicked: {
                                        timelineEngine.moveTimelineClip(index, index + 1);
                                        timelineRoot.selectedClipIndex = index + 1;
                                    }
                                }
                                ToolButton {
                                    text: "✕"
                                    font.pointSize: 8
                                    implicitWidth: 36; implicitHeight: 32
                                    Accessible.name: qsTr("Remove clip")
                                    ToolTip.text: qsTr("Remove clip")
                                    ToolTip.visible: hovered
                                    onClicked: {
                                        if (timelineRoot.selectedClipIndex === index) {
                                            timelineRoot.selectedClipIndex = -1;
                                        }
                                        timelineEngine.removeTimelineClip(index);
                                    }
                                }
                            }
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Drop Bin clips here")
                        color: "#c7c7c7"
                        font.pointSize: 9
                        visible: timelineEngine.timelineClips.length === 0
                    }

                    DropArea {
                        id: timelineDropArea
                        anchors.fill: parent
                        keys: ["application/x-primoreels-clip"]
                        onDropped: function(drop) {
                            var path = drop.getDataAsString("application/x-primoreels-clip");
                            if (path && path.length > 0) {
                                var idx = timelineRoot.dropIndexAt(drop.x);
                                timelineEngine.insertClipToTimeline(path, idx);
                                timelineRoot.selectedClipIndex = Math.min(idx, timelineEngine.timelineClips.length - 1);
                            }
                        }
                    }
                }

                // Video Track 2 (overlay clips, absolute positioning)
                Rectangle {
                    id: videoTrack2
                    y: 95
                    width: timelineFlick.contentWidth
                    height: 45
                    color: overlayDropArea.containsDrag ? "#3a3a3a" : "#262626"
                    border.color: overlayDropArea.containsDrag ? "#9a6a00" : "#3a3a3a"

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.margins: 6
                        width: video2Label.implicitWidth + 12
                        height: video2Label.implicitHeight + 6
                        color: "#1e1e1e"
                        opacity: 0.85
                        radius: 3
                        z: 2
                        Text {
                            id: video2Label
                            anchors.centerIn: parent
                            text: qsTr("Video 2 (Overlay)")
                            color: "#cccccc"
                            font.pointSize: 9
                        }
                    }

                    Repeater {
                        model: timelineEngine.timelineModel
                        delegate: Rectangle {
                            id: overlayClipDelegate
                            visible: (model.track || 0) === 1
                            y: 14
                            width: Math.max(4, model.duration * pixelsPerSecond - 4)
                            height: 28
                            color: model.path === timelineEngine.currentSource ? "#0098ff" : "#6a4d8a"
                            radius: 4
                            border.color: timelineRoot.selectedClipIndex === index ? "#ffaa00" : "#4a3560"
                            border.width: timelineRoot.selectedClipIndex === index ? 2 : 1
                            Accessible.role: Accessible.Button
                            Accessible.name: model.name + qsTr(", overlay clip")
                            // DragHandler mutates x directly, which would
                            // destroy a plain x: binding. The Binding element
                            // reapplies the computed position whenever the
                            // drag is not active.
                            Binding {
                                target: overlayClipDelegate
                                property: "x"
                                value: 6 + model.startTime * pixelsPerSecond
                                when: !overlayDragHandler.active
                            }

                            Image {
                                anchors.fill: parent
                                anchors.margins: 2
                                fillMode: Image.PreserveAspectCrop
                                opacity: 0.25
                                asynchronous: true
                                cache: false
                                visible: pixelsPerSecond >= 40 && model.thumbTick >= 0
                                         && timelineEngine.hasThumb(model.path)
                                source: visible ? ("image://thumbs/" + encodeURIComponent(model.path) + "/" + model.thumbTick) : ""
                            }

                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                activeFocusOnTab: true
                                Accessible.role: Accessible.Button
                                Accessible.name: model.name
                                onClicked: function(mouse) {
                                    if (mouse.button === Qt.RightButton) {
                                        timelineRoot.contextClipIndex = index;
                                        clipContextMenu.popup();
                                    } else {
                                        timelineFlick.forceActiveFocus();
                                        timelineRoot.selectedClipIndex = index;
                                        timelineEngine.previewTimelineClip(index);
                                    }
                                }
                                Keys.onReturnPressed: {
                                    timelineRoot.selectedClipIndex = index;
                                    timelineEngine.previewTimelineClip(index);
                                }
                                Keys.onEnterPressed: {
                                    timelineRoot.selectedClipIndex = index;
                                    timelineEngine.previewTimelineClip(index);
                                }
                                Keys.onMenuPressed: {
                                    timelineRoot.contextClipIndex = index;
                                    clipContextMenu.popup();
                                }
                                Keys.onDeletePressed: {
                                    if (timelineRoot.selectedClipIndex === index) {
                                        timelineRoot.selectedClipIndex = -1;
                                    }
                                    timelineEngine.removeTimelineClip(index);
                                }
                            }

                            DragHandler {
                                id: overlayDragHandler
                                target: parent
                                xAxis.enabled: true
                                yAxis.enabled: false
                                onActiveChanged: {
                                    if (!active) {
                                        var newStart = Math.max(0, (parent.x - trackOrigin) / pixelsPerSecond);
                                        timelineEngine.setClipStartTime(index, newStart);
                                    }
                                }
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 6
                                spacing: 6
                                Text {
                                    Layout.fillWidth: true
                                    leftPadding: 2
                                    rightPadding: 2
                                    text: (model.effect ? "✨ " : "") + model.name
                                    color: "#ffffff"
                                    font.pointSize: 8
                                    elide: Text.ElideRight
                                }
                                ToolButton {
                                    text: "✕"
                                    font.pointSize: 8
                                    implicitWidth: 36; implicitHeight: 32
                                    Accessible.name: qsTr("Remove overlay clip")
                                    ToolTip.text: qsTr("Remove clip")
                                    ToolTip.visible: hovered
                                    onClicked: {
                                        if (timelineRoot.selectedClipIndex === index) {
                                            timelineRoot.selectedClipIndex = -1;
                                        }
                                        timelineEngine.removeTimelineClip(index);
                                    }
                                }
                            }
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Drop overlay clips here")
                        color: "#c7c7c7"
                        font.pointSize: 8
                        visible: !hasOverlayClips()
                    }

                    DropArea {
                        id: overlayDropArea
                        anchors.fill: parent
                        keys: ["application/x-primoreels-clip"]
                        onDropped: function(drop) {
                            var path = drop.getDataAsString("application/x-primoreels-clip");
                            if (path && path.length > 0) {
                                var t = Math.max(0, (drop.x - trackOrigin) / pixelsPerSecond);
                                timelineEngine.appendOverlayClip(path, t);
                                timelineRoot.selectedClipIndex = timelineEngine.timelineClips.length - 1;
                            }
                        }
                    }
                }

                // Audio Track 1 (per-clip waveforms follow the video layout)
                Rectangle {
                    y: 145
                    width: timelineFlick.contentWidth
                    height: 50
                    color: "#252525"
                    border.color: "#3a3a3a"

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.margins: 6
                        width: audio1Label.implicitWidth + 12
                        height: audio1Label.implicitHeight + 6
                        color: "#1e1e1e"
                        opacity: 0.85
                        radius: 3
                        z: 2
                        Text {
                            id: audio1Label
                            anchors.centerIn: parent
                            text: qsTr("Audio 1")
                            color: "#cccccc"
                            font.pointSize: 9
                        }
                    }

                    Repeater {
                        model: timelineEngine.timelineModel
                        delegate: Rectangle {
                            id: audioClipDelegate
                            visible: (model.track || 0) === 0 || (model.track || 0) === 2
                            y: 12
                            width: Math.max(4, model.duration * pixelsPerSecond - 4)
                            height: 33
                            color: (model.track || 0) === 2 ? "#1f6f7a" : "#2d8a4e"
                            opacity: model.muted ? 0.45 : 1.0
                            radius: 4
                            clip: true
                            border.color: timelineRoot.selectedClipIndex === index ? "#ffaa00" : "transparent"
                            border.width: timelineRoot.selectedClipIndex === index ? 2 : 0
                            // DragHandler mutates x directly, which would
                            // destroy a plain x: binding (same pattern as the
                            // V2 overlay delegate above). Track-0 mirror rows
                            // are never draggable.
                            Binding {
                                target: audioClipDelegate
                                property: "x"
                                value: 6 + model.startTime * pixelsPerSecond
                                when: !audioDragHandler.active
                            }
                            // Text alternative for the painted waveform
                            // (Canvas itself is not accessible).
                            Accessible.role: (model.track || 0) === 2 ? Accessible.Button : Accessible.Graphic
                            Accessible.name: model.name + ((model.track || 0) === 2 ? qsTr(", audio bed clip") : qsTr(", audio waveform"))
                            Accessible.description: timelineEngine.hasWaveform(model.path)
                                ? qsTr("Audio waveform, %1 peaks").arg(timelineEngine.waveform(model.path).length)
                                : qsTr("Audio waveform loading")

                            // Role-backed path; request once per row, repaint
                            // on data + per-row waveform arrival tick.
                            property string wavePath: model.path || ""
                            property int waveTick: model.waveTick || 0
                            onWavePathChanged: {
                                if (wavePath.length > 0) {
                                    timelineEngine.requestWaveform(wavePath);
                                }
                                waveCanvas.requestPaint();
                            }
                            // Per-row tick: only this canvas repaints when
                            // its waveform peaks arrive (no global storm).
                            onWaveTickChanged: waveCanvas.requestPaint()
                            Component.onCompleted: {
                                if (model.path) {
                                    timelineEngine.requestWaveform(model.path);
                                }
                                waveCanvas.requestPaint();
                            }
                            onWidthChanged: waveCanvas.requestPaint()

                            Canvas {
                                id: waveCanvas
                                anchors.fill: parent
                                anchors.margins: 5
                                visible: parent.width > 12
                                         && waveTick >= 0
                                         && timelineEngine.hasWaveform(model.path)
                                onPaint: {
                                    var ctx = getContext("2d");
                                    ctx.clearRect(0, 0, width, height);
                                    var peaks = timelineEngine.waveform(model.path);
                                    if (!peaks || peaks.length === 0) {
                                        return;
                                    }
                                    ctx.fillStyle = "#bfe8c9";
                                    var n = peaks.length;
                                    var barW = Math.max(1, width / n);
                                    for (var i = 0; i < n; ++i) {
                                        var h = Math.max(1, peaks[i] * height);
                                        var y = (height - h) / 2;
                                        ctx.fillRect(i * barW, y, Math.max(1, barW - 0.5), h);
                                    }
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                activeFocusOnTab: true
                                Accessible.role: Accessible.Button
                                Accessible.name: model.name
                                onClicked: function(mouse) {
                                    if (mouse.button === Qt.RightButton) {
                                        timelineRoot.contextClipIndex = index;
                                        clipContextMenu.popup();
                                    } else {
                                        timelineFlick.forceActiveFocus();
                                        timelineRoot.selectedClipIndex = index;
                                        timelineEngine.previewTimelineClip(index);
                                    }
                                }
                                // Keyboard alternative to mouse: Enter previews,
                                // Menu key opens the context menu, Delete removes.
                                Keys.onReturnPressed: {
                                    timelineRoot.selectedClipIndex = index;
                                    timelineEngine.previewTimelineClip(index);
                                }
                                Keys.onEnterPressed: {
                                    timelineRoot.selectedClipIndex = index;
                                    timelineEngine.previewTimelineClip(index);
                                }
                                Keys.onMenuPressed: {
                                    timelineRoot.contextClipIndex = index;
                                    clipContextMenu.popup();
                                }
                                Keys.onDeletePressed: {
                                    if (timelineRoot.selectedClipIndex === index) {
                                        timelineRoot.selectedClipIndex = -1;
                                    }
                                    timelineEngine.removeTimelineClip(index);
                                }
                            }

                            DragHandler {
                                id: audioDragHandler
                                target: parent
                                xAxis.enabled: true
                                yAxis.enabled: false
                                // Only real bed rows move; V1 mirror rows stay
                                // put (the engine also ignores track 0 here).
                                enabled: (model.track || 0) === 2
                                onActiveChanged: {
                                    if (!active) {
                                        var newStart = Math.max(0, (parent.x - trackOrigin) / pixelsPerSecond);
                                        timelineEngine.setClipStartTime(index, newStart);
                                    }
                                }
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 6
                                spacing: 6
                                visible: (model.track || 0) === 2
                                Text {
                                    Layout.fillWidth: true
                                    leftPadding: 2
                                    rightPadding: 2
                                    text: model.name
                                    color: "#ffffff"
                                    font.pointSize: 8
                                    elide: Text.ElideRight
                                }
                                ToolButton {
                                    text: "✕"
                                    font.pointSize: 8
                                    implicitWidth: 36; implicitHeight: 32
                                    Accessible.name: qsTr("Remove audio clip")
                                    ToolTip.text: qsTr("Remove clip")
                                    ToolTip.visible: hovered
                                    onClicked: {
                                        if (timelineRoot.selectedClipIndex === index) {
                                            timelineRoot.selectedClipIndex = -1;
                                        }
                                        timelineEngine.removeTimelineClip(index);
                                    }
                                }
                            }
                        }
                    }

                    DropArea {
                        id: audioDropArea
                        anchors.fill: parent
                        keys: ["application/x-primoreels-clip"]
                        onDropped: function(drop) {
                            var path = drop.getDataAsString("application/x-primoreels-clip");
                            if (path && path.length > 0) {
                                var t = Math.max(0, (drop.x - trackOrigin) / pixelsPerSecond);
                                timelineEngine.appendAudioClip(path, t);
                                timelineRoot.selectedClipIndex = timelineEngine.timelineClips.length - 1;
                            }
                        }
                    }
                }
                // Playhead indicator line (draggable)
                Rectangle {
                    x: trackOrigin + timelineEngine.position * pixelsPerSecond
                    y: 0
                    width: 2
                    // Cover scrolled tracks, not just the viewport.
                    height: timelineFlick.contentHeight
                    color: "#ff6b6b"
                    z: 10

                    Rectangle {
                        x: -9
                        y: 0
                        width: 20
                        height: 20
                        color: "#ff6b6b"
                        border.color: "#ffffff"
                        border.width: 1
                        rotation: 45
                    }

                    MouseArea {
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: 0
                        width: 32
                        height: parent.height
                        cursorShape: Qt.SplitHCursor
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Slider
                        Accessible.name: qsTr("Playhead. Left and right arrows seek.")
                        function scrubTo(mouseX) {
                            // MouseArea is centered on the playhead line:
                            // center (width/2) maps to parent.x.
                            var t = (parent.x + (mouseX - width / 2) - trackOrigin) / pixelsPerSecond;
                            timelineEngine.seek(Math.max(0, Math.min(t, timelineDuration)));
                        }
                        onPressed: function(mouse) { scrubTo(mouse.x); }
                        onPositionChanged: function(mouse) {
                            if (pressed) {
                                scrubTo(mouse.x);
                            }
                        }
                        // Only a subset of keys has Keys.on<Name>Pressed
                        // convenience signals (no Home/End/Insert); handle
                        // navigation keys via the generic handler.
                        Keys.onPressed: function(event) {
                            if (event.key === Qt.Key_Left) {
                                timelineEngine.seek(Math.max(0, timelineEngine.position - 1));
                                event.accepted = true;
                            } else if (event.key === Qt.Key_Right) {
                                timelineEngine.seek(Math.min(timelineDuration, timelineEngine.position + 1));
                                event.accepted = true;
                            } else if (event.key === Qt.Key_Home) {
                                timelineEngine.seek(0);
                                event.accepted = true;
                            } else if (event.key === Qt.Key_End) {
                                timelineEngine.seek(timelineDuration);
                                event.accepted = true;
                            }
                        }
                    }
                }
            }
        }
    }
}
