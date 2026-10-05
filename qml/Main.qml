import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQuick.Window
import QtCore

ApplicationWindow {
    id: root
    width: 1280
    height: 720
    visible: true
    // Reactive title: plain property binding (not a function call) so
    // isModified/currentProjectPath changes always re-evaluate.
    property string displayName: {
        if (currentProjectPath.length === 0) {
            return qsTr("Untitled");
        }
        var filename = String(currentProjectPath).split('/').pop();
        try {
            return decodeURIComponent(filename);
        } catch (e) {
            return filename;
        }
    }
    //: Window title suffix; %1 is the project name with optional dirty dot.
    title: qsTr("%1 — PrimoReels").arg(displayName + (timelineEngine.isModified ? " •" : ""))
    color: "#1e1e1e"

    property string currentProjectPath: ""
    property string pendingOpenPath: ""
    property bool pendingQuit: false
    property bool pendingNew: false
    property bool quittingAllowed: false
    // While true, saveViewState() is a no-op: the transient width/height
    // events fired by restoreViewState() itself (window resize, SplitView
    // redistribution) must not overwrite the persisted layout they restore.
    property bool restoringLayout: false

    function doNewProject() {
        timelineEngine.newProject();
        currentProjectPath = "";
        pendingNew = false;
        flashStatus(qsTr("New project."))
    }

    function requestNew() {
        if (timelineEngine.isModified) {
            pendingOpenPath = "";
            pendingQuit = false;
            pendingNew = true;
            unsavedChangesDialog.open();
        } else {
            doNewProject();
        }
    }

    // Kept for toolbar/menu callers; prefer displayName property for bindings.
    function projectDisplayName() {
        return displayName;
    }

    function addRecentProject(path) {
        var recents = appSettings.recentProjects.slice();
        var idx = recents.indexOf(path);
        if (idx !== -1) {
            recents.splice(idx, 1);
        }
        recents.unshift(path);
        appSettings.recentProjects = recents.slice(0, 8);
    }

    function openProjectPath(path) {
        if (timelineEngine.loadProject(path)) {
            currentProjectPath = path;
            addRecentProject(path);
            appSettings.lastProjectPath = path;
        }
    }

    // Startup behavior (Options dialog): reopen the previous project when
    // configured, otherwise start untitled (default). A stale/missing path
    // is forgotten so the next launch does not retry a dead entry.
    function applyStartupMode() {
        if (appSettings.startupMode !== "last") {
            return;
        }
        var path = appSettings.lastProjectPath;
        if (path && path.length > 0) {
            if (timelineEngine.loadProject(path)) {
                currentProjectPath = path;
                addRecentProject(path);
                return;
            }
            appSettings.lastProjectPath = "";
            root.showError(qsTr("Could not load previous project; starting untitled."));
        }
    }

    function requestOpen(path) {
        if (timelineEngine.isModified) {
            pendingOpenPath = path;
            pendingQuit = false;
            unsavedChangesDialog.open();
        } else {
            openProjectPath(path);
        }
    }

    function requestQuit() {
        if (quittingAllowed) {
            Qt.quit();
            return;
        }
        if (timelineEngine.isModified) {
            pendingOpenPath = "";
            pendingQuit = true;
            unsavedChangesDialog.open();
        } else {
            quittingAllowed = true;
            Qt.quit();
        }
    }

    function togglePlayback() {
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

    function isTextInputFocused() {
        var item = root.activeFocusItem;
        return item !== null && (item instanceof TextField || item instanceof TextInput
            || item instanceof TextArea);
    }

    onClosing: function(close) {
        // Don't persist geometry when the close is vetoed (user pressed
        // Cancel in the unsaved-changes dialog); save only on real quit.
        if (!quittingAllowed && timelineEngine.isModified) {
            close.accepted = false;
            requestQuit();
            return;
        }
        // A real quit always persists: drop the restore guard (a close
        // within 600 ms of launch would otherwise skip the save, and the
        // settings already hold the restored values anyway).
        root.restoringLayout = false;
        restoreGuardTimer.stop();
        root.saveViewState();
    }

    onWidthChanged: viewSaveTimer.restart()
    onHeightChanged: viewSaveTimer.restart()
    onVisibilityChanged: viewSaveTimer.restart()

    Settings {
        id: appSettings
        objectName: "appSettings"
        property var recentProjects: []
        // Startup behavior: "new" (untitled, default) or "last" (reopen).
        property string startupMode: "new"
        property string lastProjectPath: ""
        // View layout (persisted between runs; defaults = first-run layout).
        property bool showLibrary: true
        property bool showSource: true
        property bool showProgram: true
        property bool showInspector: true
        property bool showTimeline: true
        property real timelineZoom: 100
        property int libraryTab: 0
        property real libraryWidth: 250
        property real sourceWidth: 300
        property real programWidth: 500
        property real inspectorWidth: 230
        property real timelineHeight: 250
        property real winWidth: 1280
        property real winHeight: 720
        property int winVisibility: Window.Windowed
    }

    // Debounced writer: drag-resizing fires width/height changes per pixel.
    Timer {
        id: viewSaveTimer
        interval: 500
        repeat: false
        onTriggered: root.saveViewState()
    }

    // Clears restoringLayout once a programmatic restore has settled. Must
    // outlive viewSaveTimer (500 ms) so any debounced save fired by
    // restore-driven resizes is still suppressed when it runs.
    Timer {
        id: restoreGuardTimer
        interval: 600
        repeat: false
        onTriggered: root.restoringLayout = false
    }

    function clampedZoom(z) { return Math.max(10, Math.min(800, z || 100)); }

    function saveViewState() {
        // Ignored while a programmatic restore is settling (see
        // restoreGuardTimer): startup resize/SplitView transients must not
        // overwrite the persisted layout they just restored.
        if (root.restoringLayout) {
            return;
        }
        appSettings.showLibrary = libraryPanel.visible;
        appSettings.showSource = sourceMonitorPanel.visible;
        appSettings.showProgram = programMonitorPanel.visible;
        appSettings.showInspector = inspectorPanel.visible;
        appSettings.showTimeline = timelinePanel.visible;
        appSettings.timelineZoom = timelinePanel.pixelsPerSecond;
        appSettings.libraryTab = libraryPanel.activeTab;
        if (libraryPanel.visible) {
            appSettings.libraryWidth = libraryPanel.width;
        }
        if (sourceMonitorPanel.visible) {
            appSettings.sourceWidth = sourceMonitorPanel.width;
        }
        if (programMonitorPanel.visible) {
            appSettings.programWidth = programMonitorPanel.width;
        }
        if (inspectorPanel.visible) {
            appSettings.inspectorWidth = inspectorPanel.width;
        }
        if (timelinePanel.visible) {
            appSettings.timelineHeight = timelinePanel.height;
        }
        // Window state: normal geometry only when windowed (a maximized
        // window reports its maximized size, not the geometry to restore
        // when un-maximized); visibility whenever it is meaningful.
        if (root.visibility === Window.Windowed) {
            appSettings.winWidth = root.width;
            appSettings.winHeight = root.height;
        }
        if (root.visibility === Window.Windowed || root.visibility === Window.Maximized
                || root.visibility === Window.FullScreen) {
            appSettings.winVisibility = root.visibility;
        }
    }

    function restoreViewState() {
        // Suppress the debounced saver while the programmatic restore
        // settles; cleared by restoreGuardTimer (longer than viewSaveTimer).
        root.restoringLayout = true;
        restoreGuardTimer.restart();
        // Sizes go through appSettings (single source of truth): the
        // SplitView.preferredWidth/Height bindings track these, so panels
        // update without breaking their bindings.
        appSettings.libraryWidth = Math.max(200, appSettings.libraryWidth || 250);
        appSettings.sourceWidth = Math.max(200, appSettings.sourceWidth || 300);
        appSettings.programWidth = Math.max(300, appSettings.programWidth || 500);
        appSettings.inspectorWidth = Math.max(180, appSettings.inspectorWidth || 230);
        appSettings.timelineHeight = Math.max(150, appSettings.timelineHeight || 250);
        root.width = Math.max(800, appSettings.winWidth || 1280);
        root.height = Math.max(500, appSettings.winHeight || 720);
        var vis = appSettings.winVisibility || Window.Windowed;
        // Assign only on a real change: touching visibility recreates the
        // window surface (visible flicker in production, frozen layout in
        // tests), so a redundant write is not free.
        if ((vis === Window.Windowed || vis === Window.Maximized || vis === Window.FullScreen)
                && root.visibility !== vis) {
            root.visibility = vis;
        }
        libraryPanel.visible = appSettings.showLibrary !== false;
        sourceMonitorPanel.visible = appSettings.showSource !== false;
        programMonitorPanel.visible = appSettings.showProgram !== false;
        inspectorPanel.visible = appSettings.showInspector !== false;
        timelinePanel.visible = appSettings.showTimeline !== false;
        timelinePanel.pixelsPerSecond = clampedZoom(appSettings.timelineZoom);
        libraryPanel.activeTab = Math.max(0, Math.min(3, appSettings.libraryTab || 0));
    }

    function resetLayout() {
        appSettings.showLibrary = true;
        appSettings.showSource = true;
        appSettings.showProgram = true;
        appSettings.showInspector = true;
        appSettings.showTimeline = true;
        appSettings.timelineZoom = 100;
        appSettings.libraryTab = 0;
        appSettings.libraryWidth = 250;
        appSettings.sourceWidth = 300;
        appSettings.programWidth = 500;
        appSettings.inspectorWidth = 230;
        appSettings.timelineHeight = 250;
        restoreViewState();
        root.flashStatus(qsTr("Layout reset to defaults."));
    }

    Component.onCompleted: {
        restoreViewState();
        applyStartupMode();
    }

    // Global Play shortcut that yields to text editing (search, titles).
    Shortcut {
        sequence: "Space"
        context: Qt.WindowShortcut
        enabled: playPauseAction.enabled && !root.isTextInputFocused()
        onActivated: root.togglePlayback()
    }

    // Global Split shortcut: same text-input guard as Space, plus
    // timeline state. Lives here (not in TimelineView) so typing "s"
    // in Bin search or clip-title fields never triggers a split.
    Shortcut {
        sequence: "S"
        context: Qt.WindowShortcut
        enabled: timelinePanel.canSplitAtPlayhead() && !root.isTextInputFocused()
        onActivated: timelinePanel.splitSelectedAtPlayhead()
    }

    menuBar: MenuBar {
        Menu {
            title: qsTr("&File")
            Action {
                text: qsTr("&New Project")
                shortcut: StandardKey.New
                onTriggered: root.requestNew()
            }
            Action {
                text: qsTr("&Import Media…")
                shortcut: StandardKey.Open
                onTriggered: fileDialog.open()
            }
            Action {
                text: qsTr("&Clear Bin")
                enabled: timelineEngine.mediaList.length > 0
                onTriggered: timelineEngine.clearMedia()
            }
            MenuSeparator {}
            Action {
                text: qsTr("&Save Project…")
                shortcut: StandardKey.Save
                onTriggered: saveProjectDialog.open()
            }
            Action {
                text: qsTr("&Open Project…")
                onTriggered: openProjectDialog.open()
            }
            Menu {
                id: recentMenu
                title: qsTr("Open &Recent")
                enabled: recentProjectsMenu.count > 0

                Instantiator {
                    id: recentProjectsMenu
                    model: appSettings.recentProjects
                    delegate: Action {
                        text: decodeURIComponent(String(modelData).split('/').pop())
                        onTriggered: root.requestOpen(modelData)
                    }
                    onObjectAdded: function(index, object) { recentMenu.insertAction(index, object) }
                    onObjectRemoved: function(index, object) { recentMenu.removeAction(object) }
                }
            }
            MenuSeparator {}
            Action {
                text: qsTr("&Export Sequence…")
                enabled: timelineEngine.timelineClips.length > 0 && !timelineEngine.isExporting
                onTriggered: exportDialog.open()
            }
            MenuSeparator {}
            Action {
                text: qsTr("&Quit")
                shortcut: StandardKey.Quit
                onTriggered: root.requestQuit()
            }
        }
        Menu {
            title: qsTr("&Edit")
            Action {
                text: qsTr("&Undo")
                shortcut: StandardKey.Undo
                enabled: timelineEngine.canUndo
                onTriggered: timelineEngine.undo()
            }
            Action {
                text: qsTr("&Redo")
                shortcut: StandardKey.Redo
                enabled: timelineEngine.canRedo
                onTriggered: timelineEngine.redo()
            }
        }
        Menu {
            title: qsTr("&View")
            Action {
                text: qsTr("Library Panel")
                checkable: true
                checked: libraryPanel.visible
                onToggled: { libraryPanel.visible = checked; appSettings.showLibrary = checked; }
            }
            Action {
                text: qsTr("Source Monitor Panel")
                checkable: true
                checked: sourceMonitorPanel.visible
                onToggled: { sourceMonitorPanel.visible = checked; appSettings.showSource = checked; }
            }
            Action {
                text: qsTr("Program Monitor Panel")
                checkable: true
                checked: programMonitorPanel.visible
                onToggled: { programMonitorPanel.visible = checked; appSettings.showProgram = checked; }
            }
            Action {
                text: qsTr("Inspector Panel")
                checkable: true
                checked: inspectorPanel.visible
                onToggled: { inspectorPanel.visible = checked; appSettings.showInspector = checked; }
            }
            Action {
                text: qsTr("Timeline Panel")
                checkable: true
                checked: timelinePanel.visible
                onToggled: { timelinePanel.visible = checked; appSettings.showTimeline = checked; }
            }
            MenuSeparator {}
            Action {
                text: qsTr("Reset Layout")
                onTriggered: root.resetLayout()
            }
            MenuSeparator {}
            Action {
                text: qsTr("Zoom Timeline In")
                shortcut: StandardKey.ZoomIn
                onTriggered: timelinePanel.pixelsPerSecond = Math.min(800, timelinePanel.pixelsPerSecond * 1.25)
            }
            Action {
                text: qsTr("Zoom Timeline Out")
                shortcut: StandardKey.ZoomOut
                onTriggered: timelinePanel.pixelsPerSecond = Math.max(10, timelinePanel.pixelsPerSecond / 1.25)
            }
        }
        Menu {
            title: qsTr("&Tools")
            Action {
                id: playPauseAction
                text: timelineEngine.isPlaying ? qsTr("&Pause") : qsTr("&Play")
                enabled: timelineEngine.duration > 0
                onTriggered: root.togglePlayback()
            }
            Action {
                text: qsTr("Play Sequence from &Start")
                enabled: timelineEngine.timelineClips.length > 0
                onTriggered: timelineEngine.playSequenceFrom(0)
            }
            Action {
                text: qsTr("Go to &Start")
                // Note: shortcuts must not be translated; qsTr("Home")
                // would break QKeySequence in non-English locales.
                shortcut: "Home"
                enabled: timelineEngine.duration > 0
                onTriggered: timelineEngine.seek(0)
            }
            Action {
                text: qsTr("Go to &End")
                shortcut: "End"
                enabled: timelineEngine.duration > 0
                onTriggered: timelineEngine.seek(timelineEngine.duration)
            }
            MenuSeparator {}
            Action {
                text: qsTr("&Options…")
                onTriggered: optionsDialog.open()
            }
        }
        Menu {
            title: qsTr("&Help")
            Action {
                text: qsTr("&About PrimoReels")
                onTriggered: aboutDialog.open()
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Top Toolbar
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            color: "#252526"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10

                Label {
                    text: projectDisplayName() + (timelineEngine.isModified ? qsTr(" •") : "")
                    color: "#ffffff"
                    font.bold: true
                    font.pointSize: 12
                    Accessible.name: timelineEngine.isModified ? qsTr("Project, unsaved changes") : qsTr("Project")
                }

                Item { Layout.fillWidth: true }

                RowLayout {
                    spacing: 8

                    Button {
                        text: qsTr("Import Media")
                        Accessible.name: qsTr("Import media")
                        onClicked: fileDialog.open()
                    }

                    Button {
                        text: qsTr("Export Sequence")
                        Accessible.name: qsTr("Export sequence")
                        enabled: timelineEngine.timelineClips.length > 0 && !timelineEngine.isExporting
                        onClicked: exportDialog.open()
                    }
                }
            }
        }

        FileDialog {
            id: fileDialog
            objectName: "fileDialog"
            title: qsTr("Choose a Video")
            fileMode: FileDialog.OpenFile
            nameFilters: [qsTr("Video files (*.mp4 *.mkv *.avi *.mov *.webm)"), qsTr("All files (*)")]
            onAccepted: {
                timelineEngine.importMedia(selectedFile.toString())
            }
        }

        // Center Splitter (Top: Bin + Monitor + Inspector, Bottom: Timeline)
        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Vertical

            // Top Section (Bin, Monitor, Inspector)
            SplitView {
                SplitView.fillWidth: true
                SplitView.fillHeight: true
                orientation: Qt.Horizontal

                // Library Panel (Bin, Effects, Audio, Transitions)
                LibraryView {
                    id: libraryPanel
                    objectName: "libraryPanel"
                    SplitView.preferredWidth: appSettings.libraryWidth
                    SplitView.minimumWidth: 200
                    onWidthChanged: viewSaveTimer.restart()
                }

                // Source Monitor View (bound to bin selection + engine).
                SourceMonitorView {
                    id: sourceMonitorPanel
                    objectName: "sourceMonitorPanel"
                    SplitView.preferredWidth: appSettings.sourceWidth
                    SplitView.minimumWidth: 200
                    SplitView.fillWidth: true
                    sourcePath: libraryPanel.selectedPath
                    onWidthChanged: viewSaveTimer.restart()
                }

                // Program Monitor View
                MonitorView {
                    id: programMonitorPanel
                    objectName: "programMonitorPanel"
                    SplitView.preferredWidth: appSettings.programWidth
                    SplitView.minimumWidth: 300
                    SplitView.fillWidth: true
                    selectedClipIndex: timelinePanel.selectedClipIndex
                    onWidthChanged: viewSaveTimer.restart()
                }

                // Inspector View
                InspectorView {
                    id: inspectorPanel
                    objectName: "inspectorPanel"
                    SplitView.preferredWidth: appSettings.inspectorWidth
                    SplitView.minimumWidth: 180
                    selectedClipIndex: timelinePanel.selectedClipIndex
                    onWidthChanged: viewSaveTimer.restart()
                }
            }

            // Bottom Section: Timeline (docked, fixed initial height)
            TimelineView {
                id: timelinePanel
                objectName: "timelinePanel"
                SplitView.fillWidth: true
                SplitView.preferredHeight: appSettings.timelineHeight
                SplitView.minimumHeight: 150
                onHeightChanged: viewSaveTimer.restart()
                onPixelsPerSecondChanged: {
                    if (appSettings.timelineZoom !== pixelsPerSecond) {
                        appSettings.timelineZoom = pixelsPerSecond;
                    }
                }
            }
        }

        // Status Bar
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 22
            color: "#252526"

            Label {
                id: statusLabel
                anchors.fill: parent
                anchors.leftMargin: 10
                verticalAlignment: Text.AlignVCenter
                text: root.statusMessage
                color: root.statusIsError ? "#ffa3a3" : "#cccccc"
                font.pointSize: 9
                elide: Text.ElideRight
                Accessible.role: Accessible.StatusBar
                Accessible.name: root.statusIsError ? qsTr("Error: %1").arg(root.statusMessage) : root.statusMessage
            }
        }
    }

    property string statusMessage: qsTr("Ready")
    property bool statusIsError: false

    function flashStatus(message) {
        statusMessage = message;
        statusIsError = false;
        statusTimer.restart();
    }

    // Sticky: errors stay visible until the next successful action
    // clears them via flashStatus(). The 4s timer must not hide them.
    function showError(message) {
        statusMessage = message;
        statusIsError = true;
        statusTimer.stop();
    }

    Timer {
        id: statusTimer
        interval: 4000
        repeat: false
        onTriggered: root.statusMessage = qsTr("Ready")
    }

    Connections {
        target: libraryPanel
        function onEffectSelected(name) {
            root.flashStatus(name.length > 0
                ? qsTr("Effect \"%1\" selected — double-click to apply to the timeline selection.").arg(name)
                : qsTr("Effect cleared."))
        }
        function onEffectApplied(name) {
            if (timelinePanel.applyEffectToSelected(name)) {
                var clip = timelineEngine.timelineClips[timelinePanel.selectedClipIndex];
                root.flashStatus(clip.effect.length > 0
                    ? qsTr("Effect \"%1\" applied to \"%2\".").arg(name).arg(clip.name)
                    : qsTr("Effect removed from \"%1\".").arg(clip.name))
            } else {
                root.flashStatus(qsTr("Select a timeline clip first, then double-click the effect."))
            }
        }
        function onTrackSelected(name) {
            root.flashStatus(name.length > 0
                ? qsTr("Track \"%1\" selected — double-click a bin file in the Audio tab to attach it to A1 at the playhead.").arg(name)
                : qsTr("Audio track cleared."))
        }
        function onTrackAttached(path) {
            timelinePanel.attachAudioAtPlayhead(path);
            root.flashStatus(qsTr("Audio attached to A1 at the playhead."))
        }
        function onTransitionSelected(name) {
            root.flashStatus(name.length > 0
                ? qsTr("Transition \"%1\" selected — double-click to apply at the timeline selection.").arg(name)
                : qsTr("Transition cleared."))
        }
        function onTransitionApplied(name) {
            if (timelinePanel.applyTransitionToSelected(name)) {
                var clip = timelineEngine.timelineClips[timelinePanel.selectedClipIndex];
                root.flashStatus(clip.transition.length > 0
                    ? qsTr("Transition \"%1\" applied after \"%2\" (renders in export).").arg(name).arg(clip.name)
                    : qsTr("Transition removed from \"%1\".").arg(clip.name))
            } else {
                root.flashStatus(qsTr("Select a non-final timeline clip first, then double-click the transition."))
            }
        }
        function onActiveTabChanged() {
            if (appSettings.libraryTab !== libraryPanel.activeTab) {
                appSettings.libraryTab = libraryPanel.activeTab;
            }
        }
    }

    Connections {
        target: timelineEngine
        function onFailed(message) { root.showError(message) }
        function onWarning(message) { root.flashStatus(message) }
    }

    MessageDialog {
        id: aboutDialog
        objectName: "aboutDialog"
        title: qsTr("About PrimoReels")
        text: qsTr("PrimoReels - Modern Video Editor\nVersion 0.1.0\n\nBuilt with Qt Quick, C++17 and FFmpeg.")
        buttons: MessageDialog.Ok
    }

    FileDialog {
        id: saveProjectDialog
        objectName: "saveProjectDialog"
        title: qsTr("Save Project")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "reels.json"
        nameFilters: [qsTr("PrimoReels projects (*.reels.json)"), qsTr("All files (*)")]
        onAccepted: {
            var path = selectedFile.toString();
            if (timelineEngine.saveProject(path)) {
                currentProjectPath = path;
                addRecentProject(path);
                appSettings.lastProjectPath = path;
                if (pendingQuit) {
                    pendingQuit = false;
                    quittingAllowed = true;
                    Qt.quit();
                } else if (pendingNew) {
                    doNewProject();
                } else if (pendingOpenPath.length > 0) {
                    var openPath = pendingOpenPath;
                    pendingOpenPath = "";
                    openProjectPath(openPath);
                }
            }
        }
        onRejected: {
            // Keep quit/open/new intent so the user can Discard afterwards
            // instead of restarting the action.
        }
    }

    FileDialog {
        id: openProjectDialog
        objectName: "openProjectDialog"
        title: qsTr("Open Project")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("PrimoReels projects (*.reels.json)"), qsTr("All files (*)")]
        onAccepted: root.requestOpen(selectedFile.toString())
    }

    FileDialog {
        id: exportDialog
        objectName: "exportDialog"
        title: qsTr("Export Sequence")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "mp4"
        nameFilters: [qsTr("MP4 video (*.mp4)"), qsTr("All files (*)")]
        onAccepted: timelineEngine.exportSequence(selectedFile.toString())
    }

    Dialog {
        id: exportProgressDialog
        objectName: "exportProgressDialog"
        title: qsTr("Exporting Sequence")
        modal: true
        closePolicy: Popup.NoAutoClose
        anchors.centerIn: parent
        implicitWidth: 360
        implicitHeight: 180

        ColumnLayout {
            spacing: 10

            Label {
                Layout.fillWidth: true
                text: qsTr("Rendering timeline to MP4…")
                color: "#ffffff"
                font.pointSize: 10
                wrapMode: Text.WordWrap
            }
            ProgressBar {
                Layout.fillWidth: true
                from: 0
                to: 1
                value: timelineEngine.exportProgress
                Accessible.role: Accessible.ProgressBar
                Accessible.name: qsTr("Export progress")
                Accessible.description: qsTr("%1 percent exported").arg(Math.round(timelineEngine.exportProgress * 100))
            }
            Button {
                Layout.alignment: Qt.AlignRight
                text: qsTr("Cancel")
                Accessible.name: qsTr("Cancel export")
                onClicked: timelineEngine.cancelExport()
            }
        }
    }

    Connections {
        target: timelineEngine
        function onIsExportingChanged() {
            if (timelineEngine.isExporting) {
                exportProgressDialog.open()
            } else {
                exportProgressDialog.close()
            }
        }
        function onExportSucceeded(path) {
            var name = decodeURIComponent(String(path).split('/').pop());
            root.flashStatus(qsTr("Export finished: %1").arg(name))
        }
    }

    Dialog {
        id: optionsDialog
        objectName: "optionsDialog"
        title: qsTr("Options")
        modal: true
        anchors.centerIn: parent
        implicitWidth: 380
        standardButtons: Dialog.Ok | Dialog.Cancel
        // Staged state: edited here, applied to live settings only on OK.
        property real pendingZoom: 100
        property string pendingStartupMode: "new"
        property bool pendingClearRecents: false

        function applyOptions() {
            timelinePanel.pixelsPerSecond = clampedZoom(pendingZoom);
            appSettings.startupMode = pendingStartupMode === "last" ? "last" : "new";
            if (pendingClearRecents) {
                appSettings.recentProjects = [];
                appSettings.lastProjectPath = "";
            }
        }

        onOpened: {
            pendingZoom = timelinePanel.pixelsPerSecond;
            pendingStartupMode = appSettings.startupMode === "last" ? "last" : "new";
            pendingClearRecents = false;
        }
        onAccepted: applyOptions()

        ColumnLayout {
            spacing: 10

            Label {
                Layout.fillWidth: true
                text: qsTr("Timeline zoom")
                color: "#ffffff"
                font.pointSize: 10
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Slider {
                    Layout.fillWidth: true
                    from: 10
                    to: 800
                    value: optionsDialog.pendingZoom
                    Accessible.name: qsTr("Timeline zoom")
                    onMoved: optionsDialog.pendingZoom = value
                }
                SpinBox {
                    from: 10
                    to: 800
                    value: Math.round(optionsDialog.pendingZoom)
                    Accessible.name: qsTr("Timeline zoom value")
                    // valueModified (not valueChanged): user edits only, so
                    // binding updates from the slider never write back.
                    onValueModified: optionsDialog.pendingZoom = value
                }
            }
            Button {
                Layout.fillWidth: true
                text: qsTr("Reset Layout to Defaults")
                Accessible.name: qsTr("Reset layout to defaults")
                onClicked: {
                    root.resetLayout();
                    optionsDialog.pendingZoom = timelinePanel.pixelsPerSecond;
                }
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("On startup")
                color: "#ffffff"
                font.pointSize: 10
            }
            ComboBox {
                Layout.fillWidth: true
                model: [qsTr("New Untitled Project"), qsTr("Load Previous Project")]
                currentIndex: optionsDialog.pendingStartupMode === "last" ? 1 : 0
                Accessible.name: qsTr("On startup")
                // activated (not currentIndexChanged): user choice only, so
                // staging updates from onOpened never write back.
                onActivated: optionsDialog.pendingStartupMode = index === 1 ? "last" : "new"
            }
            CheckBox {
                Layout.fillWidth: true
                text: qsTr("Clear recent projects on OK")
                checked: optionsDialog.pendingClearRecents
                Accessible.name: qsTr("Clear recent projects on OK")
                onToggled: optionsDialog.pendingClearRecents = checked
            }
        }
    }

    MessageDialog {
        id: unsavedChangesDialog
        objectName: "unsavedChangesDialog"
        title: qsTr("Unsaved Changes")
        text: qsTr("The project has unsaved changes. Save before continuing?")
        buttons: MessageDialog.Save | MessageDialog.Discard | MessageDialog.Cancel
        onButtonClicked: function(button, role) {
            if (button === MessageDialog.Save) {
                if (currentProjectPath.length > 0) {
                    if (timelineEngine.saveProject(currentProjectPath)) {
                        addRecentProject(currentProjectPath);
                        appSettings.lastProjectPath = currentProjectPath;
                        unsavedContinue();
                    }
                } else {
                    saveProjectDialog.open();
                }
            } else if (button === MessageDialog.Discard) {
                unsavedContinue();
            } else {
                pendingOpenPath = "";
                pendingQuit = false;
                pendingNew = false;
            }
        }
    }

    function unsavedContinue() {
        if (pendingQuit) {
            pendingQuit = false;
            quittingAllowed = true;
            Qt.quit();
        } else if (pendingNew) {
            doNewProject();
        } else if (pendingOpenPath.length > 0) {
            var path = pendingOpenPath;
            pendingOpenPath = "";
            openProjectPath(path);
        }
    }
}
