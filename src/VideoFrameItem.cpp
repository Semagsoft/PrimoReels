#include "VideoFrameItem.h"
#include <QPainter>
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

  const QRectF targetRect = boundingRect();
  painter->drawImage(targetRect, frame, frame.rect());
}
