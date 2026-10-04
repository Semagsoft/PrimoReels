#include "DecoderThread.h"

#include <QDebug>

#include <algorithm>

#include "PreloadThread.h"

DecoderThread::DecoderThread() = default;

DecoderThread::~DecoderThread() {
  {
    QMutexLocker locker(&m_mutex);
    m_abort = true;
    m_condition.wakeOne();
  }
  wait();
}

void DecoderThread::requestOpen(const QString& path) {
  QMutexLocker locker(&m_mutex);
  m_pendingPath = path;
  m_hasOpen = true;
  m_condition.wakeOne();
}

void DecoderThread::requestSeek(double seconds) {
  QMutexLocker locker(&m_mutex);
  m_seekSeconds = seconds;
  m_hasSeek = true;
  m_hasNextFrame = false;
  m_seekSeq = ++m_seqCounter;
  m_condition.wakeOne();
}

void DecoderThread::requestNextFrame() {
  QMutexLocker locker(&m_mutex);
  if (m_hasSeek || m_hasOpen) {
    return;  // Don't queue next frame if a seek or open is pending
  }
  m_hasNextFrame = true;
  m_condition.wakeOne();
}

void DecoderThread::requestAudioRestart(double seconds) {
  QMutexLocker locker(&m_mutex);
  m_audioRestartSeconds = seconds;
  m_hasAudioRestart = true;
  m_audioRestartSeq = ++m_seqCounter;
  m_condition.wakeOne();
}

void DecoderThread::requestAudioTopup() {
  QMutexLocker locker(&m_mutex);
  // Never lost: runs after pending open/seek/restart.
  m_audioTopupPending = true;
  m_condition.wakeOne();
}

void DecoderThread::requestClose() {
  QMutexLocker locker(&m_mutex);
  m_hasClose = true;
  m_hasOpen = false;
  m_hasSeek = false;
  m_hasAudioRestart = false;
  m_audioTopupPending = false;
  m_condition.wakeOne();
}

void DecoderThread::setPreloader(PreloadThread* preloader) {
  QMutexLocker locker(&m_mutex);
  m_preloader = preloader;
}

void DecoderThread::stopAndWait() {
  {
    QMutexLocker locker(&m_mutex);
    m_abort = true;
    m_condition.wakeOne();
  }
  wait();
}

void DecoderThread::requestThumbnail(const QString& path) {
  QMutexLocker locker(&m_mutex);
  // Playback requests always win; queued thumbnails render when idle.
  // Bound the queue so bin scrolling cannot starve playback/audio.
  static constexpr int kMaxThumbQueue = 32;
  if (!m_thumbQueue.contains(path)) {
    if (m_thumbQueue.size() >= kMaxThumbQueue) {
      m_thumbQueue.takeFirst();
    }
    m_thumbQueue.append(path);
  }
  m_condition.wakeOne();
}

void DecoderThread::ensureDecoder() {
  if (!m_decoder) {
    m_decoder = std::make_unique<MediaDecoder>();
  }
}

void DecoderThread::run() {
  while (true) {
    RequestType req = None;
    QString path;
    double seekSec = 0.0;

    {
      QMutexLocker locker(&m_mutex);
      while (!m_hasClose && !m_hasOpen && !m_hasSeek && !m_hasNextFrame && !m_hasAudioRestart &&
             !m_audioTopupPending && m_thumbQueue.isEmpty() && !m_abort) {
        m_condition.wait(&m_mutex);
      }
      if (m_abort) {
        break;
      }
      // Priority: Close > Open > newest of Seek/AudioRestart > NextFrame > Topup > Thumb.
      if (m_hasClose) {
        req = RequestClose;
        m_hasClose = false;
        m_hasNextFrame = false;
      } else if (m_hasOpen) {
        req = RequestOpen;
        path = m_pendingPath;
        m_hasOpen = false;
        m_hasNextFrame = false;
      } else if (m_hasSeek || m_hasAudioRestart) {
        const bool seekNewer = m_hasSeek && (!m_hasAudioRestart || m_seekSeq >= m_audioRestartSeq);
        if (seekNewer) {
          req = RequestSeek;
          seekSec = m_seekSeconds;
          m_hasSeek = false;
          m_hasNextFrame = false;
        } else {
          req = RequestAudioRestart;
          seekSec = m_audioRestartSeconds;
          m_hasAudioRestart = false;
        }
      } else if (m_hasNextFrame) {
        req = RequestNextFrame;
        m_hasNextFrame = false;
      } else if (m_audioTopupPending) {
        m_audioTopupPending = false;
        req = RequestAudioTopup;
      } else if (!m_thumbQueue.isEmpty()) {
        req = RequestThumbnail;
        path = m_thumbQueue.takeFirst();
      }
    }

    if (req == RequestOpen) {
      // Prefer a warmed decoder when the preloader already opened this file.
      // MediaDecoder is a plain value type (no QObject affinity), so the
      // unique_ptr handoff is a pure ownership transfer — no moveToThread.
      std::unique_ptr<MediaDecoder> promoted;
      {
        QMutexLocker locker(&m_mutex);
        if (m_preloader) {
          // Release m_mutex before calling into PreloadThread's mutex to
          // avoid nested locking across two mutex types (deadlock risk if
          // any other path acquires them in reverse order).
          locker.unlock();
          promoted = m_preloader->takeReadyDecoder(path);
        }
      }
      if (promoted) {
        m_decoder = std::move(promoted);
      } else {
        ensureDecoder();
        m_decoder->close();
        if (!m_decoder->open(path)) {
          emit failed(QStringLiteral("Could not open media: %1").arg(path));
          continue;
        }
      }
      emit loaded(path, m_decoder->duration(), m_decoder->width(), m_decoder->height(),
                  m_decoder->hasAudio(), m_decoder->displayAspectRatio());
    } else if (req == RequestAudioRestart) {
      ensureDecoder();
      constexpr int chunkBytes = 48000;
      QByteArray chunk;
      if (!m_decoder->seekAudioTo(seekSec)) {
        emit audioError(QStringLiteral("Could not decode audio at %1 seconds").arg(seekSec));
      } else {
        chunk = m_decoder->readAudioChunk(chunkBytes);
        if (chunk.isEmpty()) {
          emit audioFinished();
        } else {
          emit audioDataReady(chunk, seekSec);
        }
      }
    } else if (req == RequestAudioTopup) {
      ensureDecoder();
      constexpr int chunkBytes = 48000;
      const QByteArray chunk = m_decoder->readAudioChunk(chunkBytes);
      if (chunk.isEmpty()) {
        emit audioFinished();
      } else {
        const double startTime =
            m_decoder->audioPosition() -
            static_cast<double>(chunk.size()) / MediaDecoder::audioBytesPerSecond();
        emit audioDataReady(chunk, startTime);
      }
    } else if (req == RequestNextFrame) {
      ensureDecoder();
      double frameTime = 0.0;
      QImage frame = m_decoder->readNextVideoFrame(&frameTime);
      if (!frame.isNull()) {
        bool stale = false;
        {
          QMutexLocker locker(&m_mutex);
          stale = m_hasClose || m_hasOpen || m_hasSeek;
        }
        if (!stale) {
          emit frameReady(frameTime, frame);
        }
      }
    } else if (req == RequestSeek) {
      ensureDecoder();
      double currentSeekSec = seekSec;
      QImage frame;
      bool cancelled = false;
      bool failedDecode = false;
      for (;;) {
        cancelled = false;
        frame = m_decoder->getFrameAt(currentSeekSec, [&]() {
          QMutexLocker locker(&m_mutex);
          // Any high-priority request preempts a long decode;
          // a newer seek for a different target also preempts.
          if (m_hasClose || m_hasOpen || m_hasAudioRestart) {
            cancelled = true;
            return true;
          }
          if (m_hasSeek && !qFuzzyCompare(m_seekSeconds, currentSeekSec)) {
            cancelled = true;
            return true;
          }
          return false;
        });
        if (!frame.isNull()) {
          break;
        }
        {
          QMutexLocker locker(&m_mutex);
          if (m_hasClose || m_hasOpen || m_hasAudioRestart || m_hasSeek) {
            cancelled = true;
          }
        }
        if (cancelled) {
          // Leave pending flags intact; the run loop picks them up
          // next iteration in priority order. Emit nothing stale.
          // The dropped seek still needs a terminal signal: without it a
          // preemption by open/audio-restart (which never produce a frame
          // for this seek) leaves the engine's seeking gate stuck.
          emit seekDropped();
          failedDecode = false;
          frame = QImage();
          break;
        }
        failedDecode = true;
        break;
      }

      if (!frame.isNull()) {
        // Drop the frame if a higher-priority request arrived while
        // decoding so opens/closes never show a stale image.
        bool stale = false;
        {
          QMutexLocker locker(&m_mutex);
          stale = m_hasClose || m_hasOpen;
        }
        if (!stale) {
          emit frameReady(currentSeekSec, frame);
        }
      } else if (failedDecode) {
        emit failed(QStringLiteral("Could not decode frame at %1 seconds").arg(currentSeekSec));
      }
    } else if (req == RequestClose) {
      ensureDecoder();
      m_decoder->close();
      emit closed();
    } else if (req == RequestThumbnail) {
      // Check for preempting playback work before starting a blocking thumb.
      {
        QMutexLocker locker(&m_mutex);
        if (m_hasClose || m_hasOpen || m_hasSeek || m_hasAudioRestart || m_audioTopupPending ||
            m_hasNextFrame) {
          // Requeue at front and handle playback first.
          m_thumbQueue.prepend(path);
          continue;
        }
      }
      // Isolated decoder so bin thumbnails never disturb playback state.
      // Cancellable: abort the forward scan as soon as playback work arrives.
      MediaDecoder thumbDecoder;
      QImage thumb;
      if (thumbDecoder.open(path)) {
        const double at = std::min(0.5, thumbDecoder.duration() * 0.1);
        const QImage frame = thumbDecoder.getFrameAt(at, [&]() {
          QMutexLocker locker(&m_mutex);
          return m_hasClose || m_hasOpen || m_hasSeek || m_hasAudioRestart || m_audioTopupPending ||
                 m_hasNextFrame || m_abort;
        });
        {
          QMutexLocker locker(&m_mutex);
          if (m_hasClose || m_hasOpen || m_hasSeek || m_hasAudioRestart || m_audioTopupPending ||
              m_hasNextFrame) {
            // Preempted mid-decode: requeue and handle playback first.
            // Drop the frame even if one was decoded to avoid stale work.
            m_thumbQueue.prepend(path);
            continue;
          }
        }
        if (!frame.isNull()) {
          thumb = frame.scaledToWidth(96, Qt::SmoothTransformation);
        }
      }
      emit thumbnailReady(path, thumb);
    }
  }
  m_decoder.reset();
}
