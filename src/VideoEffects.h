#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

// CPU video effects applied per-frame in the monitor path and the exporter.
// Effect names match the Library panel catalog; unknown/empty names are
// passed through unchanged.
class VideoEffects {
 public:
  static QStringList availableEffects();
  static bool isKnownEffect(const QString& name);
  static QImage apply(const QImage& frame, const QString& effectName);

 private:
  static QImage toRgb32(const QImage& frame);
  static QImage blur(const QImage& frame);
  static QImage colorCorrect(const QImage& frame);
  static QImage sharpen(const QImage& frame);
  static QImage vignette(const QImage& frame);
  static QImage glitch(const QImage& frame);
  static QImage sepia(const QImage& frame);
  static QImage grayscale(const QImage& frame);
  static QImage invert(const QImage& frame);
};
