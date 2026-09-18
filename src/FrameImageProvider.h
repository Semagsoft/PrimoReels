#pragma once

#include <QImage>
#include <QPointer>
#include <QQuickImageProvider>
#include <QSize>

class TimelineEngine;

class FrameImageProvider : public QQuickImageProvider {
 public:
  explicit FrameImageProvider(TimelineEngine* engine);

  QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

 private:
  QPointer<TimelineEngine> m_engine;
};

// Serves bin thumbnails from the engine cache.
// Id scheme: percent-encoded media path + "/" + version (cache-buster).
class ThumbImageProvider : public QQuickImageProvider {
 public:
  explicit ThumbImageProvider(TimelineEngine* engine);

  QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

 private:
  QPointer<TimelineEngine> m_engine;
};

// Serves the independent source-preview frame (no effect applied).
class SourceImageProvider : public QQuickImageProvider {
 public:
  explicit SourceImageProvider(TimelineEngine* engine);

  QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

 private:
  QPointer<TimelineEngine> m_engine;
};
