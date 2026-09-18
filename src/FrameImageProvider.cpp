#include "FrameImageProvider.h"

#include <QUrl>

#include "TimelineEngine.h"
#include "VideoEffects.h"

FrameImageProvider::FrameImageProvider(TimelineEngine* engine)
    : QQuickImageProvider(QQuickImageProvider::Image), m_engine(engine) {}

QImage FrameImageProvider::requestImage(const QString& id,
                                        QSize* size,
                                        const QSize& requestedSize) {
  Q_UNUSED(id)
  Q_UNUSED(requestedSize)

  QImage frame;
  if (m_engine) {
    QString effect;
    const QImage current = m_engine->currentFrameWithEffect(&effect);
    frame = VideoEffects::apply(current, effect);
  }
  if (frame.isNull()) {
    frame = QImage(1, 1, QImage::Format_ARGB32);
    frame.fill(Qt::transparent);
  }
  if (size) {
    *size = frame.size();
  }
  return frame;
}

ThumbImageProvider::ThumbImageProvider(TimelineEngine* engine)
    : QQuickImageProvider(QQuickImageProvider::Image), m_engine(engine) {}

QImage ThumbImageProvider::requestImage(const QString& id,
                                        QSize* size,
                                        const QSize& requestedSize) {
  Q_UNUSED(requestedSize)

  QImage thumb;
  if (m_engine) {
    // Strip the "/version" cache-buster suffix.
    QString encoded = id;
    const int slash = id.lastIndexOf(QChar('/'));
    if (slash > 0) {
      encoded = id.left(slash);
    }
    thumb = m_engine->thumbImage(QUrl::fromPercentEncoding(encoded.toUtf8()));
  }
  if (thumb.isNull()) {
    thumb = QImage(1, 1, QImage::Format_ARGB32);
    thumb.fill(Qt::transparent);
  }
  if (size) {
    *size = thumb.size();
  }
  return thumb;
}

SourceImageProvider::SourceImageProvider(TimelineEngine* engine)
    : QQuickImageProvider(QQuickImageProvider::Image), m_engine(engine) {}

QImage SourceImageProvider::requestImage(const QString& id,
                                         QSize* size,
                                         const QSize& requestedSize) {
  Q_UNUSED(id)
  Q_UNUSED(requestedSize)

  QImage frame;
  if (m_engine) {
    frame = m_engine->sourceFrame();
  }
  if (frame.isNull()) {
    frame = QImage(1, 1, QImage::Format_ARGB32);
    frame.fill(Qt::transparent);
  }
  if (size) {
    *size = frame.size();
  }
  return frame;
}
