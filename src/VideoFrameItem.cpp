#include "VideoFrameItem.h"
#include <QPainter>
#include <algorithm>
#include <cmath>
#include "TimelineEngine.h"
#include "VideoEffects.h"

VideoFrameItem::VideoFrameItem(QQuickItem* parent) : QQuickPaintedItem(parent) {
  setRenderTarget(QQuickPaintedItem::FramebufferObject);
}

QObject* VideoFrameItem::engine() const {
  return m_engine.data();
}

void VideoFrameItem::setEngine(QObject* engine) {
  if (m_engine == engine) {
    return;
  }
  if (m_engine) {
    disconnect(m_engine, nullptr, this, nullptr);
  }
  m_engine = qobject_cast<TimelineEngine*>(engine);
  if (m_engine) {
    if (m_sourceMode) {
      connect(m_engine, &TimelineEngine::sourceFrameChanged, this, &VideoFrameItem::onFrameChanged);
    } else {
      connect(m_engine, &TimelineEngine::currentFrameChanged, this,
              &VideoFrameItem::onFrameChanged);
    }
  }
  emit engineChanged();
  update();
}

bool VideoFrameItem::sourceMode() const {
  return m_sourceMode;
}

void VideoFrameItem::setSourceMode(bool sourceMode) {
  if (m_sourceMode == sourceMode) {
    return;
  }
  m_sourceMode = sourceMode;
  if (m_engine) {
    disconnect(m_engine, nullptr, this, nullptr);
    if (m_sourceMode) {
      connect(m_engine, &TimelineEngine::sourceFrameChanged, this, &VideoFrameItem::onFrameChanged);
    } else {
      connect(m_engine, &TimelineEngine::currentFrameChanged, this,
              &VideoFrameItem::onFrameChanged);
    }
  }
  emit sourceModeChanged();
  update();
}

void VideoFrameItem::onFrameChanged() {
  update();
}

void VideoFrameItem::paint(QPainter* painter) {
  if (!m_engine) {
    return;
  }

  QImage frame;
  if (m_sourceMode) {
    frame = m_engine->sourceFrame();
  } else {
    QString effect;
    const QImage current = m_engine->currentFrameWithEffect(&effect);
    frame = VideoEffects::apply(current, effect);
  }

  if (frame.isNull()) {
    return;
  }

  // Letterbox using the decoder-reported display aspect (storage aspect
  // adjusted by sample aspect ratio, e.g. anamorphic or 9:16 phone clips)
  // instead of stretching to the item's rect.
  const double aspect =
      m_sourceMode ? m_engine->sourceAspectRatio() : m_engine->currentAspectRatio();
  if (aspect <= 0.0) {
    return;
  }
  const QRectF target = boundingRect();
  const double scale = std::min(target.width() / aspect, target.height());
  const QSizeF fitted(aspect * scale, scale);
  const QRectF dest(target.x() + (target.width() - fitted.width()) / 2.0,
                    target.y() + (target.height() - fitted.height()) / 2.0, fitted.width(),
                    fitted.height());
  painter->setRenderHint(QPainter::SmoothPixmapTransform, scale < 1.0);
  painter->drawImage(dest, frame, frame.rect());
}
