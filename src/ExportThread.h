#pragma once

#include <QMutex>
#include <QString>
#include <QThread>
#include <QVariant>
#include <QVariantList>
#include <QWaitCondition>

// Background sequence exporter: re-encodes timeline clips (video + audio)
// into a single MP4 file. All FFmpeg work happens in the worker thread.
class ExportThread : public QThread {
  Q_OBJECT
 public:
  static constexpr int outputFps() { return 30; }

  ExportThread();
  ~ExportThread() override;

  void requestExport(const QVariantList& clips, const QString& outputPath, double volume = 1.0);
  void requestCancel();
  bool isExporting() const;

 signals:
  void progressChanged(double doneSeconds, double totalSeconds);
  void exportFinished(const QString& outputPath);
  void exportFailed(const QString& message);
  // Non-fatal: export succeeded but some clips were skipped.
  void exportWarning(const QString& message);

 protected:
  void run() override;

 private:
  bool runExport(const QVariantList& clips,
                 const QString& outputPath,
                 QString* error,
                 QString* warning = nullptr,
                 double volume = 1.0);
  bool isCancelRequested() const;

  mutable QMutex m_mutex;
  QWaitCondition m_condition;
  bool m_abort = false;
  bool m_hasRequest = false;
  bool m_cancelRequested = false;
  bool m_exporting = false;
  QVariantList m_pendingClips;
  QString m_pendingOutput;
  double m_pendingVolume = 1.0;
};
