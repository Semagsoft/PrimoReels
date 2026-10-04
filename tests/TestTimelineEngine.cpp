#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <cmath>
#include "AudioPlayer.h"
#include "ClipUtils.h"
#include "DecoderThread.h"
#include "ListModels.h"
#include "MediaDecoder.h"
#include "PreloadThread.h"
#include "TimelineEngine.h"

namespace {

bool makeImportTestClip(const QString& path) {
  QProcess ffmpeg;
  ffmpeg.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i",
                          "color=c=green:size=160x120:rate=10:duration=1", "-pix_fmt", "yuv420p",
                          "-c:v", "mpeg4", path});
  return ffmpeg.waitForFinished(15000) && ffmpeg.exitCode() == 0 && QFileInfo::exists(path);
}

// Long clip with sparse keyframes so a far seek walks many frames,
// widening the window where a second request can preempt it in flight.
bool makeLongSeekClip(const QString& path) {
  QProcess ffmpeg;
  ffmpeg.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i",
                          "color=c=blue:size=320x240:rate=25:duration=30", "-pix_fmt", "yuv420p",
                          "-c:v", "mpeg4", "-g", "300", path});
  return ffmpeg.waitForFinished(30000) && ffmpeg.exitCode() == 0 && QFileInfo::exists(path);
}

// Audio+video clip with the moov box up front so a truncated copy still
// opens (duration/streams parse) but mid-file reads hit EOF.
bool makeAudioVideoClip(const QString& path) {
  QProcess ffmpeg;
  ffmpeg.start("ffmpeg", {"-y",
                          "-v",
                          "error",
                          "-f",
                          "lavfi",
                          "-i",
                          "color=c=red:size=160x120:rate=10:duration=5",
                          "-f",
                          "lavfi",
                          "-i",
                          "sine=frequency=440:duration=5",
                          "-pix_fmt",
                          "yuv420p",
                          "-c:v",
                          "mpeg4",
                          "-c:a",
                          "aac",
                          "-movflags",
                          "+faststart",
                          "-shortest",
                          path});
  return ffmpeg.waitForFinished(30000) && ffmpeg.exitCode() == 0 && QFileInfo::exists(path);
}

// Constant-value stereo s16 PCM chunk (frames = stereo sample frames).
QByteArray makeConstChunk(int frames, qint16 value) {
  QByteArray chunk(static_cast<qsizetype>(frames) * 4, 0);
  auto* s = reinterpret_cast<qint16*>(chunk.data());
  for (int i = 0; i < frames * 2; ++i) {
    s[i] = value;
  }
  return chunk;
}

double meanS16(const QByteArray& pcm, int fromFrame, int toFrame) {
  const auto* s = reinterpret_cast<const qint16*>(pcm.constData());
  const int total = static_cast<int>(pcm.size() / 4);
  const int lo = std::max(0, fromFrame);
  const int hi = std::min(toFrame, total);
  if (hi <= lo) {
    return 0.0;
  }
  double sum = 0.0;
  int n = 0;
  for (int f = lo; f < hi; ++f) {
    for (int ch = 0; ch < 2; ++ch) {
      sum += s[f * 2 + ch];
      ++n;
    }
  }
  return sum / n;
}

// Goertzel power of the left channel at freq over n frames from fromFrame.
double toneEnergy(const QByteArray& pcm, int fromFrame, int frames, double freq) {
  const auto* s = reinterpret_cast<const qint16*>(pcm.constData());
  const int total = static_cast<int>(pcm.size() / 4);
  const int n = std::min(frames, total - fromFrame);
  if (n <= 0) {
    return 0.0;
  }
  const double w = 2.0 * M_PI * freq / 48000.0;
  const double coeff = 2.0 * std::cos(w);
  double q0 = 0.0, q1 = 0.0, q2 = 0.0;
  for (int i = 0; i < n; ++i) {
    q0 = coeff * q1 - q2 + s[static_cast<ptrdiff_t>(fromFrame + i) * 2];
    q2 = q1;
    q1 = q0;
  }
  return q1 * q1 + q2 * q2 - coeff * q1 * q2;
}

// Video + sine-tone clip with caller-chosen tone and length.
bool makeAVToneClip(const QString& path, const QString& color, int frequency, double duration) {
  QProcess ffmpeg;
  ffmpeg.start(
      "ffmpeg",
      {"-y", "-v", "error", "-f", "lavfi", "-i",
       QString("color=c=%1:size=160x120:rate=10:duration=%2").arg(color).arg(duration), "-f",
       "lavfi", "-i", QString("sine=frequency=%1:duration=%2").arg(frequency).arg(duration),
       "-pix_fmt", "yuv420p", "-c:v", "mpeg4", "-c:a", "aac", "-shortest", path});
  return ffmpeg.waitForFinished(30000) && ffmpeg.exitCode() == 0 && QFileInfo::exists(path);
}

// Audio-only sine bed (no video stream).
bool makeSineBedClip(const QString& path, int frequency, double duration) {
  QProcess ffmpeg;
  ffmpeg.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i",
                          QString("sine=frequency=%1:duration=%2").arg(frequency).arg(duration),
                          "-c:a", "aac", path});
  return ffmpeg.waitForFinished(30000) && ffmpeg.exitCode() == 0 && QFileInfo::exists(path);
}

// Silent video clip (no audio stream) with caller-chosen length.
bool makeSilentVideoClip(const QString& path, double duration) {
  QProcess ffmpeg;
  ffmpeg.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i",
                          QString("color=c=blue:size=160x120:rate=10:duration=%1").arg(duration),
                          "-pix_fmt", "yuv420p", "-c:v", "mpeg4", path});
  return ffmpeg.waitForFinished(30000) && ffmpeg.exitCode() == 0 && QFileInfo::exists(path);
}

}  // namespace

class TestTimelineEngine : public QObject {
  Q_OBJECT
 private slots:
  void invalidLoad_reportsFailure() {
    TimelineEngine engine;
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    engine.loadMedia("/path/does/not/exist.mp4");
    QVERIFY(failedSpy.wait(2000));
    QVERIFY(failedSpy.count() > 0);
  }

  void importMedia_leavesProgramUntouched() {
    TimelineEngine engine;
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    // Import registers a placeholder synchronously without loading the
    // program monitor (no source, no duration, no playback).
    engine.importMedia("/path/does/not/exist.mp4");
    QCOMPARE(engine.mediaList().size(), 1);
    QCOMPARE(engine.mediaList().first().toMap().value("path").toString(),
             QString("/path/does/not/exist.mp4"));
    QVERIFY(engine.currentSource().isEmpty());
    QCOMPARE(engine.duration(), 0.0);
    QVERIFY(!engine.isPlaying());
    // The background probe fails for the bogus path: the placeholder is
    // removed and a single error is reported.
    QVERIFY(failedSpy.wait(5000));
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY(engine.mediaList().isEmpty());
    QVERIFY(engine.currentSource().isEmpty());
    QCOMPARE(engine.duration(), 0.0);
  }

  void importMedia_probesRealFileWithoutLoadingProgram() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clip = dir.filePath("import.mp4");
    QVERIFY(makeImportTestClip(clip));

    TimelineEngine engine;
    QSignalSpy listSpy(&engine, &TimelineEngine::mediaListChanged);
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    engine.importMedia(clip);
    QCOMPARE(engine.mediaList().size(), 1);  // placeholder, duration unknown
    // Probe completion re-emits mediaListChanged with real metadata.
    QTRY_COMPARE_WITH_TIMEOUT(listSpy.count(), 2, 5000);
    const QVariantMap entry = engine.mediaList().first().toMap();
    QVERIFY(entry.value("duration").toDouble() > 0.5);
    QCOMPARE(failedSpy.count(), 0);
    // The program monitor never loaded the import.
    QVERIFY(engine.currentSource().isEmpty());
    QCOMPARE(engine.duration(), 0.0);
    QVERIFY(!engine.isPlaying());
    // Timeline clips cut from the probed entry carry real durations.
    engine.appendClipToTimeline(clip);
    QVERIFY(engine.timelineClips().first().toMap().value("duration").toDouble() > 0.5);
  }

  void seekBeforeLoad_isIgnored() {
    TimelineEngine engine;
    engine.seek(10.0);
    QCOMPARE(engine.position(), 0.0);
    QVERIFY(!engine.frameAvailable());
  }

  void effectDefaults_areSane() {
    TimelineEngine engine;
    QCOMPARE(engine.clipScaleX(), 1.0);
    QCOMPARE(engine.clipScaleY(), 1.0);
    QCOMPARE(engine.clipRotation(), 0.0);
    QCOMPARE(engine.volume(), 1.0);
    QVERIFY(engine.mediaList().isEmpty());
  }

  void engineStartsWithoutAudio() {
    TimelineEngine engine;
    QVERIFY(!engine.hasAudio());
  }

  void removeMedia_removesEntryAndEmits() {
    TimelineEngine engine;
    QSignalSpy spy(&engine, &TimelineEngine::mediaListChanged);
    // Simulate loaded entries via onLoaded indirectly is private,
    // so verify remove/clear on empty list are safe no-ops.
    engine.removeMedia("/nonexistent/clip.mp4");
    QCOMPARE(engine.mediaList().size(), 0);
    QCOMPARE(spy.count(), 0);
    engine.clearMedia();
    QCOMPARE(engine.mediaList().size(), 0);
    QCOMPARE(spy.count(), 0);
  }

  void timelineAppend_unknownPathUsesPlaceholder() {
    TimelineEngine engine;
    QSignalSpy spy(&engine, &TimelineEngine::timelineClipsChanged);
    engine.appendClipToTimeline("/clips/a.mp4");
    QCOMPARE(engine.timelineClips().size(), 1);
    QCOMPARE(spy.count(), 1);
    const QVariantMap clip = engine.timelineClips().first().toMap();
    QCOMPARE(clip.value("path").toString(), QString("/clips/a.mp4"));
    QCOMPARE(clip.value("startTime").toDouble(), 0.0);
    QVERIFY(clip.value("duration").toDouble() > 0.0);
  }

  void timelineSequence_startTimesAccumulate() {
    TimelineEngine engine;
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.appendClipToTimeline("/clips/b.mp4");
    engine.appendClipToTimeline("/clips/c.mp4");
    QCOMPARE(engine.timelineClips().size(), 3);
    const double d0 = engine.timelineClips().at(0).toMap().value("duration").toDouble();
    const double d1 = engine.timelineClips().at(1).toMap().value("duration").toDouble();
    QCOMPARE(engine.timelineClips().at(0).toMap().value("startTime").toDouble(), 0.0);
    QCOMPARE(engine.timelineClips().at(1).toMap().value("startTime").toDouble(), d0);
    QCOMPARE(engine.timelineClips().at(2).toMap().value("startTime").toDouble(), d0 + d1);
  }

  void timelineRemoveMoveClear() {
    TimelineEngine engine;
    QSignalSpy spy(&engine, &TimelineEngine::timelineClipsChanged);
    engine.removeTimelineClip(0);   // no-op on empty
    engine.moveTimelineClip(0, 1);  // no-op on empty
    QCOMPARE(spy.count(), 0);
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.appendClipToTimeline("/clips/b.mp4");
    engine.moveTimelineClip(0, 1);
    QCOMPARE(engine.timelineClips().at(0).toMap().value("path").toString(),
             QString("/clips/b.mp4"));
    QCOMPARE(engine.timelineClips().at(0).toMap().value("startTime").toDouble(), 0.0);
    engine.removeTimelineClip(0);
    QCOMPARE(engine.timelineClips().size(), 1);
    QCOMPARE(engine.timelineClips().at(0).toMap().value("startTime").toDouble(), 0.0);
    engine.clearTimeline();
    QVERIFY(engine.timelineClips().isEmpty());
    const int countBefore = spy.count();
    engine.clearTimeline();  // no-op when empty
    QCOMPARE(spy.count(), countBefore);
  }

  void timelineInsert_placesClipAndRecomputesStarts() {
    TimelineEngine engine;
    QSignalSpy spy(&engine, &TimelineEngine::timelineClipsChanged);
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.appendClipToTimeline("/clips/c.mp4");
    engine.insertClipToTimeline("/clips/b.mp4", 1);
    QCOMPARE(engine.timelineClips().size(), 3);
    QCOMPARE(engine.timelineClips().at(1).toMap().value("path").toString(),
             QString("/clips/b.mp4"));
    const double d0 = engine.timelineClips().at(0).toMap().value("duration").toDouble();
    const double d1 = engine.timelineClips().at(1).toMap().value("duration").toDouble();
    QCOMPARE(engine.timelineClips().at(1).toMap().value("startTime").toDouble(), d0);
    QCOMPARE(engine.timelineClips().at(2).toMap().value("startTime").toDouble(), d0 + d1);
    // Out-of-range index clamps to append; negative clamps to front.
    engine.insertClipToTimeline("/clips/z.mp4", 99);
    QCOMPARE(engine.timelineClips().last().toMap().value("path").toString(),
             QString("/clips/z.mp4"));
    engine.insertClipToTimeline("/clips/front.mp4", -5);
    QCOMPARE(engine.timelineClips().first().toMap().value("path").toString(),
             QString("/clips/front.mp4"));
    QCOMPARE(engine.timelineClips().first().toMap().value("startTime").toDouble(), 0.0);
    QCOMPARE(spy.count(), 5);
    engine.insertClipToTimeline("", 0);  // empty path is a no-op
    QCOMPARE(spy.count(), 5);
  }

  void sequenceIndexOf_findsClips() {
    TimelineEngine engine;
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.appendClipToTimeline("/clips/b.mp4");
    QCOMPARE(engine.sequenceIndexOf("/clips/a.mp4"), 0);
    QCOMPARE(engine.sequenceIndexOf("/clips/b.mp4"), 1);
    QCOMPARE(engine.sequenceIndexOf("/clips/missing.mp4"), -1);
    QCOMPARE(engine.sequenceIndexOf("file:///clips/a.mp4"), 0);
    QVERIFY(!engine.sequencePlaying());
    QCOMPARE(engine.sequenceIndex(), -1);
    engine.playSequenceFrom(0);  // out of scope decode; disarm right away
    QVERIFY(engine.sequencePlaying());
    engine.stopSequence();
  }

  void playSequenceFrom_emptyIsNoOp() {
    TimelineEngine engine;
    QSignalSpy spy(&engine, &TimelineEngine::sequencePlayingChanged);
    engine.playSequenceFrom(0);
    QVERIFY(!engine.sequencePlaying());
    QCOMPARE(spy.count(), 0);
  }

  void sequencePlayback_skipsFailedClipThenStops() {
    TimelineEngine engine;
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.appendClipToTimeline("/clips/b.mp4");
    engine.playSequenceFrom(0);
    QVERIFY(engine.sequencePlaying());
    QCOMPARE(engine.sequenceIndex(), 0);
    QVERIFY(failedSpy.wait(2000));  // clip 0 fails -> skip
    QVERIFY(engine.sequencePlaying());
    QCOMPARE(engine.sequenceIndex(), 1);
    QVERIFY(failedSpy.wait(2000));  // clip 1 fails -> end of sequence
    QVERIFY(!engine.sequencePlaying());
    QCOMPARE(engine.sequenceIndex(), -1);
  }

  void sequencePlayback_structuralEditCancels() {
    TimelineEngine engine;
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.appendClipToTimeline("/clips/b.mp4");
    engine.playSequenceFrom(1);
    QVERIFY(engine.sequencePlaying());
    engine.removeTimelineClip(0);
    QVERIFY(!engine.sequencePlaying());
    engine.playSequenceFrom(0);
    QVERIFY(engine.sequencePlaying());
    engine.clearTimeline();
    QVERIFY(!engine.sequencePlaying());
    engine.stopSequence();
  }

  void sequencePlayback_skipsOverlayClips() {
    TimelineEngine engine;
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.appendOverlayClip("/clips/logo.mp4", 2.0);
    engine.appendClipToTimeline("/clips/b.mp4");
    QCOMPARE(engine.sequenceCount(), 2);
    engine.playSequenceFrom(0);
    QVERIFY(engine.sequencePlaying());
    QCOMPARE(engine.sequenceIndex(), 0);
    QCOMPARE(engine.sequencePosition(), 1);
    QVERIFY(failedSpy.wait(2000));  // clip 0 fails -> advance skips the V2 overlay
    QVERIFY(engine.sequencePlaying());
    QCOMPARE(engine.sequenceIndex(), 2);
    QCOMPARE(engine.sequencePosition(), 2);
    QVERIFY(failedSpy.wait(2000));  // last V1 clip fails -> end of sequence
    QVERIFY(!engine.sequencePlaying());
    QCOMPARE(engine.sequenceIndex(), -1);
  }

  void playSequenceFrom_resolvesOverlayStart() {
    TimelineEngine engine;
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.appendOverlayClip("/clips/logo.mp4", 2.0);
    engine.appendClipToTimeline("/clips/b.mp4");
    // Starting on the overlay snaps forward to the next V1 clip.
    engine.playSequenceFrom(1);
    QVERIFY(engine.sequencePlaying());
    QCOMPARE(engine.sequenceIndex(), 2);
    engine.stopSequence();
    // With no later V1 clip, snap backward to the previous one.
    engine.removeTimelineClip(2);
    engine.playSequenceFrom(1);
    QCOMPARE(engine.sequenceIndex(), 0);
    engine.stopSequence();
  }

  void playSequenceFrom_allOverlayIsNoOp() {
    TimelineEngine engine;
    QSignalSpy spy(&engine, &TimelineEngine::sequencePlayingChanged);
    engine.appendOverlayClip("/clips/logo.mp4", 0.0);
    engine.playSequenceFrom(0);
    QVERIFY(!engine.sequencePlaying());
    QCOMPARE(spy.count(), 0);
  }

  void pause_preservesSequenceResume() {
    TimelineEngine engine;
    // Seed a real duration so play() can resume without a decoder.
    QMetaObject::invokeMethod(&engine, "onLoaded", Q_ARG(QString, QString("/clips/a.mp4")),
                              Q_ARG(double, 10.0), Q_ARG(int, 640), Q_ARG(int, 480),
                              Q_ARG(bool, false));
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.appendClipToTimeline("/clips/b.mp4");
    engine.playSequenceFrom(0);
    QVERIFY(engine.sequencePlaying());
    engine.play();  // no decoder yet, but the seeded duration lets the clock run
    QVERIFY(engine.isPlaying());
    engine.pause();
    QVERIFY(!engine.isPlaying());
    // Pause is transport-only: the lane position and trim window survive.
    QVERIFY(engine.sequencePlaying());
    QCOMPARE(engine.sequenceIndex(), 0);
    engine.play();
    QVERIFY(engine.isPlaying());
    QVERIFY(engine.sequencePlaying());
    QCOMPARE(engine.sequenceIndex(), 0);
    engine.pause();
    QVERIFY(engine.sequencePlaying());
    QCOMPARE(engine.sequenceIndex(), 0);
    engine.stopSequence();
  }

  void sequenceIndexOf_prefersV1Lane() {
    TimelineEngine engine;
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.appendOverlayClip("/clips/a.mp4", 1.0);
    // Same file on both lanes: resume targets the program clip.
    QCOMPARE(engine.sequenceIndexOf("/clips/a.mp4"), 0);
    QCOMPARE(engine.sequenceCount(), 1);
    QCOMPARE(engine.sequencePosition(), 0);  // idle
    engine.playSequenceFrom(0);
    QCOMPARE(engine.sequencePosition(), 1);
    engine.stopSequence();
  }

  void moveTimelineClip_keepsPlayingIndex() {
    TimelineEngine engine;
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.appendClipToTimeline("/clips/b.mp4");
    engine.appendClipToTimeline("/clips/c.mp4");
    engine.playSequenceFrom(2);
    QCOMPARE(engine.sequenceIndex(), 2);
    engine.moveTimelineClip(2, 0);
    QVERIFY(engine.sequencePlaying());
    QCOMPARE(engine.sequenceIndex(), 0);
    QCOMPARE(engine.timelineClips().at(0).toMap().value("path").toString(),
             QString("/clips/c.mp4"));
    engine.moveTimelineClip(0, 2);
    QCOMPARE(engine.sequenceIndex(), 2);
    QCOMPARE(engine.timelineClips().at(2).toMap().value("path").toString(),
             QString("/clips/c.mp4"));
    engine.stopSequence();
  }

  void projectSaveLoad_roundTrip() {
    TimelineEngine engine;
    // Seed the bin without a real decoder via the loaded() path.
    QMetaObject::invokeMethod(&engine, "onLoaded", Q_ARG(QString, QString("/clips/a.mp4")),
                              Q_ARG(double, 10.0), Q_ARG(int, 640), Q_ARG(int, 480),
                              Q_ARG(bool, false));
    QMetaObject::invokeMethod(&engine, "onLoaded", Q_ARG(QString, QString("/clips/b.mp4")),
                              Q_ARG(double, 20.0), Q_ARG(int, 640), Q_ARG(int, 480),
                              Q_ARG(bool, false));
    QCOMPARE(engine.mediaList().size(), 2);
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.appendClipToTimeline("/clips/b.mp4");
    engine.setClipEffect(0, "Vignette");
    engine.setClipTitle(1, "Second");
    engine.setVolume(0.5);
    engine.setClipRotation(90.0);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("project.reels.json");
    QVERIFY(engine.saveProject(path));

    engine.clearTimeline();
    engine.clearMedia();
    engine.setVolume(1.0);
    engine.setClipRotation(0.0);
    QVERIFY(engine.timelineClips().isEmpty());

    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    QVERIFY(engine.loadProject(path));
    QCOMPARE(engine.mediaList().size(), 2);
    QCOMPARE(engine.timelineClips().size(), 2);
    QCOMPARE(engine.timelineClips().at(0).toMap().value("duration").toDouble(), 10.0);
    QCOMPARE(engine.timelineClips().at(0).toMap().value("startTime").toDouble(), 0.0);
    QCOMPARE(engine.timelineClips().at(1).toMap().value("duration").toDouble(), 20.0);
    QCOMPARE(engine.timelineClips().at(1).toMap().value("startTime").toDouble(), 10.0);
    QCOMPARE(engine.volume(), 0.5);
    QCOMPARE(engine.clipRotation(), 90.0);
    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(engine.timelineClips().first().toMap().value("effect").toString(),
             QString("Vignette"));
    QCOMPARE(engine.timelineClips().at(1).toMap().value("title").toString(), QString("Second"));
  }

  void importMedia_marksDirtyAndUndoable() {
    TimelineEngine engine;
    QVERIFY(!engine.isModified());
    QVERIFY(!engine.canUndo());
    // Bogus path: assertions run synchronously on the placeholder, before
    // the background probe failure cleans it up.
    engine.importMedia("/path/does/not/exist.mp4");
    QCOMPARE(engine.mediaList().size(), 1);
    QVERIFY(engine.isModified());
    QVERIFY(engine.canUndo());
    engine.undo();
    QVERIFY(engine.mediaList().isEmpty());
    QVERIFY(!engine.canUndo());
    QVERIFY(engine.canRedo());
    engine.redo();
    QCOMPARE(engine.mediaList().size(), 1);
  }

  void importMedia_staysDirtyAfterProbe() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clip = dir.filePath("import-dirty.mp4");
    QVERIFY(makeImportTestClip(clip));

    TimelineEngine engine;
    QSignalSpy listSpy(&engine, &TimelineEngine::mediaListChanged);
    engine.importMedia(clip);
    QVERIFY(engine.isModified());
    // Probe enrichment (placeholder duration 0 -> real) must not clear the
    // dirty flag, even if the user saved while the probe was in flight.
    QTRY_COMPARE_WITH_TIMEOUT(listSpy.count(), 2, 5000);
    QVERIFY(engine.isModified());
    QVERIFY(engine.canUndo());
    engine.undo();
    QVERIFY(engine.mediaList().isEmpty());
  }

  void importMedia_undoRedoReprobes() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clip = dir.filePath("import-undo.mp4");
    QVERIFY(makeImportTestClip(clip));

    TimelineEngine engine;
    engine.importMedia(clip);
    // Undo before the probe lands: the snapshot keeps the duration-0
    // placeholder, and the in-flight probe must be discarded as stale.
    engine.undo();
    QVERIFY(engine.mediaList().isEmpty());
    // Redo restores the placeholder; applySnapshot must re-arm the probe so
    // the entry is enriched instead of staying a 0s ghost.
    engine.redo();
    QCOMPARE(engine.mediaList().size(), 1);
    QVERIFY(engine.isModified());
    QTRY_VERIFY_WITH_TIMEOUT(engine.mediaList().first().toMap().value("duration").toDouble() > 0.5,
                             5000);
  }

  void projectSave_overwriteKeepsUsableFile() {
    TimelineEngine engine;
    QMetaObject::invokeMethod(&engine, "onLoaded", Q_ARG(QString, QString("/clips/a.mp4")),
                              Q_ARG(double, 10.0), Q_ARG(int, 640), Q_ARG(int, 480),
                              Q_ARG(bool, false));
    engine.appendClipToTimeline("/clips/a.mp4");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("project.reels.json");
    QVERIFY(engine.saveProject(path));
    QVERIFY(!engine.isModified());

    // Overwriting an existing project (previously a remove+rename window
    // that could lose both copies) must succeed and hold the new content.
    engine.appendClipToTimeline("/clips/a.mp4");
    QVERIFY(engine.saveProject(path));
    TimelineEngine reloaded;
    QVERIFY(reloaded.loadProject(path));
    QCOMPARE(reloaded.timelineClips().size(), 2);

    // A failed save must leave the previous project intact.
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    QVERIFY(!engine.saveProject(dir.filePath("no-such-dir/x.json")));
    QCOMPARE(failedSpy.count(), 1);
    TimelineEngine intact;
    QVERIFY(intact.loadProject(path));
    QCOMPARE(intact.timelineClips().size(), 2);
  }

  void projectLoad_invalidFileKeepsState() {
    TimelineEngine engine;
    engine.appendClipToTimeline("/clips/a.mp4");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString corruptPath = dir.filePath("corrupt.reels.json");
    QFile corrupt(corruptPath);
    QVERIFY(corrupt.open(QIODevice::WriteOnly));
    corrupt.write("not json {{{");
    corrupt.close();

    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    QVERIFY(!engine.loadProject(corruptPath));
    QVERIFY(!engine.loadProject(dir.filePath("missing.reels.json")));
    QVERIFY(!engine.saveProject(dir.filePath("no-such-dir/x.json")));
    QCOMPARE(failedSpy.count(), 3);
    QCOMPARE(engine.timelineClips().size(), 1);  // state preserved
  }

  void undoRedo_timelineEdits() {
    TimelineEngine engine;
    QVERIFY(!engine.canUndo());
    QVERIFY(!engine.canRedo());
    QVERIFY(!engine.isModified());
    engine.undo();  // no-ops on empty stacks
    engine.redo();

    engine.appendClipToTimeline("/clips/a.mp4");
    engine.appendClipToTimeline("/clips/b.mp4");
    QVERIFY(engine.canUndo());
    QVERIFY(engine.isModified());

    engine.undo();
    QCOMPARE(engine.timelineClips().size(), 1);
    QVERIFY(engine.canRedo());
    engine.undo();
    QVERIFY(engine.timelineClips().isEmpty());
    QVERIFY(!engine.canUndo());

    engine.redo();
    QCOMPARE(engine.timelineClips().size(), 1);
    engine.redo();
    QCOMPARE(engine.timelineClips().size(), 2);
    QVERIFY(!engine.canRedo());

    // A new edit clears the redo stack.
    engine.undo();
    engine.appendClipToTimeline("/clips/c.mp4");
    QVERIFY(!engine.canRedo());
    QCOMPARE(engine.timelineClips().size(), 2);
    QCOMPARE(engine.timelineClips().at(1).toMap().value("path").toString(),
             QString("/clips/c.mp4"));
  }

  void undoRedo_removeAndClear() {
    TimelineEngine engine;
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.appendClipToTimeline("/clips/b.mp4");
    engine.removeTimelineClip(0);
    QCOMPARE(engine.timelineClips().size(), 1);
    engine.undo();
    QCOMPARE(engine.timelineClips().size(), 2);
    QCOMPARE(engine.timelineClips().at(0).toMap().value("startTime").toDouble(), 0.0);
    engine.clearTimeline();
    QVERIFY(engine.timelineClips().isEmpty());
    engine.undo();
    QCOMPARE(engine.timelineClips().size(), 2);
    // Undo back to the pristine state, then redo everything.
    while (engine.canUndo()) {
      engine.undo();
    }
    QVERIFY(engine.timelineClips().isEmpty());
    while (engine.canRedo()) {
      engine.redo();
    }
    QVERIFY(engine.timelineClips().isEmpty());  // last action was clearTimeline
  }

  void newProject_resetsAndIsUndoable() {
    TimelineEngine engine;
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.setVolume(0.5);
    QVERIFY(engine.isModified());
    engine.newProject();
    QVERIFY(engine.timelineClips().isEmpty());
    QVERIFY(engine.mediaList().isEmpty());
    QCOMPARE(engine.volume(), 1.0);
    QCOMPARE(engine.duration(), 0.0);
    QVERIFY(!engine.isModified());
    QVERIFY(engine.canUndo());
    engine.undo();
    QCOMPARE(engine.timelineClips().size(), 1);
    // History covers bin/timeline lists plus mixer settings.
    QCOMPARE(engine.volume(), 0.5);
  }

  void thumbnails_failedDecodeStillResolves() {
    TimelineEngine engine;
    QSignalSpy spy(&engine, &TimelineEngine::thumbnailsChanged);
    QCOMPARE(engine.thumbVersion(), 0);
    QVERIFY(!engine.hasThumb("/clips/ghost.mp4"));
    engine.requestThumb("/clips/ghost.mp4");
    engine.requestThumb("/clips/ghost.mp4");  // deduped while in flight
    QVERIFY(spy.wait(2000));
    QVERIFY(!engine.hasThumb("/clips/ghost.mp4"));
    QVERIFY(engine.thumbImage("/clips/ghost.mp4").isNull());
    QVERIFY(engine.thumbVersion() > 0);
  }

  void trim_clampsAndPreservesPlayable() {
    TimelineEngine engine;
    engine.appendClipToTimeline("/clips/a.mp4");  // placeholder 5.0s source
    engine.setClipTrimStart(0, 1.5);
    QVariantMap clip = engine.timelineClips().first().toMap();
    QCOMPARE(clip.value("trimStart").toDouble(), 1.5);
    QCOMPARE(clip.value("duration").toDouble(), 3.5);
    engine.setClipTrimEnd(0, 1.0);
    clip = engine.timelineClips().first().toMap();
    QCOMPARE(clip.value("duration").toDouble(), 2.5);
    // Over-trimming clamps to a 0.1s minimum playable range.
    engine.setClipTrimStart(0, 99.0);
    clip = engine.timelineClips().first().toMap();
    QVERIFY(clip.value("duration").toDouble() >= 0.1);
    QCOMPARE(clip.value("trimStart").toDouble() + clip.value("duration").toDouble() +
                 clip.value("trimEnd").toDouble(),
             5.0);
    // Out-of-range indices are no-ops.
    engine.setClipTrimStart(-1, 1.0);
    engine.setClipTrimEnd(99, 1.0);
    QVERIFY(engine.canUndo());
  }

  void split_dividesClipAndCarriesTrims() {
    TimelineEngine engine;
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.setClipTrimStart(0, 1.0);  // playable [1,5), 4.0s
    engine.splitTimelineClip(0, 1.5);
    QCOMPARE(engine.timelineClips().size(), 2);
    const QVariantMap left = engine.timelineClips().at(0).toMap();
    const QVariantMap right = engine.timelineClips().at(1).toMap();
    QCOMPARE(left.value("trimStart").toDouble(), 1.0);
    QCOMPARE(left.value("duration").toDouble(), 1.5);
    QCOMPARE(right.value("trimStart").toDouble(), 2.5);
    QCOMPARE(right.value("duration").toDouble(), 2.5);
    QCOMPARE(right.value("startTime").toDouble(), 1.5);
    // Invalid offsets are no-ops.
    engine.splitTimelineClip(0, 0.0);
    engine.splitTimelineClip(5, 1.0);
    engine.splitTimelineClip(1, 99.0);
    QCOMPARE(engine.timelineClips().size(), 2);
    engine.undo();
    QCOMPARE(engine.timelineClips().size(), 1);
  }

  void split_absoluteTracksShiftRightStart() {
    TimelineEngine engine;
    engine.appendClipToTimeline("/clips/a.mp4");     // V1: start 0, dur 5
    engine.appendAudioClip("/clips/bed.mp3", 2.0);   // A1: start 2, dur 5
    engine.appendOverlayClip("/clips/ov.mp4", 1.0);  // V2: start 1, dur 5
    engine.splitTimelineClip(1, 2.0);
    QCOMPARE(engine.timelineClips().size(), 4);
    const QVariantMap left = engine.timelineClips().at(1).toMap();
    const QVariantMap right = engine.timelineClips().at(2).toMap();
    QCOMPARE(left.value("duration").toDouble(), 2.0);
    QCOMPARE(left.value("startTime").toDouble(), 2.0);
    QCOMPARE(right.value("duration").toDouble(), 3.0);
    // Absolute tracks keep their times (no re-layout): the right half must
    // start where the cut lands, not where the clip did.
    QCOMPARE(right.value("startTime").toDouble(), 4.0);
    QCOMPARE(right.value("trimStart").toDouble(), 2.0);
    // V2 overlay splits the same way (index 3 after the A1 split above).
    engine.splitTimelineClip(3, 1.0);
    QCOMPARE(engine.timelineClips().size(), 5);
    QCOMPARE(engine.timelineClips().at(4).toMap().value("startTime").toDouble(), 2.0);
    QCOMPARE(engine.timelineClips().at(4).toMap().value("duration").toDouble(), 4.0);
    // V1 splits still re-lay out sequentially.
    engine.splitTimelineClip(0, 2.0);
    QCOMPARE(engine.timelineClips().at(0).toMap().value("duration").toDouble(), 2.0);
    QCOMPARE(engine.timelineClips().at(0).toMap().value("startTime").toDouble(), 0.0);
    QCOMPARE(engine.timelineClips().at(1).toMap().value("duration").toDouble(), 3.0);
    QCOMPARE(engine.timelineClips().at(1).toMap().value("startTime").toDouble(), 2.0);
    engine.undo();
    QCOMPARE(engine.timelineClips().size(), 5);
    QCOMPARE(engine.timelineClips().at(0).toMap().value("duration").toDouble(), 5.0);
  }

  void setClipEffect_validatesAndIsUndoable() {
    TimelineEngine engine;
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.setClipEffect(0, "Blur");
    QCOMPARE(engine.timelineClips().first().toMap().value("effect").toString(), QString("Blur"));
    // Unknown names normalize to empty; same-value sets are no-ops.
    engine.setClipEffect(0, "Bogus");
    QCOMPARE(engine.timelineClips().first().toMap().value("effect").toString(), QString(""));
    engine.setClipEffect(-1, "Blur");
    engine.undo();  // undoes the Bogus normalization
    engine.undo();  // undoes the Blur application
    QCOMPARE(engine.timelineClips().first().toMap().value("effect").toString(), QString(""));
  }

  void setClipTransition_validatesClampsAndUndoes() {
    TimelineEngine engine;
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.appendClipToTimeline("/clips/b.mp4");
    engine.setClipTransition(0, "Cross Dissolve", 0.5);
    QVariantMap clip = engine.timelineClips().first().toMap();
    QCOMPARE(clip.value("transition").toString(), QString("Cross Dissolve"));
    QCOMPARE(clip.value("transitionDuration").toDouble(), 0.5);
    // Unknown types clear; oversized durations clamp to the playable length.
    engine.setClipTransition(0, "Bogus", 0.5);
    QCOMPARE(engine.timelineClips().first().toMap().value("transition").toString(), QString(""));
    engine.setClipTransition(0, "Dip to Black", 99.0);
    clip = engine.timelineClips().first().toMap();
    QCOMPARE(clip.value("transition").toString(), QString("Dip to Black"));
    QVERIFY(clip.value("transitionDuration").toDouble() <= 5.0);
    engine.setClipTransition(9, "Cross Dissolve", 0.5);  // no-op, no history
    engine.undo();
    QCOMPARE(engine.timelineClips().first().toMap().value("transition").toString(), QString(""));
  }

  void overlayClips_keepAbsolutePosition() {
    TimelineEngine engine;
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.appendClipToTimeline("/clips/b.mp4");
    engine.appendOverlayClip("/clips/logo.mp4", 2.0);
    QCOMPARE(engine.timelineClips().size(), 3);
    const QVariantMap overlay = engine.timelineClips().last().toMap();
    QCOMPARE(overlay.value("track").toInt(), 1);
    QCOMPARE(overlay.value("startTime").toDouble(), 2.0);
    // V1 layout ignores the overlay: b still starts at 5.0.
    QCOMPARE(engine.timelineClips().at(1).toMap().value("startTime").toDouble(), 5.0);
    engine.setClipTrack(2, 0);
    QCOMPARE(engine.timelineClips().at(2).toMap().value("startTime").toDouble(), 10.0);
    engine.undo();
    QCOMPARE(engine.timelineClips().at(2).toMap().value("track").toInt(), 1);
  }

  void setClipTitle_truncatesUndoableAndPersists() {
    TimelineEngine engine;
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.setClipTitle(0, "Opening Scene");
    QCOMPARE(engine.timelineClips().first().toMap().value("title").toString(),
             QString("Opening Scene"));
    engine.setClipTitle(0, QString(300, 'x'));
    QCOMPARE(engine.timelineClips().first().toMap().value("title").toString().size(), 200);
    engine.setClipTitle(9, "Nope");  // out of range: no-op
    engine.undo();
    engine.undo();
    QCOMPARE(engine.timelineClips().first().toMap().value("title").toString(), QString(""));
  }

  void audioGainScalesAndClamps() {
    QByteArray samples;
    samples.resize(4);
    auto* data = reinterpret_cast<qint16*>(samples.data());
    data[0] = 1000;
    data[1] = 20000;

    const QByteArray unity = AudioPlayer::applyGain(QByteArray(samples), 1.0);
    const auto* unityData = reinterpret_cast<const qint16*>(unity.constData());
    QCOMPARE(unityData[0], 1000);
    QCOMPARE(unityData[1], 20000);

    const QByteArray doubled = AudioPlayer::applyGain(QByteArray(samples), 2.0);
    const auto* doubledData = reinterpret_cast<const qint16*>(doubled.constData());
    QCOMPARE(doubledData[0], 2000);
    QCOMPARE(doubledData[1], 32767);

    const QByteArray muted = AudioPlayer::applyGain(QByteArray(samples), 0.0);
    const auto* mutedData = reinterpret_cast<const qint16*>(muted.constData());
    QCOMPARE(mutedData[0], 0);
    QCOMPARE(mutedData[1], 0);
  }

  void clipFadeGain_ramps() {
    QVariantMap clip;
    clip.insert("fadeIn", 1.0);
    clip.insert("fadeOut", 2.0);
    QCOMPARE(ClipUtils::clipFadeGain(clip, 0.0, 4.0), 0.0);
    QCOMPARE(ClipUtils::clipFadeGain(clip, 0.5, 4.0), 0.5);
    QCOMPARE(ClipUtils::clipFadeGain(clip, 2.0, 4.0), 1.0);
    QCOMPARE(ClipUtils::clipFadeGain(clip, 3.0, 4.0), 0.5);
    QCOMPARE(ClipUtils::clipFadeGain(clip, 4.0, 4.0), 0.0);
    QVariantMap plain;
    QCOMPARE(ClipUtils::clipFadeGain(plain, 1.0, 4.0), 1.0);
    QCOMPARE(ClipUtils::clipEffectiveGain(plain, 0.5), 0.5);
    plain.insert("muted", true);
    QCOMPARE(ClipUtils::clipEffectiveGain(plain, 0.5), 0.0);
  }

  void mixAudio_sumsAndClamps() {
    QByteArray a(4, 0);
    QByteArray b(4, 0);
    auto* pa = reinterpret_cast<qint16*>(a.data());
    auto* pb = reinterpret_cast<qint16*>(b.data());
    pa[0] = 1000;
    pa[1] = -1000;
    pb[0] = 500;
    pb[1] = 30000;
    const QByteArray mixed = AudioPlayer::mixAudio(a, b);
    const auto* pm = reinterpret_cast<const qint16*>(mixed.constData());
    QCOMPARE(pm[0], 1500);
    QCOMPARE(pm[1], 29000);
    pa[0] = 20000;
    pb[0] = 20000;
    const QByteArray clipped = AudioPlayer::mixAudio(a, b);
    QCOMPARE(reinterpret_cast<const qint16*>(clipped.constData())[0], 32767);
    QCOMPARE(AudioPlayer::mixAudio(QByteArray(), b), b);
    QCOMPARE(AudioPlayer::mixAudio(a, QByteArray()), a);
  }

  void audioBed_modelOps() {
    TimelineEngine engine;
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.appendAudioClip("/clips/bed.mp3", 2.0);
    QCOMPARE(engine.timelineClips().size(), 2);
    QCOMPARE(engine.timelineClips().at(1).toMap().value("track").toInt(), 2);
    QCOMPARE(engine.timelineClips().at(1).toMap().value("startTime").toDouble(), 2.0);
    engine.setClipGain(1, 9.0);
    QCOMPARE(engine.timelineClips().at(1).toMap().value("gain").toDouble(), 2.0);
    engine.setClipGain(1, -3.0);
    QCOMPARE(engine.timelineClips().at(1).toMap().value("gain").toDouble(), 0.0);
    engine.setClipMuted(1, true);
    QVERIFY(engine.timelineClips().at(1).toMap().value("muted").toBool());
    engine.setClipFadeIn(1, 99.0);
    QCOMPARE(engine.timelineClips().at(1).toMap().value("fadeIn").toDouble(), 30.0);
    engine.setClipFadeOut(1, -1.0);
    QCOMPARE(engine.timelineClips().at(1).toMap().value("fadeOut").toDouble(), 0.0);
    engine.setClipGain(9, 1.0);  // out of range: no-op
    engine.setClipMuted(9, true);
    // detachAudio: V1 picture clip muted, bed copy appended.
    engine.setClipMuted(1, false);
    const int before = engine.timelineClips().size();
    engine.detachAudio(0);
    QCOMPARE(engine.timelineClips().size(), before + 1);
    QVERIFY(engine.timelineClips().at(0).toMap().value("muted").toBool());
    QCOMPARE(engine.timelineClips().last().toMap().value("track").toInt(), 2);
    engine.detachAudio(9);  // out of range: no-op
    engine.detachAudio(1);  // bed row is V1-only: no-op
    QCOMPARE(engine.timelineClips().size(), before + 1);
  }

  void bedClipAt_resolvesTopmostAudible() {
    TimelineEngine engine;
    engine.appendClipToTimeline("/clips/a.mp4");    // V1: start 0, dur 5
    engine.appendAudioClip("/clips/bed.mp3", 2.0);  // A1: start 2, dur 5
    QVERIFY(engine.bedClipAt(1.0).isEmpty());
    QCOMPARE(engine.bedClipAt(3.0).value("path").toString(), QString("/clips/bed.mp3"));
    QVERIFY(engine.bedClipAt(7.0).isEmpty());  // exclusive end at 2 + 5
    // No loaded source: output time is identity.
    QCOMPARE(engine.outputTimeFor(3.0), 3.0);
    // Muted or zero-gain beds never cover.
    engine.setClipMuted(1, true);
    QVERIFY(engine.bedClipAt(3.0).isEmpty());
    engine.setClipMuted(1, false);
    engine.setClipGain(1, 0.0);
    QVERIFY(engine.bedClipAt(3.0).isEmpty());
    // Overlay beds resolve too; last in list wins.
    engine.setClipGain(1, 1.0);
    engine.appendOverlayClip("/clips/ov.mp4", 3.0);
    QCOMPARE(engine.bedClipAt(4.0).value("path").toString(), QString("/clips/ov.mp4"));
  }

  void projectSaveLoad_preservesAudioFields() {
    TimelineEngine engine;
    QMetaObject::invokeMethod(&engine, "onLoaded", Q_ARG(QString, QString("/clips/a.mp4")),
                              Q_ARG(double, 10.0), Q_ARG(int, 640), Q_ARG(int, 480),
                              Q_ARG(bool, true));
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.appendAudioClip("/clips/a.mp4", 4.0);
    engine.setClipGain(0, 0.5);
    engine.setClipMuted(1, true);
    engine.setClipFadeIn(1, 0.5);
    engine.setClipFadeOut(1, 1.0);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("audio.reels.json");
    QVERIFY(engine.saveProject(path));

    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    QVERIFY(engine.loadProject(path));
    QCOMPARE(engine.timelineClips().size(), 2);
    QCOMPARE(engine.timelineClips().at(0).toMap().value("gain").toDouble(), 0.5);
    const QVariantMap bed = engine.timelineClips().at(1).toMap();
    QCOMPARE(bed.value("track").toInt(), 2);
    QCOMPARE(bed.value("startTime").toDouble(), 4.0);
    QVERIFY(bed.value("muted").toBool());
    QCOMPARE(bed.value("fadeIn").toDouble(), 0.5);
    QCOMPARE(bed.value("fadeOut").toDouble(), 1.0);
    // Resolved against the reloaded lists, the (unmuted) bed still covers.
    engine.setClipMuted(1, false);
    QCOMPARE(engine.bedClipAt(5.0).value("startTime").toDouble(), 4.0);
    QCOMPARE(failedSpy.count(), 0);
  }

  // Per-chunk preview gain x fade, driven synchronously (no fixtures): the
  // played-frames clock advances across chunks, and row edits apply live.
  void previewChunkAppliesGainAndFade() {
    TimelineEngine engine;
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.setClipGain(0, 0.5);
    engine.setClipFadeIn(0, 1.0);
    QMetaObject::invokeMethod(&engine, "onLoaded", Q_ARG(QString, QString("/clips/a.mp4")),
                              Q_ARG(double, 10.0), Q_ARG(int, 640), Q_ARG(int, 480),
                              Q_ARG(bool, true));
    // Chunk 1: tMid = 2400/48000 = 0.05 -> fade 0.05 -> ~25.
    QMetaObject::invokeMethod(&engine, "onAudioData", Q_ARG(QByteArray, makeConstChunk(4800, 1000)),
                              Q_ARG(double, 0.0));
    QByteArray fifo = engine.peekPreviewAudio();
    QCOMPARE(fifo.size(), 4800 * 4);
    QVERIFY2(qAbs(meanS16(fifo, 0, 4800) - 25.0) < 4.0,
             qPrintable(QString("mean=%1").arg(meanS16(fifo, 0, 4800))));
    // Chunk 2 continues the fade clock: tMid = 0.15 -> ~75.
    QMetaObject::invokeMethod(&engine, "onAudioData", Q_ARG(QByteArray, makeConstChunk(4800, 1000)),
                              Q_ARG(double, 0.1));
    fifo = engine.peekPreviewAudio();
    QCOMPARE(fifo.size(), 2 * 4800 * 4);
    QVERIFY2(qAbs(meanS16(fifo, 4800, 9600) - 75.0) < 4.0,
             qPrintable(QString("mean=%1").arg(meanS16(fifo, 4800, 9600))));
    // Clearing the fade restores the full gained level with no restart.
    engine.setClipFadeIn(0, 0.0);
    QMetaObject::invokeMethod(&engine, "onAudioData", Q_ARG(QByteArray, makeConstChunk(4800, 1000)),
                              Q_ARG(double, 0.2));
    fifo = engine.peekPreviewAudio();
    QCOMPARE(fifo.size(), 3 * 4800 * 4);
    QVERIFY2(qAbs(meanS16(fifo, 9600, 14400) - 500.0) < 4.0,
             qPrintable(QString("mean=%1").arg(meanS16(fifo, 9600, 14400))));
  }

  // End-to-end live mix: program 440 Hz + bed 880 Hz both present in the
  // queued preview PCM (Goertzel over the buffered tail). Note the dummy
  // audio backend drains the FIFO, so it hovers near the 400 ms refill
  // level instead of accumulating: key off playhead progress, not size.
  void liveBedMix_containsBothTones() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString prog = dir.filePath("prog.mp4");
    const QString bed = dir.filePath("bed.m4a");
    QVERIFY(makeAVToneClip(prog, "red", 440, 4.0));
    QVERIFY(makeSineBedClip(bed, 880, 4.0));

    TimelineEngine engine;
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    engine.appendClipToTimeline(prog);
    engine.appendAudioClip(bed, 0.0);
    engine.playSequenceFrom(0);
    // Both decoder threads long since flowing (bed open lands async).
    QTRY_VERIFY_WITH_TIMEOUT(engine.position() > 2.0, 15000);
    engine.pause();
    const QByteArray pcm = engine.peekPreviewAudio();
    const int totalFrames = static_cast<int>(pcm.size() / 4);
    QVERIFY2(totalFrames >= 20000, qPrintable(QString("frames=%1").arg(totalFrames)));
    const int from = totalFrames - std::min(totalFrames, 48000);
    const int n = totalFrames - from;
    const double e440 = toneEnergy(pcm, from, n, 440.0);
    const double e880 = toneEnergy(pcm, from, n, 880.0);
    const double eRef = toneEnergy(pcm, from, n, 1000.0);
    QVERIFY2(e440 > 20.0 * eRef, qPrintable(QString("440=%1 ref=%2").arg(e440).arg(eRef)));
    QVERIFY2(e880 > 20.0 * eRef, qPrintable(QString("880=%1 ref=%2").arg(e880).arg(eRef)));
    QCOMPARE(failedSpy.count(), 0);
  }

  // Bed-only preview: silent V1 program, audible bed still flows.
  void liveBedOnly_withoutProgram() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString silent = dir.filePath("silent.mp4");
    const QString bed = dir.filePath("bed.m4a");
    QVERIFY(makeSilentVideoClip(silent, 4.0));
    QVERIFY(makeSineBedClip(bed, 880, 4.0));

    TimelineEngine engine;
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    engine.appendClipToTimeline(silent);
    engine.appendAudioClip(bed, 0.0);
    engine.playSequenceFrom(0);
    QTRY_VERIFY_WITH_TIMEOUT(engine.position() > 2.0, 15000);
    engine.pause();
    const QByteArray pcm = engine.peekPreviewAudio();
    const int totalFrames = static_cast<int>(pcm.size() / 4);
    QVERIFY2(totalFrames >= 20000, qPrintable(QString("frames=%1").arg(totalFrames)));
    const int from = totalFrames - std::min(totalFrames, 48000);
    const int n = totalFrames - from;
    const double e880 = toneEnergy(pcm, from, n, 880.0);
    const double eRef = toneEnergy(pcm, from, n, 1000.0);
    QVERIFY2(e880 > 20.0 * eRef, qPrintable(QString("880=%1 ref=%2").arg(e880).arg(eRef)));
    QCOMPARE(failedSpy.count(), 0);
  }

  // Dead bed file: transient warning (not sticky failure), program plays on.
  void bedFailure_warnsTransiently() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString prog = dir.filePath("prog.mp4");
    QVERIFY(makeAVToneClip(prog, "red", 440, 4.0));

    TimelineEngine engine;
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    QSignalSpy warnSpy(&engine, &TimelineEngine::warning);
    engine.appendClipToTimeline(prog);
    engine.appendAudioClip(dir.filePath("missing-bed.mp3"), 0.0);
    engine.playSequenceFrom(0);
    QTRY_VERIFY_WITH_TIMEOUT(warnSpy.count() >= 1, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(engine.position() > 1.0, 15000);
    // Program audio keeps queuing through the dead bed (FIFO hovers near
    // the refill level under the dummy backend; just require flow).
    QTRY_VERIFY_WITH_TIMEOUT(engine.peekPreviewAudio(200000).size() >= 40000, 15000);
    QVERIFY(engine.isPlaying());
    QCOMPARE(failedSpy.count(), 0);
    engine.pause();
  }

  // Source monitor is silent by design: concurrent source playback leaves
  // program transport and program PCM untouched (no 660 Hz source tone in
  // the program FIFO, only the 440 Hz program tone).
  void sourcePlayback_leavesProgramAudioUntouched() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString prog = dir.filePath("prog.mp4");
    const QString src = dir.filePath("src.mp4");
    QVERIFY(makeAVToneClip(prog, "red", 440, 6.0));
    QVERIFY(makeAVToneClip(src, "blue", 660, 3.0));

    TimelineEngine engine;
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    engine.appendClipToTimeline(prog);
    engine.playSequenceFrom(0);
    QTRY_VERIFY_WITH_TIMEOUT(engine.position() > 0.5, 15000);
    const double posA = engine.position();
    engine.loadSource(src);  // auto-plays on ready, program undisturbed
    QTRY_VERIFY_WITH_TIMEOUT(engine.sourcePosition() > 0.2, 15000);
    QVERIFY(engine.isPlaying());
    QVERIFY(engine.sourcePlaying());
    QTest::qWait(1000);  // overlap window: both transports running
    QVERIFY(engine.isPlaying());
    QVERIFY(engine.position() > posA);
    QVERIFY(engine.peekPreviewAudio(200000).size() >= 40000);
    engine.pause();
    engine.pauseSource();
    // PCM proof: program tone strong, source tone absent from program FIFO.
    const QByteArray pcm = engine.peekPreviewAudio();
    const int totalFrames = static_cast<int>(pcm.size() / 4);
    QVERIFY2(totalFrames >= 20000, qPrintable(QString("frames=%1").arg(totalFrames)));
    const int from = totalFrames - std::min(totalFrames, 48000);
    const int n = totalFrames - from;
    const double e440 = toneEnergy(pcm, from, n, 440.0);
    const double e660 = toneEnergy(pcm, from, n, 660.0);
    const double eRef = toneEnergy(pcm, from, n, 1000.0);
    QVERIFY2(e440 > 20.0 * eRef, qPrintable(QString("440=%1 ref=%2").arg(e440).arg(eRef)));
    QVERIFY2(e660 < e440 / 20.0, qPrintable(QString("660=%1 prog440=%2").arg(e660).arg(e440)));
    QCOMPARE(failedSpy.count(), 0);
  }

  void audioPlayerBuffersWithoutBackend() {
    AudioPlayer player;
    player.setVolume(9.0);
    QCOMPARE(player.volume(), 2.0);
    player.setVolume(-1.0);
    QCOMPARE(player.volume(), 0.0);

    QByteArray chunk(4800, '\x01');
    player.queueData(chunk);
    QCOMPARE(player.bufferedBytes(), static_cast<qint64>(chunk.size()));
    QVERIFY(player.bufferedMsecs() > 0);
    player.stop();
    QCOMPARE(player.bufferedBytes(), 0);
  }

  void effectSetters_clampOutOfRange() {
    TimelineEngine engine;
    engine.setClipScaleX(99.0);
    QCOMPARE(engine.clipScaleX(), 5.0);
    engine.setClipScaleY(0.0);
    QCOMPARE(engine.clipScaleY(), 0.1);
    engine.setClipRotation(720.0);
    QCOMPARE(engine.clipRotation(), 360.0);
    engine.setVolume(-1.0);
    QCOMPARE(engine.volume(), 0.0);
    engine.setVolume(9.0);
    QCOMPARE(engine.volume(), 2.0);
  }

  void catalogs_matchLibrary() {
    TimelineEngine engine;
    const QStringList effects = engine.availableEffects();
    QVERIFY(effects.contains("Blur"));
    QVERIFY(effects.contains("Glitch"));
    const QStringList transitions = engine.availableTransitions();
    QVERIFY(transitions.contains("Cross Dissolve"));
    QVERIFY(transitions.contains("Dip to Black"));
    QVERIFY(TimelineEngine::isKnownTransition(transitions.first()));
  }

  void cancelExport_withoutExportIsNoop() {
    TimelineEngine engine;
    QVERIFY(!engine.isExporting());
    engine.cancelExport();
    QVERIFY(!engine.isExporting());
  }

  void exportEmpty_reportsFailure() {
    TimelineEngine engine;
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    engine.exportSequence(dir.filePath("primoreels-empty-test.mp4"));
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY(!engine.isExporting());
  }

  void continuousScrubbing_rapidSeeksConverge() {
    TimelineEngine engine;
    QMetaObject::invokeMethod(&engine, "onLoaded", Q_ARG(QString, QString("/clips/a.mp4")),
                              Q_ARG(double, 10.0), Q_ARG(int, 640), Q_ARG(int, 480),
                              Q_ARG(bool, false));
    QCOMPARE(engine.duration(), 10.0);

    engine.seek(1.0);
    engine.seek(3.0);
    engine.seek(5.0);
    engine.seek(9.0);
    QCOMPARE(engine.position(), 0.0);  // placeholder decode fails gracefully
  }

  void listModels_stayInSyncWithLists() {
    TimelineEngine engine;
    auto* mediaModel = qobject_cast<QAbstractListModel*>(engine.mediaModel());
    auto* timelineModel = qobject_cast<QAbstractListModel*>(engine.timelineModel());
    auto* filterModel = qobject_cast<QSortFilterProxyModel*>(engine.mediaFilterModel());
    QVERIFY(mediaModel && timelineModel && filterModel);
    QCOMPARE(mediaModel->rowCount(), 0);
    QCOMPARE(timelineModel->rowCount(), 0);

    QMetaObject::invokeMethod(&engine, "onLoaded", Q_ARG(QString, QString("/clips/a.mp4")),
                              Q_ARG(double, 10.0), Q_ARG(int, 640), Q_ARG(int, 480),
                              Q_ARG(bool, false));
    QCOMPARE(mediaModel->rowCount(), 1);
    QCOMPARE(filterModel->rowCount(), 1);

    engine.appendClipToTimeline("/clips/a.mp4");
    engine.appendClipToTimeline("/clips/b.mp4");
    QCOMPARE(engine.timelineClips().size(), 2);
    QCOMPARE(timelineModel->rowCount(), 2);
    QCOMPARE(timelineModel->data(timelineModel->index(0, 0), Qt::UserRole + 1).toString(),
             QString("/clips/a.mp4"));

    filterModel->setProperty("filterText", "nomatch_xyz");
    QCOMPARE(filterModel->rowCount(), 0);
    filterModel->setProperty("filterText", "");
    QCOMPARE(filterModel->rowCount(), 1);

    engine.clearTimeline();
    QCOMPARE(timelineModel->rowCount(), 0);
  }

  void listModels_incrementalSync() {
    TimelineEngine engine;
    auto* timelineModel = qobject_cast<TimelineClipModel*>(engine.timelineModel());
    QVERIFY(timelineModel);
    QSignalSpy insertedSpy(timelineModel, &TimelineClipModel::rowsInserted);
    QSignalSpy removedSpy(timelineModel, &TimelineClipModel::rowsRemoved);
    QSignalSpy movedSpy(timelineModel, &TimelineClipModel::rowsMoved);
    QSignalSpy resetSpy(timelineModel, &TimelineClipModel::modelReset);
    QSignalSpy dataSpy(timelineModel, &TimelineClipModel::dataChanged);

    // Appends notify per-row inserts (no downstream V1 shifts).
    engine.appendClipToTimeline("/clips/a.mp4");
    engine.appendClipToTimeline("/clips/b.mp4");
    engine.appendClipToTimeline("/clips/c.mp4");
    QCOMPARE(timelineModel->rowCount(), 3);
    QCOMPARE(insertedSpy.count(), 3);
    QCOMPARE(resetSpy.count(), 0);

    // Mid-list V1 insert shifts downstream startTimes via
    // recomputeTimelineStarts, so the model resets (still correct, delegates
    // rebuild once) while content stays exact.
    engine.insertClipToTimeline("/clips/x.mp4", 1);
    QCOMPARE(timelineModel->rowCount(), 4);
    QCOMPARE(resetSpy.count(), 1);
    QCOMPARE(timelineModel->get(1).value("path").toString(), QString("/clips/x.mp4"));

    // V1 moves likewise re-lay downstream clips: ranged dataChanged over the
    // affected span [0..2] (delegates update in place, none recreated).
    movedSpy.clear();
    dataSpy.clear();
    resetSpy.clear();
    engine.moveTimelineClip(0, 2);
    QCOMPARE(movedSpy.count(), 0);
    QCOMPARE(resetSpy.count(), 0);
    QCOMPARE(dataSpy.count(), 1);
    QCOMPARE(dataSpy.last().at(0).toModelIndex().row(), 0);
    QCOMPARE(dataSpy.last().at(1).toModelIndex().row(), 2);
    QCOMPARE(timelineModel->get(0).value("path").toString(), QString("/clips/x.mp4"));
    QCOMPARE(timelineModel->get(2).value("path").toString(), QString("/clips/a.mp4"));

    // Mid-list V1 remove also re-lays downstream clips: reset, exact content.
    engine.removeTimelineClip(1);
    QCOMPARE(timelineModel->rowCount(), 3);
    QCOMPARE(timelineModel->get(0).value("path").toString(), QString("/clips/x.mp4"));
    QCOMPARE(timelineModel->get(1).value("path").toString(), QString("/clips/a.mp4"));

    // Same-size edit emits a ranged dataChanged, not a reset.
    dataSpy.clear();
    resetSpy.clear();
    engine.setClipTitle(0, "Hello");
    QCOMPARE(dataSpy.count(), 1);
    QCOMPARE(resetSpy.count(), 0);
    QCOMPARE(dataSpy.last().at(0).toModelIndex().row(), 0);
    QCOMPARE(dataSpy.last().at(1).toModelIndex().row(), 0);
    QCOMPARE(timelineModel->get(0).value("title").toString(), QString("Hello"));

    // Per-path arrival ticks notify only the matching rows.
    // (clips are now [x, a, c]).
    dataSpy.clear();
    timelineModel->bumpThumb("/clips/x.mp4");
    QCOMPARE(
        timelineModel->data(timelineModel->index(0, 0), TimelineClipModel::ThumbTickRole).toInt(),
        1);
    QCOMPARE(
        timelineModel->data(timelineModel->index(1, 0), TimelineClipModel::ThumbTickRole).toInt(),
        0);
    timelineModel->bumpWave("/clips/c.mp4");
    QCOMPARE(
        timelineModel->data(timelineModel->index(2, 0), TimelineClipModel::WaveTickRole).toInt(),
        1);
    QCOMPARE(
        timelineModel->data(timelineModel->index(0, 0), TimelineClipModel::WaveTickRole).toInt(),
        0);
    QVERIFY(dataSpy.count() >= 2);
  }

  static QVariantMap makeClip(const QString& path) {
    QVariantMap m;
    m.insert("path", path);
    m.insert("name", path);
    m.insert("duration", 5.0);
    m.insert("startTime", 0.0);
    m.insert("track", 0);
    return m;
  }

  void listModels_directSyncOps() {
    // Pure list ops (no startTime re-layout) exercise the incremental paths.
    TimelineClipModel model;
    QSignalSpy insertedSpy(&model, &TimelineClipModel::rowsInserted);
    QSignalSpy removedSpy(&model, &TimelineClipModel::rowsRemoved);
    QSignalSpy movedSpy(&model, &TimelineClipModel::rowsMoved);
    const QVariantMap a = makeClip("/a.mp4");
    const QVariantMap b = makeClip("/b.mp4");
    const QVariantMap c = makeClip("/c.mp4");
    const QVariantMap x = makeClip("/x.mp4");

    model.setItems({a, b, c});
    QCOMPARE(model.rowCount(), 3);

    // Mid-list insert notifies exactly one row.
    model.setItems({a, x, b, c});
    QCOMPARE(insertedSpy.count(), 1);
    QCOMPARE(insertedSpy.last().at(1).toInt(), 1);
    QCOMPARE(insertedSpy.last().at(2).toInt(), 1);
    QCOMPARE(model.get(1).value("path").toString(), QString("/x.mp4"));

    // Pure reorder notifies rowsMoved (dest follows the beginMoveRows
    // downward convention: final position + 1).
    model.setItems({x, b, a, c});
    QCOMPARE(movedSpy.count(), 1);
    QCOMPARE(movedSpy.last().at(1).toInt(), 0);
    QCOMPARE(movedSpy.last().at(4).toInt(), 3);
    QCOMPARE(model.get(0).value("path").toString(), QString("/x.mp4"));
    QCOMPARE(model.get(2).value("path").toString(), QString("/a.mp4"));

    // Backward reorder.
    model.setItems({a, x, b, c});
    QCOMPARE(movedSpy.count(), 2);
    QCOMPARE(movedSpy.last().at(1).toInt(), 2);
    QCOMPARE(movedSpy.last().at(4).toInt(), 0);

    // Single remove notifies exactly one row.
    model.setItems({a, b, c});
    QCOMPARE(removedSpy.count(), 1);
    QCOMPARE(removedSpy.last().at(1).toInt(), 1);
    QCOMPARE(model.rowCount(), 3);
  }

  void droppedSeek_releasesSeekingGate() {
    TimelineEngine engine;
    QMetaObject::invokeMethod(&engine, "onLoaded", Q_ARG(QString, QString("/clips/a.mp4")),
                              Q_ARG(double, 10.0), Q_ARG(int, 640), Q_ARG(int, 480),
                              Q_ARG(bool, false));
    engine.seek(5.0);  // arms the seeking gate; bogus path fails async
    // A preempted seek reports via onSeekDropped (no such slot before the
    // fix, so the invoke itself is the regression guard).
    QVERIFY(QMetaObject::invokeMethod(&engine, "onSeekDropped"));
    // Gate released: loadMedia opens directly instead of parking as pending.
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    engine.loadMedia("/path/does/not/exist.mp4");
    QVERIFY(failedSpy.wait(5000));
    QVERIFY(!failedSpy.isEmpty());
  }

  void seekPreemptedByAudioRestart_emitsSeekDropped() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clip = dir.filePath("long.mp4");
    QVERIFY(makeLongSeekClip(clip));

    DecoderThread decoder;
    decoder.start();
    QSignalSpy loadedSpy(&decoder, &DecoderThread::loaded);
    QSignalSpy droppedSpy(&decoder, &DecoderThread::seekDropped);
    decoder.requestOpen(clip);
    QVERIFY(loadedSpy.wait(10000));

    // Far seeks walk ~125 frames from the last keyframe; an audio restart
    // landing mid-walk must terminate the dropped seek with seekDropped
    // instead of silence (which stuck the engine's seeking gate).
    int attempts = 0;
    while (droppedSpy.isEmpty() && attempts < 60) {
      decoder.requestSeek(29.0);
      QThread::msleep(5);
      decoder.requestAudioRestart(0.0);
      QTest::qWait(100);
      ++attempts;
    }
    QVERIFY2(!droppedSpy.isEmpty(),
             "in-flight seek was never preempted in 60 attempts; widen the window");
  }

  void preloadRetarget_samePathNewPositionRewarms() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clip = dir.filePath("warm.mp4");
    QVERIFY(makeImportTestClip(clip));

    PreloadThread preloader;
    preloader.start();
    QSignalSpy preloadedSpy(&preloader, &PreloadThread::preloaded);
    preloader.requestPreload(clip, 0.1);
    QVERIFY(preloadedSpy.wait(10000));
    QCOMPARE(preloadedSpy.count(), 1);
    // Same path, new position (e.g. trim change): must re-warm instead of
    // promoting the stale decoder. Before the fix this was a no-op.
    preloader.requestPreload(clip, 0.9);
    QVERIFY(preloadedSpy.wait(10000));
    QCOMPARE(preloadedSpy.count(), 2);
  }

  void audioSeekPastTruncatedTail_reportsFailure() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString full = dir.filePath("full.mp4");
    QVERIFY(makeAudioVideoClip(full));
    const QString cut = dir.filePath("cut.mp4");
    QVERIFY(QFile::copy(full, cut));
    QFile truncated(cut);
    QVERIFY(truncated.open(QIODevice::ReadWrite));
    const qint64 keep = std::max<qint64>(32768, QFileInfo(full).size() / 3);
    QVERIFY(truncated.resize(keep));

    MediaDecoder dec;
    QVERIFY(dec.open(full));
    QVERIFY(dec.hasAudio());
    // Sane seek on intact media still succeeds.
    QVERIFY(dec.seekAudioTo(0.5));

    MediaDecoder broken;
    // faststart moov survives truncation: open + stream parse succeed...
    QVERIFY(broken.open(cut));
    QVERIFY(broken.hasAudio());
    // ...but catch-up toward the missing tail hits EOF. Claiming success
    // here used to desync A/V (position lied); now it must fail so the
    // caller falls back to silent video.
    QVERIFY(!broken.seekAudioTo(4.5));
  }

  void redoOfFailedImport_dropsGhostPlaceholder() {
    TimelineEngine engine;
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    engine.importMedia("/path/does/not/exist.mp4");
    QCOMPARE(engine.mediaList().size(), 1);  // placeholder, probe in flight
    // Undo/redo run synchronously on the GUI thread, so the snapshots are
    // deterministic regardless of the in-flight probe: undo stashes the
    // placeholder snapshot, redo restores it.
    engine.undo();
    QCOMPARE(engine.mediaList().size(), 0);
    engine.redo();
    QCOMPARE(engine.mediaList().size(), 1);  // placeholder restored...
    // ...and its re-probe resolves asynchronously. Before the fix, redo
    // blocked in a synchronous probe and left the 0s ghost entry behind;
    // now the failed re-probe drops it exactly like a fresh import.
    QTRY_VERIFY_WITH_TIMEOUT(engine.mediaList().isEmpty(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!failedSpy.isEmpty(), 5000);
  }

  void exportWarning_isAdvisoryNotFailure() {
    TimelineEngine engine;
    QSignalSpy warningSpy(&engine, &TimelineEngine::warning);
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    // Export warnings (e.g. skipped undecodable clips on an otherwise
    // successful export) must surface transiently, not as sticky errors.
    QVERIFY(QMetaObject::invokeMethod(&engine, "onExportWarning",
                                      Q_ARG(QString, QString("skipped 1 clip"))));
    QCOMPARE(warningSpy.count(), 1);
    QCOMPARE(failedSpy.count(), 0);
  }

  void decoderStopAndWait_joinsWorker() {
    DecoderThread decoder;
    decoder.start();
    decoder.requestOpen("/path/does/not/exist.mp4");
    decoder.stopAndWait();
    QVERIFY(decoder.isFinished());
    // Second call (as in member destruction after an explicit join) is safe.
    decoder.stopAndWait();
    QVERIFY(decoder.isFinished());
  }

  void engineTeardown_withInflightOpen_isClean() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clip = dir.filePath("teardown.mp4");
    QVERIFY(makeImportTestClip(clip));
    // Repeatedly destroy an engine with an open in flight: before the
    // teardown sequencing fix, the decoder could deref the preloader
    // mid-destruction (use-after-free, caught by ASan/TSan).
    for (int i = 0; i < 10; ++i) {
      TimelineEngine engine;
      engine.loadMedia(clip);
      // No waiting: destroy while RequestOpen (and the preloader
      // promotion) may still be in flight.
    }
    QVERIFY(true);
  }

  void loadProject_rejectsOversizeFile() {
    TimelineEngine engine;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString big = dir.filePath("big.reels.json");
    QFile f(big);
    QVERIFY(f.open(QIODevice::WriteOnly));
    // 33 MiB of junk: past the 32 MiB GUI-thread parse cap.
    QVERIFY(f.write(QByteArray(33LL * 1024 * 1024, 'x')) == 33LL * 1024 * 1024);
    f.close();
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    QVERIFY(!engine.loadProject(big));
    QVERIFY(!failedSpy.isEmpty());
    QVERIFY(engine.mediaList().isEmpty());
    QVERIFY(engine.timelineClips().isEmpty());
  }

  void loadProject_validatesMediaRows() {
    TimelineEngine engine;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    QJsonArray media;
    media.append(QJsonObject{{QStringLiteral("path"), QStringLiteral("/tmp/ok.mp4")},
                             {QStringLiteral("duration"), 5.0},
                             {QStringLiteral("name"), QStringLiteral("ok")}});
    // Object path: not a string, dropped.
    media.append(QJsonObject{{QStringLiteral("path"), QJsonObject{{QStringLiteral("nested"), 1}}},
                             {QStringLiteral("duration"), 1.0}});
    // String duration: kept as an unknown placeholder (re-probed async).
    media.append(QJsonObject{{QStringLiteral("path"), QStringLiteral("/tmp/str.mp4")},
                             {QStringLiteral("duration"), QStringLiteral("abc")}});
    // Negative duration: dropped.
    media.append(QJsonObject{{QStringLiteral("path"), QStringLiteral("/tmp/neg.mp4")},
                             {QStringLiteral("duration"), -3.0}});
    // Overlong path: dropped.
    media.append(QJsonObject{{QStringLiteral("path"), QString(2000, 'x')},
                             {QStringLiteral("duration"), 1.0}});
    // Overlong name: truncated to 256.
    media.append(QJsonObject{{QStringLiteral("path"), QStringLiteral("/tmp/long.mp4")},
                             {QStringLiteral("duration"), 2.0},
                             {QStringLiteral("name"), QString(5000, 'n')}});
    root[QStringLiteral("mediaList")] = media;
    QJsonArray clips;
    clips.append(QJsonObject{{QStringLiteral("path"), QStringLiteral("/tmp/ok.mp4")},
                             {QStringLiteral("duration"), 5.0}});
    // Non-object row: dropped.
    clips.append(QStringLiteral("just a string"));
    // Absurd durations: clamped finite, never Inf.
    clips.append(QJsonObject{{QStringLiteral("path"), QStringLiteral("/tmp/huge.mp4")},
                             {QStringLiteral("duration"), 1e18},
                             {QStringLiteral("sourceDuration"), 1e18}});
    root[QStringLiteral("timelineClips")] = clips;
    root[QStringLiteral("currentSource")] = QStringLiteral("");
    const QString path = dir.filePath("crafted.reels.json");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QVERIFY(f.write(QJsonDocument(root).toJson()) > 0);
    f.close();

    QVERIFY(engine.loadProject(path));
    // Synchronous validation results (async re-probe of the placeholder
    // lands later and only enriches/removes it).
    QCOMPARE(engine.mediaList().size(), 3);
    QCOMPARE(engine.mediaList().at(0).toMap().value("duration").toDouble(), 5.0);
    QCOMPARE(engine.mediaList().at(1).toMap().value("path").toString(), QString("/tmp/str.mp4"));
    QCOMPARE(engine.mediaList().at(1).toMap().value("duration").toDouble(), 0.0);
    QCOMPARE(engine.mediaList().at(2).toMap().value("name").toString().size(), 256);
    QCOMPARE(engine.timelineClips().size(), 2);
    const double hugeDur = engine.timelineClips().at(1).toMap().value("duration").toDouble();
    QVERIFY(std::isfinite(hugeDur));
    QVERIFY(hugeDur <= 86400.0);
    QVERIFY(hugeDur > 0.0);
  }

  void loadProject_rejectsForeignCurrentSource() {
    TimelineEngine engine;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("mediaList")] =
        QJsonArray{QJsonObject{{QStringLiteral("path"), QStringLiteral("/tmp/real.mp4")},
                               {QStringLiteral("duration"), 5.0}}};
    root[QStringLiteral("timelineClips")] = QJsonArray{};
    // Source outside the bin: must not trigger a decoder open.
    root[QStringLiteral("currentSource")] = QStringLiteral("/etc/passwd");
    const QString path = dir.filePath("foreign.reels.json");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QVERIFY(f.write(QJsonDocument(root).toJson()) > 0);
    f.close();

    QSignalSpy warningSpy(&engine, &TimelineEngine::warning);
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    QVERIFY(engine.loadProject(path));
    QVERIFY(engine.currentSource().isEmpty());
    QCOMPARE(warningSpy.count(), 1);
    QCOMPARE(failedSpy.count(), 0);
  }

  void newProject_clearsWaveThumbCaches() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString video = dir.filePath("cache.mp4");
    QVERIFY(makeImportTestClip(video));
    const QString av = dir.filePath("cache-av.mp4");
    QVERIFY(makeAudioVideoClip(av));

    TimelineEngine engine;
    engine.importMedia(video);
    engine.importMedia(av);
    engine.requestThumb(video);
    engine.requestWaveform(av);
    // Seed both caches, then switch projects: no stale artwork may survive.
    QTRY_VERIFY_WITH_TIMEOUT(engine.hasThumb(video), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(engine.hasWaveform(av), 10000);
    engine.newProject();
    QVERIFY(engine.mediaList().isEmpty());
    QVERIFY(!engine.hasThumb(video));
    QVERIFY(!engine.hasWaveform(av));
  }
};

QTEST_MAIN(TestTimelineEngine)
#include "TestTimelineEngine.moc"
