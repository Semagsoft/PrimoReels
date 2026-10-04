#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <limits>
#include "ClipUtils.h"
#include "FrameImageProvider.h"
#include "TimelineEngine.h"

namespace {

bool makeTestClip(const QString& path, const QString& color) {
  QProcess ffmpeg;
  ffmpeg.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i",
                          QString("color=c=%1:size=160x120:rate=10:duration=1").arg(color),
                          "-pix_fmt", "yuv420p", "-c:v", "mpeg4", path});
  return ffmpeg.waitForFinished(15000) && ffmpeg.exitCode() == 0 && QFileInfo::exists(path);
}

// Video + sine-tone clip (deterministic export-audio source).
bool makeAVClip(const QString& path, const QString& color, int frequency, double duration) {
  QProcess ffmpeg;
  ffmpeg.start(
      "ffmpeg",
      {"-y", "-v", "error", "-f", "lavfi", "-i",
       QString("color=c=%1:size=160x120:rate=10:duration=%2").arg(color).arg(duration), "-f",
       "lavfi", "-i", QString("sine=frequency=%1:duration=%2").arg(frequency).arg(duration),
       "-pix_fmt", "yuv420p", "-c:v", "mpeg4", "-c:a", "aac", "-shortest", path});
  return ffmpeg.waitForFinished(30000) && ffmpeg.exitCode() == 0 && QFileInfo::exists(path);
}

// Audio-only clip (no video stream).
bool makeAudioOnlyClip(const QString& path, int frequency, double duration) {
  QProcess ffmpeg;
  ffmpeg.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i",
                          QString("sine=frequency=%1:duration=%2").arg(frequency).arg(duration),
                          "-c:a", "aac", path});
  return ffmpeg.waitForFinished(30000) && ffmpeg.exitCode() == 0 && QFileInfo::exists(path);
}

// Mean/max volume in dB via volumedetect (n/a, e.g. digital silence, -> NaN).
// When ss/t are >= 0, measures that window instead of the whole file.
bool audioLevels(const QString& path,
                 double* meanDb,
                 double* maxDb,
                 double ss = -1.0,
                 double t = -1.0) {
  // Note: volumedetect stats print at info level; -v error would hide them.
  QStringList args{"-v", "info", "-i", path};
  if (ss >= 0.0) {
    args << "-ss" << QString::number(ss);
  }
  if (t >= 0.0) {
    args << "-t" << QString::number(t);
  }
  args << "-af" << "volumedetect" << "-f" << "null" << "-";
  QProcess ffmpeg;
  ffmpeg.start("ffmpeg", args);
  if (!ffmpeg.waitForFinished(30000) || ffmpeg.exitCode() != 0) {
    return false;
  }
  const QString err = QString::fromUtf8(ffmpeg.readAllStandardError());
  auto parse = [&](const QString& key, double* out) {
    const int at = err.indexOf(key);
    if (at < 0) {
      return false;
    }
    const QString tail = err.mid(at + key.size()).trimmed();
    if (tail.startsWith(QStringLiteral("n/a"))) {
      *out = std::numeric_limits<double>::quiet_NaN();
      return true;
    }
    // "  -8.3 dB" -> -8.3
    *out = tail.split(' ').first().toDouble();
    return true;
  };
  return parse(QStringLiteral("mean_volume:"), meanDb) &&
         parse(QStringLiteral("max_volume:"), maxDb);
}

bool hasStreamOfType(const QString& path, const QString& codecType) {
  QProcess probe;
  probe.start("ffprobe", {"-v", "error", "-show_entries", "stream=codec_type", "-of",
                          "default=noprint_wrappers=1:nokey=1", path});
  if (!probe.waitForFinished(15000) || probe.exitCode() != 0) {
    return false;
  }
  const QStringList types = QString::fromUtf8(probe.readAllStandardOutput()).trimmed().split('\n');
  return types.contains(codecType);
}

double probeDuration(const QString& path, bool* ok = nullptr) {
  QProcess probe;
  probe.start("ffprobe", {"-v", "error", "-show_entries", "format=duration", "-of",
                          "default=noprint_wrappers=1:nokey=1", path});
  const bool done = probe.waitForFinished(15000) && probe.exitCode() == 0;
  if (ok) {
    *ok = done;
  }
  return done ? QString::fromUtf8(probe.readAllStandardOutput()).trimmed().toDouble() : 0.0;
}

}  // namespace

class TestExport : public QObject {
  Q_OBJECT
 private slots:
  void exportEmptyTimeline_reportsFailure() {
    TimelineEngine engine;
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    engine.exportSequence(dir.filePath("should-not-exist.mp4"));
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY(!engine.isExporting());
  }

  void exportSequence_rendersMp4() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clipA = dir.filePath("a.mp4");
    const QString clipB = dir.filePath("b.mp4");
    QVERIFY(makeTestClip(clipA, "red"));
    QVERIFY(makeTestClip(clipB, "blue"));

    TimelineEngine engine;
    QSignalSpy progressSpy(&engine, &TimelineEngine::exportProgressChanged);
    QSignalSpy doneSpy(&engine, &TimelineEngine::exportSucceeded);
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);

    engine.appendClipToTimeline(clipA);
    engine.appendClipToTimeline(clipB);
    engine.setClipEffect(0, "Vignette");
    const QString out = dir.filePath("sequence.mp4");
    engine.exportSequence(out);
    QVERIFY(engine.isExporting());
    QVERIFY(doneSpy.wait(60000));
    QVERIFY(!engine.isExporting());
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(!progressSpy.isEmpty());
    QVERIFY(QFileInfo::exists(out));
    QVERIFY(QFileInfo(out).size() > 1000);
    QCOMPARE(engine.exportProgress(), 1.0);
  }

  void exportSequence_honorsTrim() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    if (QStandardPaths::findExecutable("ffprobe").isEmpty()) {
      QSKIP("ffprobe CLI not available for duration check");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 2s clip; trim to middle 1s.
    const QString clip = dir.filePath("long.mp4");
    QProcess gen;
    gen.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i",
                         "color=c=yellow:size=160x120:rate=10:duration=2", "-pix_fmt", "yuv420p",
                         "-c:v", "mpeg4", clip});
    QVERIFY(gen.waitForFinished(15000) && gen.exitCode() == 0);

    TimelineEngine engine;
    QSignalSpy doneSpy(&engine, &TimelineEngine::exportSucceeded);
    // Decode once so the bin carries the true 2s duration (not the placeholder).
    QSignalSpy durSpy(&engine, &TimelineEngine::durationChanged);
    engine.loadMedia(clip);
    QVERIFY(durSpy.wait(5000));
    engine.appendClipToTimeline(clip);
    engine.setClipTrimStart(0, 0.5);
    engine.setClipTrimEnd(0, 0.5);
    QCOMPARE(engine.timelineClips().first().toMap().value("duration").toDouble(), 1.0);
    const QString out = dir.filePath("trimmed.mp4");
    engine.exportSequence(out);
    QVERIFY(doneSpy.wait(60000));
    QProcess probe;
    probe.start("ffprobe", {"-v", "error", "-show_entries", "format=duration", "-of",
                            "default=noprint_wrappers=1:nokey=1", out});
    QVERIFY(probe.waitForFinished(15000) && probe.exitCode() == 0);
    const double outDur = QString::fromUtf8(probe.readAllStandardOutput()).trimmed().toDouble();
    QVERIFY2(outDur > 0.5 && outDur < 1.5, qPrintable(QString("duration=%1").arg(outDur)));
  }

  void exportSequence_rendersDissolve() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty() ||
        QStandardPaths::findExecutable("ffprobe").isEmpty()) {
      QSKIP("ffmpeg/ffprobe CLIs not available");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clipA = dir.filePath("a.mp4");
    const QString clipB = dir.filePath("b.mp4");
    QVERIFY(makeTestClip(clipA, "red"));
    QVERIFY(makeTestClip(clipB, "blue"));

    TimelineEngine engine;
    QSignalSpy doneSpy(&engine, &TimelineEngine::exportSucceeded);
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    // Decode for real durations (1s each), then dissolve 0.5s across the cut.
    QSignalSpy durSpy(&engine, &TimelineEngine::durationChanged);
    engine.loadMedia(clipA);
    QVERIFY(durSpy.wait(5000));
    engine.loadMedia(clipB);
    QVERIFY(durSpy.wait(5000));
    engine.appendClipToTimeline(clipA);
    engine.appendClipToTimeline(clipB);
    engine.setClipTransition(0, "Cross Dissolve", 0.5);
    const QString out = dir.filePath("dissolve.mp4");
    engine.exportSequence(out);
    QVERIFY(doneSpy.wait(60000));
    QCOMPARE(failedSpy.count(), 0);
    QProcess probe;
    probe.start("ffprobe", {"-v", "error", "-show_entries", "format=duration", "-of",
                            "default=noprint_wrappers=1:nokey=1", out});
    QVERIFY(probe.waitForFinished(15000) && probe.exitCode() == 0);
    const double outDur = QString::fromUtf8(probe.readAllStandardOutput()).trimmed().toDouble();
    // Overlap replaces time: 1 + 1 - 0.5 = 1.5s.
    QVERIFY2(outDur > 1.0 && outDur < 2.0, qPrintable(QString("duration=%1").arg(outDur)));
  }

  void sequencePlayback_advancesThroughRealClips() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clipA = dir.filePath("a.mp4");
    const QString clipB = dir.filePath("b.mp4");
    QVERIFY(makeTestClip(clipA, "red"));
    QVERIFY(makeTestClip(clipB, "blue"));

    TimelineEngine engine;
    engine.appendClipToTimeline(clipA);
    engine.appendClipToTimeline(clipB);
    engine.playSequenceFrom(0);
    QVERIFY(engine.sequencePlaying());
    // Each clip is ~1s; the whole sequence must finish on its own.
    const QDeadlineTimer deadline(20000);
    while ((engine.isPlaying() || engine.sequencePlaying()) && !deadline.hasExpired()) {
      QTest::qWait(100);
    }
    QVERIFY(!engine.isPlaying());
    QVERIFY(!engine.sequencePlaying());
    QCOMPARE(engine.currentSource(), clipB);
  }

  void waveforms_audioClipCachesPeaks() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clip = dir.filePath("av.mp4");
    QProcess gen;
    gen.start("ffmpeg",
              {"-y", "-v", "error", "-f", "lavfi", "-i", "testsrc=size=160x120:rate=10:duration=1",
               "-f", "lavfi", "-i", "sine=frequency=440:duration=1", "-pix_fmt", "yuv420p", "-c:v",
               "mpeg4", "-c:a", "aac", "-shortest", clip});
    QVERIFY(gen.waitForFinished(15000) && gen.exitCode() == 0);

    TimelineEngine engine;
    QSignalSpy spy(&engine, &TimelineEngine::waveformsChanged);
    engine.requestWaveform(clip);
    QVERIFY(spy.wait(10000));
    QVERIFY(engine.hasWaveform(clip));
    const QVariantList peaks = engine.waveform(clip);
    QCOMPARE(peaks.size(), 120);
    double maxPeak = 0.0;
    for (const QVariant& p : peaks) {
      const double v = p.toDouble();
      QVERIFY(v >= 0.0 && v <= 1.0);
      maxPeak = std::max(maxPeak, v);
    }
    QVERIFY(maxPeak > 0.05);  // default sine level peaks near 0.09
  }

  void exportSequence_compositesOverlay() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty() ||
        QStandardPaths::findExecutable("ffprobe").isEmpty()) {
      QSKIP("ffmpeg/ffprobe CLIs not available");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clipA = dir.filePath("a.mp4");
    const QString clipB = dir.filePath("b.mp4");
    QVERIFY(makeTestClip(clipA, "red"));
    QVERIFY(makeTestClip(clipB, "blue"));

    TimelineEngine engine;
    QSignalSpy doneSpy(&engine, &TimelineEngine::exportSucceeded);
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    QSignalSpy durSpy(&engine, &TimelineEngine::durationChanged);
    engine.loadMedia(clipA);
    QVERIFY(durSpy.wait(5000));
    engine.appendClipToTimeline(clipA);
    engine.appendOverlayClip(clipB, 0.25);
    const QString out = dir.filePath("pip.mp4");
    engine.exportSequence(out);
    QVERIFY(doneSpy.wait(60000));
    QCOMPARE(failedSpy.count(), 0);
    QProcess probe;
    probe.start("ffprobe", {"-v", "error", "-show_entries", "format=duration", "-of",
                            "default=noprint_wrappers=1:nokey=1", out});
    QVERIFY(probe.waitForFinished(15000) && probe.exitCode() == 0);
    const double outDur = QString::fromUtf8(probe.readAllStandardOutput()).trimmed().toDouble();
    // Overlay adds no time: output matches the single V1 clip (~1s).
    QVERIFY2(outDur > 0.5 && outDur < 1.5, qPrintable(QString("duration=%1").arg(outDur)));
  }

  void exportSequence_mixesA1Bed() {
#ifdef PRIMOREELS_TSAN
    QSKIP("skipped under TSan: recreating engines in one process trips the TSan thread-registry CHECK on this platform");
#endif
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty() ||
        QStandardPaths::findExecutable("ffprobe").isEmpty()) {
      QSKIP("ffmpeg/ffprobe CLIs not available");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // Different tone per lane (440 vs 880 Hz) so mixed energy strictly adds.
    const QString v1 = dir.filePath("v1.mp4");
    const QString bed = dir.filePath("bed.m4a");
    QVERIFY(makeAVClip(v1, "red", 440, 2.0));
    QVERIFY(makeAudioOnlyClip(bed, 880, 2.0));

    double soloMean = 0.0, soloMax = 0.0;
    {
      TimelineEngine engine;
      QSignalSpy doneSpy(&engine, &TimelineEngine::exportSucceeded);
      QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
      QSignalSpy durSpy(&engine, &TimelineEngine::durationChanged);
      engine.loadMedia(v1);
      QVERIFY(durSpy.wait(5000));
      engine.appendClipToTimeline(v1);
      const QString out = dir.filePath("solo.mp4");
      engine.exportSequence(out);
      QVERIFY(doneSpy.wait(60000));
      QCOMPARE(failedSpy.count(), 0);
      QVERIFY(hasStreamOfType(out, QStringLiteral("audio")));
      QVERIFY(audioLevels(out, &soloMean, &soloMax));
    }

    TimelineEngine engine;
    QSignalSpy doneSpy(&engine, &TimelineEngine::exportSucceeded);
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    QSignalSpy durSpy(&engine, &TimelineEngine::durationChanged);
    engine.loadMedia(v1);
    QVERIFY(durSpy.wait(5000));
    engine.appendClipToTimeline(v1);
    engine.appendAudioClip(bed, 0.0);
    const QString out = dir.filePath("mixed.mp4");
    engine.exportSequence(out);
    QVERIFY(doneSpy.wait(60000));
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(hasStreamOfType(out, QStringLiteral("audio")));
    double mixMean = 0.0, mixMax = 0.0;
    QVERIFY(audioLevels(out, &mixMean, &mixMax));
    // Two equal-level sines at different frequencies: +3 dB expected.
    QVERIFY2(mixMean > soloMean + 1.0,
             qPrintable(QString("solo=%1 mixed=%2").arg(soloMean).arg(mixMean)));
    // Bed adds no time: output still matches the V1 lane (~2s).
    bool ok = false;
    const double outDur = probeDuration(out, &ok);
    QVERIFY(ok);
    QVERIFY2(outDur > 1.5 && outDur < 2.5, qPrintable(QString("duration=%1").arg(outDur)));
  }

  void exportSequence_appliesGainMuteAndVolume() {
#ifdef PRIMOREELS_TSAN
    QSKIP("skipped under TSan: recreating engines in one process trips the TSan thread-registry CHECK on this platform");
#endif
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty() ||
        QStandardPaths::findExecutable("ffprobe").isEmpty()) {
      QSKIP("ffmpeg/ffprobe CLIs not available");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clip = dir.filePath("av.mp4");
    QVERIFY(makeAVClip(clip, "green", 440, 2.0));

    auto exportWith = [&](double gain, double volume, bool muted, const QString& name,
                          double* meanDb, double* maxDb) {
      TimelineEngine engine;
      QSignalSpy doneSpy(&engine, &TimelineEngine::exportSucceeded);
      QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
      QSignalSpy durSpy(&engine, &TimelineEngine::durationChanged);
      engine.loadMedia(clip);
      if (!durSpy.wait(5000)) {
        return false;
      }
      engine.appendClipToTimeline(clip);
      engine.setClipGain(0, gain);
      engine.setVolume(volume);
      engine.setClipMuted(0, muted);
      const QString out = dir.filePath(name);
      engine.exportSequence(out);
      if (!doneSpy.wait(60000) || failedSpy.count() != 0) {
        return false;
      }
      return audioLevels(out, meanDb, maxDb);
    };

    double baseMean = 0.0, baseMax = 0.0;
    QVERIFY(exportWith(1.0, 1.0, false, "base.mp4", &baseMean, &baseMax));
    double gainMean = 0.0, gainMax = 0.0;
    QVERIFY(exportWith(0.5, 1.0, false, "gain.mp4", &gainMean, &gainMax));
    // 0.5x amplitude is ~-6 dB (AAC tolerance +/-2 dB).
    QVERIFY2(gainMean < baseMean - 4.0 && gainMean > baseMean - 8.0,
             qPrintable(QString("base=%1 gain=%2").arg(baseMean).arg(gainMean)));
    // Global volume burns in identically to per-clip gain.
    double volMean = 0.0, volMax = 0.0;
    QVERIFY(exportWith(1.0, 0.5, false, "vol.mp4", &volMean, &volMax));
    QVERIFY2(volMean < baseMean - 4.0 && volMean > baseMean - 8.0,
             qPrintable(QString("base=%1 vol=%2").arg(baseMean).arg(volMean)));
    Q_UNUSED(gainMax);
    Q_UNUSED(volMax);
    // Muted: digital silence (volumedetect reports n/a).
    double muteMean = 0.0, muteMax = 0.0;
    QVERIFY(exportWith(1.0, 1.0, true, "muted.mp4", &muteMean, &muteMax));
    QVERIFY2(std::isnan(muteMax) || muteMax < -60.0,
             qPrintable(QString("muted max=%1").arg(muteMax)));
  }

  void exportSequence_appliesFades() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty() ||
        QStandardPaths::findExecutable("ffprobe").isEmpty()) {
      QSKIP("ffmpeg/ffprobe CLIs not available");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clip = dir.filePath("av.mp4");
    QVERIFY(makeAVClip(clip, "blue", 440, 2.0));

    TimelineEngine engine;
    QSignalSpy doneSpy(&engine, &TimelineEngine::exportSucceeded);
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    QSignalSpy durSpy(&engine, &TimelineEngine::durationChanged);
    engine.loadMedia(clip);
    QVERIFY(durSpy.wait(5000));
    engine.appendClipToTimeline(clip);
    engine.setClipFadeIn(0, 1.0);  // 0 -> full over the first second
    const QString out = dir.filePath("faded.mp4");
    engine.exportSequence(out);
    QVERIFY(doneSpy.wait(60000));
    QCOMPARE(failedSpy.count(), 0);
    double earlyMean = 0.0, earlyMax = 0.0, lateMean = 0.0, lateMax = 0.0;
    QVERIFY(audioLevels(out, &earlyMean, &earlyMax, 0.0, 0.5));
    QVERIFY(audioLevels(out, &lateMean, &lateMax, 1.5, 0.5));
    // Linear 0->1 ramp over 1s: first half-second averages well below full.
    QVERIFY2(lateMean > earlyMean + 3.0,
             qPrintable(QString("early=%1 late=%2").arg(earlyMean).arg(lateMean)));
  }

  void exportSequence_audioOnlyRendersPlayableVideo() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty() ||
        QStandardPaths::findExecutable("ffprobe").isEmpty()) {
      QSKIP("ffmpeg/ffprobe CLIs not available");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bed = dir.filePath("bed.m4a");
    QVERIFY(makeAudioOnlyClip(bed, 440, 2.0));

    TimelineEngine engine;
    QSignalSpy doneSpy(&engine, &TimelineEngine::exportSucceeded);
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    QSignalSpy durSpy(&engine, &TimelineEngine::durationChanged);
    engine.loadMedia(bed);
    QVERIFY(durSpy.wait(5000));
    engine.appendClipToTimeline(bed);
    const QString out = dir.filePath("audioonly.mp4");
    engine.exportSequence(out);
    QVERIFY(doneSpy.wait(60000));
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(hasStreamOfType(out, QStringLiteral("video")));
    QVERIFY(hasStreamOfType(out, QStringLiteral("audio")));
    bool ok = false;
    const double outDur = probeDuration(out, &ok);
    QVERIFY(ok);
    QVERIFY2(outDur > 1.5 && outDur < 2.5, qPrintable(QString("duration=%1").arg(outDur)));
  }

  void thumbnails_realClipCachesImage() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clip = dir.filePath("thumb.mp4");
    QVERIFY(makeTestClip(clip, "green"));

    TimelineEngine engine;
    QSignalSpy spy(&engine, &TimelineEngine::thumbnailsChanged);
    engine.requestThumb(clip);
    QVERIFY(spy.wait(5000));
    QVERIFY(engine.hasThumb(clip));
    const QImage thumb = engine.thumbImage(clip);
    QVERIFY(!thumb.isNull());
    QCOMPARE(thumb.width(), 96);
  }

  void playbackWithRealMedia_playPauseSeek() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clip = dir.filePath("play.mp4");
    QVERIFY(makeTestClip(clip, "red"));

    TimelineEngine engine;
    QSignalSpy durSpy(&engine, &TimelineEngine::durationChanged);
    engine.loadMedia(clip);
    QVERIFY(durSpy.wait(5000));
    QVERIFY(engine.duration() > 0.5);
    QSignalSpy frameSpy(&engine, &TimelineEngine::currentFrameChanged);
    engine.play();
    QVERIFY(engine.isPlaying());
    QVERIFY(frameSpy.wait(5000));
    QVERIFY(engine.frameAvailable());
    engine.pause();
    QVERIFY(!engine.isPlaying());
    const double half = engine.duration() / 2.0;
    engine.seek(half);
    QTRY_VERIFY_WITH_TIMEOUT(engine.frameAvailable(), 5000);
    QVERIFY(std::abs(engine.position() - half) < 0.6);
  }

  void sourceTransport_playPauseSeek() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clip = dir.filePath("src.mp4");
    QVERIFY(makeTestClip(clip, "blue"));

    TimelineEngine engine;
    QSignalSpy durSpy(&engine, &TimelineEngine::sourceDurationChanged);
    engine.loadSource(clip);
    QVERIFY(durSpy.wait(5000));
    QVERIFY(engine.sourceDuration() > 0.5);
    engine.playSource();
    // Autoplay-on-load may already be playing; either state is fine.
    QVERIFY(engine.sourcePlaying() || !engine.sourcePlaying());
    engine.pauseSource();
    QVERIFY(!engine.sourcePlaying());
    engine.seekSource(engine.sourceDuration() / 2.0);
    QTRY_VERIFY_WITH_TIMEOUT(engine.sourceFrameAvailable(), 5000);
  }

  void previewTimelineClip_setsEffectWindow() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clip = dir.filePath("prev.mp4");
    QVERIFY(makeTestClip(clip, "green"));

    TimelineEngine engine;
    QSignalSpy durSpy(&engine, &TimelineEngine::durationChanged);
    engine.loadMedia(clip);
    QVERIFY(durSpy.wait(5000));
    engine.appendClipToTimeline(clip);
    engine.setClipEffect(0, "Grayscale");
    engine.previewTimelineClip(0);
    QCOMPARE(engine.activeEffect(), QString("Grayscale"));
    QTRY_VERIFY_WITH_TIMEOUT(engine.frameAvailable(), 5000);
  }

  void overlayStartTime_movesFreely() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clip = dir.filePath("ov.mp4");
    QVERIFY(makeTestClip(clip, "red"));
    TimelineEngine engine;
    QSignalSpy durSpy(&engine, &TimelineEngine::durationChanged);
    engine.loadMedia(clip);
    QVERIFY(durSpy.wait(5000));
    engine.appendClipToTimeline(clip);
    engine.appendOverlayClip(clip, 0.25);
    QCOMPARE(engine.timelineClips().size(), 2);
    engine.setClipStartTime(1, 0.5);
    QCOMPARE(engine.timelineClips().at(1).toMap().value("startTime").toDouble(), 0.5);
    // V1 clips ignore absolute moves.
    engine.setClipStartTime(0, 5.0);
    QCOMPARE(engine.timelineClips().at(0).toMap().value("startTime").toDouble(), 0.0);
  }

  void removeMedia_cascadesToTimeline() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clip = dir.filePath("casc.mp4");
    QVERIFY(makeTestClip(clip, "red"));
    TimelineEngine engine;
    QSignalSpy durSpy(&engine, &TimelineEngine::durationChanged);
    engine.loadMedia(clip);
    QVERIFY(durSpy.wait(5000));
    engine.appendClipToTimeline(clip);
    QCOMPARE(engine.timelineClips().size(), 1);
    engine.removeMedia(clip);
    QCOMPARE(engine.timelineClips().size(), 0);
    QVERIFY(engine.mediaList().isEmpty());
  }

  void seekNonFinite_isIgnored() {
    TimelineEngine engine;
    engine.seek(std::numeric_limits<double>::quiet_NaN());
    engine.seek(std::numeric_limits<double>::infinity());
    QCOMPARE(engine.position(), 0.0);
    engine.seekSource(std::numeric_limits<double>::quiet_NaN());
    QCOMPARE(engine.sourcePosition(), 0.0);
  }

  void newProject_clearsPendingLoad() {
    TimelineEngine engine;
    // Queue a failing load, then reset before it resolves.
    engine.loadMedia("/path/does/not/exist-pending.mp4");
    engine.newProject();
    QTest::qWait(500);
    QVERIFY(engine.currentSource().isEmpty());
    QCOMPARE(engine.duration(), 0.0);
    QVERIFY(!engine.isPlaying());
    QVERIFY(engine.mediaList().isEmpty());
    QVERIFY(engine.timelineClips().isEmpty());
  }

  void cancelExport_duringActiveExport() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // Longer clip so cancel lands mid-encode, not after finish.
    const QString clip = dir.filePath("longexp.mp4");
    QProcess gen;
    gen.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i",
                         "color=c=red:size=320x240:rate=30:duration=4", "-pix_fmt", "yuv420p",
                         "-c:v", "mpeg4", clip});
    QVERIFY(gen.waitForFinished(30000) && gen.exitCode() == 0);
    TimelineEngine engine;
    QSignalSpy durSpy(&engine, &TimelineEngine::durationChanged);
    engine.loadMedia(clip);
    QVERIFY(durSpy.wait(5000));
    engine.appendClipToTimeline(clip);
    engine.appendClipToTimeline(clip);
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    const QString out = dir.filePath("cancelled.mp4");
    engine.exportSequence(out);
    QVERIFY(engine.isExporting());
    engine.cancelExport();
    QVERIFY(failedSpy.wait(30000));
    QVERIFY(!engine.isExporting());
    QVERIFY(!QFileInfo::exists(out) || QFileInfo(out).size() == 0);
  }

  void exportDestination_data() {
    QTest::addColumn<bool>("existing");
    QTest::addColumn<bool>("cancel");
    QTest::newRow("cancel-existing") << true << true;
    QTest::newRow("cancel-new") << false << true;
    QTest::newRow("overwrite-existing") << true << false;
  }

  void exportDestination() {
    QFETCH(bool, existing);
    QFETCH(bool, cancel);
#ifdef PRIMOREELS_TSAN
    QSKIP("skipped under TSan: recreating engines in one process trips the TSan thread-registry CHECK on this platform");
#endif
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clip = dir.filePath("source.mp4");
    const QString out = dir.filePath("destination.mp4");
    QVERIFY(makeTestClip(clip, "red"));
    const QByteArray original("previous export must survive");
    if (existing) {
      QFile file(out);
      QVERIFY(file.open(QIODevice::WriteOnly));
      QCOMPARE(file.write(original), original.size());
    }
    const QStringList entries = QDir(dir.path()).entryList(QDir::Files | QDir::Hidden);
    bool sawProgress = false;
    bool destinationIntact = false;
    ExportThread exporter;
    connect(
        &exporter, &ExportThread::progressChanged, &exporter,
        [&](double done, double total) {
          if (sawProgress || done <= 0.0 || done >= total) {
            return;
          }
          sawProgress = true;
          QFile file(out);
          destinationIntact = existing
                                  ? file.open(QIODevice::ReadOnly) && file.readAll() == original
                                  : !file.exists();
          if (cancel) {
            exporter.requestCancel();
          }
        },
        Qt::DirectConnection);
    QSignalSpy failedSpy(&exporter, &ExportThread::exportFailed);
    QSignalSpy doneSpy(&exporter, &ExportThread::exportFinished);
    QVariantMap entry;
    entry.insert("path", clip);
    entry.insert("duration", 1.0);
    exporter.requestExport(QVariantList{entry}, out);
    exporter.start();
    if (cancel) {
      QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 30000);
      QCOMPARE(doneSpy.count(), 0);
    } else {
      QTRY_COMPARE_WITH_TIMEOUT(doneSpy.count(), 1, 30000);
      QCOMPARE(failedSpy.count(), 0);
    }
    QVERIFY(sawProgress);
    QVERIFY(destinationIntact);
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden), entries);
    if (cancel && !existing) {
      QVERIFY(!QFileInfo::exists(out));
    } else {
      QFile file(out);
      QVERIFY(file.open(QIODevice::ReadOnly));
      const QByteArray bytes = file.readAll();
      if (cancel) {
        QCOMPARE(bytes, original);
      } else {
        QVERIFY(bytes != original);
        MediaDecoder decoder;
        QVERIFY(decoder.open(out));
        QVERIFY(!decoder.getFrameAt(0.0).isNull());
      }
    }
  }

  void failedExport_preservesExistingDestination() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString out = dir.filePath("destination.mp4");
    const QByteArray original("previous export must survive");
    QFile file(out);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(original), original.size());
    file.close();
    const QStringList entries = QDir(dir.path()).entryList(QDir::Files | QDir::Hidden);
    ExportThread exporter;
    QSignalSpy failedSpy(&exporter, &ExportThread::exportFailed);
    QSignalSpy doneSpy(&exporter, &ExportThread::exportFinished);
    QVariantMap entry;
    entry.insert("path", dir.filePath("missing.mp4"));
    entry.insert("duration", 1.0);
    exporter.requestExport(QVariantList{entry}, out);
    exporter.start();
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 30000);
    QCOMPARE(doneSpy.count(), 0);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), original);
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden), entries);
  }

  void imageProviders_renderWithoutCrash() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clip = dir.filePath("prov.mp4");
    QVERIFY(makeTestClip(clip, "green"));
    TimelineEngine engine;
    QSignalSpy thumbSpy(&engine, &TimelineEngine::thumbnailsChanged);
    engine.requestThumb(clip);
    QVERIFY(thumbSpy.wait(5000));
    QSize size;
    ThumbImageProvider thumbs(&engine);
    const QImage thumb =
        thumbs.requestImage(QString::fromUtf8(QUrl::toPercentEncoding(clip)) + "/0", &size, {});
    QVERIFY(!thumb.isNull());
    FrameImageProvider preview(&engine);
    // No frame yet: provider must return the 1x1 fallback, not crash.
    const QImage empty = preview.requestImage("frame/0", &size, {});
    QVERIFY(!empty.isNull());
    SourceImageProvider source(&engine);
    const QImage srcEmpty = source.requestImage("frame/0", &size, {});
    QVERIFY(!srcEmpty.isNull());
  }

  void clipUtils_normalizesFileUrls() {
    QCOMPARE(ClipUtils::normalizedMediaPath("file:///tmp/a%20b.mp4"), QString("/tmp/a b.mp4"));
    QCOMPARE(ClipUtils::normalizedMediaPath("/tmp/plain.mp4"), QString("/tmp/plain.mp4"));
    QVERIFY(ClipUtils::normalizedMediaPath("").isEmpty());
    // Non-file schemes pass through for a clear open error downstream.
    QCOMPARE(ClipUtils::normalizedMediaPath("qrc:/qml/Main.qml"), QString("qrc:/qml/Main.qml"));
    // Embedded NUL would truncate at the native boundary (as in JSON
    // \u0000 surviving into QString): rejected.
    QVERIFY(ClipUtils::normalizedMediaPath(QString("/tmp/a") + QChar(0) + "b").isEmpty());
    // Overlong paths would pollute caches and messages: rejected.
    QVERIFY(ClipUtils::normalizedMediaPath(QString(2000, 'x')).isEmpty());
  }

  void exportRejectsOverlongTimeline() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clip = dir.filePath("short.mp4");
    QVERIFY(makeTestClip(clip, "red"));
    const QString out = dir.filePath("huge.mp4");
    // Real decodable path but a forged 50000s duration: must fail fast on
    // the duration cap instead of overflowing 32-bit frame/sample counters
    // (or encoding for hours).
    ExportThread exporter;
    QSignalSpy failedSpy(&exporter, &ExportThread::exportFailed);
    QSignalSpy doneSpy(&exporter, &ExportThread::exportFinished);
    QVariantMap entry;
    entry.insert("path", clip);
    entry.insert("duration", 50000.0);
    exporter.requestExport(QVariantList{entry}, out);
    exporter.start();
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 30000);
    QCOMPARE(doneSpy.count(), 0);
    QVERIFY(failedSpy.at(0).at(0).toString().contains(QStringLiteral("too long")));
  }

  void projectVersionMismatch_isRejected() {
    TimelineEngine engine;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString proj = dir.filePath("v.reels.json");
    QVERIFY(engine.saveProject(proj));
    QFile f(proj);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    QJsonObject root = doc.object();
    root["version"] = 999;
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(QJsonDocument(root).toJson());
    f.close();
    QSignalSpy failedSpy(&engine, &TimelineEngine::failed);
    QVERIFY(!engine.loadProject(proj));
    QVERIFY(!failedSpy.isEmpty());
  }

  void independentSourcePlayback_doesNotDisturbProgram() {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) {
      QSKIP("ffmpeg CLI not available for fixture generation");
    }
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString clipA = dir.filePath("a.mp4");
    const QString clipB = dir.filePath("b.mp4");
    QVERIFY(makeTestClip(clipA, "red"));
    QVERIFY(makeTestClip(clipB, "blue"));

    TimelineEngine engine;
    // Load a into program monitor
    QSignalSpy loadedSpy(&engine, &TimelineEngine::currentSourceChanged);
    engine.loadMedia(clipA);
    QVERIFY(loadedSpy.wait(5000));
    QCOMPARE(engine.currentSource(), clipA);

    // Load b into source monitor (independent decoder thread)
    QSignalSpy sourceDurSpy(&engine, &TimelineEngine::sourceDurationChanged);
    engine.loadSource(clipB);
    QVERIFY(sourceDurSpy.wait(5000));
    QCOMPARE(engine.sourcePath(), clipB);
    QVERIFY(engine.sourceDuration() > 0.0);

    // Program monitor source must remain clipA
    QCOMPARE(engine.currentSource(), clipA);
  }
};

QTEST_MAIN(TestExport)
#include "TestExport.moc"
