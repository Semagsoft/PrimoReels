#include "PreloadThread.h"

#include <QMutexLocker>
#include <algorithm>
#include <functional>

PreloadThread::PreloadThread() = default;

PreloadThread::~PreloadThread() {
  {
    QMutexLocker locker(&m_mutex);
    m_abort = true;
    m_condition.wakeOne();
  }
  wait();
}

void PreloadThread::requestPreload(const QString& path, double seconds) {
  QMutexLocker locker(&m_mutex);
  // Same target already warmed or already queued: nothing to do.
  if (m_hasJob && m_jobPath == path && qFuzzyCompare(1.0 + m_jobSeconds, 1.0 + seconds)) {
    return;
  }
  if (!m_hasJob && m_readyDecoder && m_readyPath == path && m_hasReadySeconds &&
      qFuzzyCompare(1.0 + m_readySeconds, 1.0 + seconds)) {
    return;
  }
  // Otherwise (re)queue: a retarget to the same path with a new position
  // (e.g. trim change) must re-warm instead of promoting a stale decoder.
  m_jobPath = path;
  m_jobSeconds = seconds;
  m_hasJob = true;
  m_condition.wakeOne();
}

void PreloadThread::requestWaveform(const QString& path, int buckets) {
  if (path.isEmpty()) {
    return;
  }
  QMutexLocker locker(&m_mutex);
  if (m_waveInflight.contains(path) || m_activeWavePath == path) {
    return;  // already queued or currently rendering
  }
  for (const WaveJob& job : std::as_const(m_waveQueue)) {
    if (job.path == path) {
      return;  // already queued
    }
  }
  m_waveQueue.append(WaveJob{path, std::max(8, buckets)});
  m_waveInflight.insert(path);
  m_condition.wakeOne();
}

void PreloadThread::requestDrop() {
  QMutexLocker locker(&m_mutex);
  m_hasJob = false;
  m_waveQueue.clear();
  m_waveInflight.clear();
  // Do not clear m_activeWavePath: the in-flight render checks it on
  // completion and emits empty so the engine's inflight set resolves.
  m_probeQueue.clear();
  m_readyDecoder.reset();
  m_readyPath.clear();
  m_hasReadySeconds = false;
  m_condition.wakeOne();
}

void PreloadThread::requestProbe(const QString& path) {
  if (path.isEmpty()) {
    return;
  }
  QMutexLocker locker(&m_mutex);
  if (m_probeQueue.contains(path)) {
    return;  // already queued
  }
  // Bound the queue: a crafted project can otherwise park thousands of
  // decoder opens here (contrast the capped thumbnail queue). Evict the
  // oldest still-queued probe; its requester re-queues on next need, and
  // requestDrop() clears the queue on project switches.
  static constexpr int kMaxProbeQueue = 64;
  if (m_probeQueue.size() >= kMaxProbeQueue) {
    m_probeQueue.takeFirst();
  }
  m_probeQueue.append(path);
  m_condition.wakeOne();
}

std::unique_ptr<MediaDecoder> PreloadThread::takeReadyDecoder(const QString& path) {
  QMutexLocker locker(&m_mutex);
  if (!m_readyDecoder || m_readyPath != path) {
    return nullptr;
  }
  m_readyPath.clear();
  return std::move(m_readyDecoder);
}

static QVariantList renderWaveform(MediaDecoder& decoder,
                                   int buckets,
                                   const std::function<bool()>& shouldAbort) {
  // Contract: may return FEWER than `buckets` peaks for short files or
  // streams that hit EOF early (callers must use peaks.length, never assume
  // the requested count). Empty means no audio data or preemption.
  QVariantList peaks;
  peaks.reserve(buckets);
  const double totalBytes = decoder.duration() * MediaDecoder::audioBytesPerSecond();
  if (!(totalBytes > 0.0)) {
    return peaks;
  }
  // A failed rewind would silently offset every bucket: report no data
  // (the spinner resolves) instead of a wrong-position waveform.
  if (!decoder.seekAudioTo(0.0)) {
    return peaks;
  }
  const double bytesPerBucket = totalBytes / buckets;
  for (int b = 0; b < buckets; ++b) {
    if (shouldAbort && shouldAbort()) {
      return {};  // preempted by a preload job; caller requeues
    }
    int remaining = static_cast<int>(bytesPerBucket);
    int peak = 0;
    while (remaining > 0) {
      const QByteArray chunk = decoder.readAudioChunk(std::min(65536, remaining));
      if (chunk.isEmpty()) {
        break;
      }
      const auto* samples = reinterpret_cast<const qint16*>(chunk.constData());
      const int count = chunk.size() / static_cast<int>(sizeof(qint16));
      for (int i = 0; i < count; ++i) {
        peak = std::max(peak, std::abs(static_cast<int>(samples[i])));
      }
      remaining -= chunk.size();
    }
    peaks.append(peak / 32768.0);
  }
  return peaks;
}

void PreloadThread::run() {
  for (;;) {
    QString path;
    double seconds = 0.0;
    WaveJob wave;
    bool isWave = false;
    bool isProbe = false;
    {
      QMutexLocker locker(&m_mutex);
      while (!m_hasJob && m_probeQueue.isEmpty() && m_waveQueue.isEmpty() && !m_abort) {
        m_condition.wait(&m_mutex);
      }
      if (m_abort) {
        break;
      }
      // Priority: sequence preload (playback-critical) > import probes >
      // waveforms (long renders).
      if (m_hasJob) {
        path = m_jobPath;
        seconds = m_jobSeconds;
        m_hasJob = false;
      } else if (!m_probeQueue.isEmpty()) {
        path = m_probeQueue.takeFirst();
        isProbe = true;
      } else {
        wave = m_waveQueue.takeFirst();
        isWave = true;
        m_activeWavePath = wave.path;
      }
    }

    if (isProbe) {
      MediaDecoder decoder;
      if (decoder.open(path)) {
        emit probeReady(path, decoder.duration(), decoder.width(), decoder.height(),
                        decoder.hasAudio());
      } else {
        emit probeFailed(path);
      }
      continue;
    }

    if (isWave) {
      MediaDecoder decoder;
      QVariantList peaks;
      if (decoder.open(wave.path) && decoder.hasAudio()) {
        // Capture preemption in a local flag so a preload/probe arriving
        // mid-render still requeues even if it is consumed before we
        // re-check (no TOCTOU between render end and queue check).
        bool wasPreempted = false;
        peaks = renderWaveform(decoder, wave.buckets, [this, &wasPreempted] {
          QMutexLocker locker(&m_mutex);
          const bool preempt = m_hasJob || !m_probeQueue.isEmpty() || m_abort;
          if (preempt) {
            wasPreempted = true;
          }
          return preempt;
        });
        if (peaks.isEmpty() && wasPreempted && !m_abort) {
          QMutexLocker locker(&m_mutex);
          // Preempted: preload/probe wins; requeue waveform for later.
          m_waveQueue.prepend(wave);
          m_activeWavePath.clear();
          continue;
        }
        // Genuinely empty (no audio data): fall through and emit
        // empty so inflight spinners resolve.
      }
      {
        QMutexLocker locker(&m_mutex);
        m_waveInflight.remove(wave.path);
        m_activeWavePath.clear();
      }
      emit waveformReady(wave.path, peaks);
      continue;
    }

    auto decoder = std::make_unique<MediaDecoder>();
    if (!decoder->open(path)) {
      continue;  // advance falls back to a normal open
    }
    // Warm caches at the exact position playback will start from.
    // Cancellable: stop warming if a drop/new job arrived mid-decode.
    const QImage warmed = decoder->getFrameAt(seconds, [this]() {
      QMutexLocker locker(&m_mutex);
      return m_abort || m_hasJob;
    });
    Q_UNUSED(warmed);

    {
      QMutexLocker locker(&m_mutex);
      if (m_abort) {
        break;
      }
      if (m_hasJob || (m_readyDecoder && m_readyPath != path)) {
        // New preload request arrived or shutdown signaled; discard
        // the partially-warmed decoder and let the run loop handle it.
        continue;
      }
      m_readyDecoder = std::move(decoder);
      m_readyPath = path;
      m_readySeconds = seconds;
      m_hasReadySeconds = true;
    }
    emit preloaded(path);
  }
}
