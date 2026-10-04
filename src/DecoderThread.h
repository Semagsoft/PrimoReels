#pragma once

#include <QImage>
#include <QList>
#include <QMutex>
#include <QString>
#include <QThread>
#include <QWaitCondition>
#include <cstdint>
#include <memory>

#include "MediaDecoder.h"

class PreloadThread;

class DecoderThread : public QThread {
  Q_OBJECT
 public:
  DecoderThread();
  ~DecoderThread() override;

  void requestOpen(const QString& path);
  void requestSeek(double seconds);
  void requestNextFrame();
  void requestAudioRestart(double seconds);
  void requestAudioTopup();
  void requestClose();
  void requestThumbnail(const QString& path);

  // Abort pending work and join the worker thread now (same abort+wait the
  // destructor performs). Owners must call this on decoder threads before
  // destroying objects those threads reference mid-request — in particular
  // the PreloadThread consulted by requestOpen. Never call from the worker
  // thread itself.
  void stopAndWait();

  // Optional preload source consulted on requestOpen: when it holds a
  // warmed decoder for the requested path, that decoder is promoted
  // instead of re-opening the file. Must be set before start().
  void setPreloader(PreloadThread* preloader);

 signals:
  void loaded(const QString& path,
              double duration,
              int width,
              int height,
              bool hasAudio,
              double aspectRatio);
  void frameReady(double position, QImage frame);
  void audioDataReady(QByteArray data, double startTime);
  void audioFinished();
  void audioError(const QString& message);
  void failed(const QString& message);
  void closed();
  // An in-flight seek was preempted by a higher-priority request (open,
  // audio restart, or a newer seek) and will never produce a frame. The
  // superseding request's own signal carries the fresh state; this one only
  // releases waiters on the dropped seek (e.g. the engine's seeking gate).
  void seekDropped();
  void thumbnailReady(const QString& path, const QImage& thumb);

 protected:
  void run() override;

 private:
  void ensureDecoder();

  QMutex m_mutex;
  QWaitCondition m_condition;
  bool m_abort = false;
  // Independent coalescing flags: each request type keeps only its latest
  // target (last-target-wins per type) so e.g. an audio restart is never
  // dropped by a concurrent seek. Priority in run(): Close > Open >
  // Seek/AudioRestart (newest first by sequence) > Topup > Thumbnail.
  enum RequestType : std::uint8_t {
    None,
    RequestOpen,
    RequestSeek,
    RequestNextFrame,
    RequestAudioRestart,
    RequestAudioTopup,
    RequestClose,
    RequestThumbnail
  };
  bool m_hasOpen = false;
  bool m_hasSeek = false;
  bool m_hasNextFrame = false;
  bool m_hasAudioRestart = false;
  bool m_hasClose = false;
  bool m_audioTopupPending = false;
  QString m_pendingPath;
  QList<QString> m_thumbQueue;
  double m_seekSeconds = 0.0;
  double m_audioRestartSeconds = 0.0;
  quint64 m_seqCounter = 0;
  quint64 m_seekSeq = 0;
  quint64 m_audioRestartSeq = 0;
  PreloadThread* m_preloader = nullptr;
  // Active decoder lives on the heap so a preloaded one can be swapped in.
  std::unique_ptr<MediaDecoder> m_decoder;
};
