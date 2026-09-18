#include "TimelineEngine.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMutexLocker>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include "ClipUtils.h"
#include "VideoEffects.h"

TimelineEngine::TimelineEngine(QObject* parent) : QObject(parent) {
  m_mediaFilterModel.setSourceModel(&m_mediaModel);
  syncModels();
  connect(&m_decoderThread, &DecoderThread::loaded, this, &TimelineEngine::onLoaded);
  connect(&m_decoderThread, &DecoderThread::frameReady, this, &TimelineEngine::onFrameReady);
  connect(&m_decoderThread, &DecoderThread::audioDataReady, this, &TimelineEngine::onAudioData);
  connect(&m_decoderThread, &DecoderThread::audioFinished, this, &TimelineEngine::onAudioFinished);
  connect(&m_decoderThread, &DecoderThread::audioError, this, &TimelineEngine::onAudioError);
  connect(&m_decoderThread, &DecoderThread::failed, this, &TimelineEngine::onDecoderFailed);
  connect(&m_decoderThread, &DecoderThread::closed, this, &TimelineEngine::onDecoderClosed);
  connect(&m_decoderThread, &DecoderThread::seekDropped, this, &TimelineEngine::onSeekDropped);
  connect(&m_exportThread, &ExportThread::progressChanged, this, &TimelineEngine::onExportProgress);
  connect(&m_exportThread, &ExportThread::exportFinished, this, &TimelineEngine::onExportFinished);
  connect(&m_exportThread, &ExportThread::exportFailed, this, &TimelineEngine::onExportFailed);
  connect(&m_exportThread, &ExportThread::exportWarning, this, &TimelineEngine::onExportWarning);
  connect(&m_decoderThread, &DecoderThread::thumbnailReady, this, &TimelineEngine::onThumbReady);
  connect(&m_preloadThread, &PreloadThread::waveformReady, this, &TimelineEngine::onWaveformReady);
  connect(&m_preloadThread, &PreloadThread::probeReady, this, &TimelineEngine::onProbeReady);
  connect(&m_preloadThread, &PreloadThread::probeFailed, this, &TimelineEngine::onProbeFailed);
  // Independent source preview thread: no preloader, no audio requests.
  connect(&m_sourceDecoderThread, &DecoderThread::loaded, this, &TimelineEngine::onSourceLoaded);
  connect(&m_sourceDecoderThread, &DecoderThread::frameReady, this,
          &TimelineEngine::onSourceFrameReady);
  connect(&m_sourceDecoderThread, &DecoderThread::failed, this, &TimelineEngine::onSourceFailed);
  connect(&m_sourceDecoderThread, &DecoderThread::closed, this, &TimelineEngine::onSourceClosed);
  // Bed preview thread: audio restarts + top-ups only (no preloader, no
  // video/thumbnail requests), mixed with program audio in tryFlushAudioStages.
  connect(&m_bedDecoderThread, &DecoderThread::loaded, this, &TimelineEngine::onBedLoaded);
  connect(&m_bedDecoderThread, &DecoderThread::audioDataReady, this,
          &TimelineEngine::onBedAudioData);
  connect(&m_bedDecoderThread, &DecoderThread::audioFinished, this,
          &TimelineEngine::onBedAudioFinished);
  connect(&m_bedDecoderThread, &DecoderThread::audioError, this, &TimelineEngine::onBedAudioError);
  connect(&m_bedDecoderThread, &DecoderThread::failed, this, &TimelineEngine::onBedFailed);

  m_timer.setInterval(33);
  connect(&m_timer, &QTimer::timeout, this, &TimelineEngine::onTick);
  m_sourceTimer.setInterval(33);
  connect(&m_sourceTimer, &QTimer::timeout, this, &TimelineEngine::onSourceTick);

  m_decoderThread.setPreloader(&m_preloadThread);
  m_decoderThread.start();
  m_sourceDecoderThread.start();
  m_bedDecoderThread.start();
  m_preloadThread.start();
  m_exportThread.start();
}

TimelineEngine::~TimelineEngine() {
  m_timer.stop();
  m_sourceTimer.stop();
  // Prevent worker-thread signals from reaching a half-destroyed object
  // while member QThreads abort+wait in their own destructors.
  disconnect(&m_decoderThread, nullptr, this, nullptr);
  disconnect(&m_sourceDecoderThread, nullptr, this, nullptr);
  disconnect(&m_bedDecoderThread, nullptr, this, nullptr);
  disconnect(&m_preloadThread, nullptr, this, nullptr);
  disconnect(&m_exportThread, nullptr, this, nullptr);
  // Join decoder workers BEFORE the preloader is torn down. RequestOpen
  // unlocks the decoder mutex before calling takeReadyDecoder(), so merely
  // nulling the back-pointer leaves a window where a racing open derefs a
  // PreloadThread that member destruction (reverse declaration order:
  // PreloadThread dies before DecoderThread) has already freed. Joined
  // threads cannot be inside that call, closing the window by construction.
  // The bed thread never consults the preloader, but joins with the rest so
  // no audio callback can land mid-destruction.
  m_decoderThread.requestClose();
  m_sourceDecoderThread.requestClose();
  m_bedDecoderThread.requestClose();
  m_decoderThread.stopAndWait();
  m_sourceDecoderThread.stopAndWait();
  m_bedDecoderThread.stopAndWait();
  m_decoderThread.setPreloader(nullptr);
  m_audioPlayer.stop();
  m_exportThread.requestCancel();
  m_preloadThread.requestDrop();
  m_opening = false;
  m_seeking = false;
  m_hasPendingSeek = false;
  m_sourceOpening = false;
  m_sourceSeeking = false;
  m_sourceHasPendingSeek = false;
  m_sourcePlaying = false;
  m_sourceAutoplay = false;
}

QString TimelineEngine::activeEffect() const {
  QMutexLocker locker(&m_dataMutex);
  return m_activeEffect;
}

int TimelineEngine::thumbVersion() const {
  QMutexLocker locker(&m_dataMutex);
  return m_thumbVersion;
}

int TimelineEngine::waveVersion() const {
  QMutexLocker locker(&m_dataMutex);
  return m_waveVersion;
}

void TimelineEngine::setPosition(double pos) {
  seek(pos);
}

void TimelineEngine::loadMedia(const QString& filePath) {
  const QString path = ClipUtils::normalizedMediaPath(filePath);
  if (path.isEmpty()) {
    return;
  }

  {
    QMutexLocker locker(&m_dataMutex);
    m_activeEffect.clear();
  }
  // Preview intent: leave sequence mode (pause() alone preserves it now).
  // playSequenceFrom() re-arms the sequence state after this returns.
  stopSequence();
  pause();
  if (m_opening || m_seeking) {
    m_pendingLoad = path;
    m_hasPendingSeek = false;
    return;
  }

  // A new explicit open supersedes any pending project-load source context.
  // (loadProject sets m_loadingSource after this call returns.)
  m_loadingSource = false;
  m_opening = true;
  m_hasPendingSeek = false;
  m_decoderThread.requestOpen(path);
}

void TimelineEngine::importMedia(const QString& filePath) {
  // Import registers the file in the bin and probes its metadata on the
  // preload thread. The program monitor is deliberately untouched: no pause,
  // no open/seek, no currentSource/duration change.
  const QString path = ClipUtils::normalizedMediaPath(filePath);
  if (path.isEmpty()) {
    return;
  }
  bool known = false;
  bool hasMetadata = false;
  for (const QVariant& v : std::as_const(m_mediaList)) {
    const QVariantMap entry = v.toMap();
    if (entry.value(QStringLiteral("path")).toString() == path) {
      known = true;
      hasMetadata = entry.value(QStringLiteral("duration"), 0.0).toDouble() > 0.0;
      break;
    }
  }
  if (known) {
    // Already imported; make sure artwork is queued and re-probe when the
    // entry is still an unknown placeholder with no probe in flight
    // (requestProbe dedups, so this is cheap).
    requestThumb(path);
    if (!hasMetadata) {
      m_preloadThread.requestProbe(path);
    }
    return;
  }
  QVariantMap entry;
  entry.insert(QStringLiteral("path"), path);
  entry.insert(QStringLiteral("name"), QFileInfo(path).fileName());
  entry.insert(QStringLiteral("duration"), 0.0);  // unknown until the probe lands
  // Bin import is a project edit: snapshot for undo and mark dirty so the
  // unsaved-changes prompt cannot be bypassed. The async probe enrichment
  // (onProbeReady) and failure cleanup (onProbeFailed) are part of this same
  // logical edit — they update the entry in place without pushing extra
  // history, only keeping the dirty flag.
  pushHistory();
  m_mediaList.append(entry);
  syncModels();
  emit mediaListChanged();
  requestThumb(path);
  m_preloadThread.requestProbe(path);
}

void TimelineEngine::onProbeReady(const QString& path,
                                  double duration,
                                  int width,
                                  int height,
                                  bool hasAudio) {
  Q_UNUSED(width)
  Q_UNUSED(height)
  // Stale probe (entry removed or project replaced while probing): ignore.
  bool stillThere = false;
  for (const QVariant& v : std::as_const(m_mediaList)) {
    if (v.toMap().value(QStringLiteral("path")).toString() == path) {
      stillThere = true;
      break;
    }
  }
  if (!stillThere || !(duration > 0.0)) {
    return;
  }
  const QVariantMap entry = upsertMediaEntry(path, duration, hasAudio);
  refreshTimelineClipsForPath(path, duration, entry.value(QStringLiteral("name")).toString());
  // Part of the import edit (see importMedia): no extra undo step, but keep
  // the project dirty in case the user saved while the probe was in flight.
  setModified(true);
}

void TimelineEngine::onProbeFailed(const QString& path) {
  // Only drop the entry when it is still the unknown placeholder this
  // import created; a concurrently probed success wins over the failure.
  for (int i = 0; i < m_mediaList.size(); ++i) {
    const QVariantMap entry = m_mediaList.at(i).toMap();
    if (entry.value(QStringLiteral("path")).toString() == path) {
      if (!(entry.value(QStringLiteral("duration"), 0.0).toDouble() > 0.0)) {
        m_mediaList.removeAt(i);
        bool timelineTouched = false;
        for (int j = m_timelineClips.size() - 1; j >= 0; --j) {
          if (m_timelineClips.at(j).toMap().value(QStringLiteral("path")).toString() == path) {
            m_timelineClips.removeAt(j);
            timelineTouched = true;
          }
        }
        if (timelineTouched) {
          recomputeTimelineStarts();
          setSequenceState(false, -1);
          m_sequenceAutoplay = false;
        }
        syncModels();
        emit mediaListChanged();
        if (timelineTouched) {
          emit timelineClipsChanged();
        }
        // Part of the import edit: no extra undo step (undo returns to the
        // pre-import snapshot, which matches this state), but keep dirty in
        // case the user saved the placeholder while probing.
        setModified(true);
        emit failed(QStringLiteral("Could not import media: %1").arg(path));
      }
      return;
    }
  }
}

void TimelineEngine::play() {
  if (m_isPlaying || m_duration <= 0.0) {
    return;
  }

  if (m_position >= m_duration) {
    m_position = 0.0;
    m_playbackBasePosition = 0.0;
    m_playbackTimer.restart();
    requestSeek(0.0, true);
  } else {
    m_playbackBasePosition = m_position;
    m_playbackTimer.restart();
  }

  m_isPlaying = true;
  emit isPlayingChanged();
  m_timer.start();

  // Program and/or bed audio: restartAudio resolves both (bed by output
  // time), so an A1 bed stays audible even when the program clip is silent.
  if (m_hasAudio || !bedClipAt(outputTimeFor(m_position)).isEmpty()) {
    // Resume audio where the picture resumes: m_position already holds the
    // intra-clip offset (beginPlaybackAtPending seeks to m_seqStart on a
    // fresh clip start, so this matches there too).
    restartAudio(m_position);
  }
}

void TimelineEngine::pause() {
  if (!m_isPlaying) {
    return;
  }

  m_isPlaying = false;
  emit isPlayingChanged();
  m_timer.stop();
  m_audioPlayer.suspend();
  // Pure transport pause: the sequence lane position, trim window and index
  // survive so play() resumes mid-clip and onTick() still auto-advances.
  // Callers that mean "leave sequence mode" (stop button, preview,
  // structural edits, project switches) call stopSequence() explicitly.
  // A pending autoplay is still disarmed so a load racing this pause cannot
  // resume playback against the user's intent.
  m_sequenceAutoplay = false;
}

QByteArray TimelineEngine::peekPreviewAudio(qint64 maxBytes) const {
  return m_audioPlayer.peekBuffered(maxBytes);
}

void TimelineEngine::setSequenceState(bool playing, int index) {
  if (m_sequencePlaying == playing && m_sequenceIndex == index) {
    return;
  }
  m_sequencePlaying = playing;
  m_sequenceIndex = index;
  emit sequencePlayingChanged();
}

int TimelineEngine::sequenceIndexOf(const QString& filePath) const {
  const QString path = ClipUtils::normalizedMediaPath(filePath);
  // Prefer the V1 lane so resume/restart targets the program clip even when
  // the same file also sits on the V2 overlay lane.
  for (int i = 0; i < m_timelineClips.size(); ++i) {
    const QVariantMap clip = m_timelineClips.at(i).toMap();
    if (clip.value(QStringLiteral("path")).toString() == path &&
        clip.value(QStringLiteral("track"), 0).toInt() == 0) {
      return i;
    }
  }
  for (int i = 0; i < m_timelineClips.size(); ++i) {
    if (m_timelineClips.at(i).toMap().value(QStringLiteral("path")).toString() == path) {
      return i;
    }
  }
  return -1;
}

bool TimelineEngine::isSequenceClip(int index) const {
  return index >= 0 && index < m_timelineClips.size() &&
         m_timelineClips.at(index).toMap().value(QStringLiteral("track"), 0).toInt() == 0;
}

int TimelineEngine::nextSequenceIndex(int from) const {
  for (int i = from + 1; i < m_timelineClips.size(); ++i) {
    if (isSequenceClip(i)) {
      return i;
    }
  }
  return -1;
}

int TimelineEngine::resolveSequenceIndex(int index) const {
  if (m_timelineClips.isEmpty()) {
    return -1;
  }
  const int pos = std::clamp(index, 0, static_cast<int>(m_timelineClips.size()) - 1);
  if (isSequenceClip(pos)) {
    return pos;
  }
  const int next = nextSequenceIndex(pos);
  if (next >= 0) {
    return next;
  }
  for (int i = pos - 1; i >= 0; --i) {
    if (isSequenceClip(i)) {
      return i;
    }
  }
  return -1;
}

int TimelineEngine::sequencePosition() const {
  if (!isSequenceClip(m_sequenceIndex)) {
    return 0;
  }
  int ordinal = 0;
  for (int i = 0; i <= m_sequenceIndex; ++i) {
    if (isSequenceClip(i)) {
      ++ordinal;
    }
  }
  return ordinal;
}

int TimelineEngine::sequenceCount() const {
  int count = 0;
  for (int i = 0; i < m_timelineClips.size(); ++i) {
    if (isSequenceClip(i)) {
      ++count;
    }
  }
  return count;
}

void TimelineEngine::playSequenceFrom(int index) {
  const int pos = resolveSequenceIndex(index);
  if (pos < 0) {
    return;
  }
  const QVariantMap clip = m_timelineClips.at(pos).toMap();
  const QString path = clip.value(QStringLiteral("path")).toString();
  m_seqStart = ClipUtils::clipTrimStart(clip);
  m_seqEnd = m_seqStart + clip.value(QStringLiteral("duration"), 0.0).toDouble();
  // loadMedia() pauses first (pure transport pause: sequence state survives)
  // and stops sequence mode, so arm the sequence state afterwards.
  loadMedia(path);
  {
    QMutexLocker locker(&m_dataMutex);
    m_activeEffect = clip.value(QStringLiteral("effect")).toString();
  }
  m_pendingSeek = m_seqStart;
  m_hasPendingSeek = true;
  m_sequenceAutoplay = true;
  setSequenceState(true, pos);
  // Warm the following V1 clip in parallel so the advance promotes it.
  const int next = nextSequenceIndex(pos);
  if (next >= 0) {
    const QVariantMap nextClip = m_timelineClips.at(next).toMap();
    m_preloadThread.requestPreload(nextClip.value(QStringLiteral("path")).toString(),
                                   ClipUtils::clipTrimStart(nextClip));
  }
}

void TimelineEngine::stopSequence() {
  m_sequenceAutoplay = false;
  setSequenceState(false, -1);
}

void TimelineEngine::requestThumb(const QString& filePath) {
  const QString path = ClipUtils::normalizedMediaPath(filePath);
  {
    QMutexLocker locker(&m_dataMutex);
    if (path.isEmpty() || m_thumbCache.contains(path) || m_thumbInflight.contains(path)) {
      return;
    }
    m_thumbInflight.insert(path);
  }
  m_decoderThread.requestThumbnail(path);
}

bool TimelineEngine::hasThumb(const QString& filePath) const {
  QMutexLocker locker(&m_dataMutex);
  return m_thumbCache.contains(ClipUtils::normalizedMediaPath(filePath));
}

QImage TimelineEngine::thumbImage(const QString& filePath) const {
  QMutexLocker locker(&m_dataMutex);
  return m_thumbCache.value(ClipUtils::normalizedMediaPath(filePath));
}

bool TimelineEngine::isPathInProject(const QString& path) const {
  for (const QVariant& v : std::as_const(m_mediaList)) {
    if (v.toMap().value(QStringLiteral("path")).toString() == path) {
      return true;
    }
  }
  for (const QVariant& v : std::as_const(m_timelineClips)) {
    if (v.toMap().value(QStringLiteral("path")).toString() == path) {
      return true;
    }
  }
  return false;
}

void TimelineEngine::clearArtworkCaches() {
  // Only called with empty media/timeline lists (newProject): the caller's
  // syncModels() then resets the models (including their tick maps), so no
  // delegate can observe a cleared cache with a stale tick. Deliberately
  // NOT used on loadProject, where surviving paths keep valid cache entries
  // and row ticks.
  QMutexLocker locker(&m_dataMutex);
  m_thumbCache.clear();
  m_thumbInflight.clear();
  m_thumbOrder.clear();
  m_waveCache.clear();
  m_waveInflight.clear();
  m_waveOrder.clear();
}

void TimelineEngine::onThumbReady(const QString& path, const QImage& thumb) {
  {
    QMutexLocker locker(&m_dataMutex);
    m_thumbInflight.remove(path);
    // Caches stay keyed by exact path and bounded (FIFO), so entries for
    // removed or never-imported paths are harmless: nothing looks them up,
    // and they age out. (An earlier revision skipped caching here, but the
    // thumb/wave API is deliberately usable standalone — e.g. the source
    // monitor and several tests request artwork for unimported paths.)
    if (!thumb.isNull() && !m_thumbCache.contains(path)) {
      m_thumbCache.insert(path, thumb);
      m_thumbOrder.append(path);
      while (m_thumbOrder.size() > kThumbCacheLimit) {
        m_thumbCache.remove(m_thumbOrder.takeFirst());
      }
    }
    // Always resolve (even on failure) so QML inflight spinners terminate.
    ++m_thumbVersion;
  }
  // Per-row ticks so only delegates showing this path reload.
  m_mediaModel.bumpThumb(path);
  m_timelineModel.bumpThumb(path);
  emit thumbnailsChanged();
}

void TimelineEngine::requestWaveform(const QString& filePath) {
  const QString path = ClipUtils::normalizedMediaPath(filePath);
  {
    QMutexLocker locker(&m_dataMutex);
    if (path.isEmpty() || m_waveCache.contains(path) || m_waveInflight.contains(path)) {
      return;
    }
    m_waveInflight.insert(path);
  }
  m_preloadThread.requestWaveform(path, kWaveBuckets);
}

bool TimelineEngine::hasWaveform(const QString& filePath) const {
  QMutexLocker locker(&m_dataMutex);
  return m_waveCache.contains(ClipUtils::normalizedMediaPath(filePath));
}

QVariantList TimelineEngine::waveform(const QString& filePath) const {
  QMutexLocker locker(&m_dataMutex);
  return m_waveCache.value(ClipUtils::normalizedMediaPath(filePath));
}

void TimelineEngine::onWaveformReady(const QString& path, const QVariantList& peaks) {
  {
    QMutexLocker locker(&m_dataMutex);
    m_waveInflight.remove(path);
    // Same policy as thumbs above: resolve always, cache by exact path.
    if (!peaks.isEmpty() && !m_waveCache.contains(path)) {
      m_waveCache.insert(path, peaks);
      m_waveOrder.append(path);
      while (m_waveOrder.size() > kWaveCacheLimit) {
        m_waveCache.remove(m_waveOrder.takeFirst());
      }
    }
    // Always resolve (even on failure) so QML inflight spinners terminate.
    ++m_waveVersion;
  }
  // Per-row tick so only delegates showing this path repaint.
  m_timelineModel.bumpWave(path);
  emit waveformsChanged();
}

void TimelineEngine::exportSequence(const QString& filePath) {
  if (m_isExporting) {
    return;
  }
  if (m_timelineClips.isEmpty()) {
    emit failed(QStringLiteral("Nothing to export: the timeline is empty."));
    return;
  }
  m_isExporting = true;
  m_exportProgress = 0.0;
  emit isExportingChanged();
  emit exportProgressChanged();
  m_exportThread.requestExport(m_timelineClips, filePath, m_volume);
}

void TimelineEngine::cancelExport() {
  if (!m_isExporting) {
    return;
  }
  m_exportThread.requestCancel();
}

void TimelineEngine::onExportProgress(double doneSeconds, double totalSeconds) {
  const double progress =
      (totalSeconds > 0.0) ? std::clamp(doneSeconds / totalSeconds, 0.0, 1.0) : 0.0;
  if (!qFuzzyCompare(m_exportProgress, progress)) {
    m_exportProgress = progress;
    emit exportProgressChanged();
  }
}

void TimelineEngine::onExportFinished(const QString& outputPath) {
  m_isExporting = false;
  m_exportProgress = 1.0;
  emit isExportingChanged();
  emit exportProgressChanged();
  emit exportSucceeded(outputPath);
}

void TimelineEngine::onExportFailed(const QString& message) {
  m_isExporting = false;
  m_exportProgress = 0.0;
  emit isExportingChanged();
  emit exportProgressChanged();
  emit failed(message);
}

void TimelineEngine::onExportWarning(const QString& message) {
  // Non-fatal: export already succeeded (exportSucceeded emitted first).
  // A dedicated signal so the UI shows this transiently instead of as a
  // sticky error.
  emit warning(message);
}

bool TimelineEngine::saveProject(const QString& filePath) {
  const QString path = ClipUtils::normalizedMediaPath(filePath);
  if (path.isEmpty()) {
    emit failed(QStringLiteral("Cannot save project: empty file path."));
    return false;
  }
  QJsonObject root;
  root.insert(QStringLiteral("version"), 1);
  root.insert(QStringLiteral("mediaList"), QJsonArray::fromVariantList(m_mediaList));
  root.insert(QStringLiteral("timelineClips"), QJsonArray::fromVariantList(m_timelineClips));
  root.insert(QStringLiteral("volume"), m_volume);
  root.insert(QStringLiteral("clipScaleX"), m_clipScaleX);
  root.insert(QStringLiteral("clipScaleY"), m_clipScaleY);
  root.insert(QStringLiteral("clipRotation"), m_clipRotation);
  root.insert(QStringLiteral("currentSource"), m_currentSource);

  // Atomic save via QSaveFile: writes to a temp sibling and renames over
  // the target on commit, so a crash/power loss or a failed write can never
  // destroy the previous project — the original stays intact on failure.
  const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
  QSaveFile file(path);
  // QSaveFile::open fails (e.g. unwritable destination) without touching
  // the existing file.
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    emit failed(QStringLiteral("Cannot save project to %1: %2").arg(path, file.errorString()));
    return false;
  }
  if (file.write(payload) != payload.size()) {
    const QString err = file.errorString();
    file.cancelWriting();
    emit failed(QStringLiteral("Cannot save project to %1: %2").arg(path, err));
    return false;
  }
  // commit() atomically replaces the target; on failure the original file
  // is preserved and QSaveFile cleans up its temp file.
  if (!file.commit()) {
    emit failed(QStringLiteral("Cannot save project to %1: %2").arg(path, file.errorString()));
    return false;
  }
  setModified(false);
  return true;
}

bool TimelineEngine::loadProject(const QString& filePath) {
  const QString path = ClipUtils::normalizedMediaPath(filePath);
  if (path.isEmpty()) {
    emit failed(QStringLiteral("Cannot open project: empty file path."));
    return false;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    emit failed(QStringLiteral("Cannot open project %1: %2").arg(path, file.errorString()));
    return false;
  }
  if (file.size() > kMaxProjectBytes) {
    emit failed(QStringLiteral("Cannot open project %1: file too large (%2 bytes).")
                    .arg(path, QString::number(file.size())));
    return false;
  }
  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
    emit failed(QStringLiteral("Cannot open project %1: invalid project file (%2).")
                    .arg(path, parseError.errorString()));
    return false;
  }
  const QJsonObject root = doc.object();
  if (!root.value(QStringLiteral("mediaList")).isArray() ||
      !root.value(QStringLiteral("timelineClips")).isArray()) {
    emit failed(
        QStringLiteral("Cannot open project %1: missing media or timeline data.").arg(path));
    return false;
  }
  const QJsonArray mediaArray = root.value(QStringLiteral("mediaList")).toArray();
  const QJsonArray clipsArray = root.value(QStringLiteral("timelineClips")).toArray();
  if (mediaArray.size() > kMaxProjectMediaEntries || clipsArray.size() > kMaxProjectTimelineClips) {
    emit failed(
        QStringLiteral("Cannot open project %1: too many entries (%2 media, %3 clips).")
            .arg(path, QString::number(mediaArray.size()), QString::number(clipsArray.size())));
    return false;
  }
  if (root.value(QStringLiteral("version")).toInt(-1) != 1) {
    emit failed(
        QStringLiteral("Cannot open project %1: unsupported version %2.")
            .arg(path, root.value(QStringLiteral("version")).toVariant().toString().left(64)));
    return false;
  }

  // Project switch: pause() alone preserves sequence mode now, and the old
  // lane is meaningless once the lists below are replaced.
  stopSequence();
  pause();
  stopBed();
  // Validate media rows like timeline clips below: crafted types must not
  // reach models/threads/QML as 0-second placeholder ghosts.
  QVariantList mediaList;
  mediaList.reserve(mediaArray.size());
  for (const QJsonValue& v : mediaArray) {
    if (!v.isObject()) {
      continue;
    }
    QVariantMap entry = v.toObject().toVariantMap();
    const QString entryPath = entry.value(QStringLiteral("path")).toString();
    if (entryPath.isEmpty() || entryPath.size() > ClipUtils::kMaxPathChars) {
      continue;
    }
    const QVariant durVar = entry.value(QStringLiteral("duration"), 0.0);
    double entryDur = 0.0;
    if (durVar.typeId() == QMetaType::Double || durVar.typeId() == QMetaType::Float ||
        durVar.typeId() == QMetaType::Int || durVar.typeId() == QMetaType::UInt ||
        durVar.typeId() == QMetaType::LongLong || durVar.typeId() == QMetaType::ULongLong) {
      entryDur = durVar.toDouble();
    }
    if (!ClipUtils::isFiniteDouble(entryDur) || entryDur < 0.0) {
      continue;
    }
    entry.insert(QStringLiteral("path"), entryPath);
    entry.insert(QStringLiteral("duration"), std::min(entryDur, kMaxMediaSeconds));
    if (!entry.contains(QStringLiteral("name")) ||
        entry.value(QStringLiteral("name")).toString().isEmpty()) {
      entry.insert(QStringLiteral("name"), QFileInfo(entryPath).fileName());
    } else {
      entry.insert(QStringLiteral("name"),
                   entry.value(QStringLiteral("name")).toString().left(256));
    }
    mediaList.append(entry);
  }
  // Normalize + validate clips (older files predate keys; hand-edited files
  // may carry garbage). Clips with an empty path are dropped.
  QVariantList validated;
  validated.reserve(clipsArray.size());
  for (const QJsonValue& v : clipsArray) {
    QVariantMap clip = v.isObject() ? v.toObject().toVariantMap() : QVariantMap();
    const QString clipPath = clip.value(QStringLiteral("path")).toString();
    if (clipPath.isEmpty() || clipPath.size() > ClipUtils::kMaxPathChars) {
      continue;
    }
    double sourceDur = clip.value(QStringLiteral("sourceDuration"), -1.0).toDouble();
    double dur = clip.value(QStringLiteral("duration"), 0.0).toDouble();
    if (!ClipUtils::isFiniteDouble(sourceDur) || !ClipUtils::isFiniteDouble(dur)) {
      continue;
    }
    if (!(sourceDur > 0.0)) {
      sourceDur = (dur > 0.0) ? dur : 0.0;
    }
    sourceDur = std::clamp(sourceDur, 0.0, kMaxMediaSeconds);
    double trimStart = std::clamp(clip.value(QStringLiteral("trimStart"), 0.0).toDouble(), 0.0,
                                  std::max(0.0, sourceDur - 0.1));
    double trimEnd = std::clamp(clip.value(QStringLiteral("trimEnd"), 0.0).toDouble(), 0.0,
                                std::max(0.0, sourceDur - trimStart - 0.1));
    dur = std::max(0.1, sourceDur - trimStart - trimEnd);
    clip.insert(QStringLiteral("sourceDuration"), sourceDur);
    clip.insert(QStringLiteral("trimStart"), trimStart);
    clip.insert(QStringLiteral("trimEnd"), trimEnd);
    clip.insert(QStringLiteral("duration"), dur);
    const QString effect = clip.value(QStringLiteral("effect")).toString();
    clip.insert(QStringLiteral("effect"), VideoEffects::isKnownEffect(effect) ? effect : QString());
    const QString transition = clip.value(QStringLiteral("transition")).toString();
    if (!isKnownTransition(transition)) {
      clip.insert(QStringLiteral("transition"), QString());
      clip.insert(QStringLiteral("transitionDuration"), 0.5);
    } else {
      clip.insert(QStringLiteral("transitionDuration"),
                  std::clamp(clip.value(QStringLiteral("transitionDuration"), 0.5).toDouble(), 0.1,
                             std::max(0.1, std::min(2.0, dur))));
    }
    clip.insert(QStringLiteral("track"),
                std::clamp(clip.value(QStringLiteral("track"), 0).toInt(), 0, 2));
    clip.insert(QStringLiteral("gain"),
                std::clamp(clip.value(QStringLiteral("gain"), 1.0).toDouble(), 0.0, 2.0));
    clip.insert(QStringLiteral("muted"), clip.value(QStringLiteral("muted"), false).toBool());
    clip.insert(QStringLiteral("fadeIn"),
                std::clamp(clip.value(QStringLiteral("fadeIn"), 0.0).toDouble(), 0.0, 30.0));
    clip.insert(QStringLiteral("fadeOut"),
                std::clamp(clip.value(QStringLiteral("fadeOut"), 0.0).toDouble(), 0.0, 30.0));
    clip.insert(QStringLiteral("title"), clip.value(QStringLiteral("title")).toString().left(200));
    if (!clip.contains(QStringLiteral("name")) ||
        clip.value(QStringLiteral("name")).toString().isEmpty()) {
      clip.insert(QStringLiteral("name"), QFileInfo(clipPath).fileName());
    } else {
      clip.insert(QStringLiteral("name"), clip.value(QStringLiteral("name")).toString().left(256));
    }
    if (clip.value(QStringLiteral("track"), 0).toInt() != 1 &&
        clip.value(QStringLiteral("track"), 0).toInt() != 2) {
      clip.insert(QStringLiteral("startTime"), 0.0);  // recomputed below
    } else {
      const double overlayStart = clip.value(QStringLiteral("startTime"), 0.0).toDouble();
      clip.insert(QStringLiteral("startTime"), ClipUtils::isFiniteDouble(overlayStart)
                                                   ? std::clamp(overlayStart, 0.0, kMaxMediaSeconds)
                                                   : 0.0);
    }
    validated.append(clip);
  }
  // Commit only after validation: a load that drops rows must not burn an
  // undo slot or dirty flag on partial state.
  pushHistory();
  m_mediaList = mediaList;
  syncModels();
  emit mediaListChanged();
  m_timelineClips = validated;
  recomputeTimelineStarts();
  syncModels();
  emit timelineClipsChanged();

  setVolume(root.value(QStringLiteral("volume")).toDouble(m_volume));
  setClipScaleX(root.value(QStringLiteral("clipScaleX")).toDouble(m_clipScaleX));
  setClipScaleY(root.value(QStringLiteral("clipScaleY")).toDouble(m_clipScaleY));
  setClipRotation(root.value(QStringLiteral("clipRotation")).toDouble(m_clipRotation));

  // Only auto-load a source that belongs to this project: a crafted file
  // must not trigger decoder opens of arbitrary paths.
  const QString source = root.value(QStringLiteral("currentSource")).toString();
  if (!source.isEmpty()) {
    const QString normalized = ClipUtils::normalizedMediaPath(source);
    if (isPathInProject(normalized)) {
      m_loadingSource = true;
      loadMedia(normalized);
    } else {
      emit warning(QStringLiteral("Project source not in media bin; nothing loaded."));
    }
  }
  setModified(false);
  return true;
}

void TimelineEngine::newProject() {
  pause();
  pauseSource();
  // An in-flight export describes the discarded project: cancel first so
  // its progress/finish signals cannot land on the fresh state. (The worker
  // holds its own clip snapshot, so cancelling is purely a UI concern.)
  cancelExport();
  m_loadingSource = false;
  pushHistory();
  m_mediaList.clear();
  m_timelineClips.clear();
  clearArtworkCaches();
  syncModels();
  emit mediaListChanged();
  emit timelineClipsChanged();
  // Clear coalesced program/source state so a stale onLoaded/onFrameReady
  // racing newProject cannot resurrect the previous file.
  m_pendingLoad.clear();
  m_pendingSeek = 0.0;
  m_hasPendingSeek = false;
  m_opening = false;
  m_seeking = false;
  m_sourceWantedPath.clear();
  m_sourcePendingSeek = 0.0;
  m_sourceHasPendingSeek = false;
  m_sourceOpening = false;
  m_sourceSeeking = false;
  // Reset playback/sequence windows too: setSequenceState() only clears the
  // playing/index pair, leaving stale base positions behind otherwise.
  m_playbackBasePosition = 0.0;
  m_seqStart = 0.0;
  m_seqEnd = 0.0;
  m_decoderThread.requestClose();
  m_sourceDecoderThread.requestClose();
  stopBed();
  m_preloadThread.requestDrop();
  {
    QMutexLocker locker(&m_frameMutex);
    m_currentFrame = QImage();
  }
  ++m_frameVersion;
  emit currentFrameChanged();
  m_currentSource.clear();
  emit currentSourceChanged();
  m_duration = 0.0;
  emit durationChanged();
  m_position = 0.0;
  emit positionChanged();
  m_hasAudio = false;
  m_audioEof = false;
  m_currentIsAudioOnly = false;
  emit hasAudioChanged();
  setVolume(1.0);
  setClipScaleX(1.0);
  setClipScaleY(1.0);
  setClipRotation(0.0);
  setSequenceState(false, -1);
  m_sequenceAutoplay = false;
  setModified(false);
}

void TimelineEngine::seek(double seconds) {
  if (m_duration <= 0.0 || !ClipUtils::isFiniteDouble(seconds)) {
    return;
  }

  if (m_opening) {
    m_pendingSeek = std::clamp(seconds, 0.0, m_duration);
    m_hasPendingSeek = true;
    return;
  }

  const double clamped = std::clamp(seconds, 0.0, m_duration);
  if (m_isPlaying && (m_hasAudio || m_bedActive || !bedClipAt(outputTimeFor(clamped)).isEmpty())) {
    restartAudio(clamped);
  }
  requestSeek(clamped);
}

void TimelineEngine::onTick() {
  if (!m_isPlaying) {
    return;
  }

  const double targetPosition = m_playbackBasePosition + m_playbackTimer.elapsed() / 1000.0;
  const double endLimit = (m_sequencePlaying && m_seqEnd > m_seqStart) ? m_seqEnd : m_duration;
  if (targetPosition >= endLimit) {
    // Advance along the V1 lane only; overlays are composited, never
    // promoted to program clips.
    const int next = m_sequencePlaying ? nextSequenceIndex(m_sequenceIndex) : -1;
    if (next >= 0) {
      playSequenceFrom(next);
      return;
    }
    // End of the lane (pause() alone would leave sequence mode armed now).
    stopSequence();
    pause();
    return;
  }

  // Bed coverage follows the output timeline: a bed starting (or ending)
  // mid-clip switches the bed thread without disturbing program audio.
  // Transport actions (play/seek/advance) re-sync via restartBed instead.
  updateBedForOutput(outputTimeFor(targetPosition));

  if (m_currentIsAudioOnly) {
    // No video frames: drive the cursor from the wall clock and keep the
    // audio FIFO topped up. The black placeholder frame stays put.
    m_position = targetPosition;
    emit positionChanged();
    topUpAudio();
    tryFlushAudioStages();
    return;
  }

  // Don't spam the decoder while a seek is in flight; the pending
  // target converges on frameReady.
  if (m_seeking || m_hasPendingSeek) {
    topUpAudio();
    tryFlushAudioStages();
    return;
  }
  if (targetPosition >= m_position + 0.025) {
    m_decoderThread.requestNextFrame();
  }

  topUpAudio();
  tryFlushAudioStages();
}

// Bed identity switch for the playing output time. Same-clip continuation
// is a no-op (the running decoder is already there); transport-level
// re-syncs go through restartBed, not here.
void TimelineEngine::updateBedForOutput(double outTime) {
  const QVariantMap cover = bedClipAt(outTime);
  if (cover.isEmpty()) {
    if (m_bedActive || m_bedOpening) {
      stopBed();
    }
    return;
  }
  const QString path = ClipUtils::clipPath(cover);
  const double start = ClipUtils::clipStartTime(cover);
  const bool sameActive =
      m_bedActive && m_bedPath == path && qFuzzyCompare(m_bedStartTime + 1.0, start + 1.0);
  const bool sameOpening = !m_bedActive && m_bedOpening && m_bedWantedPath == path &&
                           !m_bedClip.isEmpty() &&
                           qFuzzyCompare(ClipUtils::clipStartTime(m_bedClip) + 1.0, start + 1.0);
  if (!sameActive && !sameOpening) {
    restartBed(outTime);
  }
}

// Top up program + bed decoders while the mixed pending level (sink FIFO +
// engine stages, same output rate) is below the refill threshold.
void TimelineEngine::topUpAudio() {
  const qint64 pendingBytes =
      m_audioPlayer.bufferedBytes() + m_progStage.size() + m_bedStage.size();
  const qint64 pendingMsecs =
      static_cast<qint64>(pendingBytes / AudioPlayer::bytesPerSecond() * 1000.0);
  if (pendingMsecs >= 400) {
    return;
  }
  if (m_hasAudio && !m_audioEof) {
    m_decoderThread.requestAudioTopup();
  }
  if (m_bedActive && !m_bedEof) {
    m_bedDecoderThread.requestAudioTopup();
  }
}

void TimelineEngine::restartAudio(double position) {
  clearAudioStages();
  m_audioEof = false;
  m_progPlayedFrames = 0;
  m_audioPlayer.stop();
  // Gains (global x per-clip, incl. fades) are applied per staged chunk in
  // the engine now, so the sink stays at unity: queueData must not scale
  // a second time, and mixed program+bed chunks share one FIFO.
  m_audioPlayer.setVolume(1.0);
  m_audioPlayer.start();
  if (m_hasAudio) {
    m_decoderThread.requestAudioRestart(position);
  } else {
    m_audioEof = true;
  }
  restartBed(outputTimeFor(position));
}

void TimelineEngine::setClipScaleX(double scale) {
  const double clamped = std::clamp(scale, 0.1, 5.0);
  if (qFuzzyCompare(m_clipScaleX, clamped)) {
    return;
  }
  m_clipScaleX = clamped;
  emit clipTransformChanged();
}

void TimelineEngine::setClipScaleY(double scale) {
  const double clamped = std::clamp(scale, 0.1, 5.0);
  if (qFuzzyCompare(m_clipScaleY, clamped)) {
    return;
  }
  m_clipScaleY = clamped;
  emit clipTransformChanged();
}

void TimelineEngine::setClipRotation(double degrees) {
  const double clamped = std::clamp(degrees, -360.0, 360.0);
  if (qFuzzyCompare(m_clipRotation, clamped)) {
    return;
  }
  m_clipRotation = clamped;
  emit clipTransformChanged();
}

void TimelineEngine::setVolume(double volume) {
  const double clamped = std::clamp(volume, 0.0, 2.0);
  if (qFuzzyCompare(m_volume, clamped)) {
    return;
  }
  m_volume = clamped;
  // No player touch: per-chunk gains already multiply the live m_volume, so
  // a slider move takes effect on the next staged chunk without a restart.
  emit volumeChanged();
}

QVariantList TimelineEngine::mediaList() const {
  return m_mediaList;
}

void TimelineEngine::syncModels() {
  m_mediaModel.setItems(m_mediaList);
  m_timelineModel.setItems(m_timelineClips);
}

void TimelineEngine::pushHistory() {
  ListSnapshot snapshot;
  snapshot.mediaList = m_mediaList;
  snapshot.timelineClips = m_timelineClips;
  snapshot.volume = m_volume;
  snapshot.clipScaleX = m_clipScaleX;
  snapshot.clipScaleY = m_clipScaleY;
  snapshot.clipRotation = m_clipRotation;
  snapshot.currentSource = m_currentSource;
  {
    QMutexLocker locker(&m_dataMutex);
    snapshot.activeEffect = m_activeEffect;
  }
  m_undoStack.append(snapshot);
  while (m_undoStack.size() > kHistoryLimit) {
    m_undoStack.removeFirst();
  }
  if (!m_redoStack.isEmpty()) {
    m_redoStack.clear();
  }
  emit historyChanged();
  setModified(true);
}

void TimelineEngine::applySnapshot(const ListSnapshot& snapshot) {
  m_mediaList = snapshot.mediaList;
  m_timelineClips = snapshot.timelineClips;
  syncModels();
  emit mediaListChanged();
  emit timelineClipsChanged();
  // Placeholders restored without metadata (duration 0) are re-probed
  // asynchronously on the preload thread: opening files here would block
  // the GUI thread on file I/O. onProbeReady enriches the entry and
  // refreshes timeline clip metadata; onProbeFailed drops the still-unknown
  // placeholder — the same outcomes as a fresh import, so a redone import
  // is enriched and a failed import cannot linger as a 0s ghost entry.
  for (int i = 0; i < m_mediaList.size(); ++i) {
    const QVariantMap entry = m_mediaList.at(i).toMap();
    if (entry.value(QStringLiteral("duration"), 0.0).toDouble() > 0.0) {
      continue;
    }
    m_preloadThread.requestProbe(entry.value(QStringLiteral("path")).toString());
  }
  // Restore playback-adjacent settings without re-pushing history.
  // The audio sink stays at unity (per-chunk engine gains); just keep the
  // stored global for the next staged chunk.
  m_volume = std::clamp(snapshot.volume, 0.0, 2.0);
  emit volumeChanged();
  m_clipScaleX = std::clamp(snapshot.clipScaleX, 0.1, 5.0);
  m_clipScaleY = std::clamp(snapshot.clipScaleY, 0.1, 5.0);
  m_clipRotation = std::clamp(snapshot.clipRotation, -360.0, 360.0);
  emit clipTransformChanged();
  {
    QMutexLocker locker(&m_dataMutex);
    m_activeEffect = snapshot.activeEffect;
  }
  if (m_currentSource != snapshot.currentSource) {
    m_currentSource = snapshot.currentSource;
    emit currentSourceChanged();
    if (!m_currentSource.isEmpty()) {
      loadMedia(m_currentSource);
    } else {
      m_decoderThread.requestClose();
    }
  } else {
    refreshPreview();
  }
}

void TimelineEngine::setModified(bool modified) {
  if (m_modified == modified) {
    return;
  }
  m_modified = modified;
  emit modifiedChanged();
}

void TimelineEngine::undo() {
  if (m_undoStack.isEmpty()) {
    return;
  }
  setSequenceState(false, -1);
  m_sequenceAutoplay = false;
  ListSnapshot current;
  current.mediaList = m_mediaList;
  current.timelineClips = m_timelineClips;
  current.volume = m_volume;
  current.clipScaleX = m_clipScaleX;
  current.clipScaleY = m_clipScaleY;
  current.clipRotation = m_clipRotation;
  current.currentSource = m_currentSource;
  {
    QMutexLocker locker(&m_dataMutex);
    current.activeEffect = m_activeEffect;
  }
  m_redoStack.append(current);
  const ListSnapshot snapshot = m_undoStack.takeLast();
  applySnapshot(snapshot);
  setModified(true);
  emit historyChanged();
}

void TimelineEngine::redo() {
  if (m_redoStack.isEmpty()) {
    return;
  }
  setSequenceState(false, -1);
  m_sequenceAutoplay = false;
  ListSnapshot current;
  current.mediaList = m_mediaList;
  current.timelineClips = m_timelineClips;
  current.volume = m_volume;
  current.clipScaleX = m_clipScaleX;
  current.clipScaleY = m_clipScaleY;
  current.clipRotation = m_clipRotation;
  current.currentSource = m_currentSource;
  {
    QMutexLocker locker(&m_dataMutex);
    current.activeEffect = m_activeEffect;
  }
  m_undoStack.append(current);
  const ListSnapshot snapshot = m_redoStack.takeLast();
  applySnapshot(snapshot);
  setModified(true);
  emit historyChanged();
}

void TimelineEngine::removeMedia(const QString& filePath) {
  const QString path = ClipUtils::normalizedMediaPath(filePath);
  bool inBin = false;
  for (int i = 0; i < m_mediaList.size(); ++i) {
    if (m_mediaList.at(i).toMap().value(QStringLiteral("path")).toString() == path) {
      inBin = true;
      break;
    }
  }
  // Cascade: timeline clips referencing the file would otherwise dangle
  // (still playable/exportable after the bin entry is gone).
  bool inTimeline = false;
  for (const QVariant& v : std::as_const(m_timelineClips)) {
    if (v.toMap().value(QStringLiteral("path")).toString() == path) {
      inTimeline = true;
      break;
    }
  }
  if (!inBin && !inTimeline) {
    return;
  }
  pushHistory();
  for (int i = m_mediaList.size() - 1; i >= 0; --i) {
    if (m_mediaList.at(i).toMap().value(QStringLiteral("path")).toString() == path) {
      m_mediaList.removeAt(i);
    }
  }
  bool timelineTouched = false;
  for (int i = m_timelineClips.size() - 1; i >= 0; --i) {
    if (m_timelineClips.at(i).toMap().value(QStringLiteral("path")).toString() == path) {
      m_timelineClips.removeAt(i);
      timelineTouched = true;
    }
  }
  if (timelineTouched) {
    recomputeTimelineStarts();
    setSequenceState(false, -1);
    m_sequenceAutoplay = false;
  }
  syncModels();
  emit mediaListChanged();
  if (timelineTouched) {
    emit timelineClipsChanged();
  }
}

void TimelineEngine::clearMedia() {
  if (m_mediaList.isEmpty()) {
    return;
  }
  pushHistory();
  m_mediaList.clear();
  syncModels();
  emit mediaListChanged();
}

QVariantList TimelineEngine::timelineClips() const {
  return m_timelineClips;
}

void TimelineEngine::recomputeTimelineStarts() {
  // V1 clips lay out end-to-end in list order; V2 overlay and A1 audio
  // clips keep their absolute start times.
  double cursor = 0.0;
  for (int i = 0; i < m_timelineClips.size(); ++i) {
    QVariantMap clip = m_timelineClips.at(i).toMap();
    if (clip.value(QStringLiteral("track"), 0).toInt() == 0) {
      clip.insert(QStringLiteral("startTime"), cursor);
      m_timelineClips[i] = clip;
      cursor += clip.value(QStringLiteral("duration"), 0.0).toDouble();
    }
  }
}

QVariantMap TimelineEngine::buildTimelineClip(const QString& path) const {
  QString name = QFileInfo(path).fileName();
  double duration = 5.0;  // placeholder until the file is decoded
  for (const QVariant& v : std::as_const(m_mediaList)) {
    const QVariantMap entry = v.toMap();
    if (entry.value(QStringLiteral("path")).toString() == path) {
      name = entry.value(QStringLiteral("name"), name).toString();
      // Import placeholders carry duration 0 (unknown until the background
      // probe lands); fall back to the 5s default rather than a 0s clip.
      const double entryDuration = entry.value(QStringLiteral("duration"), duration).toDouble();
      duration = (entryDuration > 0.0) ? entryDuration : 5.0;
      break;
    }
  }
  QVariantMap clip;
  clip.insert(QStringLiteral("path"), path);
  clip.insert(QStringLiteral("name"), name);
  clip.insert(QStringLiteral("duration"), duration);
  clip.insert(QStringLiteral("sourceDuration"), duration);
  clip.insert(QStringLiteral("trimStart"), 0.0);
  clip.insert(QStringLiteral("trimEnd"), 0.0);
  clip.insert(QStringLiteral("effect"), QString());
  clip.insert(QStringLiteral("transition"), QString());
  clip.insert(QStringLiteral("transitionDuration"), 0.5);
  clip.insert(QStringLiteral("track"), 0);
  clip.insert(QStringLiteral("title"), QString());
  clip.insert(QStringLiteral("startTime"), 0.0);
  clip.insert(QStringLiteral("gain"), 1.0);
  clip.insert(QStringLiteral("muted"), false);
  clip.insert(QStringLiteral("fadeIn"), 0.0);
  clip.insert(QStringLiteral("fadeOut"), 0.0);
  return clip;
}

void TimelineEngine::setClipTitle(int index, const QString& title) {
  if (index < 0 || index >= m_timelineClips.size()) {
    return;
  }
  const QString trimmed = title.left(200);
  QVariantMap clip = m_timelineClips.at(index).toMap();
  if (clip.value(QStringLiteral("title")).toString() == trimmed) {
    return;
  }
  pushHistory();
  clip.insert(QStringLiteral("title"), trimmed);
  m_timelineClips[index] = clip;
  syncModels();
  emit timelineClipsChanged();
}

void TimelineEngine::setClipTrack(int index, int track) {
  if (index < 0 || index >= m_timelineClips.size()) {
    return;
  }
  // 0 = V1 sequence, 1 = V2 overlay, 2 = A1 audio bed (absolute start).
  const int clamped = std::clamp(track, 0, 2);
  QVariantMap clip = m_timelineClips.at(index).toMap();
  if (clip.value(QStringLiteral("track"), 0).toInt() == clamped) {
    return;
  }
  pushHistory();
  clip.insert(QStringLiteral("track"), clamped);
  m_timelineClips[index] = clip;
  recomputeTimelineStarts();
  syncModels();
  emit timelineClipsChanged();
}

void TimelineEngine::setClipStartTime(int index, double startSeconds) {
  if (index < 0 || index >= m_timelineClips.size()) {
    return;
  }
  QVariantMap clip = m_timelineClips.at(index).toMap();
  const int track = clip.value(QStringLiteral("track"), 0).toInt();
  if (track != 1 && track != 2) {
    return;
  }
  const double clamped = std::max(0.0, startSeconds);
  if (qFuzzyCompare(clip.value(QStringLiteral("startTime"), 0.0).toDouble() + 1.0, clamped + 1.0)) {
    return;
  }
  pushHistory();
  clip.insert(QStringLiteral("startTime"), clamped);
  m_timelineClips[index] = clip;
  syncModels();
  emit timelineClipsChanged();
}

void TimelineEngine::appendOverlayClip(const QString& filePath, double startSeconds) {
  const QString path = ClipUtils::normalizedMediaPath(filePath);
  if (path.isEmpty()) {
    return;
  }
  pushHistory();
  QVariantMap clip = buildTimelineClip(path);
  clip.insert(QStringLiteral("track"), 1);
  clip.insert(QStringLiteral("startTime"), std::max(0.0, startSeconds));
  m_timelineClips.append(clip);
  syncModels();
  emit timelineClipsChanged();
}

void TimelineEngine::appendAudioClip(const QString& filePath, double startSeconds) {
  const QString path = ClipUtils::normalizedMediaPath(filePath);
  if (path.isEmpty()) {
    return;
  }
  pushHistory();
  QVariantMap clip = buildTimelineClip(path);
  clip.insert(QStringLiteral("track"), 2);
  clip.insert(QStringLiteral("startTime"), std::max(0.0, startSeconds));
  m_timelineClips.append(clip);
  syncModels();
  emit timelineClipsChanged();
}

void TimelineEngine::setClipGain(int index, double gain) {
  if (index < 0 || index >= m_timelineClips.size()) {
    return;
  }
  const double clamped = std::clamp(gain, 0.0, 2.0);
  QVariantMap clip = m_timelineClips.at(index).toMap();
  if (qFuzzyCompare(ClipUtils::clipGain(clip) + 1.0, clamped + 1.0)) {
    return;
  }
  pushHistory();
  clip.insert(QStringLiteral("gain"), clamped);
  m_timelineClips[index] = clip;
  syncModels();
  emit timelineClipsChanged();
}

void TimelineEngine::setClipMuted(int index, bool muted) {
  if (index < 0 || index >= m_timelineClips.size()) {
    return;
  }
  QVariantMap clip = m_timelineClips.at(index).toMap();
  if (ClipUtils::clipMuted(clip) == muted) {
    return;
  }
  pushHistory();
  clip.insert(QStringLiteral("muted"), muted);
  m_timelineClips[index] = clip;
  syncModels();
  emit timelineClipsChanged();
}

void TimelineEngine::setClipFadeIn(int index, double seconds) {
  if (index < 0 || index >= m_timelineClips.size()) {
    return;
  }
  const double clamped = std::clamp(seconds, 0.0, 30.0);
  QVariantMap clip = m_timelineClips.at(index).toMap();
  if (qFuzzyCompare(ClipUtils::clipFadeIn(clip) + 1.0, clamped + 1.0)) {
    return;
  }
  pushHistory();
  clip.insert(QStringLiteral("fadeIn"), clamped);
  m_timelineClips[index] = clip;
  syncModels();
  emit timelineClipsChanged();
}

void TimelineEngine::setClipFadeOut(int index, double seconds) {
  if (index < 0 || index >= m_timelineClips.size()) {
    return;
  }
  const double clamped = std::clamp(seconds, 0.0, 30.0);
  QVariantMap clip = m_timelineClips.at(index).toMap();
  if (qFuzzyCompare(ClipUtils::clipFadeOut(clip) + 1.0, clamped + 1.0)) {
    return;
  }
  pushHistory();
  clip.insert(QStringLiteral("fadeOut"), clamped);
  m_timelineClips[index] = clip;
  syncModels();
  emit timelineClipsChanged();
}

void TimelineEngine::detachAudio(int index) {
  if (index < 0 || index >= m_timelineClips.size()) {
    return;
  }
  QVariantMap clip = m_timelineClips.at(index).toMap();
  if (clip.value(QStringLiteral("track"), 0).toInt() != 0) {
    return;
  }
  pushHistory();
  // Mute the video clip's audio, keep picture; add an A1 bed copy.
  clip.insert(QStringLiteral("muted"), true);
  m_timelineClips[index] = clip;
  QVariantMap bed = clip;
  bed.insert(QStringLiteral("track"), 2);
  bed.insert(QStringLiteral("muted"), false);
  bed.insert(QStringLiteral("startTime"), clip.value(QStringLiteral("startTime"), 0.0));
  m_timelineClips.append(bed);
  syncModels();
  emit timelineClipsChanged();
}

double TimelineEngine::currentEffectiveGain() const {
  const QVariantMap clip = programClipNow();
  if (clip.isEmpty()) {
    return std::clamp(m_volume, 0.0, 2.0);
  }
  return ClipUtils::clipEffectiveGain(clip, m_volume);
}

QVariantMap TimelineEngine::programClipNow() const {
  int idx = m_sequencePlaying ? m_sequenceIndex : sequenceIndexOf(m_currentSource);
  if (idx >= 0 && idx < m_timelineClips.size()) {
    return m_timelineClips.at(idx).toMap();
  }
  return {};
}

double TimelineEngine::outputTimeFor(double filePos) const {
  // Sequence playback: output = V1 row start + intra offset from the window.
  if (m_sequencePlaying && isSequenceClip(m_sequenceIndex)) {
    const QVariantMap clip = m_timelineClips.at(m_sequenceIndex).toMap();
    return ClipUtils::clipStartTime(clip) + (filePos - m_seqStart);
  }
  // Single-clip preview of a V1 row: output = row start + file offset past
  // its trim. Anything else (overlays, ad-hoc loads): identity fallback.
  const QVariantMap prog = programClipNow();
  if (!prog.isEmpty() && prog.value(QStringLiteral("track"), 0).toInt() == 0) {
    return ClipUtils::clipStartTime(prog) + (filePos - ClipUtils::clipTrimStart(prog));
  }
  return filePos;
}

QVariantMap TimelineEngine::bedClipAt(double outTime) const {
  if (!ClipUtils::isFiniteDouble(outTime)) {
    return {};
  }
  // Topmost audible bed wins (last in list order), mirroring the export
  // overlay pick. Muted/zero-gain rows never cover.
  QVariantMap cover;
  for (const QVariant& v : std::as_const(m_timelineClips)) {
    const QVariantMap c = v.toMap();
    const int track = c.value(QStringLiteral("track"), 0).toInt();
    if (track != 1 && track != 2) {
      continue;
    }
    const double start = c.value(QStringLiteral("startTime"), 0.0).toDouble();
    const double dur = ClipUtils::clipDuration(c);
    if (!(dur > 0.0) || outTime < start || outTime >= start + dur) {
      continue;
    }
    if (!(ClipUtils::clipEffectiveGain(c, m_volume) > 0.0)) {
      continue;
    }
    cover = c;
  }
  return cover;
}

QVariantMap TimelineEngine::liveBedClipNow() const {
  if (m_bedPath.isEmpty()) {
    return {};
  }
  for (const QVariant& v : std::as_const(m_timelineClips)) {
    const QVariantMap c = v.toMap();
    const int track = c.value(QStringLiteral("track"), 0).toInt();
    if ((track == 1 || track == 2) && ClipUtils::clipPath(c) == m_bedPath &&
        qFuzzyCompare(c.value(QStringLiteral("startTime"), 0.0).toDouble() + 1.0,
                      m_bedStartTime + 1.0)) {
      return c;
    }
  }
  return {};
}

void TimelineEngine::restartBed(double outTime) {
  const QVariantMap cover = bedClipAt(outTime);
  if (cover.isEmpty()) {
    stopBed();
    return;
  }
  const QString path = ClipUtils::clipPath(cover);
  const double srcOff =
      ClipUtils::clipTrimStart(cover) + (outTime - ClipUtils::clipStartTime(cover));
  m_bedClip = cover;
  m_bedStartTime = ClipUtils::clipStartTime(cover);
  m_bedPlayedFrames = 0;
  m_bedEof = false;
  m_bedStage.clear();
  if (m_bedActive && !m_bedOpening && m_bedPath == path) {
    // Same file already open: seek the running decoder to the new offset.
    m_bedDecoderThread.requestAudioRestart(std::max(0.0, srcOff));
    return;
  }
  // (Re)open path: the restart is issued from onBedLoaded once the open
  // lands, so a slow open never blocks program audio.
  m_bedWantedPath = path;
  m_bedPendingOffset = std::max(0.0, srcOff);
  m_bedOpening = true;
  m_bedActive = false;
  m_bedPath.clear();
  m_bedDecoderThread.requestOpen(path);
}

void TimelineEngine::stopBed() {
  m_bedActive = false;
  m_bedOpening = false;
  m_bedEof = false;
  m_bedWantedPath.clear();
  m_bedPath.clear();
  m_bedClip.clear();
  m_bedStage.clear();
  m_bedPlayedFrames = 0;
  m_bedDecoderThread.requestClose();
}

void TimelineEngine::clearAudioStages() {
  m_progStage.clear();
  m_bedStage.clear();
}

namespace {

// Whole sample frames (stereo s16) in a PCM chunk.
int previewChunkFrames(const QByteArray& chunk) {
  return static_cast<int>(chunk.size() / (2 * static_cast<int>(sizeof(qint16))));
}

}  // namespace

void TimelineEngine::tryFlushAudioStages() {
  const bool bedLive = m_bedActive && !m_bedEof;
  const bool progLive = m_hasAudio && !m_audioEof;
  // Steady state: both sides present — mix the common prefix at whole-frame
  // granularity and keep any remainder staged for the next chunk.
  if (!m_progStage.isEmpty() && !m_bedStage.isEmpty()) {
    const int frameBytes = 2 * static_cast<int>(sizeof(qint16));
    qsizetype n = std::min(m_progStage.size(), m_bedStage.size());
    n -= n % frameBytes;
    if (n > 0) {
      m_audioPlayer.queueData(AudioPlayer::mixAudio(m_progStage.left(n), m_bedStage.left(n)));
      m_progStage.remove(0, n);
      m_bedStage.remove(0, n);
    }
    return;
  }
  // Solo drain when the other side can no longer produce.
  if (!m_progStage.isEmpty() && !bedLive) {
    m_audioPlayer.queueData(m_progStage);
    m_progStage.clear();
    return;
  }
  if (!m_bedStage.isEmpty() && !progLive) {
    m_audioPlayer.queueData(m_bedStage);
    m_bedStage.clear();
    return;
  }
  // One side live but quiet so far: hold briefly for its chunk, then flush
  // solo so a slow bed open (or gap) never stalls program audio.
  if (!m_progStage.isEmpty() && bedLive && m_progStageTime.hasExpired(kBedMixWaitMs)) {
    m_audioPlayer.queueData(m_progStage);
    m_progStage.clear();
    return;
  }
  if (!m_bedStage.isEmpty() && progLive && m_bedStageTime.hasExpired(kBedMixWaitMs)) {
    m_audioPlayer.queueData(m_bedStage);
    m_bedStage.clear();
  }
}

bool TimelineEngine::isKnownTransition(const QString& type) {
  return type == QLatin1String("Cross Dissolve") || type == QLatin1String("Dip to Black");
}

QStringList TimelineEngine::availableEffects() const {
  return VideoEffects::availableEffects();
}

QStringList TimelineEngine::availableTransitions() const {
  return {QStringLiteral("Cross Dissolve"), QStringLiteral("Dip to Black")};
}

void TimelineEngine::setClipTransition(int index, const QString& type, double duration) {
  if (index < 0 || index >= m_timelineClips.size()) {
    return;
  }
  const QString normalized = isKnownTransition(type) ? type : QString();
  QVariantMap clip = m_timelineClips.at(index).toMap();
  const double playable = std::max(0.0, clip.value(QStringLiteral("duration"), 0.0).toDouble());
  const double clampedDur = normalized.isEmpty()
                                ? clip.value(QStringLiteral("transitionDuration"), 0.5).toDouble()
                                : std::clamp(duration, 0.1, std::max(0.1, std::min(2.0, playable)));
  if (clip.value(QStringLiteral("transition")).toString() == normalized &&
      qFuzzyCompare(clip.value(QStringLiteral("transitionDuration"), 0.0).toDouble() + 1.0,
                    clampedDur + 1.0)) {
    return;
  }
  pushHistory();
  clip.insert(QStringLiteral("transition"), normalized);
  clip.insert(QStringLiteral("transitionDuration"), clampedDur);
  m_timelineClips[index] = clip;
  syncModels();
  emit timelineClipsChanged();
}

void TimelineEngine::setClipEffect(int index, const QString& effectName) {
  if (index < 0 || index >= m_timelineClips.size()) {
    return;
  }
  const QString normalized = VideoEffects::isKnownEffect(effectName) ? effectName : QString();
  QVariantMap clip = m_timelineClips.at(index).toMap();
  if (clip.value(QStringLiteral("effect")).toString() == normalized) {
    return;
  }
  pushHistory();
  clip.insert(QStringLiteral("effect"), normalized);
  m_timelineClips[index] = clip;
  syncModels();
  emit timelineClipsChanged();
  if (clip.value(QStringLiteral("path")).toString() == m_currentSource) {
    {
      QMutexLocker locker(&m_dataMutex);
      m_activeEffect = normalized;
    }
    refreshPreview();
  }
}

void TimelineEngine::refreshPreview() {
  if (frameAvailable()) {
    ++m_frameVersion;
    emit currentFrameChanged();
  }
}

void TimelineEngine::setClipTrimStart(int index, double seconds) {
  if (index < 0 || index >= m_timelineClips.size()) {
    return;
  }
  QVariantMap clip = m_timelineClips.at(index).toMap();
  const double source = ClipUtils::clipSourceDuration(clip);
  const double trimEnd = std::max(0.0, clip.value(QStringLiteral("trimEnd"), 0.0).toDouble());
  const double clamped = std::clamp(seconds, 0.0, std::max(0.0, source - trimEnd - 0.1));
  const double playable = std::max(0.1, source - clamped - trimEnd);
  if (qFuzzyCompare(ClipUtils::clipTrimStart(clip) + 1.0, clamped + 1.0) &&
      qFuzzyCompare(clip.value(QStringLiteral("duration"), 0.0).toDouble() + 1.0, playable + 1.0)) {
    return;
  }
  pushHistory();
  clip.insert(QStringLiteral("trimStart"), clamped);
  clip.insert(QStringLiteral("duration"), playable);
  m_timelineClips[index] = clip;
  recomputeTimelineStarts();
  syncModels();
  emit timelineClipsChanged();
}

void TimelineEngine::setClipTrimEnd(int index, double seconds) {
  if (index < 0 || index >= m_timelineClips.size()) {
    return;
  }
  QVariantMap clip = m_timelineClips.at(index).toMap();
  const double source = ClipUtils::clipSourceDuration(clip);
  const double trimStart = ClipUtils::clipTrimStart(clip);
  const double clamped = std::clamp(seconds, 0.0, std::max(0.0, source - trimStart - 0.1));
  const double playable = std::max(0.1, source - trimStart - clamped);
  if (qFuzzyCompare(clip.value(QStringLiteral("trimEnd"), 0.0).toDouble() + 1.0, clamped + 1.0) &&
      qFuzzyCompare(clip.value(QStringLiteral("duration"), 0.0).toDouble() + 1.0, playable + 1.0)) {
    return;
  }
  pushHistory();
  clip.insert(QStringLiteral("trimEnd"), clamped);
  clip.insert(QStringLiteral("duration"), playable);
  m_timelineClips[index] = clip;
  recomputeTimelineStarts();
  syncModels();
  emit timelineClipsChanged();
}

void TimelineEngine::splitTimelineClip(int index, double offsetSeconds) {
  if (index < 0 || index >= m_timelineClips.size()) {
    return;
  }
  const QVariantMap clip = m_timelineClips.at(index).toMap();
  const double dur = clip.value(QStringLiteral("duration"), 0.0).toDouble();
  if (!(offsetSeconds >= 0.1 && offsetSeconds <= dur - 0.1)) {
    return;
  }
  const double trimStart = ClipUtils::clipTrimStart(clip);
  const double trimEnd = std::max(0.0, clip.value(QStringLiteral("trimEnd"), 0.0).toDouble());
  const double source = ClipUtils::clipSourceDuration(clip);
  pushHistory();
  QVariantMap left = clip;
  left.insert(QStringLiteral("trimEnd"), source - trimStart - offsetSeconds);
  left.insert(QStringLiteral("duration"), offsetSeconds);
  QVariantMap right = clip;
  right.insert(QStringLiteral("trimStart"), trimStart + offsetSeconds);
  right.insert(QStringLiteral("trimEnd"), trimEnd);
  right.insert(QStringLiteral("duration"), dur - offsetSeconds);
  if (clip.value(QStringLiteral("track"), 0).toInt() != 0) {
    // Absolute tracks (V2 overlay, A1 bed) are not re-laid out below: the
    // right half must start where the cut lands, not where the clip did.
    right.insert(QStringLiteral("startTime"), ClipUtils::clipStartTime(clip) + offsetSeconds);
  }
  m_timelineClips[index] = left;
  m_timelineClips.insert(index + 1, right);
  recomputeTimelineStarts();
  syncModels();
  emit timelineClipsChanged();
}

void TimelineEngine::previewTimelineClip(int index) {
  if (index < 0 || index >= m_timelineClips.size()) {
    return;
  }
  const QVariantMap clip = m_timelineClips.at(index).toMap();
  loadMedia(clip.value(QStringLiteral("path")).toString());
  {
    QMutexLocker locker(&m_dataMutex);
    m_activeEffect = clip.value(QStringLiteral("effect")).toString();
  }
  m_seqStart = ClipUtils::clipTrimStart(clip);
  m_seqEnd = m_seqStart + clip.value(QStringLiteral("duration"), 0.0).toDouble();
  m_pendingSeek = m_seqStart;
  m_hasPendingSeek = true;
}

void TimelineEngine::appendClipToTimeline(const QString& filePath) {
  const QString path = ClipUtils::normalizedMediaPath(filePath);
  if (path.isEmpty()) {
    return;
  }
  insertClipToTimeline(filePath, m_timelineClips.size());
}

void TimelineEngine::insertClipToTimeline(const QString& filePath, int index) {
  const QString path = ClipUtils::normalizedMediaPath(filePath);
  if (path.isEmpty()) {
    return;
  }
  const int pos = std::clamp(index, 0, static_cast<int>(m_timelineClips.size()));
  pushHistory();
  m_timelineClips.insert(pos, buildTimelineClip(path));
  recomputeTimelineStarts();
  syncModels();
  emit timelineClipsChanged();
}

void TimelineEngine::removeTimelineClip(int index) {
  if (index < 0 || index >= m_timelineClips.size()) {
    return;
  }
  // Structural edit cancels sequence mode; the loaded clip keeps
  // playing as a single preview to its natural end.
  setSequenceState(false, -1);
  m_sequenceAutoplay = false;
  pushHistory();
  m_timelineClips.removeAt(index);
  recomputeTimelineStarts();
  syncModels();
  emit timelineClipsChanged();
}

void TimelineEngine::clearTimeline() {
  if (m_timelineClips.isEmpty()) {
    return;
  }
  setSequenceState(false, -1);
  m_sequenceAutoplay = false;
  pushHistory();
  m_timelineClips.clear();
  syncModels();
  emit timelineClipsChanged();
}

void TimelineEngine::moveTimelineClip(int from, int to) {
  if (from < 0 || from >= m_timelineClips.size() || to < 0 || to >= m_timelineClips.size() ||
      from == to) {
    return;
  }
  // Remap the playing index arithmetically: path lookup would snap to the
  // wrong row when the same file appears twice or on both tracks.
  const int playingIndex = m_sequencePlaying ? m_sequenceIndex : -1;
  pushHistory();
  m_timelineClips.move(from, to);
  recomputeTimelineStarts();
  if (playingIndex >= 0) {
    int idx = playingIndex;
    if (idx == from) {
      idx = to;
    } else if (from < to && idx > from && idx <= to) {
      --idx;
    } else if (to < from && idx >= to && idx < from) {
      ++idx;
    }
    setSequenceState(true, idx);
  }
  syncModels();
  emit timelineClipsChanged();
}

QVariantMap TimelineEngine::upsertMediaEntry(const QString& path, double duration, bool hasAudio) {
  QVariantMap entry;
  entry.insert(QStringLiteral("path"), path);
  entry.insert(QStringLiteral("name"), QFileInfo(path).fileName());
  entry.insert(QStringLiteral("duration"), duration);
  entry.insert(QStringLiteral("hasAudio"), hasAudio);
  bool found = false;
  for (int i = 0; i < m_mediaList.size(); ++i) {
    if (m_mediaList.at(i).toMap().value(QStringLiteral("path")).toString() == path) {
      m_mediaList[i] = entry;
      found = true;
      break;
    }
  }
  if (!found) {
    m_mediaList.append(entry);
  }
  syncModels();
  emit mediaListChanged();
  requestThumb(path);
  if (hasAudio) {
    requestWaveform(path);
  }
  return entry;
}

void TimelineEngine::refreshTimelineClipsForPath(const QString& path,
                                                 double duration,
                                                 const QString& name) {
  bool timelineTouched = false;
  for (int i = 0; i < m_timelineClips.size(); ++i) {
    QVariantMap clip = m_timelineClips.at(i).toMap();
    if (clip.value(QStringLiteral("path")).toString() == path) {
      const double trimStart =
          std::clamp(ClipUtils::clipTrimStart(clip), 0.0, std::max(0.0, duration - 0.1));
      const double trimEnd =
          std::clamp(ClipUtils::clipTrimEnd(clip), 0.0, std::max(0.0, duration - trimStart - 0.1));
      clip.insert(QStringLiteral("name"), name);
      clip.insert(QStringLiteral("sourceDuration"), duration);
      clip.insert(QStringLiteral("trimStart"), trimStart);
      clip.insert(QStringLiteral("trimEnd"), trimEnd);
      clip.insert(QStringLiteral("duration"), std::max(0.1, duration - trimStart - trimEnd));
      m_timelineClips[i] = clip;
      timelineTouched = true;
    }
  }
  if (!timelineTouched) {
    return;
  }
  recomputeTimelineStarts();
  syncModels();
  emit timelineClipsChanged();
  // Refresh the sequence window if the loaded file corrected trims.
  if (m_sequencePlaying && m_sequenceIndex >= 0 && m_sequenceIndex < m_timelineClips.size()) {
    const QVariantMap cur = m_timelineClips.at(m_sequenceIndex).toMap();
    if (cur.value(QStringLiteral("path")).toString() == path) {
      m_seqStart = ClipUtils::clipTrimStart(cur);
      m_seqEnd = m_seqStart + cur.value(QStringLiteral("duration"), 0.0).toDouble();
    }
  }
}

// Hands off to a coalesced pending load, preserving the sequence state armed
// by playSequenceFrom() across the handoff. Returns true when the caller
// should return early (loadMedia re-arms opening/seeking state).
bool TimelineEngine::drainPendingLoad() {
  if (m_pendingLoad.isEmpty()) {
    return false;
  }
  const QString nextPath = m_pendingLoad;
  m_pendingLoad.clear();
  // Preserve the pending sequence state armed by playSequenceFrom() for
  // the next file: loadMedia() clears the effect and does not know the
  // trim start. Stash and restore around the handoff.
  const double savedSeek = m_pendingSeek;
  const bool savedHasSeek = m_hasPendingSeek;
  QString savedEffect;
  {
    QMutexLocker locker(&m_dataMutex);
    savedEffect = m_activeEffect;
  }
  const double savedSeqStart = m_seqStart;
  const double savedSeqEnd = m_seqEnd;
  const bool savedAutoplay = m_sequenceAutoplay;
  const int savedSeqIndex = m_sequenceIndex;
  const bool savedSeqPlaying = m_sequencePlaying;
  loadMedia(nextPath);
  if (savedHasSeek) {
    m_pendingSeek = savedSeek;
    m_hasPendingSeek = true;
  }
  {
    QMutexLocker locker(&m_dataMutex);
    // loadMedia cleared the effect; restore the next clip's effect
    // when the pending load matches the armed sequence clip.
    if (!savedEffect.isEmpty()) {
      m_activeEffect = savedEffect;
    }
  }
  m_seqStart = savedSeqStart;
  m_seqEnd = savedSeqEnd;
  m_sequenceAutoplay = savedAutoplay;
  if (savedSeqPlaying) {
    setSequenceState(true, savedSeqIndex);
  }
  return true;
}

void TimelineEngine::beginPlaybackAtPending(const QString& path) {
  if (m_hasPendingSeek) {
    const double pendingPosition = m_pendingSeek;
    m_hasPendingSeek = false;
    m_position = pendingPosition;
    emit positionChanged();
    requestSeek(pendingPosition, true);
  } else {
    m_position = 0.0;
    emit positionChanged();
    requestSeek(0.0, true);
  }

  if (m_sequenceAutoplay) {
    m_sequenceAutoplay = false;
    // Keep the sequence index in sync if the timeline changed mid-load.
    // Prefer the already-armed index when it still points at this file so
    // duplicate paths (or a V1+V2 pair) do not snap to the wrong row.
    int idx = -1;
    if (m_sequenceIndex >= 0 && m_sequenceIndex < m_timelineClips.size() &&
        m_timelineClips.at(m_sequenceIndex).toMap().value(QStringLiteral("path")).toString() ==
            path) {
      idx = m_sequenceIndex;
    } else {
      idx = sequenceIndexOf(path);
    }
    if (idx >= 0) {
      setSequenceState(true, idx);
    }
    play();
  }
}

void TimelineEngine::onLoaded(const QString& path,
                              double duration,
                              int width,
                              int height,
                              bool hasAudio) {
  m_opening = false;
  m_loadingSource = false;
  m_currentSource = path;
  emit currentSourceChanged();

  m_duration = duration;
  emit durationChanged();

  m_hasAudio = hasAudio;
  m_audioEof = false;
  m_currentIsAudioOnly = (width <= 0 || height <= 0) && hasAudio;
  emit hasAudioChanged();

  if (m_currentIsAudioOnly) {
    // Audio-only program: show a black placeholder so the monitor has a
    // frame; position is driven by the timer in onTick().
    QImage black(320, 180, QImage::Format_RGB32);
    black.fill(Qt::black);
    {
      QMutexLocker locker(&m_frameMutex);
      m_currentFrame = black;
    }
    ++m_frameVersion;
    emit currentFrameChanged();
  }

  const QVariantMap entry = upsertMediaEntry(path, duration, hasAudio);
  refreshTimelineClipsForPath(path, duration, entry.value(QStringLiteral("name")).toString());

  if (drainPendingLoad()) {
    return;
  }
  beginPlaybackAtPending(path);
}

void TimelineEngine::onFrameReady(double position, QImage frame) {
  const bool hasPendingSeek = m_hasPendingSeek;
  const double pendingPosition = m_pendingSeek;
  const bool hasPendingLoad = !m_pendingLoad.isEmpty();
  m_seeking = false;
  // Drop stale frames when a newer seek or file is already queued so the
  // monitor never flashes an outdated image.
  if (hasPendingSeek || hasPendingLoad) {
    if (!hasPendingLoad) {
      m_hasPendingSeek = false;
    }
    if (!m_pendingLoad.isEmpty()) {
      const QString nextPath = m_pendingLoad;
      m_pendingLoad.clear();
      loadMedia(nextPath);
      // Re-arm the pending seek cleared by loadMedia()'s coalescing path
      // when this frame was stale.
      if (hasPendingSeek) {
        m_pendingSeek = pendingPosition;
        m_hasPendingSeek = true;
      }
      return;
    }
    if (hasPendingSeek) {
      m_hasPendingSeek = false;
      requestSeek(pendingPosition);
    }
    return;
  }
  m_hasPendingSeek = false;
  m_position = position;

  {
    QMutexLocker locker(&m_frameMutex);
    m_currentFrame = std::move(frame);
  }

  ++m_frameVersion;
  emit positionChanged();
  emit currentFrameChanged();
}

void TimelineEngine::onAudioData(const QByteArray& data, double startTime) {
  Q_UNUSED(startTime)
  if (data.isEmpty()) {
    return;
  }
  // Per-chunk program gain x fade at the chunk middle. The played-frames
  // clock is the fade time base (decoder reads are sequential from the last
  // restart, reset in restartAudio).
  QByteArray chunk(data);
  const QVariantMap clip = programClipNow();
  const double gain = clip.isEmpty() ? std::clamp(m_volume, 0.0, 2.0)
                                     : ClipUtils::clipEffectiveGain(clip, m_volume);
  if (!clip.isEmpty()) {
    const double dur = std::max(ClipUtils::clipDuration(clip), 0.1);
    const double tMid =
        (m_progPlayedFrames + previewChunkFrames(chunk) / 2.0) / AudioPlayer::sampleRate();
    AudioPlayer::applyGainInPlace(chunk, gain * ClipUtils::clipFadeGain(clip, tMid, dur));
  } else {
    AudioPlayer::applyGainInPlace(chunk, gain);
  }
  m_progPlayedFrames += previewChunkFrames(chunk);
  if (m_progStage.isEmpty()) {
    m_progStageTime.restart();
  }
  m_progStage.append(chunk);
  tryFlushAudioStages();
}

void TimelineEngine::onAudioFinished() {
  m_audioEof = true;
  tryFlushAudioStages();
}

void TimelineEngine::onAudioError(const QString& message) {
  // Audio-only failure: keep video playing silent instead of spinning
  // top-up requests against an undecodable stream. The flag is cleared by
  // the next restartAudio(), so a later seek retries.
  m_audioEof = true;
  tryFlushAudioStages();
  emit failed(message);
}

void TimelineEngine::onBedLoaded(const QString& path,
                                 double duration,
                                 int width,
                                 int height,
                                 bool hasAudio) {
  Q_UNUSED(duration)
  Q_UNUSED(width)
  Q_UNUSED(height)
  if (path != m_bedWantedPath) {
    return;  // superseded open; the newest request's callback sets state.
  }
  m_bedOpening = false;
  if (!hasAudio || !isPathInProject(path)) {
    stopBed();
    return;
  }
  m_bedPath = path;
  m_bedActive = true;
  m_bedEof = false;
  m_bedPlayedFrames = 0;
  m_bedDecoderThread.requestAudioRestart(m_bedPendingOffset);
}

void TimelineEngine::onBedAudioData(const QByteArray& data, double startTime) {
  Q_UNUSED(startTime)
  if (!m_bedActive || data.isEmpty()) {
    return;
  }
  // Same per-chunk treatment as program audio, against the live bed row
  // (falls back to the start snapshot when the row is gone).
  QByteArray chunk(data);
  QVariantMap clip = liveBedClipNow();
  if (clip.isEmpty()) {
    clip = m_bedClip;
  }
  const double gain = clip.isEmpty() ? std::clamp(m_volume, 0.0, 2.0)
                                     : ClipUtils::clipEffectiveGain(clip, m_volume);
  if (!clip.isEmpty()) {
    const double dur = std::max(ClipUtils::clipDuration(clip), 0.1);
    const double tMid =
        (m_bedPlayedFrames + previewChunkFrames(chunk) / 2.0) / AudioPlayer::sampleRate();
    AudioPlayer::applyGainInPlace(chunk, gain * ClipUtils::clipFadeGain(clip, tMid, dur));
  } else {
    AudioPlayer::applyGainInPlace(chunk, gain);
  }
  m_bedPlayedFrames += previewChunkFrames(chunk);
  if (m_bedStage.isEmpty()) {
    m_bedStageTime.restart();
  }
  m_bedStage.append(chunk);
  tryFlushAudioStages();
}

void TimelineEngine::onBedAudioFinished() {
  m_bedEof = true;
  tryFlushAudioStages();
}

void TimelineEngine::onBedAudioError(const QString& message) {
  // Bed failure must never stop program audio: mark EOF so the program
  // drains solo, and report transiently instead of the sticky failed().
  m_bedEof = true;
  tryFlushAudioStages();
  emit warning(message);
}

void TimelineEngine::onBedFailed(const QString& message) {
  // Unopenable bed file (the bed thread never seeks video, so failed() is
  // always the open): drop the bed, keep program audio, warn transiently —
  // mirroring export's skip-with-warning for undecodable clips.
  stopBed();
  tryFlushAudioStages();
  emit warning(message);
}

void TimelineEngine::onDecoderFailed(const QString& message) {
  if (m_opening) {
    m_opening = false;
  } else if (m_seeking) {
    m_seeking = false;
  }

  // Audio-only program has no video frames: a stale video-seek failure must
  // not stop audio playback.
  if (m_currentIsAudioOnly && message.contains(QStringLiteral("Could not decode frame"))) {
    return;
  }

  // A project-load auto-open that fails is not a playback error: the
  // project itself loaded fine. Clear the source with a transient warning
  // instead of the sticky-error + sequence-advance path below.
  if (m_loadingSource) {
    m_loadingSource = false;
    m_hasPendingSeek = false;
    m_sequenceAutoplay = false;
    m_currentSource.clear();
    emit currentSourceChanged();
    emit warning(message);
    return;
  }

  m_hasPendingSeek = false;
  m_sequenceAutoplay = false;
  emit failed(message);

  if (!m_pendingLoad.isEmpty()) {
    const QString nextPath = m_pendingLoad;
    m_pendingLoad.clear();
    m_hasPendingSeek = false;
    setSequenceState(false, -1);
    loadMedia(nextPath);
    return;
  }

  const int next = m_sequencePlaying ? nextSequenceIndex(m_sequenceIndex) : -1;
  if (next >= 0) {
    playSequenceFrom(next);
    return;
  }
  // Terminal video failure: stop the 33ms clock so onTick() does not spin
  // NextFrame/Topup seeks against a dead decoder. The bed has no picture to
  // follow, so it stops too (a later play re-resolves coverage).
  if (m_isPlaying) {
    m_isPlaying = false;
    emit isPlayingChanged();
    m_timer.stop();
    m_audioPlayer.suspend();
  }
  stopBed();
  setSequenceState(false, -1);
}

void TimelineEngine::onDecoderClosed() {
  m_opening = false;
  m_loadingSource = false;
  m_seeking = false;
  m_hasPendingSeek = false;
}

void TimelineEngine::onSeekDropped() {
  // A coalesced seek was preempted (open/audio-restart) and will never
  // produce a frame. Release the seeking gate so later seeks/loads flow;
  // the superseding request re-arms state via its own signal. Pending-seek
  // bookkeeping is untouched: a newer forwarded seek still converges in
  // onFrameReady, and clearing twice is harmless (idempotent).
  m_seeking = false;
}

QImage TimelineEngine::currentFrame() const {
  QMutexLocker locker(&m_frameMutex);
  return m_currentFrame;
}

QImage TimelineEngine::currentFrameWithEffect(QString* effectOut) const {
  // Fixed lock order (frame then data) so the render thread always gets a
  // consistent frame/effect pair with no TOCTOU between two copies.
  QMutexLocker l1(&m_frameMutex);
  QMutexLocker l2(&m_dataMutex);
  if (effectOut) {
    *effectOut = m_activeEffect;
  }
  return m_currentFrame;
}

bool TimelineEngine::frameAvailable() const {
  QMutexLocker locker(&m_frameMutex);
  return !m_currentFrame.isNull();
}

int TimelineEngine::frameWidth() const {
  QMutexLocker locker(&m_frameMutex);
  return m_currentFrame.width();
}

int TimelineEngine::frameHeight() const {
  QMutexLocker locker(&m_frameMutex);
  return m_currentFrame.height();
}

void TimelineEngine::requestSeek(double seconds, bool force) {
  if (m_duration <= 0.0 || !ClipUtils::isFiniteDouble(seconds)) {
    return;
  }

  const double clampedPosition = std::clamp(seconds, 0.0, m_duration);
  if (m_opening) {
    m_pendingSeek = clampedPosition;
    m_hasPendingSeek = true;
    return;
  }

  if (m_seeking) {
    m_pendingSeek = clampedPosition;
    m_hasPendingSeek = true;
    m_decoderThread.requestSeek(clampedPosition);
    return;
  }

  if (!force && qFuzzyCompare(m_position, clampedPosition)) {
    return;
  }

  m_hasPendingSeek = false;
  m_seeking = true;
  // Note: m_pendingSeek intentionally untouched here — every reader
  // (onFrameReady, drainPendingLoad, beginPlaybackAtPending) consults it
  // only when m_hasPendingSeek is set, so writing it on the fresh-seek
  // path would leave a stale value for a later coalesced seek to reuse.

  if (m_isPlaying) {
    m_playbackBasePosition = clampedPosition;
    m_playbackTimer.restart();
  }

  if (m_currentIsAudioOnly) {
    // No video to seek: move the cursor, keep the black placeholder.
    m_hasPendingSeek = false;
    m_seeking = false;
    m_position = clampedPosition;
    emit positionChanged();
    return;
  }
  m_decoderThread.requestSeek(clampedPosition);
}

// ---- Independent source preview (own decoder thread, silent) ----

QImage TimelineEngine::sourceFrame() const {
  QMutexLocker locker(&m_sourceFrameMutex);
  return m_sourceFrame;
}

bool TimelineEngine::sourceFrameAvailable() const {
  QMutexLocker locker(&m_sourceFrameMutex);
  return !m_sourceFrame.isNull();
}

void TimelineEngine::setSourcePlaying(bool playing) {
  if (m_sourcePlaying == playing) {
    return;
  }
  m_sourcePlaying = playing;
  emit sourcePlayingChanged();
}

void TimelineEngine::loadSource(const QString& filePath) {
  const QString path = ClipUtils::normalizedMediaPath(filePath);
  if (path.isEmpty()) {
    return;
  }
  pauseSource();
  // No engine-level pending-open queue: DecoderThread coalesces opens
  // last-target-wins, and flags below are re-armed for the newest request.
  // m_sourceWantedPath lets stale loaded callbacks (for a superseded open)
  // drop out without touching state.
  m_sourceWantedPath = path;
  m_sourceOpening = true;
  m_sourceHasPendingSeek = false;
  m_sourceAutoplay = true;
  m_sourceDecoderThread.requestOpen(path);
}

void TimelineEngine::playSource() {
  if (m_sourcePlaying || m_sourcePath.isEmpty() || m_sourceDuration <= 0.0) {
    return;
  }
  if (m_sourcePosition >= m_sourceDuration) {
    m_sourcePosition = 0.0;
    m_sourceBasePosition = 0.0;
    m_sourcePlaybackTimer.restart();
    requestSourceSeek(0.0, true);
  } else {
    m_sourceBasePosition = m_sourcePosition;
    m_sourcePlaybackTimer.restart();
  }
  setSourcePlaying(true);
  m_sourceTimer.start();
}

void TimelineEngine::pauseSource() {
  if (!m_sourcePlaying) {
    return;
  }
  setSourcePlaying(false);
  m_sourceTimer.stop();
  m_sourceAutoplay = false;
}

void TimelineEngine::seekSource(double seconds) {
  if (m_sourceDuration <= 0.0 || !ClipUtils::isFiniteDouble(seconds)) {
    return;
  }
  requestSourceSeek(seconds);
}

void TimelineEngine::requestSourceSeek(double seconds, bool force) {
  if (m_sourceDuration <= 0.0 || !ClipUtils::isFiniteDouble(seconds)) {
    return;
  }
  const double clamped = std::clamp(seconds, 0.0, m_sourceDuration);
  if (m_sourceOpening) {
    m_sourcePendingSeek = clamped;
    m_sourceHasPendingSeek = true;
    return;
  }
  if (m_sourceSeeking) {
    m_sourcePendingSeek = clamped;
    m_sourceHasPendingSeek = true;
    m_sourceDecoderThread.requestSeek(clamped);
    return;
  }
  if (!force && qFuzzyCompare(m_sourcePosition, clamped)) {
    return;
  }
  m_sourceHasPendingSeek = false;
  m_sourceSeeking = true;
  m_sourcePendingSeek = clamped;
  if (m_sourcePlaying) {
    m_sourceBasePosition = clamped;
    m_sourcePlaybackTimer.restart();
  }
  m_sourceDecoderThread.requestSeek(clamped);
}

void TimelineEngine::onSourceTick() {
  if (!m_sourcePlaying) {
    return;
  }
  const double target = m_sourceBasePosition + m_sourcePlaybackTimer.elapsed() / 1000.0;
  if (m_sourceDuration > 0.0 && target >= m_sourceDuration) {
    pauseSource();
    return;
  }
  // Don't spam the decoder while a seek is in flight; converge on frameReady.
  if (m_sourceSeeking || m_sourceHasPendingSeek) {
    return;
  }
  if (target >= m_sourcePosition + 0.025) {
    m_sourceDecoderThread.requestNextFrame();
  }
}

void TimelineEngine::onSourceLoaded(const QString& path,
                                    double duration,
                                    int width,
                                    int height,
                                    bool hasAudio) {
  Q_UNUSED(width)
  Q_UNUSED(height)
  Q_UNUSED(hasAudio)
  if (path != m_sourceWantedPath) {
    return;  // superseded open; the newest request's callback sets state.
  }
  m_sourceOpening = false;
  m_sourcePath = path;
  emit sourcePathChanged();
  m_sourceDuration = duration;
  emit sourceDurationChanged();
  requestThumb(path);
  if (m_sourceHasPendingSeek) {
    const double pending = m_sourcePendingSeek;
    m_sourceHasPendingSeek = false;
    m_sourcePosition = pending;
    emit sourcePositionChanged();
    requestSourceSeek(pending, true);
  } else {
    m_sourcePosition = 0.0;
    emit sourcePositionChanged();
    requestSourceSeek(0.0, true);
  }
  if (m_sourceAutoplay) {
    m_sourceAutoplay = false;
    playSource();
  }
}

void TimelineEngine::onSourceFrameReady(double position, QImage frame) {
  m_sourceSeeking = false;
  if (m_sourceHasPendingSeek) {
    const double pending = m_sourcePendingSeek;
    m_sourceHasPendingSeek = false;
    requestSourceSeek(pending);
    return;
  }
  m_sourcePosition = position;
  {
    QMutexLocker locker(&m_sourceFrameMutex);
    m_sourceFrame = std::move(frame);
  }
  ++m_sourceFrameVersion;
  emit sourcePositionChanged();
  emit sourceFrameChanged();
}

void TimelineEngine::onSourceFailed(const QString& message) {
  m_sourceOpening = false;
  m_sourceSeeking = false;
  m_sourceHasPendingSeek = false;
  m_sourceAutoplay = false;
  if (m_sourcePlaying) {
    setSourcePlaying(false);
    m_sourceTimer.stop();
  }
  emit failed(QStringLiteral("Source: %1").arg(message));
}

void TimelineEngine::onSourceClosed() {
  m_sourceOpening = false;
  m_sourceSeeking = false;
  m_sourceHasPendingSeek = false;
}
