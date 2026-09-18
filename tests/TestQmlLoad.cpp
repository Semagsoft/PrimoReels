#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include "FrameImageProvider.h"
#include "TimelineEngine.h"
#include "VideoFrameItem.h"

// Regression guard: the full QML component tree must load. A single bad
// binding/handler (e.g. assigning a non-existent Keys signal) aborts the
// whole application at startup, and qmllint does not catch that class.
class TestQmlLoad : public QObject {
  Q_OBJECT
 private slots:
  void mainWindow_loads() {
    TimelineEngine engine;
    QQmlApplicationEngine qmlEngine;
    qmlEngine.rootContext()->setContextProperty("timelineEngine", &engine);
    qmlEngine.addImageProvider(QStringLiteral("preview"), new FrameImageProvider(&engine));
    qmlEngine.addImageProvider(QStringLiteral("thumbs"), new ThumbImageProvider(&engine));
    qmlEngine.addImageProvider(QStringLiteral("source"), new SourceImageProvider(&engine));
    QStringList errors;
    connect(
        &qmlEngine, &QQmlEngine::warnings, this,
        [&errors](const QList<QQmlError>& warnings) {
          for (const QQmlError& w : warnings) {
            errors.append(w.toString());
          }
        },
        Qt::DirectConnection);
    qmlEngine.load(QStringLiteral("qrc:/qml/Main.qml"));
    QVERIFY2(!qmlEngine.rootObjects().isEmpty(),
             qPrintable(QString("Main.qml failed to load:\n%1").arg(errors.join("\n"))));
    // Pump the event loop so deferred bindings/queued errors surface.
    QTimer timer;
    timer.setSingleShot(true);
    timer.start(500);
    QSignalSpy timeoutSpy(&timer, &QTimer::timeout);
    QVERIFY(timeoutSpy.wait(2000));
    QVERIFY2(errors.isEmpty(), qPrintable(QString("QML warnings:\n%1").arg(errors.join("\n"))));
  }

  void mainWindow_interaction() {
    TimelineEngine engine;
    QQmlApplicationEngine qmlEngine;
    qmlEngine.rootContext()->setContextProperty("timelineEngine", &engine);
    qmlEngine.addImageProvider(QStringLiteral("preview"), new FrameImageProvider(&engine));
    qmlEngine.addImageProvider(QStringLiteral("thumbs"), new ThumbImageProvider(&engine));
    qmlEngine.addImageProvider(QStringLiteral("source"), new SourceImageProvider(&engine));
    QStringList errors;
    connect(
        &qmlEngine, &QQmlEngine::warnings, this,
        [&errors](const QList<QQmlError>& warnings) {
          for (const QQmlError& w : warnings) {
            errors.append(w.toString());
          }
        },
        Qt::DirectConnection);
    qmlEngine.load(QStringLiteral("qrc:/qml/Main.qml"));
    QVERIFY(!qmlEngine.rootObjects().isEmpty());
    QObject* root = qmlEngine.rootObjects().first();
    // Timeline panel: zoom + selection survive round-trip.
    QObject* timeline = root->findChild<QObject*>(QStringLiteral("timelinePanel"));
    if (!timeline) {
      const auto all = root->findChildren<QObject*>();
      for (QObject* o : all) {
        if (o->property("pixelsPerSecond").isValid() &&
            o->property("selectedClipIndex").isValid()) {
          timeline = o;
          break;
        }
      }
    }
    QObject* library = root->findChild<QObject*>(QStringLiteral("libraryPanel"));
    if (!library) {
      const auto all = root->findChildren<QObject*>();
      for (QObject* o : all) {
        if (o->property("selectedPath").isValid() && o->property("activeTab").isValid()) {
          library = o;
          break;
        }
      }
    }
    QVERIFY2(timeline != nullptr, "TimelineView not found in Main.qml tree");
    QVERIFY2(library != nullptr, "LibraryView not found in Main.qml tree");
    QVERIFY(timeline->setProperty("pixelsPerSecond", 200.0));
    QCOMPARE(timeline->property("pixelsPerSecond").toDouble(), 200.0);
    QVERIFY(timeline->setProperty("selectedClipIndex", -1));
    QVERIFY(library->setProperty("activeTab", 1));
    QCOMPARE(library->property("activeTab").toInt(), 1);
    QVERIFY(library->setProperty("selectedPath", QString("/tmp/fake.mp4")));
    QTest::qWait(200);
    QVERIFY2(errors.isEmpty(), qPrintable(QString("QML warnings:\n%1").arg(errors.join("\n"))));
  }

  // Dialogs exist with sane configuration (titles, file modes, policies).
  // Native File/Message dialogs are not opened offscreen; we verify static
  // config so regressions in ids/filters/buttons fail here, not at runtime.
  void dialogs_existAndConfigured() {
    TimelineEngine engine;
    QQmlApplicationEngine qmlEngine;
    setupEngine(&qmlEngine, &engine);
    QStringList errors;
    watchWarnings(&qmlEngine, &errors);
    qmlEngine.load(QStringLiteral("qrc:/qml/Main.qml"));
    QVERIFY(!qmlEngine.rootObjects().isEmpty());
    QObject* root = qmlEngine.rootObjects().first();

    QObject* fileDialog = root->findChild<QObject*>(QStringLiteral("fileDialog"));
    QObject* saveDialog = root->findChild<QObject*>(QStringLiteral("saveProjectDialog"));
    QObject* openDialog = root->findChild<QObject*>(QStringLiteral("openProjectDialog"));
    QObject* exportDialog = root->findChild<QObject*>(QStringLiteral("exportDialog"));
    QObject* exportProgress = root->findChild<QObject*>(QStringLiteral("exportProgressDialog"));
    QObject* unsaved = root->findChild<QObject*>(QStringLiteral("unsavedChangesDialog"));
    QObject* about = root->findChild<QObject*>(QStringLiteral("aboutDialog"));
    QVERIFY2(fileDialog, "fileDialog missing");
    QVERIFY2(saveDialog, "saveProjectDialog missing");
    QVERIFY2(openDialog, "openProjectDialog missing");
    QVERIFY2(exportDialog, "exportDialog missing");
    QVERIFY2(exportProgress, "exportProgressDialog missing");
    QVERIFY2(unsaved, "unsavedChangesDialog missing");
    QVERIFY2(about, "aboutDialog missing");

    // Titles must be non-empty (translatable strings resolve offscreen).
    for (QObject* dlg :
         {fileDialog, saveDialog, openDialog, exportDialog, exportProgress, unsaved}) {
      QVERIFY2(dlg->property("title").isValid(), qPrintable(dlg->objectName() + " has no title"));
      QVERIFY2(!dlg->property("title").toString().isEmpty(),
               qPrintable(dlg->objectName() + " title is empty"));
    }
    // FileDialog modes: import/open = OpenFile, save/export = SaveFile.
    // QML FileDialog.fileMode enum: OpenFile=0, OpenFiles=1, SaveFile=2.
    QCOMPARE(fileDialog->property("fileMode").toInt(), 0);
    QCOMPARE(openDialog->property("fileMode").toInt(), 0);
    QCOMPARE(saveDialog->property("fileMode").toInt(), 2);
    QCOMPARE(exportDialog->property("fileMode").toInt(), 2);
    QVERIFY(!fileDialog->property("nameFilters").toStringList().isEmpty());
    QVERIFY(!saveDialog->property("nameFilters").toStringList().isEmpty());
    QVERIFY(!exportDialog->property("nameFilters").toStringList().isEmpty());
    QVERIFY(!saveDialog->property("defaultSuffix").toString().isEmpty());
    QVERIFY(!exportDialog->property("defaultSuffix").toString().isEmpty());

    // Export progress dialog: modal, non-auto-close (blocks input mid-export).
    QVERIFY(exportProgress->property("modal").toBool());
    // closePolicy Popup.NoAutoClose == 0.
    QCOMPARE(exportProgress->property("closePolicy").toInt(), 0);
    // ProgressBar inside must be 0..1 range.
    QObject* progressBar = exportProgress->findChild<QObject*>();
    bool foundBar = false;
    for (QObject* o : exportProgress->findChildren<QObject*>()) {
      if (o->property("from").isValid() && o->property("to").isValid() &&
          o->property("value").isValid()) {
        QCOMPARE(o->property("from").toDouble(), 0.0);
        QCOMPARE(o->property("to").toDouble(), 1.0);
        foundBar = true;
        break;
      }
    }
    QVERIFY2(foundBar, "export ProgressBar (from/to/value) not found");
    Q_UNUSED(progressBar);

    // Unsaved-changes + about dialogs carry message text.
    QVERIFY(!unsaved->property("text").toString().isEmpty());
    QVERIFY(!about->property("text").toString().isEmpty());

    QTest::qWait(200);
    QVERIFY2(errors.isEmpty(), qPrintable(QString("QML warnings:\n%1").arg(errors.join("\n"))));
  }

  // Layout persistence: save/restore/reset round-trip without warnings.
  void layout_saveRestoreReset() {
    TimelineEngine engine;
    QQmlApplicationEngine qmlEngine;
    setupEngine(&qmlEngine, &engine);
    QStringList errors;
    watchWarnings(&qmlEngine, &errors);
    qmlEngine.load(QStringLiteral("qrc:/qml/Main.qml"));
    QVERIFY(!qmlEngine.rootObjects().isEmpty());
    QObject* root = qmlEngine.rootObjects().first();
    QObject* timeline = root->findChild<QObject*>(QStringLiteral("timelinePanel"));
    QObject* library = root->findChild<QObject*>(QStringLiteral("libraryPanel"));
    QObject* settings = root->findChild<QObject*>(QStringLiteral("appSettings"));
    QVERIFY(timeline && library && settings);

    // clampedZoom bounds checking (QML JS fns return QVariant).
    QVariant zv;
    QVERIFY(QMetaObject::invokeMethod(root, "clampedZoom", Q_RETURN_ARG(QVariant, zv),
                                      Q_ARG(QVariant, 1000)));
    QCOMPARE(zv.toDouble(), 800.0);
    QVERIFY(QMetaObject::invokeMethod(root, "clampedZoom", Q_RETURN_ARG(QVariant, zv),
                                      Q_ARG(QVariant, 1)));
    QCOMPARE(zv.toDouble(), 10.0);
    QVERIFY(QMetaObject::invokeMethod(root, "clampedZoom", Q_RETURN_ARG(QVariant, zv),
                                      Q_ARG(QVariant, 150)));
    QCOMPARE(zv.toDouble(), 150.0);

    // Zoom propagates into persisted settings via onPixelsPerSecondChanged.
    QVERIFY(timeline->setProperty("pixelsPerSecond", 250.0));
    QTest::qWait(100);
    QCOMPARE(settings->property("timelineZoom").toDouble(), 250.0);

    // Library tab persists.
    QVERIFY(library->setProperty("activeTab", 2));
    QTest::qWait(100);
    QCOMPARE(settings->property("libraryTab").toInt(), 2);

    // saveViewState / restoreViewState / resetLayout are invocable.
    QVERIFY(QMetaObject::invokeMethod(root, "saveViewState"));
    QVERIFY(QMetaObject::invokeMethod(root, "restoreViewState"));
    QVERIFY(QMetaObject::invokeMethod(root, "resetLayout"));
    QTest::qWait(100);
    // Reset restores defaults.
    QCOMPARE(timeline->property("pixelsPerSecond").toDouble(), 100.0);
    QCOMPARE(library->property("activeTab").toInt(), 0);
    QVERIFY(settings->property("showLibrary").toBool());
    QVERIFY(settings->property("showTimeline").toBool());

    // View toggles via panel visibility persist on save.
    QVERIFY(library->setProperty("visible", false));
    QVERIFY(QMetaObject::invokeMethod(root, "saveViewState"));
    QCOMPARE(settings->property("showLibrary").toBool(), false);
    QVERIFY(library->setProperty("visible", true));

    QTest::qWait(200);
    QVERIFY2(errors.isEmpty(), qPrintable(QString("QML warnings:\n%1").arg(errors.join("\n"))));
  }

  // Timeline DnD math, ruler adaptivity, and playhead guards on empty state.
  void timeline_dndRulerPlayhead() {
    TimelineEngine engine;
    QQmlApplicationEngine qmlEngine;
    setupEngine(&qmlEngine, &engine);
    QStringList errors;
    watchWarnings(&qmlEngine, &errors);
    qmlEngine.load(QStringLiteral("qrc:/qml/Main.qml"));
    QVERIFY(!qmlEngine.rootObjects().isEmpty());
    QObject* root = qmlEngine.rootObjects().first();
    QObject* timeline = root->findChild<QObject*>(QStringLiteral("timelinePanel"));
    QVERIFY(timeline);

    // Empty timeline: drop index is 0, no split/effect/transition possible.
    QVariant rv;
    QVERIFY(QMetaObject::invokeMethod(timeline, "dropIndexAt", Q_RETURN_ARG(QVariant, rv),
                                      Q_ARG(QVariant, 500.0)));
    QCOMPARE(rv.toInt(), 0);
    QVERIFY(QMetaObject::invokeMethod(timeline, "canSplitAtPlayhead", Q_RETURN_ARG(QVariant, rv)));
    QCOMPARE(rv.toBool(), false);
    QCOMPARE(timeline->property("canSplit").toBool(), false);
    QVERIFY(QMetaObject::invokeMethod(timeline, "applyEffectToSelected", Q_RETURN_ARG(QVariant, rv),
                                      Q_ARG(QVariant, QString("Blur"))));
    QCOMPARE(rv.toBool(), false);
    QVERIFY(QMetaObject::invokeMethod(timeline, "applyTransitionToSelected",
                                      Q_RETURN_ARG(QVariant, rv),
                                      Q_ARG(QVariant, QString("Cross Dissolve"))));
    QCOMPARE(rv.toBool(), false);
    QVERIFY(QMetaObject::invokeMethod(timeline, "hasOverlayClips", Q_RETURN_ARG(QVariant, rv)));
    QCOMPARE(rv.toBool(), false);

    // Ruler adapts to zoom: wide zoom -> small step, far zoom-out -> big step.
    QVERIFY(timeline->setProperty("pixelsPerSecond", 400.0));
    double step = timeline->property("rulerStep").toDouble();
    QVERIFY2(step <= 2.0, qPrintable(QString("rulerStep at 400pps=%1").arg(step)));
    QVERIFY(timeline->setProperty("pixelsPerSecond", 10.0));
    step = timeline->property("rulerStep").toDouble();
    QVERIFY2(step >= 10.0, qPrintable(QString("rulerStep at 10pps=%1").arg(step)));
    QVERIFY(timeline->property("rulerSegments").toInt() >= 1);
    // Duration floor keeps the ruler usable when empty.
    QVERIFY(timeline->property("timelineDuration").toDouble() >= 10.0);
    QCOMPARE(timeline->property("sequenceEnd").toDouble(), 0.0);

    // playheadClipOffset with no selection is -1.
    QVERIFY(QMetaObject::invokeMethod(timeline, "playheadClipOffset", Q_RETURN_ARG(QVariant, rv)));
    QCOMPARE(rv.toDouble(), -1.0);

    QTest::qWait(200);
    QVERIFY2(errors.isEmpty(), qPrintable(QString("QML warnings:\n%1").arg(errors.join("\n"))));
  }

  // A1 bed interactions: output-time mapping, split gating, attach helper.
  void timeline_a1Interactions() {
    TimelineEngine engine;
    QQmlApplicationEngine qmlEngine;
    setupEngine(&qmlEngine, &engine);
    QStringList errors;
    watchWarnings(&qmlEngine, &errors);
    qmlEngine.load(QStringLiteral("qrc:/qml/Main.qml"));
    QVERIFY(!qmlEngine.rootObjects().isEmpty());
    QObject* root = qmlEngine.rootObjects().first();
    QObject* timeline = root->findChild<QObject*>(QStringLiteral("timelinePanel"));
    QVERIFY(timeline);

    // Attach with nothing loaded falls back to the playhead (0.0).
    QVariant rv;
    QVERIFY(QMetaObject::invokeMethod(timeline, "attachAudioAtPlayhead", Q_RETURN_ARG(QVariant, rv),
                                      Q_ARG(QVariant, QString("/clips/a.mp3"))));
    QCOMPARE(rv.toDouble(), 0.0);
    QCOMPARE(engine.timelineClips().size(), 1);
    QCOMPARE(engine.timelineClips().first().toMap().value("track").toInt(), 2);
    QCOMPARE(timeline->property("selectedClipIndex").toInt(), 0);
    engine.clearTimeline();

    // V1 (start 0, dur 5) + A1 bed (start 2, dur 5); preview the bed at
    // file time 4.0, i.e. output 2 + (4 - 0) = 6.0.
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.appendAudioClip("/clips/bed.mp3", 2.0);
    QMetaObject::invokeMethod(&engine, "onLoaded", Q_ARG(QString, QString("/clips/bed.mp3")),
                              Q_ARG(double, 10.0), Q_ARG(int, 0), Q_ARG(int, 0), Q_ARG(bool, true));
    QVERIFY(timeline->setProperty("selectedClipIndex", 1));
    QMetaObject::invokeMethod(&engine, "onFrameReady", Q_ARG(double, 4.0),
                              Q_ARG(QImage, QImage(32, 32, QImage::Format_RGB32)));
    QCOMPARE(engine.position(), 4.0);
    QVERIFY(
        QMetaObject::invokeMethod(timeline, "outputTimeAtPlayhead", Q_RETURN_ARG(QVariant, rv)));
    QCOMPARE(rv.toDouble(), 6.0);
    QVERIFY(QMetaObject::invokeMethod(timeline, "playheadClipOffset", Q_RETURN_ARG(QVariant, rv)));
    QCOMPARE(rv.toDouble(), 4.0);
    QVERIFY(QMetaObject::invokeMethod(timeline, "canSplitAtPlayhead", Q_RETURN_ARG(QVariant, rv)));
    QCOMPARE(rv.toBool(), true);
    QCOMPARE(timeline->property("canSplit").toBool(), true);

    // Split divides the bed at the playhead output; the right half shifts.
    // (The bed was enriched to 10s by the onLoaded above, like a real probe.)
    QVERIFY(QMetaObject::invokeMethod(timeline, "splitSelectedAtPlayhead"));
    QCOMPARE(engine.timelineClips().size(), 3);
    QCOMPARE(engine.timelineClips().at(1).toMap().value("duration").toDouble(), 4.0);
    QCOMPARE(engine.timelineClips().at(2).toMap().value("startTime").toDouble(), 6.0);
    QCOMPARE(engine.timelineClips().at(2).toMap().value("duration").toDouble(), 6.0);
    QCOMPARE(timeline->property("selectedClipIndex").toInt(), 2);

    // Attaching now lands under the playhead output (right half selected).
    QVERIFY(QMetaObject::invokeMethod(timeline, "attachAudioAtPlayhead", Q_RETURN_ARG(QVariant, rv),
                                      Q_ARG(QVariant, QString("/clips/d.mp3"))));
    QCOMPARE(rv.toDouble(), 6.0);
    QCOMPARE(engine.timelineClips().last().toMap().value("startTime").toDouble(), 6.0);

    // Playhead outside the selection: no split.
    QMetaObject::invokeMethod(&engine, "onFrameReady", Q_ARG(double, 0.5),
                              Q_ARG(QImage, QImage(32, 32, QImage::Format_RGB32)));
    QVERIFY(QMetaObject::invokeMethod(timeline, "canSplitAtPlayhead", Q_RETURN_ARG(QVariant, rv)));
    QCOMPARE(rv.toBool(), false);
    QCOMPARE(timeline->property("canSplit").toBool(), false);

    QTest::qWait(200);
    QVERIFY2(errors.isEmpty(), qPrintable(QString("QML warnings:\n%1").arg(errors.join("\n"))));
  }

  // Library catalogs load from C++ and every effect/transition has a blurb.
  void library_catalogsAndTabs() {
    TimelineEngine engine;
    QQmlApplicationEngine qmlEngine;
    setupEngine(&qmlEngine, &engine);
    QStringList errors;
    watchWarnings(&qmlEngine, &errors);
    qmlEngine.load(QStringLiteral("qrc:/qml/Main.qml"));
    QVERIFY(!qmlEngine.rootObjects().isEmpty());
    QObject* root = qmlEngine.rootObjects().first();
    QObject* library = root->findChild<QObject*>(QStringLiteral("libraryPanel"));
    QVERIFY(library);
    // Component.onCompleted populates catalogs; allow a beat for it.
    QTest::qWait(300);
    const QStringList effects = engine.availableEffects();
    const QStringList transitions = engine.availableTransitions();
    QVERIFY(!effects.isEmpty());
    QVERIFY(!transitions.isEmpty());
    QCOMPARE(library->property("effectCatalog").toStringList().size(), effects.size());
    QCOMPARE(library->property("transitionCatalog").toStringList().size(), transitions.size());

    // Every catalog entry must resolve a description (covers Sepia/etc drift).
    for (const QString& name : effects) {
      QVariant desc;
      QVERIFY(QMetaObject::invokeMethod(library, "effectDescription", Q_RETURN_ARG(QVariant, desc),
                                        Q_ARG(QVariant, name)));
      QVERIFY2(!desc.toString().isEmpty(),
               qPrintable(QString("effectDescription(%1) is empty").arg(name)));
    }
    for (const QString& name : transitions) {
      QVariant desc;
      QVERIFY(QMetaObject::invokeMethod(library, "transitionDescription",
                                        Q_RETURN_ARG(QVariant, desc), Q_ARG(QVariant, name)));
      QVERIFY2(!desc.toString().isEmpty(),
               qPrintable(QString("transitionDescription(%1) is empty").arg(name)));
    }

    // All four tabs are selectable without warnings.
    for (int tab = 0; tab < 4; ++tab) {
      QVERIFY(library->setProperty("activeTab", tab));
      QCOMPARE(library->property("activeTab").toInt(), tab);
      QTest::qWait(50);
    }
    QTest::qWait(200);
    QVERIFY2(errors.isEmpty(), qPrintable(QString("QML warnings:\n%1").arg(errors.join("\n"))));
  }

  // Recent-projects list: dedup, MRU order, cap at 8; title reflects path.
  void recentsAndTitle() {
    TimelineEngine engine;
    QQmlApplicationEngine qmlEngine;
    setupEngine(&qmlEngine, &engine);
    QStringList errors;
    watchWarnings(&qmlEngine, &errors);
    qmlEngine.load(QStringLiteral("qrc:/qml/Main.qml"));
    QVERIFY(!qmlEngine.rootObjects().isEmpty());
    QObject* root = qmlEngine.rootObjects().first();
    QObject* settings = root->findChild<QObject*>(QStringLiteral("appSettings"));
    QVERIFY(settings);

    for (int i = 0; i < 10; ++i) {
      QVERIFY(QMetaObject::invokeMethod(root, "addRecentProject",
                                        Q_ARG(QVariant, QString("/tmp/p%1.reels.json").arg(i))));
    }
    QStringList recents = settings->property("recentProjects").toStringList();
    QCOMPARE(recents.size(), 8);
    QCOMPARE(recents.first(), QString("/tmp/p9.reels.json"));
    // Re-adding moves to front without growing.
    QVERIFY(QMetaObject::invokeMethod(root, "addRecentProject",
                                      Q_ARG(QVariant, QString("/tmp/p5.reels.json"))));
    recents = settings->property("recentProjects").toStringList();
    QCOMPARE(recents.size(), 8);
    QCOMPARE(recents.first(), QString("/tmp/p5.reels.json"));

    // Display name derives from the file name; empty path -> Untitled.
    QVERIFY(root->setProperty("currentProjectPath", QString("/tmp/my%20movie.reels.json")));
    QVariant display;
    QVERIFY(QMetaObject::invokeMethod(root, "projectDisplayName", Q_RETURN_ARG(QVariant, display)));
    QVERIFY(!display.toString().isEmpty());
    QVERIFY(root->setProperty("currentProjectPath", QString("")));
    QVERIFY(QMetaObject::invokeMethod(root, "projectDisplayName", Q_RETURN_ARG(QVariant, display)));
    QCOMPARE(display.toString(), QString("Untitled"));

    QTest::qWait(200);
    QVERIFY2(errors.isEmpty(), qPrintable(QString("QML warnings:\n%1").arg(errors.join("\n"))));
  }

 private:
  void setupEngine(QQmlApplicationEngine* qmlEngine, TimelineEngine* engine) {
    qmlEngine->rootContext()->setContextProperty("timelineEngine", engine);
    qmlEngine->addImageProvider(QStringLiteral("preview"), new FrameImageProvider(engine));
    qmlEngine->addImageProvider(QStringLiteral("thumbs"), new ThumbImageProvider(engine));
    qmlEngine->addImageProvider(QStringLiteral("source"), new SourceImageProvider(engine));
  }

  void watchWarnings(QQmlApplicationEngine* qmlEngine, QStringList* errors) {
    connect(
        qmlEngine, &QQmlEngine::warnings, this,
        [errors](const QList<QQmlError>& warnings) {
          for (const QQmlError& w : warnings) {
            errors->append(w.toString());
          }
        },
        Qt::DirectConnection);
  }
};

int main(int argc, char* argv[]) {
  // Offscreen in CI (QT_QPA_PLATFORM=offscreen); needs GUI for QML.
  QGuiApplication app(argc, argv);
  // Mirrors src/main.cpp so QML Settings initializes like in production.
  QCoreApplication::setOrganizationName(QStringLiteral("PrimoReels"));
  QCoreApplication::setOrganizationDomain(QStringLiteral("primoreels.example"));
  QCoreApplication::setApplicationName(QStringLiteral("PrimoReels"));

  qmlRegisterType<VideoFrameItem>("PrimoReels", 1, 0, "VideoFrameItem");

  TestQmlLoad test;
  return QTest::qExec(&test, argc, argv);
}

#include "TestQmlLoad.moc"
