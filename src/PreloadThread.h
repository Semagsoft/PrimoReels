#pragma once

#include <QList>
#include <QMutex>
#include <QSet>
#include <QString>
#include <QThread>
#include <QVariant>
#include <QVariantList>
#include <QWaitCondition>
#include <memory>

#include "MediaDecoder.h"

// Warms standby decoders ahead of sequence advances so the playback thread
// can promote one instead of re-opening the file. Runs fully in parallel
// with playback; handoff is a mutex-guarded unique_ptr transfer.
class PreloadThread : public QThread {
  Q_OBJECT
 public:
  PreloadThread();
  ~PreloadThread() override;

  void requestPreload(const QString& path, double seconds);
  void requestWaveform(const QString& path, int buckets);
  // Lightweight metadata probe for bin imports: opens the file off-thread
  // and reports duration/dimensions/audio presence without touching the
  // program/source decoder threads. Results arrive via probeReady/Failed.
  void requestProbe(const QString& path);
  void requestDrop();

  // Takes the warmed decoder for path, or null when not (yet) available.
  // Thread-safe; transfers ownership to the caller.
  std::unique_ptr<MediaDecoder> takeReadyDecoder(const QString& path);

 signals:
  void preloaded(const QString& path);
  void waveformReady(const QString& path, const QVariantList& peaks);
  void probeReady(const QString& path, double duration, int width, int height, bool hasAudio);
  void probeFailed(const QString& path);

 protected:
  void run() override;

 private:
  struct WaveJob {
    QString path;
    int buckets = 0;
  };

  mutable QMutex m_mutex;
  QWaitCondition m_condition;
  bool m_abort = false;
  bool m_hasJob = false;
  QString m_jobPath;
  double m_jobSeconds = 0.0;
  QList<WaveJob> m_waveQueue;
  QList<QString> m_probeQueue;
  // In-flight waveform (dequeued, currently rendering) + queued set for
  // dedup. requestWaveform() is a no-op when the path is already queued
  // or rendering, so duplicate renders cannot pile up.
  QSet<QString> m_waveInflight;
  QString m_activeWavePath;
  std::unique_ptr<MediaDecoder> m_readyDecoder;
  QString m_readyPath;
  // Position the ready decoder was warmed at. requestPreload() for the
  // same path with a different position (e.g. trim change) must re-warm;
  // without this the promoted decoder would start from a stale position.
  double m_readySeconds = 0.0;
  bool m_hasReadySeconds = false;
};
