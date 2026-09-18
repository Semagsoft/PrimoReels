#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <QVector>
#include "AudioPlayer.h"
#include "DecoderThread.h"
#include "ExportThread.h"
#include "ListModels.h"
#include "PreloadThread.h"

class TimelineEngine : public QObject {
  Q_OBJECT
  Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
  Q_PROPERTY(double position READ position WRITE setPosition NOTIFY positionChanged)
  Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY isPlayingChanged)
  Q_PROPERTY(QString currentSource READ currentSource NOTIFY currentSourceChanged)
  Q_PROPERTY(QImage currentFrame READ currentFrame NOTIFY currentFrameChanged)
  Q_PROPERTY(int frameVersion READ frameVersion NOTIFY currentFrameChanged)
  Q_PROPERTY(bool frameAvailable READ frameAvailable NOTIFY currentFrameChanged)
  Q_PROPERTY(int frameWidth READ frameWidth NOTIFY currentFrameChanged)
  Q_PROPERTY(int frameHeight READ frameHeight NOTIFY currentFrameChanged)
  Q_PROPERTY(double clipScaleX READ clipScaleX WRITE setClipScaleX NOTIFY clipTransformChanged)
  Q_PROPERTY(double clipScaleY READ clipScaleY WRITE setClipScaleY NOTIFY clipTransformChanged)
  Q_PROPERTY(
      double clipRotation READ clipRotation WRITE setClipRotation NOTIFY clipTransformChanged)
  Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
  Q_PROPERTY(QVariantList mediaList READ mediaList NOTIFY mediaListChanged)
  Q_PROPERTY(QVariantList timelineClips READ timelineClips NOTIFY timelineClipsChanged)
  // Model views for QML delegates (fine-grained updates, proxy filtering).
  // QVariantList props above remain the source of truth for counts/logic,
  // tests, export, and undo snapshots.
  Q_PROPERTY(QObject* mediaModel READ mediaModel CONSTANT)
  Q_PROPERTY(QObject* timelineModel READ timelineModel CONSTANT)
  Q_PROPERTY(QObject* mediaFilterModel READ mediaFilterModel CONSTANT)
  Q_PROPERTY(bool isExporting READ isExporting NOTIFY isExportingChanged)
  Q_PROPERTY(double exportProgress READ exportProgress NOTIFY exportProgressChanged)
  Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
  Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
  Q_PROPERTY(bool isModified READ isModified NOTIFY modifiedChanged)
  Q_PROPERTY(int thumbVersion READ thumbVersion NOTIFY thumbnailsChanged)
  Q_PROPERTY(bool sequencePlaying READ sequencePlaying NOTIFY sequencePlayingChanged)
  Q_PROPERTY(int sequenceIndex READ sequenceIndex NOTIFY sequencePlayingChanged)
  Q_PROPERTY(bool hasAudio READ hasAudio NOTIFY hasAudioChanged)
  // Independent source preview: its own decoder thread + frame, so bin
  // selection scrubs without disturbing program/timeline playback.
  // Video-only (silent by design — see ARCHITECTURE.md); program keeps the
  // single audio path.
  Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY sourcePathChanged)
  Q_PROPERTY(double sourceDuration READ sourceDuration NOTIFY sourceDurationChanged)
  Q_PROPERTY(double sourcePosition READ sourcePosition NOTIFY sourcePositionChanged)
  Q_PROPERTY(bool sourcePlaying READ sourcePlaying NOTIFY sourcePlayingChanged)
  Q_PROPERTY(int sourceFrameVersion READ sourceFrameVersion NOTIFY sourceFrameChanged)
  Q_PROPERTY(bool sourceFrameAvailable READ sourceFrameAvailable NOTIFY sourceFrameChanged)

 public:
  explicit TimelineEngine(QObject* parent = nullptr);
  ~TimelineEngine() override;

  double duration() const { return m_duration; }
  double position() const { return m_position; }
  void setPosition(double pos);

  bool isPlaying() const { return m_isPlaying; }
  QString currentSource() const { return m_currentSource; }
  QImage currentFrame() const;
  bool frameAvailable() const;
  int frameWidth() const;
  int frameHeight() const;
  int frameVersion() const { return m_frameVersion; }
  double clipScaleX() const { return m_clipScaleX; }
  void setClipScaleX(double scale);
  double clipScaleY() const { return m_clipScaleY; }
  void setClipScaleY(double scale);
  double clipRotation() const { return m_clipRotation; }
  void setClipRotation(double degrees);
  double volume() const { return m_volume; }
  void setVolume(double volume);
  QVariantList mediaList() const;
  QVariantList timelineClips() const;
  QObject* mediaModel() { return &m_mediaModel; }
  QObject* timelineModel() { return &m_timelineModel; }
  QObject* mediaFilterModel() { return &m_mediaFilterModel; }
  bool sequencePlaying() const { return m_sequencePlaying; }
  int sequenceIndex() const { return m_sequenceIndex; }
  bool hasAudio() const { return m_hasAudio; }
  Q_INVOKABLE int sequenceIndexOf(const QString& filePath) const;
  // 1-based V1 ordinal of the current sequence clip (0 when idle) and the
  // total V1 count, for the monitor "Clip i/n" label (overlays excluded).
  Q_INVOKABLE int sequencePosition() const;
  Q_INVOKABLE int sequenceCount() const;

  Q_INVOKABLE void loadMedia(const QString& filePath);
  // Bin import: registers the file and probes metadata off-thread without
  // touching the program monitor (unlike loadMedia, which is preview).
  Q_INVOKABLE void importMedia(const QString& filePath);
  Q_INVOKABLE void removeMedia(const QString& filePath);
  Q_INVOKABLE void clearMedia();
  Q_INVOKABLE void appendClipToTimeline(const QString& filePath);
  Q_INVOKABLE void insertClipToTimeline(const QString& filePath, int index);
  Q_INVOKABLE void removeTimelineClip(int index);
  Q_INVOKABLE void clearTimeline();
  Q_INVOKABLE void moveTimelineClip(int from, int to);
  Q_INVOKABLE void setClipTrimStart(int index, double seconds);
  Q_INVOKABLE void setClipTrimEnd(int index, double seconds);
  Q_INVOKABLE void splitTimelineClip(int index, double offsetSeconds);
  Q_INVOKABLE void previewTimelineClip(int index);
  Q_INVOKABLE void setClipEffect(int index, const QString& effectName);
  QString activeEffect() const;
  // Single-lock snapshot of frame + effect for the render-thread provider,
  // so an effect change cannot slip between two independent copies.
  QImage currentFrameWithEffect(QString* effectOut) const;
  Q_INVOKABLE void setClipTrack(int index, int track);
  Q_INVOKABLE void setClipStartTime(int index, double startSeconds);
  Q_INVOKABLE void appendOverlayClip(const QString& filePath, double startSeconds);
  Q_INVOKABLE void appendAudioClip(const QString& filePath, double startSeconds);
  Q_INVOKABLE void setClipGain(int index, double gain);
  Q_INVOKABLE void setClipMuted(int index, bool muted);
  Q_INVOKABLE void setClipFadeIn(int index, double seconds);
  Q_INVOKABLE void setClipFadeOut(int index, double seconds);
  Q_INVOKABLE void detachAudio(int index);
  Q_INVOKABLE void setClipTitle(int index, const QString& title);
  Q_INVOKABLE void setClipTransition(int index, const QString& type, double duration);
  static bool isKnownTransition(const QString& type);
  Q_INVOKABLE QStringList availableEffects() const;
  Q_INVOKABLE QStringList availableTransitions() const;
  // Re-emit the current frame (e.g. after the previewed clip's effect changed).
  Q_INVOKABLE void refreshPreview();
  Q_INVOKABLE void play();
  Q_INVOKABLE void playSequenceFrom(int index);
  Q_INVOKABLE void stopSequence();
  Q_INVOKABLE bool saveProject(const QString& filePath);
  Q_INVOKABLE bool loadProject(const QString& filePath);
  Q_INVOKABLE void newProject();
  bool isExporting() const { return m_isExporting; }
  double exportProgress() const { return m_exportProgress; }
  Q_INVOKABLE void exportSequence(const QString& filePath);
  Q_INVOKABLE void cancelExport();
  bool canUndo() const { return !m_undoStack.isEmpty(); }
  bool canRedo() const { return !m_redoStack.isEmpty(); }
  bool isModified() const { return m_modified; }
  Q_INVOKABLE void undo();
  Q_INVOKABLE void redo();
  int thumbVersion() const;
  Q_INVOKABLE void requestThumb(const QString& filePath);
  Q_INVOKABLE bool hasThumb(const QString& filePath) const;
  QImage thumbImage(const QString& filePath) const;
  Q_PROPERTY(int waveVersion READ waveVersion NOTIFY waveformsChanged)
  int waveVersion() const;
  Q_INVOKABLE void requestWaveform(const QString& filePath);
  Q_INVOKABLE bool hasWaveform(const QString& filePath) const;
  Q_INVOKABLE QVariantList waveform(const QString& filePath) const;
  Q_INVOKABLE void pause();
  Q_INVOKABLE void seek(double seconds);
  // Non-destructive copy of queued preview PCM (tests, future meters).
  // Covers whatever the program + bed mix last flushed into the FIFO.
  QByteArray peekPreviewAudio(qint64 maxBytes = 1048576) const;

  // Source preview (independent decoder thread, silent).
  QString sourcePath() const { return m_sourcePath; }
  double sourceDuration() const { return m_sourceDuration; }
  double sourcePosition() const { return m_sourcePosition; }
  bool sourcePlaying() const { return m_sourcePlaying; }
  int sourceFrameVersion() const { return m_sourceFrameVersion; }
  QImage sourceFrame() const;
  bool sourceFrameAvailable() const;
  Q_INVOKABLE void loadSource(const QString& filePath);
  Q_INVOKABLE void playSource();
  Q_INVOKABLE void pauseSource();
  Q_INVOKABLE void seekSource(double seconds);

  // Output-timeline position (seconds) for a program file position, used to
  // resolve bed (V2/A1) coverage during preview. Sequence playback maps via
  // the V1 startTime + intra offset; single-clip preview maps via the row's
  // startTime/trim when the loaded source is a V1 row, else identity.
  double outputTimeFor(double filePos) const;
  // Topmost audible bed clip (track 1/2, effective gain > 0) covering the
  // output time, or empty when none. Last-in-list wins, mirroring export.
  QVariantMap bedClipAt(double outTime) const;

 signals:
  void durationChanged();
  void positionChanged();
  void isPlayingChanged();
  void currentSourceChanged();
  void currentFrameChanged();
  void clipTransformChanged();
  void volumeChanged();
  void mediaListChanged();
  void timelineClipsChanged();
  void sequencePlayingChanged();
  void isExportingChanged();
  void exportProgressChanged();
  void exportSucceeded(const QString& outputPath);
  void historyChanged();
  void modifiedChanged();
  void thumbnailsChanged();
  void waveformsChanged();
  void hasAudioChanged();
  void failed(const QString& message);
  // Non-fatal advisory (e.g. export finished with skipped clips): shown
  // transiently by the UI, unlike failed() which sticks until cleared.
  void warning(const QString& message);
  void sourcePathChanged();
  void sourceDurationChanged();
  void sourcePositionChanged();
  void sourcePlayingChanged();
  void sourceFrameChanged();

 private slots:
  void onThumbReady(const QString& path, const QImage& thumb);
  void onWaveformReady(const QString& path, const QVariantList& peaks);
  void onProbeReady(const QString& path, double duration, int width, int height, bool hasAudio);
  void onProbeFailed(const QString& path);
  void onExportProgress(double doneSeconds, double totalSeconds);
  void onExportFinished(const QString& outputPath);
  void onExportFailed(const QString& message);
  void onExportWarning(const QString& message);
  void onTick();
  void onLoaded(const QString& path, double duration, int width, int height, bool hasAudio);
  void onFrameReady(double position, QImage frame);
  void onAudioData(const QByteArray& data, double startTime);
  void onAudioFinished();
  void onAudioError(const QString& message);
  // Bed (A1/V2) live-mix preview slots (m_bedDecoderThread signals).
  void onBedLoaded(const QString& path, double duration, int width, int height, bool hasAudio);
  void onBedAudioData(const QByteArray& data, double startTime);
  void onBedAudioFinished();
  void onBedAudioError(const QString& message);
  void onBedFailed(const QString& message);
  void onDecoderFailed(const QString& message);
  void onDecoderClosed();
  void onSeekDropped();
  // Source preview slots (m_sourceDecoderThread signals).
  void onSourceLoaded(const QString& path, double duration, int width, int height, bool hasAudio);
  void onSourceFrameReady(double position, QImage frame);
  void onSourceFailed(const QString& message);
  void onSourceClosed();
  void onSourceTick();

 private:
  void requestSeek(double seconds, bool force = false);
  void restartAudio(double position);
  // (Re)select the bed clip covering the output time: opens/seeks the bed
  // thread when the covering clip changed, stops it when nothing covers.
  void restartBed(double outTime);
  void stopBed();
  // Per-tick bed identity switch (no-op when the same bed keeps covering).
  void updateBedForOutput(double outTime);
  // Program + bed top-ups below the refill threshold.
  void topUpAudio();
  // Mix staged program/bed PCM into the player (see .cpp for the policy).
  void tryFlushAudioStages();
  void clearAudioStages();
  // Timeline row behind the current program audio (sequence row when
  // sequence-playing, else the V1-preferring row for the loaded source).
  QVariantMap programClipNow() const;
  // Live bed row matching the active bed (path + startTime), or empty when
  // the row is gone (callers fall back to the m_bedClip snapshot).
  QVariantMap liveBedClipNow() const;
  void requestSourceSeek(double seconds, bool force = false);
  void setSourcePlaying(bool playing);

  AudioPlayer m_audioPlayer;
  DecoderThread m_decoderThread;
  DecoderThread m_sourceDecoderThread;
  // Dedicated bed decoder for live A1/V2 preview mixing. No preloader and
  // no video/thumbnail requests: audio restarts + top-ups only, so program
  // video seeks are never starved by bed work.
  DecoderThread m_bedDecoderThread;
  PreloadThread m_preloadThread;
  ExportThread m_exportThread;
  bool m_isExporting = false;
  double m_exportProgress = 0.0;
  QTimer m_timer;
  QElapsedTimer m_playbackTimer;
  mutable QMutex m_frameMutex;
  // Guards m_activeEffect and thumb/waveform caches, which are read from
  // QQuickImageProvider threads (render thread) and written on the GUI thread.
  mutable QMutex m_dataMutex;

  double m_duration = 0.0;
  double m_position = 0.0;
  double m_playbackBasePosition = 0.0;
  bool m_isPlaying = false;
  bool m_opening = false;
  // Set while loadProject's auto-load is pending: its async failure clears
  // the source with a transient warning instead of the usual sticky error
  // + sequence-advance handling (the project itself did load fine).
  bool m_loadingSource = false;
  bool m_seeking = false;
  bool m_hasPendingSeek = false;
  QString m_currentSource;
  QString m_pendingLoad;
  double m_pendingSeek = 0.0;
  QImage m_currentFrame;
  int m_frameVersion = 0;
  double m_clipScaleX = 1.0;
  double m_clipScaleY = 1.0;
  double m_clipRotation = 0.0;
  double m_volume = 1.0;
  QVariantList m_mediaList;
  QVariantList m_timelineClips;
  MediaListModel m_mediaModel;
  TimelineClipModel m_timelineModel;
  MediaFilterModel m_mediaFilterModel;
  void syncModels();
  void recomputeTimelineStarts();
  QVariantMap buildTimelineClip(const QString& path) const;
  // onLoaded() stages (keeps the 130-line handler orchestration-only).
  QVariantMap upsertMediaEntry(const QString& path, double duration, bool hasAudio);
  void refreshTimelineClipsForPath(const QString& path, double duration, const QString& name);
  // True when path backs any bin entry or timeline clip. GUI-thread only
  // (reads the lists without locking); used to drop stale async results.
  bool isPathInProject(const QString& path) const;
  // Drops thumb/wave caches + inflight sets. Only valid with empty
  // media/timeline lists (newProject): row ticks reset via the caller's
  // syncModels(), and stale late arrivals are guarded by isPathInProject.
  void clearArtworkCaches();
  bool drainPendingLoad();
  void beginPlaybackAtPending(const QString& path);
  // Snapshot-based edit history (bin + timeline lists plus the
  // playback-adjacent settings the UI exposes, so undo/load/new round-trip).
  struct ListSnapshot {
    QVariantList mediaList;
    QVariantList timelineClips;
    double volume = 1.0;
    double clipScaleX = 1.0;
    double clipScaleY = 1.0;
    double clipRotation = 0.0;
    QString currentSource;
    QString activeEffect;
  };
  void pushHistory();
  void applySnapshot(const ListSnapshot& snapshot);
  void setModified(bool modified);
  static constexpr int kHistoryLimit = 200;
  // Project-file load guards (robustness): legit projects are kilobytes;
  // these bound GUI-thread parse work and downstream list math.
  static constexpr qint64 kMaxProjectBytes = 32 * 1024 * 1024;
  static constexpr int kMaxProjectMediaEntries = 20000;
  static constexpr int kMaxProjectTimelineClips = 10000;
  // Longest single media duration accepted anywhere (also bounds timestamp
  // rescaling, waveform buckets, and timeline cursor math).
  static constexpr double kMaxMediaSeconds = 86400.0;
  QVector<ListSnapshot> m_undoStack;
  QVector<ListSnapshot> m_redoStack;
  bool m_modified = false;
  // Bin thumbnail cache (bounded, LRU-ish via insertion order).
  static constexpr int kThumbCacheLimit = 200;
  QHash<QString, QImage> m_thumbCache;
  QSet<QString> m_thumbInflight;
  QStringList m_thumbOrder;
  int m_thumbVersion = 0;
  // Waveform peak cache (normalized 0..1 per bucket).
  static constexpr int kWaveBuckets = 120;
  static constexpr int kWaveCacheLimit = 100;
  QHash<QString, QVariantList> m_waveCache;
  QSet<QString> m_waveInflight;
  QStringList m_waveOrder;
  int m_waveVersion = 0;
  void setSequenceState(bool playing, int index);
  // Sequence playback follows the V1 lane only: V2 overlays keep absolute
  // start times (see recomputeTimelineStarts) and are composited, never
  // played full-screen as program clips.
  bool isSequenceClip(int index) const;
  int nextSequenceIndex(int from) const;
  int resolveSequenceIndex(int index) const;
  bool m_sequencePlaying = false;
  int m_sequenceIndex = -1;
  bool m_sequenceAutoplay = false;
  // Playback window inside the loaded file for sequence/trimmed preview.
  double m_seqStart = 0.0;
  double m_seqEnd = 0.0;
  // Effect of the currently previewed timeline clip ("" = none).
  QString m_activeEffect;
  bool m_hasAudio = false;
  bool m_audioEof = false;
  bool m_currentIsAudioOnly = false;
  double currentEffectiveGain() const;
  // Bed (A1/V2) live-mix preview state (GUI thread only). The bed thread
  // owns its decoder; the engine only tracks which clip is active and
  // stages PCM per source until both sides arrive for mixing.
  QString m_bedWantedPath;
  QString m_bedPath;
  QVariantMap m_bedClip;
  double m_bedStartTime = 0.0;
  bool m_bedActive = false;
  bool m_bedOpening = false;
  bool m_bedEof = false;
  double m_bedPendingOffset = 0.0;
  QByteArray m_progStage;
  QByteArray m_bedStage;
  QElapsedTimer m_progStageTime;
  QElapsedTimer m_bedStageTime;
  // Cumulative played frames per source since the last (re)start: the fade
  // clock for per-chunk clipFadeGain (decoder reads are sequential, so the
  // file-time signal arg is redundant and ignored for this purpose).
  int64_t m_progPlayedFrames = 0;
  int64_t m_bedPlayedFrames = 0;
  // Max time to hold one side's chunk waiting for the other before flushing
  // solo (prevents stalls at stream start/end).
  static constexpr int kBedMixWaitMs = 80;
  // Independent source preview state (GUI thread only, like the program
  // position/duration fields). Frames cross from the source decoder thread
  // under m_sourceFrameMutex.
  QTimer m_sourceTimer;
  QElapsedTimer m_sourcePlaybackTimer;
  mutable QMutex m_sourceFrameMutex;
  QImage m_sourceFrame;
  int m_sourceFrameVersion = 0;
  QString m_sourcePath;
  QString m_sourceWantedPath;
  double m_sourceDuration = 0.0;
  double m_sourcePosition = 0.0;
  double m_sourceBasePosition = 0.0;
  bool m_sourcePlaying = false;
  bool m_sourceOpening = false;
  bool m_sourceSeeking = false;
  bool m_sourceHasPendingSeek = false;
  double m_sourcePendingSeek = 0.0;
  bool m_sourceAutoplay = false;
};
