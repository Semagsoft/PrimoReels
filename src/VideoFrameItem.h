#pragma once

#include <QImage>
#include <QPointer>
#include <QQuickPaintedItem>

class TimelineEngine;

class VideoFrameItem : public QQuickPaintedItem {
  Q_OBJECT
  Q_PROPERTY(QObject* engine READ engine WRITE setEngine NOTIFY engineChanged)
  Q_PROPERTY(bool sourceMode READ sourceMode WRITE setSourceMode NOTIFY sourceModeChanged)

 public:
  explicit VideoFrameItem(QQuickItem* parent = nullptr);

  QObject* engine() const;
  void setEngine(QObject* engine);

  bool sourceMode() const;
  void setSourceMode(bool sourceMode);

  void paint(QPainter* painter) override;

 signals:
  void engineChanged();
  void sourceModeChanged();

 private slots:
  void onFrameChanged();

 private:
  QPointer<TimelineEngine> m_engine;
  bool m_sourceMode = false;
};
