import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import "TimeUtils.js" as TimeUtils

Rectangle {
    id: libraryRoot
    objectName: "libraryRoot"
    color: "#181818"
    border.color: "#333333"
    border.width: 1

    property string selectedPath: ""
    // Active tab persisted via Main.appSettings.libraryTab.
    property alias activeTab: libraryTabBar.currentIndex
    property string selectedEffect: ""
    property string selectedTrack: ""
    property string selectedTransition: ""
    // Cache C++ catalogs once (function-call models re-evaluate on
    // every polish; static lists never change at runtime).
    property var effectCatalog: []
    property var transitionCatalog: []
    Component.onCompleted: {
        effectCatalog = timelineEngine.availableEffects();
        transitionCatalog = timelineEngine.availableTransitions();
    }

    signal effectSelected(string name)
    signal effectApplied(string name)
    signal trackSelected(string name)
    signal trackAttached(string path)
    signal transitionSelected(string name)
    signal transitionApplied(string name)

    function effectDescription(name) {
        if (name === "Blur") {
            return qsTr("Gaussian blur effect");
        }
        if (name === "Color Correction") {
            return qsTr("Adjust brightness, contrast, saturation");
        }
        if (name === "Sharpen") {
            return qsTr("Enhance edge definition");
        }
        if (name === "Vignette") {
            return qsTr("Darkened corner overlay");
        }
        if (name === "Glitch") {
            return qsTr("Digital artifact distortion");
        }
        if (name === "Sepia") {
            return qsTr("Warm vintage sepia tone");
        }
        if (name === "Grayscale") {
            return qsTr("Black-and-white conversion");
        }
        if (name === "Invert") {
            return qsTr("Inverted colors");
        }
        return "";
    }

    function transitionDescription(name) {
        if (name === "Cross Dissolve") {
            return qsTr("Fade between clips");
        }
        if (name === "Dip to Black") {
            return qsTr("Fade to black and back");
        }
        return "";
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        // Library Category Tabs
        TabBar {
            id: libraryTabBar
            Layout.fillWidth: true
            background: Rectangle { color: "#222222" }

            TabButton {
                id: binTab
                text: qsTr("Bin")
                Accessible.name: qsTr("Project bin tab")
                contentItem: Text {
                    text: binTab.text
                    color: libraryTabBar.currentIndex === 0 || binTab.visualFocus || binTab.hovered ? "#ffffff" : "#cccccc"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pointSize: 9
                    font.bold: binTab.down || libraryTabBar.currentIndex === 0
                }
            }
            TabButton {
                id: effectsTab
                text: qsTr("Effects")
                Accessible.name: qsTr("Effects tab")
                contentItem: Text {
                    text: effectsTab.text
                    color: libraryTabBar.currentIndex === 1 || effectsTab.visualFocus || effectsTab.hovered ? "#ffffff" : "#cccccc"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pointSize: 9
                    font.bold: effectsTab.down || libraryTabBar.currentIndex === 1
                }
            }
            TabButton {
                id: audioTab
                text: qsTr("Audio")
                Accessible.name: qsTr("Audio tab")
                contentItem: Text {
                    text: audioTab.text
                    color: libraryTabBar.currentIndex === 2 || audioTab.visualFocus || audioTab.hovered ? "#ffffff" : "#cccccc"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pointSize: 9
                    font.bold: audioTab.down || libraryTabBar.currentIndex === 2
                }
            }
            TabButton {
                id: transitionsTab
                text: qsTr("Transitions")
                Accessible.name: qsTr("Transitions tab")
                contentItem: Text {
                    text: transitionsTab.text
                    color: libraryTabBar.currentIndex === 3 || transitionsTab.visualFocus || transitionsTab.hovered ? "#ffffff" : "#cccccc"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pointSize: 9
                    font.bold: transitionsTab.down || libraryTabBar.currentIndex === 3
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: libraryTabBar.currentIndex

            // Tab 0: Project Bin
            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 5
                    spacing: 5

                    RowLayout {
                        Layout.fillWidth: true

                        TextField {
                            id: searchField
                            Layout.fillWidth: true
                            placeholderText: qsTr("Search media...")
                            Accessible.name: qsTr("Search media")
                            Accessible.description: qsTr("Filter bin items by name")
                            font.pointSize: 9
                            color: "#ffffff"
                            background: Rectangle {
                                color: "#252526"
                                border.color: "#333333"
                                radius: 3
                            }
                            // Debounced C++ proxy filtering; no per-keystroke
                            // storm on 1k+ item bins.
                            onTextChanged: searchDebounce.restart()
                        }
                        Timer {
                            id: searchDebounce
                            interval: 150
                            repeat: false
                            onTriggered: timelineEngine.mediaFilterModel.filterText = searchField.text
                        }

                        Button {
                            text: qsTr("Import")
                            font.pointSize: 9
                            Accessible.name: qsTr("Import media to bin")
                            onClicked: binFileDialog.open()
                        }

                        Button {
                            text: qsTr("Clear")
                            font.pointSize: 9
                            Accessible.name: qsTr("Clear bin")
                            enabled: timelineEngine.mediaList.length > 0
                            onClicked: timelineEngine.clearMedia()
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: timelineEngine.mediaList.length === 0
                              ? qsTr("Bin is empty")
                              : binListView.count === timelineEngine.mediaList.length
                                ? qsTr("%n item(s)", "", timelineEngine.mediaList.length)
                                : qsTr("%1 of %2 shown").arg(binListView.count).arg(timelineEngine.mediaList.length)
                        color: "#cccccc"
                        font.pointSize: 8
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: osDropArea.containsDrag ? "#2a3a2a" : "transparent"
                        border.color: osDropArea.containsDrag ? "#2d8a4e" : "transparent"

                        ListView {
                            id: binListView
                            anchors.fill: parent
                            clip: true
                            // C++ model: delegates recycle, filter in proxy.
                            model: timelineEngine.mediaFilterModel
                        delegate: ItemDelegate {
                            id: binDelegate
                            width: binListView.width
                            height: 35
                            // Role-backed; model.path notifies via dataChanged.
                            property string binPath: model.path || ""
                            onBinPathChanged: {
                                if (binPath.length > 0) {
                                    timelineEngine.requestThumb(binPath);
                                }
                            }
                            highlighted: (model.path === timelineEngine.currentSource)
                                            || (model.path === selectedPath)
                            Accessible.role: Accessible.ListItem
                            Accessible.name: model.name + ", " + TimeUtils.formatDuration(model.duration)
                            Accessible.description: qsTr("Bin item. Enter previews in Source, Insert adds to timeline, Delete removes.")
                            Drag.active: binDragHandler.active
                            Drag.mimeData: { "application/x-primoreels-clip": model.path }
                            Drag.dragType: Drag.Automatic
                            Drag.supportedActions: Qt.CopyAction
                            contentItem: RowLayout {
                                spacing: 4
                                Image {
                                    Layout.preferredWidth: 48
                                    Layout.preferredHeight: 27
                                    Layout.alignment: Qt.AlignVCenter
                                    fillMode: Image.PreserveAspectCrop
                                    asynchronous: true
                                    cache: false
                                    // Per-row tick: only this delegate reloads
                                    // when its thumbnail arrives (the tick in
                                    // the expression re-triggers hasThumb()).
                                    visible: model.thumbTick >= 0
                                             && timelineEngine.hasThumb(model.path)
                                    source: visible
                                            ? "image://thumbs/" + encodeURIComponent(model.path)
                                              + "/" + model.thumbTick
                                            : ""
                                }
                                Label {
                                    // Emoji is decorative; screen readers use
                                    // the delegate Accessible.name instead.
                                    text: "🎬 " + model.name
                                    color: "#ffffff"
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    font.pointSize: 9
                                }
                                Label {
                                    text: TimeUtils.formatDuration(model.duration)
                                    color: "#cccccc"
                                    font.pointSize: 8
                                }
                                ToolButton {
                                    text: "+"
                                    font.pointSize: 9
                                    implicitWidth: 36; implicitHeight: 32
                                    Accessible.name: qsTr("Add to Timeline")
                                    ToolTip.text: qsTr("Add to Timeline")
                                    ToolTip.visible: hovered
                                    onClicked: timelineEngine.appendClipToTimeline(model.path)
                                }
                                ToolButton {
                                    text: "✕"
                                    font.pointSize: 9
                                    implicitWidth: 36; implicitHeight: 32
                                    Accessible.name: qsTr("Remove from Bin")
                                    ToolTip.text: qsTr("Remove from Bin")
                                    ToolTip.visible: hovered
                                    onClicked: timelineEngine.removeMedia(model.path)
                                }
                            }
                            DragHandler {
                                id: binDragHandler
                                target: null
                            }
                            Component.onCompleted: {
                                if (model.path) {
                                    timelineEngine.requestThumb(model.path);
                                }
                            }
                            onClicked: {
                                selectedPath = model.path;
                            }
                            onDoubleClicked: {
                                // Preview in the Source monitor; the Program
                                // monitor keeps whatever it was playing.
                                selectedPath = model.path;
                                timelineEngine.loadSource(model.path);
                            }
                            // Keyboard alternative to click/double-click/drag:
                            // Enter previews in Source, Insert appends to the
                            // timeline, Delete removes from the bin.
                            Keys.onReturnPressed: {
                                selectedPath = model.path;
                                timelineEngine.loadSource(model.path);
                            }
                            Keys.onEnterPressed: {
                                selectedPath = model.path;
                                timelineEngine.loadSource(model.path);
                            }
                            // No Keys.onInsertPressed convenience signal exists;
                            // handle the Insert key via the generic handler.
                            Keys.onPressed: function(event) {
                                if (event.key === Qt.Key_Insert) {
                                    timelineEngine.appendClipToTimeline(model.path);
                                    event.accepted = true;
                                }
                            }
                            Keys.onDeletePressed: timelineEngine.removeMedia(model.path)
                        }
                        }

                        DropArea {
                            id: osDropArea
                            anchors.fill: parent
                            keys: ["text/uri-list"]
                            onDropped: function(drop) {
                                for (var i = 0; i < drop.urls.length; ++i) {
                                    timelineEngine.importMedia(drop.urls[i].toString());
                                }
                            }
                        }

                        Text {
                            anchors.centerIn: parent
                            text: qsTr("Drop video files here")
                            color: "#2d8a4e"
                            font.pointSize: 10
                            visible: osDropArea.containsDrag
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: timelineEngine.mediaList.length === 0
                              ? qsTr("No media in bin.\nClick Import to add videos.\nDouble-click an item to preview it in Source.\nDrag or press + to add to Timeline.")
                              : qsTr("No matches for \"%1\".").arg(searchField.text)
                        color: "#c7c7c7"
                        horizontalAlignment: Text.AlignHCenter
                        font.pointSize: 9
                        visible: timelineEngine.mediaList.length === 0
                                 || binListView.count === 0
                    }
                }
            }

            // Tab 1: Effects Library (names from C++ single source of truth).
            Item {
                ListView {
                    anchors.fill: parent
                    anchors.margins: 5
                    clip: true
                    model: libraryRoot.effectCatalog
                    delegate: ItemDelegate {
                        width: ListView.view.width
                        height: 45
                        highlighted: modelData === selectedEffect
                        Accessible.role: Accessible.ListItem
                        Accessible.name: modelData + ". " + libraryRoot.effectDescription(modelData)
                        contentItem: ColumnLayout {
                            spacing: 2
                            Label {
                                text: "✨ " + modelData
                                color: "#ffffff"
                                font.pointSize: 9
                                font.bold: true
                            }
                            Label {
                                text: libraryRoot.effectDescription(modelData)
                                color: "#cccccc"
                                font.pointSize: 8
                            }
                        }
                        onClicked: {
                            selectedEffect = (selectedEffect === modelData) ? "" : modelData;
                            effectSelected(selectedEffect);
                        }
                        onDoubleClicked: {
                            selectedEffect = modelData;
                            effectSelected(selectedEffect);
                            effectApplied(modelData);
                        }
                    }
                }
            }

            // Tab 2: Audio Library (bin files; double-click attaches to A1)
            Item {
                ListView {
                    anchors.fill: parent
                    anchors.margins: 5
                    clip: true
                    model: timelineEngine.mediaList
                    delegate: ItemDelegate {
                        width: ListView.view.width
                        height: 35
                        highlighted: (modelData.name || "") === selectedTrack
                        Accessible.role: Accessible.ListItem
                        Accessible.name: (modelData.name || "") + qsTr(", double-click to attach to A1 at the playhead")
                        Accessible.description: qsTr("Audio source. Double-click attaches it to the A1 bed at the playhead.")
                        contentItem: RowLayout {
                            Label {
                                text: "🎵 " + (modelData.name || "")
                                color: "#ffffff"
                                Layout.fillWidth: true
                                font.pointSize: 9
                                elide: Text.ElideRight
                            }
                            Label {
                                text: TimeUtils.formatTime(modelData.duration || 0)
                                color: "#cccccc"
                                font.pointSize: 8
                            }
                        }
                        onClicked: {
                            var nm = modelData.name || "";
                            selectedTrack = (selectedTrack === nm) ? "" : nm;
                            trackSelected(selectedTrack);
                        }
                        onDoubleClicked: {
                            if (modelData.path) {
                                trackAttached(modelData.path);
                            }
                        }
                    }
                }
            }

            // Tab 3: Transitions Library (names from C++; only
            // implemented transitions are listed to avoid dead-ends).
            Item {
                ListView {
                    anchors.fill: parent
                    anchors.margins: 5
                    clip: true
                    model: libraryRoot.transitionCatalog
                    delegate: ItemDelegate {
                        width: ListView.view.width
                        height: 40
                        highlighted: modelData === selectedTransition
                        Accessible.role: Accessible.ListItem
                        Accessible.name: modelData + ". " + libraryRoot.transitionDescription(modelData)
                        contentItem: ColumnLayout {
                            spacing: 2
                            Label {
                                text: "🎞️ " + modelData
                                color: "#ffffff"
                                font.pointSize: 9
                                font.bold: true
                            }
                            Label {
                                text: libraryRoot.transitionDescription(modelData)
                                color: "#cccccc"
                                font.pointSize: 8
                            }
                        }
                        onClicked: {
                            selectedTransition = (selectedTransition === modelData) ? "" : modelData;
                            transitionSelected(selectedTransition);
                        }
                        onDoubleClicked: {
                            selectedTransition = modelData;
                            transitionSelected(selectedTransition);
                            transitionApplied(modelData);
                        }
                    }
                }
            }
        }
    }

    FileDialog {
        id: binFileDialog
        title: qsTr("Choose Videos")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("Video files (*.mp4 *.mkv *.avi *.mov *.webm)"), qsTr("All files (*)")]
        onAccepted: {
            for (var i = 0; i < selectedFiles.length; ++i) {
                timelineEngine.importMedia(selectedFiles[i].toString());
            }
        }
    }
}
